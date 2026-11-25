#include "Model.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace rt::model {
namespace {
QByteArray readFile(const QString &path, qint64 limit, const Task &t) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail(file.errorString());
    auto size = file.size();
    if (size < 0 || size > limit)
        fail(QString("文件大小 %1 超过读取上限 %2 字节").arg(size).arg(limit));
    QByteArray bytes(size, Qt::Uninitialized);
    qint64 pos = 0;
    while (pos < size) {
        t.check();
        auto n = file.read(bytes.data() + pos, std::min<qint64>(size - pos, 1 << 20));
        if (n <= 0)
            fail("读取截断 " + file.errorString());
        pos += n;
    }
    if (!file.atEnd())
        fail("读取期间文件大小发生变化");
    return bytes;
}

struct UnitScale {
    double toMm = 1;
    bool assumed = false;
    QString text;
};

std::optional<double> knownUnitScale(const QString &units) {
    const QString u = units.trimmed().toLower();
    auto matches = [&](std::initializer_list<const char *> keys) {
        for (auto key : keys)
            if (u == QString::fromUtf8(key))
                return true;
        return false;
    };
    // Match complete unit names: an unsupported prefix (e.g. nanometres) must
    // never silently become metres simply because it contains "metre".
    if (matches({"mm", "millimetre", "millimetres", "millimeter", "millimeters", "毫米"}))
        return 1;
    if (matches({"cm", "centimetre", "centimetres", "centimeter", "centimeters", "厘米"}))
        return 10;
    if (matches(
            {"um", "µm", "μm", "micrometre", "micrometres", "micrometer", "micrometers", "微米"}))
        return 0.001;
    if (matches({"km", "kilometre", "kilometres", "kilometer", "kilometers", "千米", "公里"}))
        return 1e6;
    if (matches({"m", "metre", "metres", "meter", "meters", "米"}))
        return 1000;
    if (matches({"in", "inch", "inches", "英寸"}))
        return 25.4;
    if (matches({"ft", "feet", "foot", "英尺"}))
        return 304.8;
    return std::nullopt;
}

UnitScale unitScale(const QString &units) {
    const QString raw = units.trimmed();
    const auto open = raw.indexOf('(');
    const bool annotated = open >= 0 && raw.endsWith(')');
    auto scale = knownUnitScale(annotated ? raw.first(open) : raw);
    if (annotated && scale != knownUnitScale(raw.sliced(open + 1, raw.size() - open - 2)))
        scale.reset();
    return {scale.value_or(1), !scale.has_value(), raw};
}

struct PointReader {
    const Field *xyz = nullptr;
    const Field *axis[3]{};
    qint64 count = 0;

    void read(qint64 i, double out[3]) const {
        if (xyz) {
            for (int k = 0; k < 3; ++k)
                out[k] = displayNumber(xyz->type,
                                       xyz->data.constData() + (i * 3 + k) * width(xyz->type));
        } else {
            for (int k = 0; k < 3; ++k)
                out[k] = displayNumber(axis[k]->type,
                                       axis[k]->data.constData() + i * width(axis[k]->type));
        }
    }
};

PointReader pointReader(const Dataset &d, int piece) {
    PointReader r;
    if (auto f = d.find("Points", piece)) {
        if (f->components != 3 || f->list)
            fail("点坐标必须是非变长三分量字段");
        r.xyz = f;
        r.count = f->size();
        return r;
    }
    if (piece == 0 && d.descriptor.contains("coordinateFields")) {
        auto ids = d.descriptor["coordinateFields"].toArray();
        if (ids.size() != 3)
            fail("分列坐标必须含三个字段");
        for (int k = 0; k < 3; ++k) {
            auto id = ids[k].toInt(-1);
            if (id < 0 || size_t(id) >= d.fields.size())
                fail("分列坐标字段无效");
            r.axis[k] = &d.fields[size_t(id)];
            if (r.axis[k]->components != 1 || r.axis[k]->list)
                fail("分列坐标字段必须是单分量定长字段");
        }
        r.count = r.axis[0]->size();
    }
    return r;
}

