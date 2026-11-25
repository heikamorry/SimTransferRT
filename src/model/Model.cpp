#include "Model.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>

namespace rt::model {
static_assert(Q_BYTE_ORDER == Q_LITTLE_ENDIAN,
              "The canonical-buffer adapter requires a little-endian host");
static const QStringList names{"Int8",   "UInt8", "Int16",  "UInt16",  "Int32",
                               "UInt32", "Int64", "UInt64", "Float32", "Float64"};

QString typeName(Type t) {
    return names.at(int(t));
}

Type typeOf(QString s) {
    int index = names.indexOf(s);
    if (index < 0)
        fail("不支持的数据类型 " + s);
    return Type(index);
}

int width(Type t) {
    static constexpr int bytes[]{1, 1, 2, 2, 4, 4, 8, 8, 4, 8};
    return bytes[int(t)];
}

bool integer(Type t) {
    return t != Type::F32 && t != Type::F64;
}

[[noreturn]] void fail(const QString &s) {
    throw std::runtime_error(s.toUtf8().constData());
}

qint64 checked(qint64 a, qint64 b, qint64 limit) {
    if (a < 0 || b < 0 || (b && a > limit / b))
        fail("声明的长度、数量或内存超过本模块预算");
    return a * b;
}

void Task::check() const {
    if (stopped && stopped())
        throw Cancelled();
}

void Task::report(int n, const QString &s) const {
    check();
    if (progress)
        progress(n, s);
}

QStringList cellKinds() {
    return {"Verts", "Lines", "Polys", "Strips"};
}

bool topology(const QString &s) {
    return s == "Verts" || s == "Lines" || s == "Polys" || s == "Strips";
}

Type topologyValueType(const Field &f) {
    auto declaration = f.declaration.value("valueType")
                           .toString(f.declaration.value("listValue").toString("Int64"));
    auto type = typeOf(declaration);
    if (!integer(type))
        fail("拓扑连接类型必须是整数 " + declaration);
    return type;
}

Type topologyOffsetType(const Field &f) {
    auto valueType = topologyValueType(f);
    auto defaultType = width(valueType) >= 4 ? valueType : Type::I64;
    auto declaration = f.declaration.value("offsetType").toString(typeName(defaultType));
    auto type = typeOf(declaration);
    if (!integer(type))
        fail("拓扑偏移类型必须是整数 " + declaration);
    return type;
}

QByteArray encodeIndices(const QByteArray &canonical, Type target, const Task &t) {
    if (target == Type::I64)
        return canonical;
    auto n = canonical.size() / 8;
    QByteArray result(checked(n, width(target)), Qt::Uninitialized);
    for (qsizetype i = 0; i < n; ++i) {
        if (!(i & 65535))
            t.check();
        storeIndex(target, indexValue(Type::I64, canonical.constData() + i * 8),
                   result.data() + i * width(target));
    }
    return result;
}

const Field *Dataset::find(QString section, int piece) const {
    for (const auto &f : fields)
        if (f.piece == piece && f.section == section)
            return &f;
    return nullptr;
}

qint64 Dataset::points(int p) const {
    if (auto f = find("Points", p))
        return f->tuples();
    auto separate = descriptor["coordinateFields"].toArray();
    if (!p && separate.size() == 3) {
        int i = separate.first().toInt(-1);
        if (i >= 0 && size_t(i) < fields.size())
            return fields[i].tuples();
    }
    return 0;
}

qint64 Dataset::cells(int p) const {
    qint64 n = 0;
    for (const auto &f : fields)
        if (f.piece == p && topology(f.section))
            n += f.tuples();
    return n;
}

QByteArray json(const QJsonObject &j) {
    return QJsonDocument(j).toJson(QJsonDocument::Compact);
}

QJsonObject object(const QByteArray &b) {
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(b, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        fail("描述信息 JSON 无效 " + error.errorString());
    return doc.object();
}

QJsonObject manifest(const Dataset &d) {
    QJsonArray fields;
    for (const auto &f : d.fields)
        fields.append(QJsonObject{{"id", fields.size()},
                                  {"section", f.section},
                                  {"name", f.name},
                                  {"role", f.role},
                                  {"type", typeName(f.type)},
                                  {"components", f.components},
                                  {"piece", f.piece},
                                  {"list", f.list},
                                  {"tuples", f.tuples()},
                                  {"values", f.size()},
                                  {"declaration", f.declaration}});
    return {{"version", 1}, {"dataset", d.descriptor}, {"fields", fields}};
}

Field description(const QJsonObject &j) {
    Field f;
    f.section = j["section"].toString();
    f.name = j["name"].toString();
    f.role = j["role"].toString();
    f.type = typeOf(j["type"].toString());
    f.piece = j["piece"].toInt();
    f.components = j["components"].toInt();
    f.list = j["list"].toBool();
    f.declaration = j["declaration"].toObject();
    if (f.components < 1 || f.components > 4096 || f.piece < 0 || f.piece >= 4096 ||
        f.section.isEmpty())
        fail("数组描述无效 " + f.name);
    return f;
}

void applyManifest(Dataset &d, const QJsonObject &j, const std::vector<int> &ids) {
    if (j["version"].toInt() != 1 || !j["fields"].isArray() || !j["dataset"].isObject())
        fail("不支持的属性映射版本");
    auto specs = j["fields"].toArray();
    if (size_t(specs.size()) != d.fields.size() || ids.size() != d.fields.size())
        fail("格式映射中的数组数与实际文件不一致");
    std::set<int> used;
    std::vector<Field> ordered(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        const int id = ids[i];
        if (id < 0 || id >= specs.size() || !used.insert(id).second)
            fail("无效或重复的数组 ID");
        const auto spec = specs.at(id).toObject();
        auto f = description(spec);
        auto &physical = d.fields[i];
        if (spec["id"].toInt(-1) != id || f.type != physical.type || f.list != physical.list ||
            f.components != physical.components || f.section != physical.section ||
            f.piece != physical.piece || physical.tuples() != spec["tuples"].toInteger(-1) ||
            physical.size() != spec["values"].toInteger(-1))
            fail("实际字段与映射不符 " + f.section + "/" + f.name);
        if (topology(f.section) && (topologyValueType(physical) != topologyValueType(f) ||
                                    topologyOffsetType(physical) != topologyOffsetType(f)))
            fail("实际拓扑索引类型与映射不一致 " + f.section);
        if (!topology(f.section) && f.section != "Points" && f.name != physical.name &&
            !(f.name.isEmpty() && physical.name == "unnamed"))
            fail("物理数组名称与映射不符 " + physical.name);
        f.data = std::move(physical.data);
        f.ends = std::move(physical.ends);
        ordered[id] = std::move(f);
    }
    d.fields = std::move(ordered);
    d.descriptor = j["dataset"].toObject();
}

void validate(const Dataset &d, const Task &t) {
    if (d.pieces() < 1 || d.pieces() > 4096 || d.fields.size() > 8192)
        fail("数据集分块或字段数量超限");
    if (d.descriptor.contains("coordinateFields")) {
        auto ids = d.descriptor["coordinateFields"].toArray();
        if (ids.size() != 3)
            fail("分列坐标必须含三个字段");
        for (const auto &id : ids) {
            auto i = id.toInt(-1);
            if (i < 0 || size_t(i) >= d.fields.size() || d.fields[i].list ||
                d.fields[i].components != 1 || d.fields[i].tuples() != d.points(0))
                fail("分列坐标字段无效");
        }
    }
    qint64 total = 0;
    std::set<std::pair<int, QString>> geometry;
    for (const auto &f : d.fields) {
        t.check();
        const auto label = f.section + "/" + f.name;
        if (f.components < 1 || f.components > 4096 || f.piece < 0 || f.piece >= d.pieces() ||
            f.data.size() % (width(f.type) * f.components))
            fail("字段形状无效 " + label);
        total += f.data.size() + qint64(f.ends.size()) * 8;
        if (total > budget)
            fail("模型内存预算超过 2 GiB");
        if (f.list) {
            qint64 previous = 0;
            size_t i = 0;
            for (qint64 end : f.ends) {
                if ((i++ & 65535) == 0)
                    t.check();
                if (end < previous || end > f.size())
                    fail("列表偏移越界 " + label);
                previous = end;
            }
            if (previous != f.size())
                fail("列表尾偏移与数据长度不符 " + label);
        }
        if (f.section == "Points" || topology(f.section))
            if (!geometry.emplace(f.piece, f.section).second)
                fail("重复几何段 " + label);
        if (f.section == "Points" && (f.list || f.components != 3))
            fail("点坐标必须为三分量");
        if (topology(f.section)) {
            if (!f.list || f.components != 1 || f.type != Type::I64)
                fail("单元索引格式无效 " + label);
            topologyValueType(f);
            topologyOffsetType(f);
            auto n = d.points(f.piece);
            qint64 previous = 0;
            int minimum = f.section == "Verts" ? 1 : f.section == "Lines" ? 2 : 3;
            for (auto end : f.ends) {
                if (end - previous < minimum)
                    fail("单元顶点数不足 " + label);
                previous = end;
            }
            for (qint64 i = 0; i < f.size(); ++i) {
                if (!(i & 65535))
                    t.check();
                auto v = indexValue(f.type, f.data.constData() + i * 8);
                if (v < 0 || v >= n)
                    fail(QString("拓扑 %1 第 %2 个索引 %3 超出点数 %4")
                             .arg(label)
                             .arg(i)
                             .arg(v)
                             .arg(n));
            }
        }
        if (f.section == "PointData" && f.tuples() != d.points(f.piece))
            fail("点属性数量不匹配 " + label);
        if (f.section == "CellData" && f.tuples() != d.cells(f.piece))
            fail("单元属性数量不匹配 " + label);
    }
}

QString digest(const QByteArray &b) {
    return QString::fromLatin1(QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex());
}

QString fileHash(const QString &path, const Task &t) {
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly))
        fail(path + " " + in.errorString());
    QCryptographicHash h(QCryptographicHash::Sha256);
    while (!in.atEnd()) {
        t.check();
        auto b = in.read(1 << 20);
        if (b.isEmpty() && in.error() != QFileDevice::NoError)
            fail(in.errorString());
        h.addData(b);
    }
    return QString::fromLatin1(h.result().toHex());
}

