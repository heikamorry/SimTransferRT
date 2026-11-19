#include "Model.h"
#include <QMap>
#include <QUrl>
#include <algorithm>
#include <cstring>

namespace rt::model {
namespace {
const QStringList names{"char",  "unsigned_char", "short",        "unsigned_short",
                        "int",   "unsigned_int",  "vtktypeint64", "vtktypeuint64",
                        "float", "double"};
Type legacyType(QByteArray s) {
    auto n = names.indexOf(QString::fromLatin1(s.toLower()));
    if (n >= 0)
        return Type(n);
    static const QMap<QByteArray, Type> alias{
        {"signed_char", Type::I8},         {"long_long", Type::I64},
        {"unsigned_long_long", Type::U64}, {"vtktypeint32", Type::I32},
        {"vtktypeuint32", Type::U32},      {"vtkidtype", Type::I32}};
    if (alias.contains(s.toLower()))
        return alias.value(s.toLower());
    fail("legacy VTK 尚未实现或宽度不明确的类型 " + QString::fromLatin1(s));
}
qint64 number(QByteArray s, qint64 cap = maxTuples) {
    bool ok;
    auto n = s.toLongLong(&ok);
    if (!ok || n < 0 || n > cap)
        fail("legacy VTK 数量无效 " + QString::fromLatin1(s));
    return n;
}
const QMap<QByteArray, QString> kinds{{"VERTICES", "Verts"},
                                      {"LINES", "Lines"},
                                      {"POLYGONS", "Polys"},
                                      {"TRIANGLE_STRIPS", "Strips"}};
constexpr auto marker = "SimTransferRT_schema_v1";
QString nameOf(const QByteArray &b) {
    return QUrl::fromPercentEncoding(b);
}
QByteArray safeName(const QString &n) {
    return n.isEmpty() ? QByteArray("unnamed") : QUrl::toPercentEncoding(n);
}
QByteArray readData(Cursor &c, Type type, qint64 values, bool ascii, const Task &t) {
    int w = width(type);
    auto size = checked(values, w);
    if ((ascii ? values : size) > c.remaining())
        fail(QString("legacy VTK 在字节 %1 声明的数据超过文件长度").arg(c.position()));
    QByteArray data(size, Qt::Uninitialized);
    auto p = data.data();
    if (!ascii && w == 1) {
        std::memcpy(p, c.take(size), size_t(size));
        return data;
    }
    for (qint64 i = 0; i < values; ++i) {
        if (!(i & 65535))
            t.check();
        if (ascii)
            scalarText(type, c.token(), p + i * w);
        else
            endianCopy(p + i * w, c.take(w), w, true);
    }
    return data;
}
void writeData(QIODevice &out, Type type, const QByteArray &data, bool ascii, const Task &t) {
    int w = width(type);
    QByteArray buffer;
    buffer.reserve(1 << 20);
    for (qint64 i = 0; i < data.size(); i += w) {
        if (!(i & 65535))
            t.check();
        if (ascii) {
            buffer += scalarString(type, data.constData() + i);
            buffer += ((i / w) % 9 == 8 ? '\n' : ' ');
        } else {
            char tmp[8];
            endianCopy(tmp, data.constData() + i, w, true);
            buffer.append(tmp, w);
        }
        if (buffer.size() >= (1 << 20)) {
            writeAll(out, buffer);
            buffer.resize(0);
        }
    }
    buffer += '\n';
    writeAll(out, buffer);
}
} // namespace
Dataset readLegacy(const QByteArray &bytes, const Task &t) {
    Cursor c(bytes);
    auto magic = c.line().trimmed();
    if (!magic.startsWith("# vtk DataFile Version "))
        fail("无效的 legacy VTK 文件头");
    QString ver = QString::fromLatin1(magic.sliced(23));
    bool versionOK;
    double version = ver.toDouble(&versionOK);
    if (!versionOK || version < 1 || version > 5.1)
        fail("不支持的 legacy VTK 版本 " + ver);
    auto title = c.line();
    auto format = c.line().trimmed().toUpper();
    bool ascii = format == "ASCII";
    if (!ascii && format != "BINARY")
        fail("legacy VTK 编码无效");
    auto words = [&]() {
        QByteArray line;
        while (c.remaining() && line.isEmpty())
            line = c.line().simplified();
        return line.split(' ');
    };
    auto head = words();
    if (head.size() != 2 || head[0].toUpper() != "DATASET")
        fail("缺少 legacy DATASET 声明");
    QString kind = head[1].toUpper();
    if (kind != "POLYDATA")
        fail("识别到 legacy VTK DATASET " + kind + "，此适配器仅支持 POLYDATA");
    Dataset d;
    d.descriptor = {{"origin", "legacy VTK"},
                    {"kind", "PolyData"},
                    {"version", ver},
                    {"encoding", QString::fromLatin1(format)},
                    {"title", QString::fromLatin1(title.toBase64())},
                    {"pieces", 1},
                    {"units", ""},
                    {"frame", ""}};
    QString association = "FieldData";
    qint64 tuples = 0, used = 0;
    QJsonObject extension;
    auto add = [&](Field f) {
        used += f.data.size() + qint64(f.ends.size()) * 8;
        if (used > budget || d.fields.size() >= 8192)
            fail("legacy VTK 数据超过预算");
        d.fields.push_back(std::move(f));
    };
    while (c.remaining()) {
        t.report(int(65 * c.position() / bytes.size()), "读取 legacy POLYDATA");
        auto w = words();
        auto k = w[0].toUpper();
        if (k.isEmpty())
            break;
        if (k == "POINT_DATA" || k == "CELL_DATA") {
            if (w.size() != 2)
                fail("无效的关联声明");
            association = k == "POINT_DATA" ? "PointData" : "CellData";
            tuples = number(w[1]);
            if (tuples != (association == "PointData" ? d.points(0) : d.cells(0)))
                fail("legacy 属性数量与几何不一致");
            continue;
        }
        if (k == "METADATA") {
            if (d.fields.empty())
                fail("METADATA 没有关联数组");
            QByteArray meta;
            while (c.remaining()) {
                auto line = c.line();
                if (line.trimmed().isEmpty())
                    break;
                meta += line + '\n';
            }
            d.fields.back().declaration["metadata"] = QString::fromLatin1(meta.toBase64());
            continue;
        }
        Field f;
        f.section = association;
        f.declaration["keyword"] = QString::fromLatin1(k);
        if (k == "POINTS" && w.size() == 3) {
            f.section = "Points";
            f.name = "Points";
            f.role = "coordinates";
            f.type = legacyType(w[2]);
            f.components = 3;
            f.data = readData(c, f.type, checked(number(w[1]), 3), ascii, t);
            add(std::move(f));
            continue;
        }
        if (kinds.contains(k) && w.size() == 3) {
            f.section = kinds[k];
            f.name = "connectivity";
            f.role = "connectivity";
            f.list = true;
            f.type = Type::I64;
            auto n = number(w[1]), size = number(w[2], budget / 4);
            if (version >= 5.1) {
                auto h = words();
                if (h.size() != 2 || h[0] != "OFFSETS" || !n)
                    fail("legacy 5.1 缺少 OFFSETS");
                auto ot = legacyType(h[1]);
                auto ends = readData(c, ot, n, ascii, t);
                if (indexValue(ot, ends.constData()) != 0)
                    fail("legacy OFFSETS 必须从零开始");
                for (qint64 i = 1; i < n; ++i)
                    f.ends.push_back(indexValue(ot, ends.constData() + i * width(ot)));
                h = words();
                if (h.size() != 2 || h[0] != "CONNECTIVITY")
                    fail("legacy 5.1 缺少 CONNECTIVITY");
                auto it = legacyType(h[1]);
                auto links = readData(c, it, size, ascii, t);
                f.declaration["offsetType"] = typeName(ot);
                f.declaration["valueType"] = typeName(it);
                f.data.resize(checked(size, 8));
                for (qint64 i = 0; i < size; ++i) {
                    if (!(i & 65535))
                        t.check();
                    auto v = indexValue(it, links.constData() + i * width(it));
                    std::memcpy(f.data.data() + i * 8, &v, 8);
                }
            } else {
                auto packed = readData(c, Type::I32, size, ascii, t);
                qint64 pos = 0;
                f.ends.reserve(n);
                if (size < n)
                    fail("legacy 单元长度表截断");
                f.data.reserve(checked(size - n, 8));
                for (qint64 cell = 0; cell < n; ++cell) {
                    if (!(cell & 65535))
                        t.check();
                    if (pos >= size)
                        fail("legacy 单元截断");
                    auto length = indexValue(Type::I32, packed.constData() + 4 * pos++);
                    if (length < 0 || length > size - pos)
                        fail("legacy 单元长度越界");
                    for (qint64 j = 0; j < length; ++j) {
                        auto v = indexValue(Type::I32, packed.constData() + 4 * pos++);
                        f.data.append(reinterpret_cast<const char *>(&v), 8);
                    }
                    f.ends.push_back(f.size());
                }
                if (pos != size)
                    fail("legacy 单元数据存在尾部");
                f.declaration["valueType"] = "Int32";
            }
            add(std::move(f));
            continue;
        }
        if (k == "FIELD" && w.size() == 3) {
            const auto n = number(w[2], 8192);
            for (qint64 i = 0; i < n; ++i) {
                auto h = words();
                if (h.size() != 4)
                    fail("legacy FIELD 数组声明无效");
                Field a;
                a.section = association;
                a.name = nameOf(h[0]);
                a.type = legacyType(h[3]);
                a.components = int(number(h[1], 4096));
                if (!a.components)
                    fail("FIELD 分量为零");
                a.data = readData(c, a.type, checked(number(h[2]), a.components), ascii, t);
                a.declaration = {{"group", QString::fromLatin1(w[1])}, {"keyword", "FIELD"}};
                if (a.section == "FieldData" && a.name == marker &&
                    a.data.startsWith("SimTransferRT1\n")) {
                    if (!extension.isEmpty() || a.type != Type::U8 || a.components != 1)
                        fail("无效或重复的 legacy 映射");
                    extension = object(a.data.sliced(15));
                } else
                    add(std::move(a));
            }
            continue;
        }
        if (k == "LOOKUP_TABLE" && w.size() == 3) {
            f.section = "LookupTable";
            f.name = nameOf(w[1]);
            f.type = ascii ? Type::F32 : Type::U8;
            f.components = 4;
            f.data = readData(c, f.type, checked(number(w[2]), 4), ascii, t);
            add(std::move(f));
            continue;
        }
        if (w.size() < 3 || association == "FieldData")
            fail("未实现的 legacy 声明 " + QString::fromLatin1(w.join(' ')));
        f.name = nameOf(w[1]);
        if (k == "SCALARS" && (w.size() == 3 || w.size() == 4)) {
            f.role = "Scalars";
            f.type = legacyType(w[2]);
            f.components = w.size() == 4 ? int(number(w[3], 4096)) : 1;
            auto h = words();
            if (h.size() != 2 || h[0].toUpper() != "LOOKUP_TABLE")
                fail("SCALARS 缺少 LOOKUP_TABLE");
            f.declaration["lookup"] = QString::fromLatin1(h[1]);
        } else if (k == "COLOR_SCALARS" && w.size() == 3) {
            f.role = "Colors";
            f.type = ascii ? Type::F32 : Type::U8;
            f.components = int(number(w[2], 4096));
        } else if ((k == "VECTORS" || k == "NORMALS" || k == "TENSORS") && w.size() == 3) {
            f.role = k == "VECTORS" ? "Vectors" : k == "NORMALS" ? "Normals" : "Tensors";
            f.components = k == "TENSORS" ? 9 : 3;
            f.type = legacyType(w[2]);
        } else if (k == "TEXTURE_COORDINATES" && w.size() == 4) {
            f.role = "TCoords";
            f.components = int(number(w[2], 4096));
            f.type = legacyType(w[3]);
        } else
            fail("尚未实现 legacy 属性 " + QString::fromLatin1(k));
        if (!f.components)
            fail("legacy 分量为零");
        f.data = readData(c, f.type, checked(tuples, f.components), ascii, t);
        add(std::move(f));
    }
    if (!extension.isEmpty()) {
        std::vector<int> ids;
        for (auto id : extension["wire"].toArray())
            ids.push_back(id.toInt(-1));
        applyManifest(d, extension, ids);
    }
    return d;
}
void writeLegacy(QIODevice &out, const Dataset &d, const Task &t) {
    bool ascii = false;
    for (const auto &f : d.fields)
        if ((f.role == "Colors" || f.section == "LookupTable") && f.type == Type::F32)
            ascii = true;
    auto title = QByteArray::fromBase64(d.descriptor["title"].toString().toLatin1());
    if (title.isEmpty())
        title = "SimTransferRT PolyData";
    writeAll(out, "# vtk DataFile Version 5.1\n" + title + (ascii ? "\nASCII\n" : "\nBINARY\n") +
                      "DATASET POLYDATA\n");
    QJsonArray order;
    QStringList sections{"FieldData", "Points",    "Verts",    "Lines",      "Polys",
                         "Strips",    "PointData", "CellData", "LookupTable"};
    for (auto sec : sections)
        for (size_t i = 0; i < d.fields.size(); ++i)
            if (d.fields[i].section == sec)
                order.append(int(i));
    auto ext = manifest(d);
    ext["wire"] = order;
    auto meta = "SimTransferRT1\n" + json(ext);
    writeAll(out, "FIELD FieldData 1\n" + QByteArray(marker) + " 1 " +
                      QByteArray::number(meta.size()) + " unsigned_char\n");
    writeData(out, Type::U8, meta, ascii, t);
    for (const auto &section : sections) {
        bool announced = false;
        for (const auto &f : d.fields)
            if (f.section == section) {
                auto name = safeName(f.name), type = names[int(f.type)].toLatin1(),
                     components = QByteArray::number(f.components);
                if (section == "Points")
                    writeAll(out, "POINTS " + QByteArray::number(f.tuples()) + " " + type + "\n");
                else if (topology(section)) {
                    auto valueType = topologyValueType(f), offsetType = topologyOffsetType(f);
                    if (valueType != offsetType ||
                        (valueType != Type::I32 && valueType != Type::I64))
                        fail("legacy VTK 5.1 要求相同的 Int32 或 Int64 偏移与连接 " + section);
                    auto wireType = names[int(valueType)].toLatin1();
                    writeAll(out, kinds.key(section) + " " + QByteArray::number(f.tuples() + 1) +
                                      " " + QByteArray::number(f.size()) + "\nOFFSETS " + wireType +
                                      "\n");
                    QByteArray offsets(8, 0);
                    offsets.append(reinterpret_cast<const char *>(f.ends.data()),
                                   qint64(f.ends.size()) * 8);
                    writeData(out, offsetType, encodeIndices(offsets, offsetType, t), ascii, t);
                    writeAll(out, "CONNECTIVITY " + wireType + "\n");
                    writeData(out, valueType, encodeIndices(f.data, valueType, t), ascii, t);
                    continue;
                } else if (section == "LookupTable")
                    writeAll(out,
                             "LOOKUP_TABLE " + name + " " + QByteArray::number(f.tuples()) + "\n");
                else {
                    if (!announced && (section == "PointData" || section == "CellData")) {
                        writeAll(out, (section == "PointData" ? QByteArray("POINT_DATA ")
                                                              : QByteArray("CELL_DATA ")) +
                                          QByteArray::number(f.tuples()) + "\n");
                        announced = true;
                    }
                    if (section != "FieldData" && f.role == "Colors")
                        writeAll(out, "COLOR_SCALARS " + name + " " + components + "\n");
                    else if (section != "FieldData" && f.role == "Scalars" && f.components <= 4)
                        writeAll(out, "SCALARS " + name + " " + type + " " + components +
                                          "\nLOOKUP_TABLE " +
                                          f.declaration["lookup"].toString("default").toLatin1() +
                                          "\n");
                    else if ((f.role == "Normals" || f.role == "Vectors") && f.components == 3)
                        writeAll(out, f.role.toUpper().toLatin1() + " " + name + " " + type + "\n");
                    else if (f.role == "Tensors" && f.components == 9)
                        writeAll(out, "TENSORS " + name + " " + type + "\n");
                    else if (f.role == "TCoords")
                        writeAll(out, "TEXTURE_COORDINATES " + name + " " + components + " " +
                                          type + "\n");
                    else
                        writeAll(out, "FIELD " +
                                          f.declaration["group"].toString("FieldData").toLatin1() +
                                          " 1\n" + name + " " + components + " " +
                                          QByteArray::number(f.tuples()) + " " + type + "\n");
                }
                writeData(out, f.type, f.data, ascii, t);
            }
    }
}
} // namespace rt::model