Dataset tracking(QByteArray bytes) {
    Dataset d;
    d.bx = std::move(bytes);
    d.tracking = aurora::decodeBx(reinterpret_cast<const uint8_t *>(d.bx.constData()), d.bx.size());
    d.descriptor = {{"kind", "Tracking"},
                    {"origin", "Aurora BX"},
                    {"units", "mm"},
                    {"frame", "Aurora 工具跟踪坐标系"},
                    {"pieces", 1},
                    {"version", "single BX reply"},
                    {"systemStatus", d.tracking->systemStatus}};
    auto field = [&](QString name, Type type, int n) {
        Field f;
        f.section = "Tracking";
        f.name = name;
        f.type = type;
        f.components = n;
        d.fields.push_back(f);
    };
    field("handle", Type::U8, 1);
    field("status", Type::U8, 1);
    field("transform_record", Type::U8, 1);
    field("position_mm", Type::F32, 3);
    field("quaternion_wxyz", Type::F32, 4);
    field("quality", Type::F32, 1);
    field("port_record", Type::U8, 1);
    field("port_status", Type::U32, 1);
    field("device_frame", Type::U32, 1);
    auto append = [&](int i, const auto &value) {
        d.fields[i].data.append(reinterpret_cast<const char *>(&value), sizeof value);
    };
    quint8 index = 0;
    for (const auto &tool : d.tracking->tools) {
        append(0, tool.handle);
        append(1, tool.status);
        if (tool.status == aurora::HandleStatus::Valid) {
            append(2, index);
            append(3, tool.positionMm);
            append(4, tool.quaternion);
            append(5, tool.quality);
        }
        if (tool.status != aurora::HandleStatus::Disabled) {
            append(6, index);
            append(7, tool.portStatus);
            append(8, tool.frameNumber);
        }
        ++index;
    }
    return d;
}

void resources(Dataset &d, const QString &path, const Task &t) {
    auto refs = d.descriptor.value("resources").toArray();
    if (refs.isEmpty())
        return;
    const QDir dir = QFileInfo(path).absoluteDir();
    auto base = dir.canonicalPath() + "/";
    QJsonArray details;
    qint64 used = 0;
    for (auto ref : refs) {
        QString name = ref.toString();
        QFileInfo info(dir, name);
        QString canonical = info.canonicalFilePath();
        QJsonObject record{{"reference", name}};
        if (!info.isFile() || canonical.isEmpty()) {
            record["state"] = "missing";
        } else if (QDir::isAbsolutePath(name) || !canonical.startsWith(base, Qt::CaseInsensitive)) {
            record["state"] = "external reference outside model directory";
        } else {
            Field f;
            f.section = "Resource";
            f.name = name;
            f.data = readFile(canonical, 256 * 1024 * 1024 - used, t);
            used += f.data.size();
            record["state"] = "embedded";
            record["sha256"] = digest(f.data);
            d.fields.push_back(std::move(f));
        }
        details.append(record);
    }
    d.descriptor["resourceStatus"] = details;
}

QString timestamp() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

struct Stage {
    QString final;
    QTemporaryFile temp;

    explicit Stage(const QString &path)
        : final(path), temp(QFileInfo(path).absoluteDir().filePath(".simtransfer-XXXXXX.tmp")) {
        if (!temp.open())
            fail("无法在目标目录创建临时文件 " + path + " " + temp.errorString());
    }

    void close() {
        if (!temp.flush())
            fail("临时文件 flush 失败 " + temp.errorString());
        temp.close();
        if (temp.error() != QFileDevice::NoError)
            fail("关闭临时文件失败 " + temp.errorString());
    }

    void publish() {
        if (QFileInfo::exists(final))
            fail("目标已经存在 " + final);
        if (!temp.rename(final))
            fail("文件提交失败 " + final + " " + temp.errorString());
        temp.setAutoRemove(false);
    }
};

void modelWrite(QIODevice &file, const Dataset &d, const QString &fmt, const Task &t) {
    if (fmt == "Aurora BX") {
        writeBx(file, d, t);
        return;
    }
    if (d.descriptor.value("kind").toString() == "BxPointCloud" || d.tracking)
        fail("BX 点云或跟踪数据不支持反向导出为模型文件");
    if (fmt == "PLY")
        writePly(file, d, t);
    else if (fmt == "VTP")
        writeVtp(file, d, t);
    else if (fmt == "legacy VTK")
        writeLegacy(file, d, t);
    else
        fail("未知模型导出格式 " + fmt);
}
} // namespace

