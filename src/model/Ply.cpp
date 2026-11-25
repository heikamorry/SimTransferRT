#include "Model.h"
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cstring>
#include <set>

namespace rt::model {
namespace {
struct Property {
    QString name;
    Type type = Type::U8;
    Type count = Type::U8;
    bool list = false;
    int id = -1;
    int component = 0;
};

struct Element {
    QString name;
    qint64 rows = 0;
    std::vector<Property> properties;
};

struct Layout {
    QByteArray raw;
    QString encoding;
    std::vector<Element> elements;
    QJsonObject extension;
    QStringList textures;
    QString units;
    QString frame;
};

const QStringList plyNames{"char", "uchar", "short",  "ushort", "int",
                           "uint", "int64", "uint64", "float",  "double"};

Type plyType(QString name) {
    const auto index = plyNames.indexOf(name);
    if (index >= 0)
        return Type(index);
    return typeOf(name.startsWith("uint") ? "U" + name.sliced(1, 1).toUpper() + name.sliced(2)
                                          : name.left(1).toUpper() + name.sliced(1));
}

Layout header(Cursor &c) {
    Layout l;
    auto start = c.p;
    if (c.line() != "ply")
        fail("PLY 标识错误");
    QSet<QString> elements;
    QSet<QString> properties;
    while (c.p < c.end) {
        auto row = c.line();
        auto words = row.simplified().split(' ');
        const auto key = words[0];
        if (key == "end_header") {
            if (l.encoding.isEmpty())
                fail("缺少 PLY format");
            l.raw = QByteArray(start, c.p - start);
            return l;
        }
        if (c.p - start > 8 * 1024 * 1024)
            fail("PLY 文件头超过 8 MiB");
        if (key == "format" && words.size() == 3) {
            l.encoding = words[1];
            if (words[2] != "1.0" ||
                !QStringList{"ascii", "binary_big_endian", "binary_little_endian"}.contains(
                    l.encoding))
                fail("不支持的 PLY 编码或版本 " + QString::fromLatin1(row));
        } else if (key == "comment" || key == "obj_info") {
            const auto note = QString::fromUtf8(row.sliced(key.size())).trimmed();
            if (note.startsWith("SimTransferRT1 ")) {
                if (!l.extension.isEmpty())
                    fail("重复 PLY 映射");
                l.extension = object(QByteArray::fromBase64(note.sliced(15).toLatin1()));
            }
            if (note.startsWith("TextureFile ", Qt::CaseInsensitive))
                l.textures.append(note.sliced(12).trimmed());
            static const QRegularExpression units(R"(\bunits?\s*[:=]\s*([^.;]+))",
                                                  QRegularExpression::CaseInsensitiveOption);
            static const QRegularExpression frame(R"(\bcoordinates?\s*[:=]\s*([^.;]+))",
                                                  QRegularExpression::CaseInsensitiveOption);
            auto m = units.match(note);
            if (m.hasMatch())
                l.units = m.captured(1).trimmed();
            m = frame.match(note);
            if (m.hasMatch())
                l.frame = m.captured(1).trimmed();
        } else if (key == "element" && words.size() == 3) {
            bool ok = false;
            auto count = words[2].toLongLong(&ok);
            QString name = QString::fromUtf8(words[1]);
            if (!ok || count < 0 || count > maxTuples || elements.contains(name) ||
                elements.size() >= 1024)
                fail("无效的 PLY 元素 " + name);
            elements.insert(name);
            properties.clear();
            l.elements.push_back({name, count, {}});
        } else if (key == "property" && !l.elements.empty() &&
                   (words.size() == 3 || (words.size() == 5 && words[1] == "list"))) {
            Property p;
            p.name = QString::fromUtf8(words.back());
            p.type = plyType(words[words.size() - 2]);
            p.list = words.size() == 5;
            if (p.list) {
                p.count = plyType(words[2]);
                if (!integer(p.count))
                    fail("PLY list 计数不是整数 " + p.name);
            }
            if (properties.contains(p.name) || properties.size() >= 4096)
                fail("重复或过多的 PLY 属性 " + p.name);
            properties.insert(p.name);
            l.elements.back().properties.push_back(p);
        } else
            fail("未知或位置错误的 PLY 声明 " + QString::fromUtf8(row.left(180)));
    }
    fail("PLY 缺少 end_header");
}

int newField(Dataset &d, const QString &section, const QString &name, const QString &role,
             Type type, int components, bool list) {
    Field f;
    f.section = section;
    f.name = name;
    f.role = role;
    f.type = type;
    f.components = components;
    f.list = list;
    d.fields.push_back(std::move(f));
    return int(d.fields.size()) - 1;
}

void group(Element &e, Dataset &d, const QStringList &names, const QString &section,
           const QString &field, const QString &role) {
    std::vector<Property *> props;
    for (const auto &n : names) {
        auto it = std::find_if(e.properties.begin(), e.properties.end(), [&](auto &p) {
            return p.name == n;
        });
        if (it == e.properties.end() || it->list || it->id >= 0 ||
            (!props.empty() && it->type != props.front()->type))
            return;
        props.push_back(&*it);
    }
    int id = newField(d, section, field, role, props.front()->type, int(props.size()), false);
    int component = 0;
    for (auto p : props) {
        p->id = id;
        p->component = component++;
    }
}

QJsonArray bindings(const Layout &l) {
    QJsonArray result;
    for (const auto &e : l.elements) {
        QJsonArray props;
        for (const auto &p : e.properties)
            props.append(QJsonArray{p.id, p.component});
        result.append(props);
    }
    return result;
}

void bind(Layout &l, Dataset &d) {
    const bool mapped = !l.extension.isEmpty();
    if (mapped) {
        if (l.extension.value("version").toInt() != 1 || !l.extension.value("fields").isArray() ||
            !l.extension.value("dataset").isObject())
            fail("不支持的 PLY 映射版本");
        auto fields = l.extension["fields"].toArray();
        if (fields.size() > 8192)
            fail("PLY 映射数组过多");
        for (const auto &entry : fields) {
            auto f = entry.toObject();
            if (f.value("id").toInt(-1) != int(d.fields.size()))
                fail("PLY 映射字段 ID 不连续");
            d.fields.push_back(description(f));
        }
        d.descriptor = l.extension["dataset"].toObject();
    } else {
        QJsonArray refs;
        for (const auto &r : l.textures)
            refs.append(r);
        d.descriptor = {{"kind", "PolyData"}, {"origin", "PLY"},
                        {"version", "1.0"},   {"encoding", l.encoding},
                        {"pieces", 1},        {"units", l.units},
                        {"frame", l.frame},   {"plyHeader", QString::fromLatin1(l.raw.toBase64())},
                        {"resources", refs}};
    }
    const auto ids = l.extension.value("bindings").toArray();
    if (mapped && ids.size() != qsizetype(l.elements.size()))
        fail("PLY 元素映射缺失");
    for (size_t i = 0; i < l.elements.size(); ++i) {
        auto &e = l.elements[i];
        QString section = e.name == "vertex" ? "PointData"
                          : e.name == "face" ? "CellData"
                                             : "PLY:" + e.name;
        if (mapped) {
            auto props = ids.at(i).toArray();
            if (props.size() != qsizetype(e.properties.size()))
                fail("PLY 属性映射缺失");
            for (size_t k = 0; k < e.properties.size(); ++k) {
                auto pair = props.at(k).toArray();
                auto &p = e.properties[k];
                if (pair.size() != 2)
                    fail("无效 PLY 属性映射");
                p.id = pair.at(0).toInt(-1);
                p.component = pair.at(1).toInt(-1);
                if (p.id < 0 || size_t(p.id) >= d.fields.size())
                    fail("无效 PLY 字段 ID");
            }
            continue;
        }
        if (e.name == "vertex")
            group(e, d, {"x", "y", "z"}, "Points", "Points", "coordinates");
        if (e.name == "vertex" && !d.find("Points")) {
            // Keep mixed-width coordinates in their original scalar columns; only the view
            // interleaves them.
            QJsonArray coordinateIds;
            for (const QString &name : QStringList{"x", "y", "z"}) {
                auto it =
                    std::find_if(e.properties.begin(), e.properties.end(), [&](const auto &p) {
                        return !p.list && p.name == name;
                    });
                if (it == e.properties.end())
                    continue;
                it->id =
                    newField(d, "PLY:vertex", name, "coordinate component", it->type, 1, false);
                coordinateIds.append(it->id);
            }
            if (coordinateIds.size() == 3)
                d.descriptor["coordinateFields"] = coordinateIds;
            else
                section = "PLY:vertex";
        }
        if (e.name == "face" &&
            std::none_of(e.properties.begin(), e.properties.end(), [](const Property &p) {
                return p.list && (p.name == "vertex_indices" || p.name == "vertex_index");
            }))
            section = "PLY:face";
        if (section == "PointData" || section == "CellData") {
            group(e, d, {"red", "green", "blue", "alpha"}, section, "RGBA", "Colors");
            group(e, d, {"red", "green", "blue"}, section, "RGB", "Colors");
            group(e, d, {"nx", "ny", "nz"}, section, "Normals", "Normals");
            group(e, d, {"u", "v"}, section, "UV", "TCoords");
            group(e, d, {"s", "t"}, section, "TextureCoordinates", "TCoords");
            group(e, d, {"texture_u", "texture_v"}, section, "TextureUV", "TCoords");
        }
        for (auto &p : e.properties)
            if (p.id < 0) {
                if (e.name == "face" && p.list &&
                    (p.name == "vertex_indices" || p.name == "vertex_index")) {
                    if (!integer(p.type))
                        fail("PLY 面连接必须为整数");
                    p.id = newField(d, "Polys", p.name, "connectivity", Type::I64, 1, true);
                } else
                    p.id = newField(d, section, p.name, {}, p.type, 1, p.list);
                if (p.list)
                    d.fields[p.id].declaration = {{"listCount", typeName(p.count)},
                                                  {"listValue", typeName(p.type)}};
            }
    }
    std::set<std::pair<int, int>> seen;
    std::vector<const Element *> owners(d.fields.size(), nullptr);
    for (const auto &e : l.elements)
        for (const auto &p : e.properties) {
            auto &f = d.fields.at(p.id);
            if (f.list != p.list || (!topology(f.section) && f.type != p.type) || p.component < 0 ||
                p.component >= f.components || !seen.emplace(p.id, p.component).second)
                fail("PLY 物理属性与映射冲突 " + p.name);
            if (owners[p.id] && owners[p.id] != &e)
                fail("PLY 同一字段的分量分布在不同元素 " + f.name);
            owners[p.id] = &e;
            if (p.list && f.components != 1)
                fail("PLY 列表字段不能跨分量 " + p.name);
            if (topology(f.section) && (f.section != "Polys" || e.name != "face" || !p.list ||
                                        !integer(p.type) || f.type != Type::I64))
                fail("PLY 面连接声明与映射冲突 " + p.name);
            if (mapped) {
                const bool point =
                    e.name == "vertex" && (f.section == "Points" || f.section == "PointData");
                const bool cell =
                    e.name == "face" && (f.section == "Polys" || f.section == "CellData");
                if ((!point && !cell) || f.piece != 0)
                    fail("PLY 元素与字段关联冲突 " + p.name);
                if (f.section == "Points" &&
                    (p.component >= 3 || p.name != QStringList{"x", "y", "z"}.at(p.component)))
                    fail("PLY 坐标映射与原生 XYZ 不一致");
                if (f.section == "Polys" && p.name != "vertex_indices" && p.name != "vertex_index")
                    fail("PLY 面连接属性名称与映射不一致");
            }
        }
    for (size_t i = 0; i < d.fields.size(); ++i)
        for (int k = 0; k < d.fields[i].components; ++k)
            if (!seen.count({int(i), k}))
                fail("PLY 缺少映射的物理属性 " + d.fields[i].name);
}

void readValue(Cursor &c, Type type, char *dest, bool ascii, bool big) {
    if (ascii)
        scalarText(type, c.token(), dest);
    else {
        auto w = width(type);
        endianCopy(dest, c.take(w), w, big);
    }
}

QStringList standardProperties(const Field &f) {
    if (f.section == "Points")
        return {"x", "y", "z"};
    if (f.section == "Polys")
        return {"vertex_indices"};
    if (f.role == "Colors" && f.components == 4)
        return {"red", "green", "blue", "alpha"};
    if (f.role == "Colors" && f.components == 3)
        return {"red", "green", "blue"};
    if (f.role == "Normals" && f.components == 3)
        return {"nx", "ny", "nz"};
    if (f.role == "TCoords" && f.components == 2)
        return {"u", "v"};
    return {};
}

Layout outputLayout(const Dataset &d) {
    if (d.descriptor.contains("plyHeader")) {
        auto bytes = QByteArray::fromBase64(d.descriptor["plyHeader"].toString().toLatin1());
        Cursor c(bytes);
        auto l = header(c);
        Dataset original;
        bind(l, original);
        if (original.fields.size() != d.fields.size())
            fail("源 PLY 布局与字段数不一致");
        for (const auto &e : l.elements)
            for (const auto &p : e.properties) {
                const auto &f = d.fields.at(p.id);
                const auto &expected = original.fields.at(p.id);
                if (f.tuples() != e.rows || f.section != expected.section ||
                    f.type != expected.type || f.name != expected.name ||
                    f.components != expected.components || f.list != expected.list ||
                    f.role != expected.role || f.declaration != expected.declaration)
                    fail("源 PLY 文件头与实际字段不一致 " + e.name + "/" + p.name);
            }
        return l;
    }
    Layout l;
    l.encoding = "binary_little_endian";
    for (const QString &element : QStringList{"vertex", "face"}) {
        Element e;
        e.name = element;
        e.rows = element == "vertex" ? d.points(0) : d.cells(0);
        const QString dataSection = element == "vertex" ? "PointData" : "CellData";
        const QString geometrySection = element == "vertex" ? "Points" : "Polys";
        const auto pieces = d.descriptor.value("pieceSchema").toArray();
        const auto active = pieces.isEmpty() ? QJsonObject{}
                                             : pieces.first()
                                                   .toObject()
                                                   .value("sections")
                                                   .toObject()
                                                   .value(dataSection)
                                                   .toObject();
        QMap<QString, const Field *> primary;
        for (const auto &f : d.fields) {
            if (f.section != dataSection && f.section != geometrySection)
                continue;
            const auto labels = standardProperties(f);
            if (labels.isEmpty())
                continue;
            const QString role = f.role == "Colors" ? "Scalars" : f.role;
            if (!primary.contains(labels.first()) ||
                (!f.name.isEmpty() && active.value(role) == f.name))
                primary[labels.first()] = &f;
        }
        QSet<QString> used;
        for (size_t i = 0; i < d.fields.size(); ++i) {
            const auto &f = d.fields[i];
            if (f.section != dataSection && f.section != geometrySection)
                continue;
            auto labels = standardProperties(f);
            if (!labels.isEmpty() && primary.value(labels.first()) != &f)
                labels.clear();
            for (int k = 0; k < f.components; ++k) {
                QString name = k < labels.size()   ? labels[k]
                               : f.components == 1 ? f.name
                                                   : f.name + "_" + QString::number(k);
                static const QRegularExpression safe("^[A-Za-z_][A-Za-z_0-9]*$");
                static const QSet<QString> channels{"red", "green",     "blue",     "alpha", "nx",
                                                    "ny",  "nz",        "u",        "v",     "s",
                                                    "t",   "texture_u", "texture_v"};
                bool reserved =
                    k >= labels.size() &&
                    (channels.contains(name) ||
                     (element == "vertex" && QStringList{"x", "y", "z"}.contains(name)) ||
                     (element == "face" &&
                      QStringList{"vertex_indices", "vertex_index"}.contains(name)));
                if (!safe.match(name).hasMatch() || used.contains(name) || reserved)
                    name = QString("sim_%1_%2").arg(i).arg(k);
                while (used.contains(name))
                    name += "_";
                used.insert(name);
                Property p;
                p.name = name;
                p.id = int(i);
                p.component = k;
                p.list = f.list;
                p.type = topology(f.section) ? topologyValueType(f) : f.type;
                if (topology(f.section) && width(p.type) > 4)
                    p.type = Type::I32;
                p.count = Type::U32;
                e.properties.push_back(p);
            }
        }
        if (!e.properties.empty())
            l.elements.push_back(std::move(e));
    }
    auto ext = manifest(d);
    ext["bindings"] = bindings(l);
    l.raw = "ply\nformat binary_little_endian 1.0\ncomment SimTransferRT1 " + json(ext).toBase64() +
            "\n";
    for (const auto &e : l.elements) {
        l.raw += "element " + e.name.toUtf8() + " " + QByteArray::number(e.rows) + "\n";
        for (const auto &p : e.properties)
            l.raw += "property " +
                     (p.list ? "list " + plyNames[int(p.count)].toLatin1() + " " : QByteArray{}) +
                     plyNames[int(p.type)].toLatin1() + " " + p.name.toUtf8() + "\n";
    }
    l.raw += "end_header\n";
    return l;
}
} // namespace

Dataset readPly(const QByteArray &bytes, const Task &t) {
    Cursor c(bytes);
    auto l = header(c);
    Dataset d;
    bind(l, d);
    qint64 allocated = 0;
    const bool ascii = l.encoding == "ascii";
    const bool big = l.encoding == "binary_big_endian";
    for (auto &e : l.elements) {
        qint64 rowMinimum = 0;
        for (const auto &p : e.properties)
            rowMinimum += ascii ? 1 : width(p.list ? p.count : p.type);
        if (checked(e.rows, rowMinimum) > c.remaining())
            fail("PLY " + e.name + " 声明数量超过文件剩余长度");
        for (const auto &p : e.properties)
            if (p.component == 0) {
                auto &f = d.fields[p.id];
                auto n = checked(e.rows, p.list ? 8 : width(f.type) * f.components);
                allocated += n;
                if (allocated > budget)
                    fail("PLY 分配超过 2 GiB");
                if (p.list)
                    f.ends.reserve(size_t(e.rows));
                else
                    f.data.resize(n);
            }
        for (qint64 row = 0; row < e.rows; ++row) {
            if (!(row & 16383))
                t.report(int(65 * c.position() / std::max<qint64>(1, bytes.size())),
                         "读取 PLY " + e.name);
            for (const auto &p : e.properties) {
                auto &f = d.fields[p.id];
                try {
                    if (!p.list) {
                        readValue(c, p.type,
                                  f.data.data() +
                                      (row * f.components + p.component) * width(f.type),
                                  ascii, big);
                        continue;
                    }
                    char temp[8];
                    readValue(c, p.count, temp, ascii, big);
                    const qint64 n = indexValue(p.count, temp);
                    const auto add = checked(n, width(f.type));
                    if ((allocated += add) > budget ||
                        checked(n, ascii ? 1 : width(p.type)) > c.remaining())
                        fail(
                            QString("PLY %1[%2].%3 列表长度越界").arg(e.name).arg(row).arg(p.name));
                    auto old = f.data.size();
                    f.data.resize(old + add);
                    auto dst = f.data.data() + old;
                    if (p.type == f.type && !ascii && !big) {
                        std::memcpy(dst, c.take(add), size_t(add));
                    } else
                        for (qint64 k = 0; k < n; ++k) {
                            if (!(k & 65535))
                                t.check();
                            readValue(c, p.type, temp, ascii, big);
                            if (f.type == p.type)
                                std::memcpy(dst + k * width(f.type), temp, width(f.type));
                            else
                                storeIndex(f.type, indexValue(p.type, temp),
                                           dst + k * width(f.type));
                        }
                    f.ends.push_back(f.size());
                } catch (const Cancelled &) {
                    throw;
                } catch (const std::exception &error) {
                    fail(QString("PLY %1[%2].%3，字节 %4：%5")
                             .arg(e.name)
                             .arg(row)
                             .arg(p.name)
                             .arg(c.position())
                             .arg(QString::fromUtf8(error.what())));
                }
            }
        }
    }
    if (ascii)
        c.spaces();
    if (c.remaining())
        fail(QString("PLY 在字节 %1 后有多余数据").arg(c.position()));
    if (!l.extension.isEmpty()) {
        auto specs = l.extension.value("fields").toArray();
        for (size_t i = 0; i < d.fields.size(); ++i) {
            const auto spec = specs.at(i).toObject();
            const auto &f = d.fields[i];
            if (f.tuples() != spec.value("tuples").toInteger(-1) ||
                f.size() != spec.value("values").toInteger(-1))
                fail("PLY 属性数量与字段映射不一致 " + f.name);
        }
    }
    return d;
}

void writePly(QIODevice &out, const Dataset &d, const Task &t) {
    auto l = outputLayout(d);
    writeAll(out, l.raw);
    const bool ascii = l.encoding == "ascii";
    const bool big = l.encoding == "binary_big_endian";
    QByteArray buffer;
    buffer.reserve(1 << 20);
    auto put = [&](Type type, const char *value) {
        if (ascii) {
            buffer += scalarString(type, value);
            buffer += ' ';
        } else {
            char tmp[8];
            endianCopy(tmp, value, width(type), big);
            buffer.append(tmp, width(type));
        }
    };
    for (const auto &e : l.elements)
        for (qint64 row = 0; row < e.rows; ++row) {
            if (!(row & 16383))
                t.check();
            for (const auto &p : e.properties) {
                const auto &f = d.fields.at(p.id);
                if (!p.list) {
                    put(p.type,
                        f.data.constData() + (row * f.components + p.component) * width(f.type));
                    continue;
                }
                auto start = row ? f.ends.at(row - 1) : 0;
                auto end = f.ends.at(row);
                char value[8];
                storeIndex(p.count, end - start, value);
                put(p.count, value);
                for (auto k = start; k < end; ++k) {
                    if (!(k & 65535))
                        t.check();
                    auto src = f.data.constData() + k * width(f.type);
                    if (f.type == p.type)
                        put(p.type, src);
                    else {
                        storeIndex(p.type, indexValue(f.type, src), value);
                        put(p.type, value);
                    }
                    if (buffer.size() >= (1 << 20)) {
                        writeAll(out, buffer);
                        buffer.resize(0);
                    }
                }
            }
            if (ascii)
                buffer += '\n';
            if (buffer.size() >= (1 << 20)) {
                writeAll(out, buffer);
                buffer.resize(0);
            }
        }
    writeAll(out, buffer);
}
} // namespace rt::model
