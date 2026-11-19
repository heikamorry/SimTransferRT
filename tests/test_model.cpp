#include "model/Model.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUuid>
#include <cmath>
#include <cstring>
#include <hdf5.h>
#include <iostream>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkFieldData.h>
#include <vtkIdList.h>
#include <vtkNew.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataReader.h>
#include <vtkXMLPolyDataReader.h>
#include <vtkXMLPolyDataWriter.h>

using namespace rt::model;
namespace {
void require(bool valid, const QString &why) {
    if (!valid)
        fail(why);
}
template <class F> void rejects(F fn, QString reason = {}) {
    try {
        fn();
    } catch (const std::exception &e) {
        require(QString::fromUtf8(e.what()).contains(reason), QString("错误原因不符 ") + e.what());
        return;
    }
    fail("预期拒绝，但已通过 " + reason);
}
const Field &named(const Dataset &d, QString sec, QString name) {
    for (const auto &f : d.fields)
        if (f.section == sec && f.name == name)
            return f;
    fail("missing field " + sec + "/" + name);
}
template <class T> T at(const Field &f, int i) {
    T v;
    require((i + 1) * sizeof(T) <= size_t(f.data.size()), "index");
    std::memcpy(&v, f.data.constData() + i * sizeof(T), sizeof v);
    return v;
}
} // namespace
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QDir fixtures(app.arguments().at(1));
    QTemporaryDir temp;
    int failures = 0, passed = 0;
    auto test = [&](QString title, auto fn) {
        try {
            fn();
            std::cout << "PASS " << title.toStdString() << std::endl;
            ++passed;
        } catch (const std::exception &e) {
            std::cerr << "FAIL " << title.toStdString() << " " << e.what() << std::endl;
            ++failures;
        }
    };
    auto read = [&](QString file) { return load(fixtures.filePath(file), {}); };
    auto round = [&](Snapshot data, QString format) {
        auto file =
            temp.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + "." + format);
        auto r = convert(data, file, temp.path(), {});
        require(r.status == "success", r.message);
        auto copy = load(file, {});
        compare(*data, *copy, {});
        QJsonObject job;
        auto logged = archiveRead(r.archive, {}, &job);
        compare(*data, logged, {});
        require(job["complete"].toBool() && job["sourceFileSHA256"] == data->sourceHash,
                "provenance");
        if (format != "h5")
            require(job["outputFileSHA256"] == fileHash(file, {}), "output hash");
        return file;
    };
    test("PLY precisions, RGBA, signed zero, UV, normals, point/cell homonyms, polygon order", [&] {
        auto d = read("surface.ply");
        auto p = d->find("Points");
        require(p && p->type == Type::F64 && at<double>(*p, 3) == 1.0000000000000002 &&
                    std::signbit(at<double>(*p, 2)),
                "float64 or signed zero");
        require(named(*d, "PointData", "RGBA").components == 4 &&
                    at<quint8>(named(*d, "PointData", "RGBA"), 3) == 239,
                "rgba");
        require(named(*d, "PointData", "Normals").components == 3 &&
                    named(*d, "PointData", "UV").type == Type::F64,
                "normal/UV");
        require(at<qint16>(named(*d, "CellData", "temperature"), 0) == -7 &&
                    at<qint32>(named(*d, "PointData", "temperature"), 3) == 23,
                "associations");
        require(d->find("Polys")->ends == std::vector<qint64>{3, 7} &&
                    at<qint64>(*d->find("Polys"), 3) == 3,
                "polygon winding");
        require(d->descriptor["units"] == "metres", "units");
        auto little = read("little.ply");
        require(little->find("Points")->type == Type::F32 &&
                    named(*little, "PointData", "RGB").components == 3,
                "little endian");
    });
    test("unknown PLY elements and ragged lists; rejected target archived", [&] {
        auto d = read("big-custom.ply");
        auto &f = named(*d, "PLY:calibration", "readings");
        require(f.ends == std::vector<qint64>{2, 2} && std::signbit(at<double>(f, 0)) &&
                    std::isinf(at<double>(f, 1)) && f.declaration["listCount"] == "UInt16",
                "custom list");
        round(d, "h5");
        round(d, "ply");
        auto r = convert(d, temp.filePath("blocked.vtp"), temp.path(), {});
        require(r.status == "rejected" && !QFileInfo::exists(temp.filePath("blocked.vtp")) &&
                    r.message.contains("readings"),
                r.message);
        QJsonObject job;
        compare(*d, archiveRead(r.archive, {}, &job), {});
        require(job["status"] == "rejected" && !job["complete"].toBool(), "rejected status");
    });
    test("PLY to both native VTK readers and strict return", [&] {
        auto d = read("surface.ply");
        for (QString fmt : {"vtp", "vtk"}) {
            auto path = round(d, fmt);
            vtkSmartPointer<vtkPolyData> native;
            if (fmt == "vtp") {
                vtkNew<vtkXMLPolyDataReader> reader;
                reader->SetFileName(path.toUtf8());
                reader->Update();
                native = reader->GetOutput();
            } else {
                vtkNew<vtkPolyDataReader> reader;
                reader->SetFileName(path.toUtf8());
                reader->ReadAllScalarsOn();
                reader->ReadAllFieldsOn();
                reader->ReadAllNormalsOn();
                reader->ReadAllTCoordsOn();
                reader->ReadAllColorScalarsOn();
                reader->Update();
                native = reader->GetOutput();
            }
            require(native->GetNumberOfPoints() == 4 && native->GetNumberOfPolys() == 2 &&
                        native->GetPoint(1)[0] == 1.0000000000000002,
                    "upstream VTK geometry");
            require(native->GetPointData()->GetArray("RGBA") &&
                        native->GetPointData()->GetArray("RGBA")->GetComponent(0, 3) == 239 &&
                        native->GetCellData()->GetArray("temperature")->GetComponent(0, 0) == -7,
                    "upstream VTK attrs");
            round(load(path, {}), "ply");
        }
    });
    test("empty and points only", [&] {
        for (auto n : {"empty.ply", "cloud.ply"}) {
            auto d = read(n);
            require(d->points(0) == (QString(n) == "empty.ply" ? 0 : 2) && d->cells(0) == 0,
                    "cloud");
            round(d, "vtp");
            round(d, "vtk");
        }
    });
    test("foreign UInt64 and special float bits in compressed big-endian VTP", [&] {
        for (auto n : {"raw.vtp", "inline.vtp"}) {
            auto d = read(n);
            require(at<quint64>(named(*d, "PointData", "large"), 0) == 0x8000000000000001ULL,
                    "u64");
            require(at<quint64>(named(*d, "FieldData", "special"), 0) == 0x7ff8000000000042ULL,
                    "NaN payload");
            round(d, "vtp");
            round(d, "vtk");
            round(d, "h5");
            require(!assess(*d, "PLY").allowed(), "u64 capacity");
        }
    });
    test("native VTK-generated encodings and compressors", [&] {
        vtkNew<vtkPoints> points;
        points->SetDataTypeToDouble();
        points->InsertNextPoint(1.125, 2, 3);
        vtkNew<vtkPolyData> poly;
        poly->SetPoints(points);
        for (int i = 0; i < 7; ++i) {
            auto file = temp.filePath(QString("vtk-writer-%1.vtp").arg(i));
            vtkNew<vtkXMLPolyDataWriter> writer;
            writer->SetInputData(poly);
            writer->SetFileName(file.toUtf8());
            if (!i)
                writer->SetDataModeToAscii();
            else if (i < 3)
                writer->SetDataModeToBinary();
            else {
                writer->SetDataModeToAppended();
                writer->SetEncodeAppendedData(i % 2);
            }
            if (i == 1 || i == 3)
                writer->SetCompressorTypeToNone();
            else if (i == 5)
                writer->SetCompressorTypeToLZ4();
            else if (i == 6)
                writer->SetCompressorTypeToLZMA();
            else
                writer->SetCompressorTypeToZLib();
            if (i % 2)
                writer->SetByteOrderToBigEndian();
            require(writer->Write() == 1, "VTK writer");
            auto d = load(file, {});
            require(d->points(0) == 1 && at<double>(*d->find("Points"), 0) == 1.125,
                    "upstream encoding " + QString::number(i));
            if (i == 6)
                round(d, "vtp");
        }
    });
    test("piece identity preserved; non-PolyData identified", [&] {
        auto d = read("pieces.vtp");
        require(d->pieces() == 2 && at<float>(*d->find("Points", 1), 0) == 4, "pieces");
        round(d, "vtp");
        round(d, "h5");
        require(!assess(*d, "legacy VTK").allowed(), "legacy pieces");
        for (QString k :
             {"STRUCTURED_GRID", "STRUCTURED_POINTS", "RECTILINEAR_GRID", "UNSTRUCTURED_GRID"})
            rejects([&] { read(k + ".vtk"); }, k);
    });
    test("legacy all arrays and floating COLOR_SCALARS", [&] {
        auto d = read("colors.vtk");
        require(at<float>(named(*d, "PointData", "rgb"), 0) == .12345678f &&
                    named(*d, "PointData", "tensor").components == 9 &&
                    named(*d, "PointData", "second").type == Type::U16,
                "attributes");
        require(at<qint32>(named(*d, "CellData", "same"), 0) == -8, "association");
        round(d, "vtk");
        round(d, "vtp");
        round(d, "ply");
    });
    test("legacy big endian lines, uint64 and file mapping", [&] {
        auto d = read("binary.vtk");
        require(at<quint64>(named(*d, "PointData", "large"), 1) == 0xfffffffffffffffeULL &&
                    at<qint64>(*d->find("Lines"), 0) == 1,
                "binary");
        round(d, "vtk");
        round(d, "vtp");
        round(d, "h5");
    });
    test("all four cell kinds retain indices, cell data and native 32-bit topology", [&] {
        auto d = read("all-cells.vtk");
        for (const QString fmt : {"vtp", "vtk"}) {
            auto file = round(d, fmt);
            vtkSmartPointer<vtkPolyData> native;
            if (fmt == "vtp") {
                QFile xml(file);
                require(xml.open(QIODevice::ReadOnly), "VTP read");
                vtkNew<vtkXMLPolyDataReader> reader;
                reader->SetFileName(file.toUtf8());
                reader->Update();
                native = reader->GetOutput();
                require(xml.readAll().contains("type=\"Int32\" Name=\"connectivity\""),
                        "XML index type");
            } else {
                vtkNew<vtkPolyDataReader> reader;
                reader->SetFileName(file.toUtf8());
                reader->Update();
                native = reader->GetOutput();
                require(!native->GetStrips()->IsStorage64Bit(), "legacy index type");
            }
            require(native->GetNumberOfVerts() == 1 && native->GetNumberOfLines() == 1 &&
                        native->GetNumberOfPolys() == 1 && native->GetNumberOfStrips() == 1,
                    "cell counts");
            vtkNew<vtkIdList> ids;
            native->GetCellPoints(3, ids);
            require(ids->GetNumberOfIds() == 4 && ids->GetId(0) == 3 && ids->GetId(2) == 2 &&
                        ids->GetId(3) == 1,
                    "strip order");
            require(native->GetCellData()->GetArray("ordinal")->GetComponent(3, 0) == 44,
                    "cell labels");
        }
    });
    test("distinct fields with the same native name and mixed-width coordinates", [&] {
        auto d = read("duplicate.vtk");
        round(d, "h5");
        require(!assess(*d, "VTP").allowed() && !assess(*d, "legacy VTK").allowed(),
                "native name conflict");
        round(d, "ply");
        auto mixed = read("mixed.ply");
        require(mixed->points(0) == 2 && named(*mixed, "PLY:vertex", "y").type == Type::I16 &&
                    named(*mixed, "PLY:vertex", "z").type == Type::F64,
                "mixed types");
        round(mixed, "h5");
        round(mixed, "ply");
        require(!assess(*mixed, "VTP").allowed(), "non-uniform XYZ");
    });
    test("native PLY channels belong to active colors, normals and texture coordinates", [&] {
        auto src = read("channel-owners.vtp");
        QFile file(round(src, "ply"));
        require(file.open(QIODevice::ReadOnly), "open PLY");
        auto bytes = file.readAll();
        auto start = bytes.indexOf("comment SimTransferRT1 ");
        require(start >= 0, "PLY manifest");
        bytes.remove(start, bytes.indexOf('\n', start) - start + 1);
        require(!bytes.first(bytes.indexOf("end_header")).contains(" alpha\n"),
                "unexpected native alpha");
        auto physical = readPly(bytes, {});
        require(at<quint8>(named(physical, "PointData", "RGB"), 0) == 11 &&
                    at<float>(named(physical, "PointData", "Normals"), 0) == 1 &&
                    at<float>(named(physical, "PointData", "UV"), 0) == 0.25f,
                "native properties selected a different array");
    });
    test("changed or removed native VTP active attribute is reported", [&] {
        for (const QString &source : QStringList{"colors.vtk", "channel-owners.vtp"}) {
            auto src = read(source);
            QFile file(round(src, "vtp"));
            require(file.open(QIODevice::ReadOnly), "open VTP");
            const auto original = file.readAll();
            const QByteArray prior =
                source.endsWith("vtk") ? "Scalars=\"same\"" : "Scalars=\"RGB\"";
            const QByteArray replacement =
                source.endsWith("vtk") ? "Scalars=\"second\"" : "Scalars=\"RGBA\"";
            require(original.contains(prior), "original active Scalars");
            auto modified = original;
            modified.replace(prior, replacement);
            rejects([&] { readVtp(modified, {}); }, "Scalars");
            modified = original;
            modified.replace(prior, "");
            rejects([&] { readVtp(modified, {}); }, "Scalars");
        }
    });
    test("inconsistent recovered PLY row count refused before writing", [&] {
        auto d = *read("cloud.ply");
        auto header = QByteArray::fromBase64(d.descriptor.value("plyHeader").toString().toLatin1());
        header.replace("element vertex 2", "element vertex 1");
        d.descriptor["plyHeader"] = QString::fromLatin1(header.toBase64());
        QBuffer empty;
        require(empty.open(QIODevice::WriteOnly), "buffer");
        rejects([&] { writePly(empty, d, {}); }, "文件头");
        require(empty.size() == 0, "unexpected partial PLY");
    });
    test("texture available and missing", [&] {
        auto d = read("texture.ply");
        require(named(*d, "Resource", "palette.ppm").data.endsWith(QByteArray::fromHex("ff1000")),
                "texture content");
        round(d, "h5");
        auto m = read("missing-texture.ply");
        require(m->descriptor["resourceStatus"].toArray()[0].toObject()["state"] == "missing",
                "missing texture");
    });
    test("BX fixed sample, concatenated frames and error offsets", [&] {
        rt::aurora::Reply sample;
        rt::aurora::Tool tool;
        tool.handle = 0x0A;
        tool.status = rt::aurora::HandleStatus::Valid;
        tool.quaternion = {1, 0, 0, 0};
        tool.positionMm = {1, 2, 3};
        tool.quality = 0;
        tool.portStatus = 0x31;
        tool.frameNumber = 8;
        sample.tools = {tool};
        sample.systemStatus = 0;
        const auto encoded = rt::aurora::encodeBx(sample);
        const QByteArray golden = QByteArray::fromHex(
            "C4A52D003043010A010000803F0000000000000000000000000000803F"
            "000000400000404000000000310000000800000000000439");
        require(encoded.size() == 53 && golden.size() == 53 &&
                    QByteArray(reinterpret_cast<const char *>(encoded.data()), 53) == golden,
                "fixed 53-byte sample");
        const auto single = rt::aurora::encodeSingleValidTool(0x0A, {1, 2, 3}, 8);
        require(QByteArray(reinterpret_cast<const char *>(single.data()), 53) == golden,
                "single-tool encoder matches the general encoder");
        const auto decoded = rt::aurora::decodeBx(encoded);
        require(decoded.tools.size() == 1 && decoded.tools[0].positionMm[0] == 1 &&
                    decoded.tools[0].positionMm[2] == 3 && decoded.tools[0].frameNumber == 8,
                "sample decode");
        rt::Bytes sequence = encoded;
        sequence.insert(sequence.end(), encoded.begin(), encoded.end());
        rt::aurora::BxScanStats stats;
        std::vector<rt::aurora::BxPosition> positions;
        rt::aurora::Reply last;
        rt::aurora::scanBxSequence(
            sequence.data(), sequence.size(),
            [&](const rt::aurora::BxPosition &p) { positions.push_back(p); }, stats, &last);
        require(stats.frames == 2 && stats.valid == 2 && stats.missing == 0 &&
                    stats.disabled == 0 && positions.size() == 2 && last.tools[0].frameNumber == 8,
                "concatenated sequence");
        rejects([&] {
            rt::aurora::BxScanStats s;
            rt::aurora::scanBxSequence(encoded.data(), encoded.size() - 1,
                                       std::function<void(const rt::aurora::BxPosition &)>(), s);
        }, "offset");
        auto bad = encoded;
        bad[10] ^= 1;
        rejects([&] {
            rt::aurora::BxScanStats s;
            rt::aurora::scanBxSequence(bad.data(), bad.size(),
                                       std::function<void(const rt::aurora::BxPosition &)>(), s);
        }, "CRC");
    });
    test("model to BX keeps every vertex, duplicate and unreferenced point", [&] {
        auto surface = read("surface.ply");
        const auto plan = assess(*surface, "Aurora BX");
        require(plan.allowed() && plan.summary().contains("省略属性"), "model to BX allowed");
        const auto target = temp.filePath("surface.bx");
        const auto result = convert(surface, target, temp.path(), {});
        require(result.status == "success", result.message);
        require(QFileInfo(target).size() == 4 * 53, "N x 53 bytes");
        require(result.message.contains("4 个标准 BX 响应帧") &&
                    result.message.contains("舍入误差"),
                "summary");
        auto cloud = load(target, {});
        require(cloud->descriptor["kind"] == "BxPointCloud" && cloud->points(0) == 4 &&
                    cloud->descriptor["frames"] == 4 && cloud->descriptor["validPositions"] == 4,
                "BX point cloud");
        auto &points = named(*cloud, "Points", "");
        require(at<float>(points, 0) == 0 && std::signbit(at<float>(points, 2)) &&
                    at<float>(points, 3) == 1000 && at<float>(points, 4) == 0 &&
                    at<float>(points, 6) == 1000 && at<float>(points, 7) == 1000,
                "metres to millimetres and order");
        auto &frames = named(*cloud, "PointData", "frame_number");
        require(at<std::uint32_t>(frames, 0) == 8 && at<std::uint32_t>(frames, 1) == 16 &&
                    at<std::uint32_t>(frames, 3) == 32,
                "frame numbers start at 8 and step by 8");
        auto mapping = read("bx-model.ply");
        const auto mapped = temp.filePath("bx-model.bx");
        const auto mappedResult = convert(mapping, mapped, temp.path(), {});
        require(mappedResult.status == "success" && QFileInfo(mapped).size() == 5 * 53,
                "five frames for five vertices");
        auto back = load(mapped, {});
        auto &mappedPoints = named(*back, "Points", "");
        require(back->points(0) == 5 && at<float>(mappedPoints, 0) == 0 &&
                    at<float>(mappedPoints, 9) == 0 && at<float>(mappedPoints, 12) == 5,
                "duplicate and unreferenced points kept in order");
        auto unknown = assess(*read("cloud.ply"), "Aurora BX");
        require(unknown.allowed() && unknown.summary().contains("源单位未知"), "unit assumption");
        const auto assumed = convert(read("cloud.ply"), temp.filePath("cloud.bx"), temp.path(), {});
        require(assumed.status == "success" && assumed.message.contains("仿真默认比例"),
                "assumption reported");
        const auto bulk = read("compressed-cloud.ply");
        const auto bulkFile = temp.filePath("bulk.bx");
        const auto bulkResult = convert(bulk, bulkFile, temp.path(), {});
        require(bulkResult.status == "success" &&
                    QFileInfo(bulkFile).size() == 512 * 53,
                "512 vertices produce 512 frames");
        const auto bulkCloud = load(bulkFile, {});
        require(bulkCloud->points(0) == 512 && bulkCloud->descriptor["frames"] == 512 &&
                    bulkCloud->descriptor["validPositions"] == 512,
                "all 512 positions");
    });
    test("BX point cloud keeps all valid samples and status counts", [&] {
        auto d = read("tools.bx");
        require(d->descriptor["kind"] == "BxPointCloud" && d->points(0) == 3 &&
                    d->descriptor["frames"] == 1 && d->descriptor["validPositions"] == 3 &&
                    d->descriptor["missing"] == 1 && d->descriptor["disabled"] == 1,
                "valid/missing/disabled");
        auto &points = named(*d, "Points", "");
        auto &handles = named(*d, "PointData", "handle");
        require(at<float>(points, 0) == 10 && at<float>(points, 3) == 2 &&
                    at<float>(points, 6) == 8 && at<std::uint8_t>(handles, 0) == 11 &&
                    at<std::uint8_t>(handles, 1) == 14 && at<std::uint8_t>(handles, 2) == 15,
                "frame and tool order");
        round(d, "h5");
        auto repeated = read("concat.bx");
        require(repeated->points(0) == 6 && repeated->descriptor["frames"] == 2,
                "repeated handle and coordinates kept");
        for (QString bad : {"crc", "length", "tail", "nonfinite"})
            rejects([&] { read(bad + ".bx"); });
        rt::aurora::Reply emptyReply;
        rt::aurora::Tool missing;
        missing.handle = 1;
        missing.status = rt::aurora::HandleStatus::Missing;
        missing.portStatus = 0;
        missing.frameNumber = 8;
        emptyReply.tools = {missing};
        const auto emptyBytes = rt::aurora::encodeBx(emptyReply);
        auto empty = loadBxObject(
            QByteArray(reinterpret_cast<const char *>(emptyBytes.data()), emptyBytes.size()), {},
            "empty-valid");
        require(empty->points(0) == 0 && empty->descriptor["validPositions"] == 0,
                "no fabricated missing positions");
        auto frame = [](std::uint32_t id, float x) {
            rt::aurora::Reply r;
            rt::aurora::Tool t;
            t.handle = 0x0A;
            t.status = rt::aurora::HandleStatus::Valid;
            t.quaternion = {1, 0, 0, 0};
            t.positionMm = {x, 0, 0};
            t.portStatus = 0x31;
            t.frameNumber = id;
            r.tools = {t};
            return rt::aurora::encodeBx(r);
        };
        auto a = frame(0xfffffff8u, 1), b = frame(0u, 2), c = frame(8u, 3);
        rt::Bytes wrap = a;
        wrap.insert(wrap.end(), b.begin(), b.end());
        wrap.insert(wrap.end(), c.begin(), c.end());
        auto wrapped = loadBxObject(
            QByteArray(reinterpret_cast<const char *>(wrap.data()), wrap.size()), {}, "wrap");
        auto &wrapFrames = named(*wrapped, "PointData", "frame_number");
        require(wrapped->points(0) == 3 && at<std::uint32_t>(wrapFrames, 0) == 0xfffffff8u &&
                    at<std::uint32_t>(wrapFrames, 1) == 0 && at<std::uint32_t>(wrapFrames, 2) == 8,
                "UInt32 frame wrap");
    });
    test("BX cannot be exported as a model and export errors are explicit", [&] {
        auto bx = read("tools.bx");
        for (QString fmt : {"VTP", "PLY", "legacy VTK"})
            require(!assess(*bx, fmt).allowed(), "BX to model must be rejected");
        const auto rejected = convert(bx, temp.filePath("bx-out.vtp"), temp.path(), {});
        require(rejected.status == "rejected" && rejected.output.isEmpty() &&
                    !QFileInfo::exists(temp.filePath("bx-out.vtp")),
                "backend rejects BX to model");
        auto make = [](double x, double y, double z) {
            Dataset d;
            d.descriptor = {{"pieces", 1}};
            Field f;
            f.section = "Points";
            f.type = Type::F64;
            f.components = 3;
            for (double v : {x, y, z})
                f.data.append(reinterpret_cast<const char *>(&v), 8);
            d.fields = {f};
            return d;
        };
        auto writeRejected = [&](const Dataset &d, const QString &why) {
            rejects([&] {
                QBuffer buffer;
                require(buffer.open(QIODevice::WriteOnly), "buffer");
                writeBx(buffer, d, {});
            }, why);
        };
        writeRejected(make(std::nan(""), 0, 0), "第 0 个点");
        writeRejected(make(1e300, 0, 0), "Float32");
        Dataset empty;
        empty.descriptor = {{"pieces", 1}};
        writeRejected(empty, "没有可用点");
    });
    test("bounds, cancellation, collisions and partial publication", [&] {
        rejects([&] { read("bad-index.ply"); }, "索引");
        rejects([&] { read("bad-length.ply"); });
        auto d = read("surface.ply");
        auto same = convert(d, d->path, temp.path(), {});
        require(same.status == "failed" && same.output.isEmpty() && !same.archive.isEmpty(),
                "no overwrite");
        auto dest = temp.filePath("cancel.vtp");
        bool cancel = false;
        auto r =
            convert(d, dest, temp.path(),
                    {[&] { return cancel; }, [&](int n, const QString &) { cancel = n >= 48; }});
        require(r.status == "cancelled" && !r.archive.isEmpty() && !QFileInfo::exists(dest),
                "cancel");
        auto blocked = convert(d, temp.filePath("unpublished.vtp"), d->path, {});
        require(blocked.status == "failed" && blocked.output.isEmpty() && blocked.archive.isEmpty(),
                "archive failure");
        auto race = temp.filePath("race.vtp");
        auto collision =
            convert(d, race, temp.path(), {{}, [&](int n, const QString &) {
                                               if (n == 96) {
                                                   QFile f(race);
                                                   require(f.open(QIODevice::WriteOnly), "fixture");
                                                   f.write("previous result");
                                               }
                                           }});
        require(collision.status == "failed" && collision.output.isEmpty(), "commit failure");
    });
    test("foreign HDF5 and unknown schema rejected", [&] {
        auto file = temp.filePath("foreign.h5");
        auto h = H5Fcreate(file.toUtf8(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
        require(h >= 0, "create foreign archive");
        H5Fclose(h);
        rejects([&] { load(file, {}); }, "SimTransferRT");
        auto own = round(read("cloud.ply"), "h5");
        h = H5Fopen(own.toUtf8(), H5F_ACC_RDWR, H5P_DEFAULT);
        auto ds = H5Dopen2(h, "/SimTransferRT/schema", H5P_DEFAULT);
        auto dims = H5Dget_space(ds);
        hsize_t shape[2];
        H5Sget_simple_extent_dims(dims, shape, nullptr);
        QByteArray b(shape[0], 0);
        H5Dread(ds, H5T_STD_U8LE, H5S_ALL, H5S_ALL, H5P_DEFAULT, b.data());
        b.replace("\"version\":1", "\"version\":2");
        H5Dwrite(ds, H5T_STD_U8LE, H5S_ALL, H5S_ALL, H5P_DEFAULT, b.data());
        H5Sclose(dims);
        H5Dclose(ds);
        H5Fclose(h);
        rejects([&] { load(own, {}); }, "version");
    });
    test("HDF5 typed chunks, reversible filters and field integrity", [&] {
        auto path = round(read("compressed-cloud.ply"), "h5");
        auto h = H5Fopen(path.toUtf8(), H5F_ACC_RDWR, H5P_DEFAULT);
        auto ds = H5Dopen2(h, "/SimTransferRT/arrays/a00000/values", H5P_DEFAULT);
        auto type = H5Dget_type(ds), props = H5Dget_create_plist(ds), space = H5Dget_space(ds);
        hsize_t shape[2]{}, chunk[2]{};
        require(H5Tequal(type, H5T_IEEE_F32LE) > 0 &&
                    H5Sget_simple_extent_dims(space, shape, nullptr) == 2 && shape[0] == 512 &&
                    shape[1] == 3,
                "typed HDF shape");
        require(H5Pget_chunk(props, 2, chunk) == 2 && chunk[1] == 3 &&
                    chunk[0] * chunk[1] * 4 <= 256 * 1024,
                "HDF chunks");
        unsigned args[8]{};
        size_t size = 8;
        require(H5Pget_nfilters(props) == 2 &&
                    H5Pget_filter2(props, 0, nullptr, &size, args, 0, nullptr, nullptr) ==
                        H5Z_FILTER_SHUFFLE,
                "shuffle");
        size = 8;
        require(H5Pget_filter2(props, 1, nullptr, &size, args, 0, nullptr, nullptr) ==
                        H5Z_FILTER_DEFLATE &&
                    size == 1 && args[0] == 1,
                "deflate level");
        hsize_t start[2]{1, 0}, extent[2]{1, 1};
        auto scalar = H5Screate_simple(2, extent, nullptr);
        float replacement = 900;
        require(H5Sselect_hyperslab(space, H5S_SELECT_SET, start, nullptr, extent, nullptr) >= 0 &&
                    H5Dwrite(ds, H5T_NATIVE_FLOAT, scalar, space, H5P_DEFAULT, &replacement) >= 0,
                "modify one value");
        H5Sclose(scalar);
        H5Sclose(space);
        H5Pclose(props);
        H5Tclose(type);
        H5Dclose(ds);
        H5Fclose(h);
        rejects([&] { load(path, {}); }, "字段校验失败");
    });
    std::cout << "passed=" << passed << " failed=" << failures << std::endl;
    return failures ? 1 : 0;
}