QString formatForPath(const QString &path) {
    auto suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "ply")
        return "PLY";
    if (suffix == "vtp")
        return "VTP";
    if (suffix == "vtk")
        return "legacy VTK";
    if (suffix == "h5" || suffix == "hdf5")
        return "HDF5";
    if (suffix == "bx" || suffix == "ndi")
        return "Aurora BX";
    return {};
}

Snapshot load(const QString &path, const Task &t, const QString &expected) {
    try {
        t.report(1, "打开 " + path);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            fail(file.errorString());
        auto prefix = file.peek(4096);
        file.close();
        QString format;
        if (prefix.startsWith("ply\n") || prefix.startsWith("ply\r\n"))
            format = "PLY";
        else if (prefix.startsWith("# vtk DataFile"))
            format = "legacy VTK";
        else if (prefix.startsWith("\211HDF\r\n\032\n"))
            format = "HDF5";
        else if (prefix.trimmed().startsWith('<') || prefix.startsWith("\xef\xbb\xbf"))
            format = "VTP";
        else if (prefix.startsWith("\xc4\xa5") || expected == "Aurora BX")
            format = "Aurora BX";
        else
            fail("无法识别文件内容");
        if (!expected.isEmpty() && format != expected)
            fail("识别到 " + format + "，预期 " + expected);
        Dataset d;
        if (format == "HDF5") {
            QJsonObject job;
            d = archiveRead(path, t, &job);
            d.previousJob = job;
            if (d.tracking) {
                auto parsed = tracking(d.bx);
                compare(parsed, d, t);
            }
            d.sourceHash = fileHash(path, t);
        } else {
            auto bytes = readFile(path, 768LL * 1024 * 1024, t);
            if (format == "PLY")
                d = readPly(bytes, t);
            else if (format == "VTP")
                d = readVtp(bytes, t);
            else if (format == "legacy VTK")
                d = readLegacy(bytes, t);
            else
                d = readBx(bytes, t, "local");
            d.sourceHash = digest(bytes);
            resources(d, path, t);
        }
        d.path = QFileInfo(path).absoluteFilePath();
        d.loadedFormat = format;
        t.report(72, "校验字段、数量与拓扑");
        validate(d, t);
        t.check();
        return std::make_shared<const Dataset>(std::move(d));
    } catch (const Cancelled &) {
        throw;
    } catch (const std::bad_alloc &) {
        fail(path + " 可用内存不足");
    } catch (const std::exception &e) {
        fail(path + " — " + QString::fromUtf8(e.what()));
    }
}

QString Assessment::summary() const {
    return QString("原样保留：%1\n显式映射：%2\n省略属性：%3\n无法表达：%4\n适配器限制：%5\n"
                   "类型、数值、结构：%6")
        .arg(preserved.join("、"), mapping.isEmpty() ? "无需映射" : mapping.join("；"),
             omitted.isEmpty() ? "无" : omitted.join("、"),
             impossible.isEmpty() ? "无" : impossible.join("、"),
             unavailable.isEmpty() ? "无" : unavailable.join("、"),
             allowed() ? "原声明可恢复，数值位和顺序必须通过回读校验" : "拒绝严格导出");
}

