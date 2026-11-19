#include "Model.h"
#include <QDomDocument>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <algorithm>
#include <cstring>
#include <set>
#include <vtkDataCompressor.h>
#include <vtkLZ4DataCompressor.h>
#include <vtkLZMADataCompressor.h>
#include <vtkSmartPointer.h>
#include <vtkZLibDataCompressor.h>

namespace rt::model {
namespace {
QJsonObject attributes(QDomElement e) {
    QJsonObject j;
    auto attrs = e.attributes();
    for (int i = 0; i < attrs.size(); ++i) {
        auto n = attrs.item(i);
        j[n.nodeName()] = n.nodeValue();
    }
    return j;
}
qint64 count(QString text, QString label, qint64 limit = budget) {
    bool ok;
    auto v = text.toLongLong(&ok);
    if (!ok || v < 0 || v > limit)
        fail("无效的 VTP " + label + " " + text);
    return v;
}
QByteArray b64(QByteArrayView input) {
    QByteArray clean;
    clean.reserve(input.size());
    for (auto c : input)
        if (uchar(c) > 32)
            clean += c;
    // VTK encodes the compressed header and payload as separate, padded Base64 streams.
    QByteArray out;
    qsizetype first = 0;
    while (first < clean.size()) {
        auto eq = clean.indexOf('=', first);
        auto end = eq < 0 ? clean.size() : eq + 1;
        while (end < clean.size() && clean[end] == '=')
            ++end;
        auto decoded = QByteArray::fromBase64Encoding(clean.sliced(first, end - first),
                                                      QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded)
            fail("VTP Base64 数据损坏");
        out += decoded.decoded;
        first = end;
    }
    return out;
}
struct Encoding {
    QByteArrayView appended;
    bool base64 = false, big = false;
    int headerWidth = 4;
    QString compressor;
    std::set<qint64> offsets;
};
QByteArray block(QByteArrayView bytes, const Encoding &enc, const Task &t) {
    qint64 pos = 0;
    auto number = [&]() {
        if (enc.headerWidth > bytes.size() - pos)
            fail("VTP 二进制块头截断");
        char tmp[8];
        endianCopy(tmp, bytes.data() + pos, enc.headerWidth, enc.big);
        pos += enc.headerWidth;
        return indexValue(enc.headerWidth == 4 ? Type::U32 : Type::U64, tmp);
    };
    auto padding = [&](qint64 consumed) {
        for (auto c : bytes.sliced(consumed))
            if (c != ' ' && c != '\n' && c != '\t' && c != '\r')
                fail("VTP 数据块后存在多余字节");
    };
    if (enc.compressor.isEmpty()) {
        auto len = number();
        if (len > bytes.size() - pos || len > budget)
            fail("VTP 数据块长度不符");
        padding(pos + len);
        return bytes.sliced(pos, len).toByteArray();
    }
    vtkSmartPointer<vtkDataCompressor> codec;
    if (enc.compressor == "vtkZLibDataCompressor")
        codec = vtkSmartPointer<vtkZLibDataCompressor>::New();
    else if (enc.compressor == "vtkLZ4DataCompressor")
        codec = vtkSmartPointer<vtkLZ4DataCompressor>::New();
    else if (enc.compressor == "vtkLZMADataCompressor")
        codec = vtkSmartPointer<vtkLZMADataCompressor>::New();
    else
        fail("不支持的 VTP 压缩器 " + enc.compressor);
    auto n = number(), chunk = number(), last = number();
    if (n > 1048576 || last > chunk || chunk > budget || (n && !chunk))
        fail("无效的 VTP 压缩块头");
    // VTK writes last=0 when every block has full size.
    auto final = n ? (last ? last : chunk) : 0;
    auto size = n ? checked(n - 1, chunk) + final : 0;
    if (size > budget)
        fail("解压超过预算");
    std::vector<qint64> lengths;
    for (qint64 i = 0; i < n; ++i)
        lengths.push_back(number());
    qint64 compressed = 0;
    for (auto len : lengths) {
        if (len > bytes.size() - pos - compressed)
            fail("压缩块长度越界");
        compressed += len;
    }
    padding(pos + compressed);
    QByteArray result(size, Qt::Uninitialized);
    for (qint64 i = 0; i < n; ++i) {
        t.check();
        auto expected = i + 1 == n ? final : chunk;
        auto used = codec->Uncompress(
            reinterpret_cast<const unsigned char *>(bytes.data() + pos), lengths[i],
            reinterpret_cast<unsigned char *>(result.data() + i * chunk), expected);
        if (used != size_t(expected))
            fail("VTP 解压长度不符");
        pos += lengths[i];
    }
    return result;
}
Field values(QDomElement a, const Encoding &enc, const Task &t) {
    if (a.tagName() != "DataArray")
        fail("VTP 尚未实现数组 " + a.tagName() + " / " + a.attribute("type"));
    Field f;
    f.type = typeOf(a.attribute("type"));
    f.name = a.attribute("Name");
    f.components = int(count(a.attribute("NumberOfComponents", "1"), "分量", 4096));
    if (!f.components)
        fail("VTP 分量为零");
    if (a.hasAttribute("TimeStep"))
        fail("当前适配器尚未实现 VTP 多时间步数组 " + f.name);
    f.declaration["xml"] = attributes(a);
    QString childXml;
    QByteArray text;
    for (auto child = a.firstChild(); !child.isNull(); child = child.nextSibling()) {
        if (child.isElement()) {
            if (child.nodeName() != "InformationKey")
                fail("不支持的 VTP 数组子节点 " + child.nodeName());
            QTextStream s(&childXml, QIODevice::Append);
            child.save(s, 0);
        } else if (child.isText() || child.isCDATASection())
            text += child.nodeValue().toLatin1();
    }
    if (!childXml.isEmpty())
        f.declaration["information"] = childXml;
    auto fmt = a.attribute("format", "ascii");
    int w = width(f.type);
    if (fmt == "ascii") {
        Cursor c(text);
        c.spaces();
        while (c.remaining()) {
            if (!(f.data.size() & 65535))
                t.check();
            char tmp[8];
            scalarText(f.type, c.token(), tmp);
            f.data.append(tmp, w);
            c.spaces();
            if (f.data.size() > budget)
                fail("VTP ASCII 数组超过预算");
        }
    } else if (fmt == "binary" || fmt == "appended") {
        QByteArray decoded;
        QByteArrayView input;
        if (fmt == "binary") {
            decoded = b64(text);
            input = decoded;
        } else {
            auto offset = count(a.attribute("offset"), "offset");
            auto next = enc.offsets.upper_bound(offset);
            auto end = next == enc.offsets.end() ? enc.appended.size() : *next;
            if (end > enc.appended.size() || offset > end)
                fail("VTP appended offset 越界");
            input = enc.appended.sliced(offset, end - offset);
            if (enc.base64) {
                decoded = b64(input);
                input = decoded;
            }
        }
        f.data = block(input, enc, t);
        if (f.data.size() % w)
            fail("VTP 元素截断");
        if (enc.big && w > 1) {
            auto p = f.data.data();
            for (qsizetype i = 0; i < f.data.size(); i += w) {
                if (!(i & 1048575))
                    t.check();
                std::reverse(p + i, p + i + w);
            }
        }
    } else
        fail("不支持的 VTP 数组编码 " + fmt);
    if (f.data.size() % (f.components * w) ||
        (a.hasAttribute("NumberOfTuples") &&
         count(a.attribute("NumberOfTuples"), "tuples") != f.size()))
        fail("VTP 数组形状不符 " + f.name);
    return f;
}
QString roleFor(const QJsonObject &attrs, const Field &f) {
    for (const auto &name :
         QStringList{"Normals", "TCoords", "Vectors", "Tensors", "GlobalIds", "PedigreeIds"})
        if (!f.name.isEmpty() && attrs[name] == f.name)
            return name;
    if ((f.name.compare("RGB", Qt::CaseInsensitive) == 0 ||
         f.name.compare("RGBA", Qt::CaseInsensitive) == 0 ||
         f.name.compare("Colors", Qt::CaseInsensitive) == 0) &&
        (f.components == 3 || f.components == 4))
        return "Colors";
    if (!f.name.isEmpty() && attrs["Scalars"] == f.name)
        return "Scalars";
    return {};
}
QByteArray encoded64(const std::vector<qint64> &ends) {
    return QByteArray::fromRawData(reinterpret_cast<const char *>(ends.data()),
                                   qint64(ends.size()) * 8);
}
struct Part {
    QByteArray bytes;
};
void extraAttributes(QXmlStreamWriter &writer, const QJsonObject &attrs,
                     const QStringList &handled = {}) {
    for (auto it = attrs.begin(); it != attrs.end(); ++it)
        if (!handled.contains(it.key()))
            writer.writeAttribute(it.key(), it.value().toString());
}
QJsonObject sectionAttributes(const Dataset &d, int piece, const QString &section) {
    const auto schema = d.descriptor.value("pieceSchema").toArray();
    if (piece < schema.size())
        return schema.at(piece).toObject().value("sections").toObject().value(section).toObject();
    QJsonObject active;
    if (section == "PointData" || section == "CellData") {
        for (const auto &f : d.fields) {
            if (f.section != section || f.piece != piece || f.role.isEmpty() || f.name.isEmpty())
                continue;
            const QString role = f.role == "Colors" ? "Scalars" : f.role;
            if (!active.contains(role))
                active.insert(role, f.name);
        }
    }
    return active;
}
void informationKeys(QXmlStreamWriter &writer, const QString &xml) {
    if (xml.isEmpty())
        return;
    QXmlStreamReader reader("<container>" + xml + "</container>");
    if (!reader.readNextStartElement())
        fail("VTP InformationKey 描述无效");
    int depth = 0;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isEndElement() && !depth)
            break;
        if (reader.isStartElement())
            ++depth;
        if (reader.isEndElement())
            --depth;
        if (!reader.hasError())
            writer.writeCurrentToken(reader);
    }
    if (reader.hasError())
        fail("VTP InformationKey 描述无效 " + reader.errorString());
}
} // namespace
Dataset readVtp(const QByteArray &bytes, const Task &t) {
    Encoding enc;
    QByteArray xmlBytes;
    auto start = bytes.indexOf("<AppendedData");
    auto finish = bytes.lastIndexOf("</AppendedData>");
    if (start >= 0) {
        auto open = bytes.indexOf('>', start);
        if (open < 0 || finish < open)
            fail("VTP AppendedData 边界错误");
        auto marker = open + 1;
        while (marker < finish && uchar(bytes[marker]) <= 32)
            ++marker;
        if (marker >= finish || bytes[marker] != '_')
            fail("VTP appended 缺少下划线");
        enc.appended = QByteArrayView(bytes).sliced(marker + 1, finish - marker - 1);
        xmlBytes = bytes.first(open + 1) + bytes.sliced(finish);
    } else
        xmlBytes = bytes;
    if (xmlBytes.size() > 256 * 1024 * 1024 || xmlBytes.contains("<!DOCTYPE"))
        fail("VTP XML 超出预算或含不支持的 DTD");
    QDomDocument doc;
    auto parsed = doc.setContent(xmlBytes);
    if (!parsed)
        fail(QString("VTP XML 行 %1 列 %2 %3")
                 .arg(parsed.errorLine)
                 .arg(parsed.errorColumn)
                 .arg(parsed.errorMessage));
    auto root = doc.documentElement();
    if (root.tagName() != "VTKFile" || root.attribute("type") != "PolyData")
        fail("VTP 数据集类型不支持 " + root.attribute("type"));
    auto ver = root.attribute("version");
    if (!QStringList{"0.1", "1.0", "2.0", "2.1", "2.2", "2.3"}.contains(ver))
        fail("不支持的 VTP 版本 " + ver);
    auto order = root.attribute("byte_order");
    if (order != "LittleEndian" && order != "BigEndian")
        fail("未知 VTP 字节序 " + order);
    enc.big = order == "BigEndian";
    auto ht = root.attribute("header_type", "UInt32");
    if (ht != "UInt32" && ht != "UInt64")
        fail("不支持的 VTP header_type " + ht);
    enc.headerWidth = ht == "UInt64" ? 8 : 4;
    enc.compressor = root.attribute("compressor");
    auto app = root.firstChildElement("AppendedData");
    if (!app.isNull()) {
        auto mode = app.attribute("encoding", "base64");
        if (mode != "raw" && mode != "base64")
            fail("未知 appended 编码");
        enc.base64 = mode == "base64";
    }
    auto nodes = root.elementsByTagName("DataArray");
    for (int i = 0; i < nodes.count(); ++i) {
        auto a = nodes.at(i).toElement();
        if (a.attribute("format") == "appended")
            enc.offsets.insert(count(a.attribute("offset"), "offset"));
    }
    auto poly = root.firstChildElement("PolyData");
    if (poly.isNull() || !poly.nextSiblingElement("PolyData").isNull())
        fail("VTP 需要一个 PolyData");
    for (auto n = root.firstChildElement(); !n.isNull(); n = n.nextSiblingElement())
        if (n.tagName() != "PolyData" && n.tagName() != "AppendedData")
            fail("尚未实现 VTP 节点 " + n.tagName());
    QJsonObject ext;
    for (auto n = root.firstChild(); !n.isNull(); n = n.nextSibling())
        if (n.isComment() && n.nodeValue().startsWith("SimTransferRT1 "))
            ext = object(QByteArray::fromBase64(n.nodeValue().sliced(15).toLatin1()));
    Dataset d;
    d.descriptor = {{"origin", "VTP"},
                    {"kind", "PolyData"},
                    {"version", ver},
                    {"units", ""},
                    {"frame", ""},
                    {"xmlRoot", attributes(root)},
                    {"xmlPolyData", attributes(poly)}};
    std::vector<int> ids;
    QJsonArray pieces;
    int piece = 0;
    qint64 used = 0;
    auto readSection = [&](QDomElement s, int p) {
        const auto sec = s.tagName();
        auto active = attributes(s);
        Field links, ends;
        int linkId = -1, endId = -1;
        bool haveLinks = false, haveEnds = false;
        for (auto a = s.firstChildElement(); !a.isNull(); a = a.nextSiblingElement()) {
            t.check();
            auto f = values(a, enc, t);
            used += f.data.size();
            if (used > budget)
                fail("VTP 数组总量超过预算");
            f.piece = p;
            f.section = sec;
            f.role = roleFor(active, f);
            bool validId;
            auto mapped = a.attribute("sim_id", "-1").toInt(&validId);
            if (!validId)
                fail("VTP 数组 ID 无效");
            if (sec == "Points")
                f.role = "coordinates";
            if (topology(sec)) {
                if (!integer(f.type) || f.components != 1)
                    fail("VTP 拓扑不是整数");
                if (f.name == "connectivity" && !haveLinks) {
                    links = std::move(f);
                    linkId = mapped;
                    haveLinks = true;
                } else if (f.name == "offsets" && !haveEnds) {
                    ends = std::move(f);
                    endId = mapped;
                    haveEnds = true;
                } else
                    fail("未知或重复拓扑数组 " + f.name);
            } else {
                d.fields.push_back(std::move(f));
                ids.push_back(mapped);
            }
        }
        if (topology(sec) && (haveLinks || haveEnds)) {
            if (!haveLinks || !haveEnds)
                fail("拓扑缺少 offsets 或 connectivity " + sec);
            if (!ext.isEmpty() && linkId != endId)
                fail("VTP 拓扑数组映射 ID 不一致 " + sec);
            Field f;
            f.section = sec;
            f.name = "connectivity";
            f.piece = p;
            f.type = Type::I64;
            f.role = "connectivity";
            f.list = true;
            f.declaration = {{"valueType", typeName(links.type)},
                             {"offsetType", typeName(ends.type)},
                             {"connectivity", links.declaration},
                             {"offsets", ends.declaration}};
            f.data.resize(checked(links.size(), 8));
            for (qint64 i = 0; i < links.size(); ++i) {
                if (!(i & 65535))
                    t.check();
                auto v = indexValue(links.type, links.data.constData() + i * width(links.type));
                std::memcpy(f.data.data() + 8 * i, &v, 8);
            }
            f.ends.reserve(ends.size());
            for (qint64 i = 0; i < ends.size(); ++i)
                f.ends.push_back(
                    indexValue(ends.type, ends.data.constData() + i * width(ends.type)));
            if (!f.ends.empty() || !f.data.isEmpty() || linkId >= 0) {
                d.fields.push_back(std::move(f));
                ids.push_back(linkId);
            } else {
                auto empty = d.descriptor.value("emptyCellArrays").toArray();
                empty.append(
                    QJsonObject{{"piece", p}, {"section", sec}, {"declaration", f.declaration}});
                d.descriptor["emptyCellArrays"] = empty;
            }
        }
    };
    for (auto el = poly.firstChildElement(); !el.isNull(); el = el.nextSiblingElement()) {
        if (el.tagName() == "FieldData") {
            readSection(el, 0);
            continue;
        }
        if (el.tagName() != "Piece")
            fail("不支持的 PolyData 节点 " + el.tagName());
        QJsonObject sections;
        std::set<QString> seen;
        for (auto s = el.firstChildElement(); !s.isNull(); s = s.nextSiblingElement()) {
            auto sec = s.tagName();
            if (!QStringList{"Points", "PointData", "CellData"}.contains(sec) && !topology(sec))
                fail("不支持的 Piece 子节点 " + sec);
            if (!seen.insert(sec).second)
                fail("重复 Piece 节点 " + sec);
            sections[sec] = attributes(s);
            readSection(s, piece);
        }
        if (d.points(piece) != count(el.attribute("NumberOfPoints", "0"), "点数", maxTuples))
            fail("VTP 点数与 Piece 声明不符");
        for (auto kind : cellKinds()) {
            auto f = d.find(kind, piece);
            if ((f ? f->tuples() : 0) !=
                count(el.attribute("NumberOf" + kind, "0"), kind, maxTuples))
                fail("VTP 单元数不符 " + kind);
        }
        pieces.append(QJsonObject{{"attributes", attributes(el)}, {"sections", sections}});
        if (++piece > 4096)
            fail("VTP 分块过多");
    }
    if (!piece)
        fail("VTP 缺少 Piece");
    d.descriptor["pieces"] = piece;
    d.descriptor["pieceSchema"] = pieces;
    if (!ext.isEmpty()) {
        applyManifest(d, ext, ids);
        if (d.pieces() != pieces.size())
            fail("VTP 原生 Piece 数量与字段映射不一致");
        for (int p = 0; p < pieces.size(); ++p) {
            const auto actual = pieces.at(p).toObject().value("sections").toObject();
            for (const QString &sec : QStringList{"PointData", "CellData"}) {
                const auto physical = actual.value(sec).toObject();
                const auto expected = sectionAttributes(d, p, sec);
                if (physical != expected) {
                    QStringList keys = expected.keys();
                    keys.append(physical.keys());
                    keys.removeDuplicates();
                    for (const auto &key : keys)
                        if (expected.value(key) != physical.value(key))
                            fail(QString("VTP Piece %1 %2 的原生活动属性或声明 %3 与字段映射不一致")
                                     .arg(p)
                                     .arg(sec, key));
                }
            }
        }
    }
    return d;
}
void writeVtp(QIODevice &out, const Dataset &d, const Task &t) {
    QByteArray header;
    QXmlStreamWriter x(&header);
    x.writeStartDocument();
    x.writeStartElement("VTKFile");
    x.writeAttribute("type", "PolyData");
    x.writeAttribute("version", "1.0");
    x.writeAttribute("byte_order", "LittleEndian");
    x.writeAttribute("header_type", "UInt64");
    extraAttributes(x, d.descriptor.value("xmlRoot").toObject(),
                    {"type", "version", "byte_order", "header_type", "compressor"});
    x.writeComment("SimTransferRT1 " + QString::fromLatin1(json(manifest(d)).toBase64()));
    x.writeStartElement("PolyData");
    extraAttributes(x, d.descriptor.value("xmlPolyData").toObject());
    std::vector<Part> chunks;
    qint64 offset = 0;
    auto array = [&](const QJsonObject &declaration, int id, QString name, Type type, int comps,
                     const QByteArray &b) {
        t.check();
        x.writeStartElement("DataArray");
        x.writeAttribute("type", typeName(type));
        if (!name.isEmpty())
            x.writeAttribute("Name", name);
        x.writeAttribute("NumberOfComponents", QString::number(comps));
        x.writeAttribute("NumberOfTuples", QString::number(b.size() / (comps * width(type))));
        x.writeAttribute("format", "appended");
        x.writeAttribute("offset", QString::number(offset));
        x.writeAttribute("sim_id", QString::number(id));
        extraAttributes(
            x, declaration.value("xml").toObject(),
            {"Name", "type", "format", "offset", "NumberOfComponents", "NumberOfTuples", "sim_id"});
        informationKeys(x, declaration.value("information").toString());
        x.writeEndElement();
        chunks.push_back({b});
        offset += 8 + b.size();
    };
    auto section = [&](const QString &sec, int p) {
        x.writeStartElement(sec);
        extraAttributes(x, sectionAttributes(d, p, sec));
        for (size_t i = 0; i < d.fields.size(); ++i) {
            auto &f = d.fields[i];
            if (f.section != sec || f.piece != p)
                continue;
            if (topology(sec)) {
                auto valueType = topologyValueType(f), offsetType = topologyOffsetType(f);
                array(f.declaration.value("connectivity").toObject(), int(i), "connectivity",
                      valueType, 1, encodeIndices(f.data, valueType, t));
                array(f.declaration.value("offsets").toObject(), int(i), "offsets", offsetType, 1,
                      encodeIndices(encoded64(f.ends), offsetType, t));
            } else
                array(f.declaration, int(i), f.name, f.type, f.components, f.data);
        }
        for (const auto &entry : d.descriptor.value("emptyCellArrays").toArray()) {
            auto empty = entry.toObject();
            if (empty.value("piece").toInt() != p || empty.value("section") != sec)
                continue;
            Field f;
            f.declaration = empty.value("declaration").toObject();
            array(f.declaration.value("connectivity").toObject(), -1, "connectivity",
                  topologyValueType(f), 1, {});
            array(f.declaration.value("offsets").toObject(), -1, "offsets", topologyOffsetType(f),
                  1, {});
        }
        x.writeEndElement();
    };
    section("FieldData", 0);
    for (int p = 0; p < d.pieces(); ++p) {
        x.writeStartElement("Piece");
        x.writeAttribute("NumberOfPoints", QString::number(d.points(p)));
        for (auto kind : cellKinds()) {
            auto f = d.find(kind, p);
            x.writeAttribute("NumberOf" + kind, QString::number(f ? f->tuples() : 0));
        }
        auto specs = d.descriptor.value("pieceSchema").toArray();
        if (p < specs.size())
            extraAttributes(x, specs.at(p).toObject().value("attributes").toObject(),
                            {"NumberOfPoints", "NumberOfVerts", "NumberOfLines", "NumberOfPolys",
                             "NumberOfStrips"});
        for (auto sec :
             QStringList{"PointData", "CellData", "Points", "Verts", "Lines", "Strips", "Polys"})
            section(sec, p);
        x.writeEndElement();
    }
    x.writeEndElement();
    writeAll(out, header);
    writeAll(out, "<AppendedData encoding=\"raw\">_");
    for (const auto &part : chunks) {
        qint64 len = part.bytes.size();
        writeAll(out, reinterpret_cast<const char *>(&len), 8);
        for (qint64 k = 0; k < len; k += 1 << 20) {
            t.check();
            writeAll(out, part.bytes.constData() + k, std::min<qint64>(len - k, 1 << 20));
        }
    }
    writeAll(out, "</AppendedData></VTKFile>\n");
}
} // namespace rt::model