QString fieldHash(const Field &f, const Task &t) {
    QCryptographicHash h(QCryptographicHash::Sha256);
    auto feed = [&](const char *b, qint64 n) {
        while (n) {
            t.check();
            auto k = std::min<qint64>(1 << 20, n);
            h.addData(QByteArrayView(b, k));
            b += k;
            n -= k;
        }
    };
    feed(f.data.constData(), f.data.size());
    feed(reinterpret_cast<const char *>(f.ends.data()), qint64(f.ends.size()) * 8);
    return QString::fromLatin1(h.result().toHex());
}

void compare(const Dataset &a, const Dataset &b, const Task &t) {
    if (a.bx != b.bx)
        fail("回读校验失败：BX 原始应答字节不同");
    if (a.descriptor != b.descriptor) {
        QStringList changed;
        for (auto it = a.descriptor.begin(); it != a.descriptor.end(); ++it)
            if (!b.descriptor.contains(it.key()) || it.value() != b.descriptor.value(it.key()))
                changed << it.key();
        for (auto it = b.descriptor.begin(); it != b.descriptor.end(); ++it)
            if (!a.descriptor.contains(it.key()))
                changed << it.key();
        fail("回读校验失败：数据集描述不同 " + changed.join(", "));
    }
    auto left = manifest(a)["fields"].toArray();
    auto right = manifest(b)["fields"].toArray();
    if (left.size() != right.size())
        fail("回读校验失败：字段数不同");
    for (int i = 0; i < left.size(); ++i)
        if (left[i] != right[i])
            fail(QString("回读校验失败：字段 #%1 定义或顺序不符 %2 / %3")
                     .arg(i)
                     .arg(QString::fromUtf8(json(left[i].toObject())),
                          QString::fromUtf8(json(right[i].toObject()))));
    for (size_t i = 0; i < a.fields.size(); ++i) {
        t.check();
        if (a.fields[i].data != b.fields[i].data || a.fields[i].ends != b.fields[i].ends)
            fail("回读校验失败：字段 " + a.fields[i].section + "/" + a.fields[i].name +
                 " 数值位或连接顺序不同");
    }
}