Assessment assess(const Dataset &d, const QString &fmt) {
    Assessment a;
    a.preserved << "数据集描述、单位、坐标系、原始顺序";
    const bool bxCloud = d.descriptor.value("kind").toString() == "BxPointCloud";
    const bool tracking =
        d.tracking.has_value() || d.descriptor.value("kind").toString() == "Tracking";
    if (fmt == "HDF5") {
        a.preserved << (bxCloud ? "全部 BX 点云位置、工具关联与来源统计"
                                : "全部已解析的定长和变长字段、资源状态及来源声明");
        return a;
    }
    if (fmt == "Aurora BX") {
        if (bxCloud || tracking) {
            a.impossible << "BX 仅支持加载与点云预览；本模块不支持将 BX 反向导出为 BX 或 "
                            "PLY/VTP/VTK 等模型文件，也不恢复颜色、面连接等属性。";
            return a;
        }
        qint64 total = 0;
        for (int piece = 0; piece < d.pieces(); ++piece)
            total += d.points(piece);
        if (!total) {
            a.impossible << "源模型没有可用点，无法生成 BX 帧序列。";
            return a;
        }
        const auto unit = unitScale(d.descriptor["units"].toString());
        a.preserved << "全部顶点数量、输入顺序、逐点对应关系、重复点及未被面引用的点";
        a.mapping << QString("输入 %1 个顶点，输出 %1 个标准 BX 响应帧（每帧 53 字节）").arg(total);
        a.mapping << "每个顶点生成一个单工具 BX 响应帧：Handle 0x0A、Valid、Q=(1,0,0,0)、"
                     "Indicator 0、PortHandleStatus 0x31、FrameNumber 从 8 起每帧 +8、"
                     "SystemStatus 0";
        a.mapping << (unit.assumed ? QString("源单位未知，采用仿真默认比例 1 模型单位 = 1 mm")
                                   : QString("源单位 %1，按 1 单位 = %2 mm 换算")
                                         .arg(unit.text, QString::number(unit.toMm, 'g', 12)));
        a.mapping << "坐标最后转换为 IEEE 754 Float32；允许正常舍入并统计最大绝对误差";
        a.omitted << "颜色、面连接、纹理、法向、点属性及 BX 无对应字段的其他内容";
        a.omitted << "真实工具姿态与运动时序（不从法向、面绕序或相邻点推断）";
        return a;
    }
    if (bxCloud || tracking) {
        a.impossible << "BX 仅支持加载与点云预览；本模块不支持将 BX 反向导出为 "
                        "PLY/VTP/VTK 等模型文件，也不恢复颜色、面连接等属性。";
        return a;
    }
    if (fmt != "PLY" && fmt != "VTP" && fmt != "legacy VTK") {
        a.unavailable << "未知目标格式";
        return a;
    }
    if (d.pieces() != 1 && fmt != "VTP")
        a.impossible << "目标缺少 VTP Piece 分块结构";
    if (!d.descriptor["resources"].toArray().isEmpty())
        a.unavailable << "外部贴图只归档内容或缺失状态，模型导出未实现多文件资源提交";
    bool floatColors = false;
    bool byteColors = false;
    const bool nativePly = fmt == "PLY" && d.descriptor.contains("plyHeader");
    std::set<std::tuple<int, QString, QString>> nativeNames;
    std::set<std::tuple<int, QString, QString>> nativeRoles;
    for (const auto &f : d.fields) {
        const auto label = f.section + "/" + f.name;
        if (nativePly)
            continue;
        if ((fmt == "VTP" || fmt == "legacy VTK") &&
            (f.section == "FieldData" || f.section == "PointData" || f.section == "CellData" ||
             f.section == "LookupTable")) {
            auto name = fmt == "legacy VTK" && f.name.isEmpty() ? QString("unnamed") : f.name;
            if (!nativeNames.emplace(f.piece, f.section, name).second)
                a.unavailable << label + " 在同一分块和关联中重名，常规 VTK 读者不能区分";
            if (fmt == "VTP" && !f.role.isEmpty() &&
                (f.section == "PointData" || f.section == "CellData")) {
                auto role = f.role == "Colors" ? QString("Scalars") : f.role;
                if (!nativeRoles.emplace(f.piece, f.section, role).second)
                    a.mapping << label + " 以独立原生数组保留；同一角色仅首组选为活动数组，其余源角"
                                         "色写入字段映射";
            }
        }
        if (fmt == "legacy VTK" && topology(f.section)) {
            auto vt = topologyValueType(f);
            auto ot = topologyOffsetType(f);
            if (vt != ot || (vt != Type::I32 && vt != Type::I64))
                a.unavailable << label + " 的索引类型不能原样写为 legacy 5.1 所需的同类型 Int32 或 "
                                         "Int64 偏移和连接";
        }
        if ((f.list && !topology(f.section)) || f.section.startsWith("PLY:"))
            a.unavailable << label + " 的自定义元素或变长属性";
        if (fmt == "PLY") {
            if (topology(f.section) && f.section != "Polys")
                a.impossible << label + " 的单元类型";
            if (f.section == "FieldData" || f.section == "LookupTable")
                a.unavailable << label + " 的整体关联";
            if (!topology(f.section) && (f.type == Type::I64 || f.type == Type::U64))
                a.impossible << label + " 的 64 位整数类型超出标准 PLY";
        }
        if (fmt == "VTP" && f.section == "LookupTable")
            a.unavailable << label + " 的 legacy 查找表";
        if (fmt == "legacy VTK" && (f.role == "Colors" || f.section == "LookupTable")) {
            if (f.type == Type::F32)
                floatColors = true;
            else if (f.type == Type::U8)
                byteColors = true;
            else
                a.impossible << label + " 的颜色类型无法用 COLOR_SCALARS 原样表示";
        }
    }
    if (floatColors && byteColors)
        a.impossible << "legacy ASCII 浮点颜色与二进制 UInt8 颜色不能同时原样编码";
    if (!nativePly)
        a.mapping << "使用 v1 字段映射保留原名称、关联、声明与顺序";
    a.preserved << "坐标、可表示的拓扑及数值属性";
    if (fmt == "PLY" && !nativePly)
        a.mapping << "校验索引后写入 PLY 整数连接，原始索引类型记录在映射中";
    if (floatColors)
        a.mapping << "COLOR_SCALARS 写 ASCII Float32，避免量化";
    return a;
}

void writeBx(QIODevice &out, const Dataset &d, const Task &t, BxWriteStats *statsOut) {
    if (d.descriptor.value("kind").toString() == "BxPointCloud" || d.tracking)
        fail("BX 点云或跟踪数据不能再次导出为 BX");
    BxWriteStats stats;
    const auto unit = unitScale(d.descriptor["units"].toString());
    stats.scaleToMm = unit.toMm;
    stats.assumedUnits = unit.assumed;
    stats.sourceUnits = unit.text;
    std::vector<PointReader> readers;
    qint64 total = 0;
    for (int piece = 0; piece < d.pieces(); ++piece) {
        auto reader = pointReader(d, piece);
        total += reader.count;
        readers.push_back(reader);
    }
    if (!total)
        fail("源模型没有可用点，未生成 BX 文件");
    checked(total, qint64(rt::aurora::SingleToolBxSize)); // file size budget
    QByteArray chunk;
    chunk.reserve(256 * 1024);
    qint64 index = 0;
    for (const auto &reader : readers) {
        for (qint64 i = 0; i < reader.count; ++i) {
            if (!(index & 65535))
                t.check();
            double xyz[3];
            reader.read(i, xyz);
            double mm[3];
            float value[3];
            for (int k = 0; k < 3; ++k) {
                const QChar axis(ushort('X' + k));
                if (!std::isfinite(xyz[k]))
                    fail(QString("第 %1 个点的 %2 分量不是有限数值（NaN/Inf）")
                             .arg(index)
                             .arg(axis));
                mm[k] = xyz[k] * unit.toMm;
                if (!std::isfinite(mm[k]))
                    fail(
                        QString("第 %1 个点的 %2 分量单位换算后不是有限数值").arg(index).arg(axis));
                if (std::abs(mm[k]) > double(std::numeric_limits<float>::max()))
                    fail(QString("第 %1 个点的 %2 分量 %3 超出 Float32 范围")
                             .arg(index)
                             .arg(axis)
                             .arg(mm[k], 0, 'g', 12));
                value[k] = static_cast<float>(mm[k]);
                if (!std::isfinite(value[k]))
                    fail(QString("第 %1 个点的 %2 分量转换为 Float32 后不是有限数值")
                             .arg(index)
                             .arg(axis));
                stats.maxRoundingMm =
                    std::max(stats.maxRoundingMm, std::abs(double(value[k]) - mm[k]));
            }
            const auto frame =
                static_cast<std::uint32_t>(8ull + 8ull * static_cast<std::uint64_t>(index));
            const auto bytes =
                rt::aurora::encodeSingleValidTool(0x0A, {value[0], value[1], value[2]}, frame);
            chunk.append(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size()));
            if (chunk.size() >= 256 * 1024) {
                writeAll(out, chunk);
                chunk.clear();
            }
            ++index;
        }
    }
    if (!chunk.isEmpty())
        writeAll(out, chunk);
    stats.points = index;
    stats.frames = index;
    if (statsOut)
        *statsOut = stats;
}