void writeAll(QIODevice &out, const char *p, qint64 n) {
    while (n) {
        auto k = out.write(p, n);
        if (k <= 0)
            fail("写入失败 " + out.errorString());
        p += k;
        n -= k;
    }
}

template <class T> static T value(const char *p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

template <class F> static auto dispatch(Type t, F fn) {
    switch (t) {
    case Type::I8:
        return fn(qint8{});
    case Type::U8:
        return fn(quint8{});
    case Type::I16:
        return fn(qint16{});
    case Type::U16:
        return fn(quint16{});
    case Type::I32:
        return fn(qint32{});
    case Type::U32:
        return fn(quint32{});
    case Type::I64:
        return fn(qint64{});
    case Type::U64:
        return fn(quint64{});
    case Type::F32:
        return fn(float{});
    case Type::F64:
        return fn(double{});
    }
    fail("未知数值类型");
}

void scalarText(Type t, QByteArrayView s, char *dst) {
    if (s.startsWith('+'))
        s = s.sliced(1);
    dispatch(t, [&](auto v) {
        auto parsed = std::from_chars(s.begin(), s.end(), v);
        if (parsed.ec != std::errc{} || parsed.ptr != s.end() || s.empty())
            fail("无效数值或越界 " +
                 QString::fromLatin1(s.first(std::min<qsizetype>(80, s.size()))));
        std::memcpy(dst, &v, sizeof v);
        return 0;
    });
}

QByteArray scalarString(Type t, const char *p) {
    return dispatch(t, [&](auto v) {
        char buf[96];
        auto r = std::to_chars(buf, buf + sizeof buf, value<decltype(v)>(p));
        if (r.ec != std::errc{})
            fail("数值文本编码失败");
        return QByteArray(buf, r.ptr - buf);
    });
}

qint64 indexValue(Type t, const char *p) {
    if (!integer(t))
        fail("索引或长度必须为整数");
    return dispatch(t, [&](auto v) -> qint64 {
        using T = decltype(v);
        auto n = value<T>(p);
        if constexpr (std::is_same_v<T, quint64>) {
            if (n > quint64(INT64_MAX))
                fail("索引超出 Int64");
        }
        return static_cast<qint64>(n);
    });
}

void storeIndex(Type t, qint64 n, char *p) {
    dispatch(t, [&](auto v) {
        using T = decltype(v);
        if (!integer(t) || n < 0 || quint64(n) > quint64((std::numeric_limits<T>::max)()))
            fail("索引或列表长度超出目标类型");
        v = static_cast<T>(n);
        std::memcpy(p, &v, sizeof v);
        return 0;
    });
}

double displayNumber(Type t, const char *p) {
    return dispatch(t, [&](auto v) {
        return double(value<decltype(v)>(p));
    });
}

void endianCopy(char *dst, const char *src, int n, bool big) {
    if (big)
        std::reverse_copy(src, src + n, dst);
    else
        std::memcpy(dst, src, n);
}

void Cursor::spaces() {
    while (p < end && uchar(*p) <= 32)
        ++p;
}

QByteArrayView Cursor::token() {
    spaces();
    auto first = p;
    while (p < end && uchar(*p) > 32)
        ++p;
    if (p == first)
        fail(QString("字节 %1 处缺少数值").arg(position()));
    return {first, p - first};
}

QByteArray Cursor::line() {
    if (p == end)
        fail("文件提前结束");
    const char *next = static_cast<const char *>(std::memchr(p, '\n', size_t(end - p)));
    if (!next)
        next = end;
    if (next - p > 8 * 1024 * 1024)
        fail("文本行超过 8 MiB");
    QByteArray line(p, next - p);
    p = next + (next < end);
    if (line.endsWith('\r'))
        line.chop(1);
    return line;
}

const char *Cursor::take(qint64 n) {
    if (n < 0 || n > remaining())
        fail(QString("字节 %1 处数据截断，预期 %2 字节").arg(position()).arg(n));
    auto first = p;
    p += n;
    return first;
}
} // namespace rt::model