Dataset readBx(const QByteArray &bytes, const Task &t, const QString &source) {
    if (bytes.isEmpty())
        fail("BX 文件为空");
    Dataset d;
    d.descriptor = {{"kind", "BxPointCloud"},
                    {"origin", "Aurora BX"},
                    {"units", "mm"},
                    {"frame", "Aurora 工具跟踪坐标系"},
                    {"pieces", 1},
                    {"version", "standard BX frame sequence"},
                    {"source", source}};
    Field points;
    points.section = "Points";
    points.type = Type::F32;
    points.components = 3;
    Field handle;
    handle.section = "PointData";
    handle.name = "handle";
    handle.type = Type::U8;
    Field frameNumber;
    frameNumber.section = "PointData";
    frameNumber.name = "frame_number";
    frameNumber.type = Type::U32;
    Field portStatus;
    portStatus.section = "PointData";
    portStatus.name = "port_status";
    portStatus.type = Type::U32;
    rt::aurora::BxScanStats stats;
    double low[3] = {std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::infinity()};
    double high[3] = {-std::numeric_limits<double>::infinity(),
                      -std::numeric_limits<double>::infinity(),
                      -std::numeric_limits<double>::infinity()};
    qint64 count = 0;
    rt::aurora::scanBxSequence(
        reinterpret_cast<const std::uint8_t *>(bytes.constData()), std::size_t(bytes.size()),
        [&](const rt::aurora::BxPosition &position) {
            if (!(count & 65535))
                t.check();
            points.data.append(reinterpret_cast<const char *>(position.positionMm.data()), 12);
            handle.data.append(char(position.handle));
            const std::uint32_t frame = position.frameNumber;
            const std::uint32_t port = position.portStatus;
            frameNumber.data.append(reinterpret_cast<const char *>(&frame), 4);
            portStatus.data.append(reinterpret_cast<const char *>(&port), 4);
            for (int k = 0; k < 3; ++k) {
                low[k] = std::min(low[k], double(position.positionMm[k]));
                high[k] = std::max(high[k], double(position.positionMm[k]));
            }
            ++count;
            if (!(count & 262143))
                t.report(24,
                         QString("解码 BX：%1 帧 / %2 个有效位置").arg(stats.frames).arg(count));
        },
        stats, nullptr,
        [&] {
            t.check();
        });
    if (!stats.frames)
        fail("BX 文件中没有完整帧");
    d.fields = {points, handle, frameNumber, portStatus};
    d.descriptor["frames"] = qint64(stats.frames);
    d.descriptor["validPositions"] = qint64(stats.valid);
    d.descriptor["missing"] = qint64(stats.missing);
    d.descriptor["disabled"] = qint64(stats.disabled);
    d.descriptor["pointCount"] = count;
    if (count)
        d.descriptor["bounds"] = QJsonArray{low[0], high[0], low[1], high[1], low[2], high[2]};
    return d;
}

Snapshot loadBxObject(const QByteArray &bytes, const Task &t, const QString &label) {
    try {
        t.report(1, "解码 BX：" + label);
        auto d = readBx(bytes, t, "network");
        d.path = label;
        d.loadedFormat = "Aurora BX";
        d.sourceHash = digest(bytes);
        t.report(80, "校验 BX 点云");
        validate(d, t);
        t.check();
        return std::make_shared<const Dataset>(std::move(d));
    } catch (const Cancelled &) {
        throw;
    } catch (const std::bad_alloc &) {
        fail(label + " 可用内存不足");
    } catch (const std::exception &e) {
        fail(label + " — " + QString::fromUtf8(e.what()));
    }
}

void verifyBxFile(const QString &path, const Dataset &d, const BxWriteStats &stats, const Task &t) {
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly))
        fail(path + " " + in.errorString());
    std::vector<PointReader> readers;
    qint64 total = 0;
    for (int piece = 0; piece < d.pieces(); ++piece) {
        auto reader = pointReader(d, piece);
        total += reader.count;
        readers.push_back(reader);
    }
    if (total != stats.points)
        fail(QString("BX 回读帧数 %1 与源点数 %2 不符").arg(stats.points).arg(total));
    qint64 index = 0;
    for (const auto &reader : readers) {
        for (qint64 i = 0; i < reader.count; ++i) {
            if (!(index & 65535))
                t.check();
            const auto bytes = in.read(qsizetype(rt::aurora::SingleToolBxSize));
            if (bytes.size() != qsizetype(rt::aurora::SingleToolBxSize))
                fail(QString("第 %1 帧读取截断").arg(index));
            rt::aurora::Reply reply;
            try {
                reply =
                    rt::aurora::decodeBx(reinterpret_cast<const std::uint8_t *>(bytes.constData()),
                                         std::size_t(bytes.size()));
            } catch (const std::exception &e) {
                fail(QString("第 %1 帧回读失败：%2").arg(index).arg(QString::fromUtf8(e.what())));
            }
            if (reply.tools.size() != 1)
                fail(QString("第 %1 帧工具数不是 1").arg(index));
            const auto &tool = reply.tools[0];
            const auto expectedFrame =
                static_cast<std::uint32_t>(8ull + 8ull * static_cast<std::uint64_t>(index));
            if (tool.handle != 0x0A || tool.status != rt::aurora::HandleStatus::Valid ||
                tool.portStatus != 0x31 || tool.frameNumber != expectedFrame || tool.quality != 0 ||
                tool.quaternion != std::array<float, 4>{1, 0, 0, 0} || reply.systemStatus != 0)
                fail(QString("第 %1 帧的固定字段回读不符").arg(index));
            double xyz[3];
            reader.read(i, xyz);
            for (int k = 0; k < 3; ++k) {
                const auto expected = static_cast<float>(xyz[k] * stats.scaleToMm);
                if (std::memcmp(&expected, &tool.positionMm[k], sizeof expected) != 0)
                    fail(QString("第 %1 帧的 %2 分量回读不符")
                             .arg(index)
                             .arg(QChar(ushort('X' + k))));
            }
            ++index;
        }
    }
    if (!in.atEnd())
        fail("BX 文件在预期帧数之后仍有数据");
}

JobResult convert(Snapshot data, const QString &output, const QString &archiveDir, const Task &t) {
    JobResult result;
    result.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString target = QFileInfo(output).absoluteFilePath();
    QString fmt = formatForPath(target);
    QString archive = QDir(archiveDir).absoluteFilePath(result.id + ".h5");
    const auto plan = assess(*data, fmt);
    QJsonObject rec{{"jobId", result.id},
                    {"startedUtc", timestamp()},
                    {"source", data->path},
                    {"inputFormat", data->loadedFormat},
                    {"sourceFileSHA256", data->sourceHash},
                    {"requestedOutput", target},
                    {"targetFormat", fmt},
                    {"assessment", plan.summary()},
                    {"sourceDatasetComplete", true}};
    if (!data->previousJob.isEmpty())
        rec["parentJobId"] = data->previousJob["jobId"];
    auto save = [&](const QString &path, const Task &work) {
        if (!QDir().mkpath(QFileInfo(path).absolutePath()))
            fail("无法创建档案目录 " + path);
        Stage stage(path);
        stage.close();
        archiveWrite(stage.temp.fileName(), *data, rec, work);
        {
            QJsonObject roundtrip;
            auto copy = archiveRead(stage.temp.fileName(), work, &roundtrip);
            compare(*data, copy, work);
            if (roundtrip != rec)
                fail("档案任务记录回读不一致");
        }
        work.check();
        stage.publish();
        result.archive = path;
    };
    QString phase = "能力检查";
    try {
        t.report(4, phase);
        if (!plan.allowed()) {
            result.status = "rejected";
            fail(plan.summary());
        }
        if (QFileInfo(target).exists() || QFileInfo(target).isSymLink())
            fail("输出路径已存在，请选择一个新文件名：" + target);
        if (fmt != "HDF5" && !QDir().mkpath(archiveDir))
            fail("无法创建过程档案目录 " + archiveDir);
        std::unique_ptr<Stage> stage;
        BxWriteStats bxStats;
        if (fmt != "HDF5") {
            phase = "写入目标临时文件";
            t.report(16, phase);
            stage = std::make_unique<Stage>(target);
            if (fmt == "Aurora BX")
                writeBx(stage->temp, *data, t, &bxStats);
            else
                modelWrite(stage->temp, *data, fmt, t);
            stage->close();
            phase = "目标回读校验";
            t.report(48, phase);
            if (fmt == "Aurora BX") {
                verifyBxFile(stage->temp.fileName(), *data, bxStats, t);
                rec["outputFileSHA256"] = fileHash(stage->temp.fileName(), t);
                rec["bx"] =
                    QJsonObject{{"points", bxStats.points},
                                {"frames", bxStats.frames},
                                {"bytes", bxStats.frames * qint64(rt::aurora::SingleToolBxSize)},
                                {"max_rounding_mm", bxStats.maxRoundingMm},
                                {"assumed_units", bxStats.assumedUnits},
                                {"source_units", bxStats.sourceUnits}};
            } else {
                Task verify{t.stopped, {}};
                auto back = load(stage->temp.fileName(), verify, fmt);
                compare(*data, *back, t);
                rec["outputFileSHA256"] = back->sourceHash;
            }
        } else
            archive = target;
        phase = "写入并回读 HDF5 档案";
        t.report(70, phase);
        Stage archiveStage(archive);
        archiveStage.close();
        rec["status"] = "success";
        rec["complete"] = true;
        rec["verified"] =
            fmt == "Aurora BX"
                ? "53-byte frames, header/body CRC16, fixed tool fields and Float32 positions"
                : "typed values, ragged offsets and ordered manifest";
        rec["finishedUtc"] = timestamp();
        archiveWrite(archiveStage.temp.fileName(), *data, rec, t);
        {
            QJsonObject check;
            auto back = archiveRead(archiveStage.temp.fileName(), t, &check);
            compare(*data, back, t);
            if (check != rec)
                fail("档案任务记录回读不一致");
        }
        phase = "提交已校验文件";
        t.report(96, phase);
        t.check();
        if (t.enterCommit)
            t.enterCommit();
        if (stage) {
            stage->publish();
            result.output = target;
        }
        archiveStage.publish();
        result.archive = archive;
        result.output = target;
        result.status = "success";
        result.message =
            fmt == "Aurora BX"
                ? QString("已生成 %1 个标准 BX 响应帧（源模型 %1 个顶点，%2 字节），"
                          "逐帧回读校验通过；最大 Float32 舍入误差 %3 mm%4。"
                          "颜色、面连接、纹理和法向未写入 BX。")
                      .arg(bxStats.frames)
                      .arg(bxStats.frames * qint64(rt::aurora::SingleToolBxSize))
                      .arg(bxStats.maxRoundingMm, 0, 'g', 6)
                      .arg(bxStats.assumedUnits ? "；源单位未知，按 1 模型单位 = 1 mm 仿真默认比例"
                                                : QString())
                : QString("目标及过程档案均已提交，字段、拓扑和数值位回读一致。");
        return result;
    } catch (const Cancelled &) {
        result.status = "cancelled";
        result.message = "任务已取消，阶段：" + phase;
    } catch (const std::exception &e) {
        if (result.status.isEmpty())
            result.status = "failed";
        result.message = phase + "：" + QString::fromUtf8(e.what());
    }
    rec["status"] = result.status;
    rec["complete"] = false;
    rec["detail"] = result.message;
    rec["publishedOutput"] = result.output;
    rec["finishedUtc"] = timestamp();
    try {
        save(QDir(archiveDir).absoluteFilePath(result.id + ".h5"), {});
    } catch (const std::exception &e) {
        result.message += "\n过程档案未保存：" + QString::fromUtf8(e.what());
    }
    return result;
}
} // namespace rt::model
