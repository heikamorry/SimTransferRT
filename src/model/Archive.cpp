#include "Model.h"
#include <algorithm>
#include <cstring>
#include <hdf5.h>
#include <mutex>

namespace rt::model {
namespace {
std::mutex archiveLock; // The distributed serial HDF5 build need not be thread-safe.

void ok(herr_t result, const QString &where) {
    if (result < 0)
        fail("HDF5 " + where + " 失败");
}

struct Handle {
    hid_t id;
    herr_t (*closeFn)(hid_t);

    Handle(hid_t v, herr_t (*fn)(hid_t), const QString &what) : id(v), closeFn(fn) {
        if (v < 0)
            fail("HDF5 无法打开或创建 " + what);
    }

    Handle(const Handle &) = delete;

    ~Handle() {
        if (id >= 0)
            closeFn(id);
    }

    operator hid_t() const {
        return id;
    }

    void close(const QString &name) {
        if (id >= 0) {
            auto old = id;
            id = -1;
            ok(closeFn(old), "关闭 " + name);
        }
    }
};

hid_t fileType(Type t) {
    switch (t) {
    case Type::I8:
        return H5T_STD_I8LE;
    case Type::U8:
        return H5T_STD_U8LE;
    case Type::I16:
        return H5T_STD_I16LE;
    case Type::U16:
        return H5T_STD_U16LE;
    case Type::I32:
        return H5T_STD_I32LE;
    case Type::U32:
        return H5T_STD_U32LE;
    case Type::I64:
        return H5T_STD_I64LE;
    case Type::U64:
        return H5T_STD_U64LE;
    case Type::F32:
        return H5T_IEEE_F32LE;
    case Type::F64:
        return H5T_IEEE_F64LE;
    }
    fail("未知的档案数值类型");
}

void localLink(hid_t parent, const QByteArray &name) {
    H5L_info2_t info{};
    if (H5Lget_info2(parent, name.constData(), &info, H5P_DEFAULT) < 0 ||
        info.type != H5L_TYPE_HARD)
        fail("HDF5 缺少本地节点 " + QString::fromLatin1(name));
}

Handle group(hid_t parent, const QByteArray &name, bool create) {
    if (!create)
        localLink(parent, name);
    return Handle(create
                      ? H5Gcreate2(parent, name.constData(), H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)
                      : H5Gopen2(parent, name.constData(), H5P_DEFAULT),
                  H5Gclose, QString::fromLatin1(name));
}

void put(hid_t parent, const QByteArray &name, Type type, qint64 rows, int cols, const char *data,
         const Task &t) {
    hsize_t dims[]{hsize_t(rows), hsize_t(cols)};
    Handle space(H5Screate_simple(2, dims, nullptr), H5Sclose, "dataspace");
    Handle properties(H5Pcreate(H5P_DATASET_CREATE), H5Pclose, "properties");
    const qint64 rowBytes = qint64(cols) * width(type);
    auto block = std::max<qint64>(1, 262144 / rowBytes);
    if (rows && checked(rows, rowBytes) >= 4096) {
        hsize_t chunk[]{hsize_t(std::min(rows, block)), hsize_t(cols)};
        ok(H5Pset_chunk(properties, 2, chunk), "分块");
        ok(H5Pset_shuffle(properties), "shuffle");
        ok(H5Pset_deflate(properties, 1), "DEFLATE");
    }
    Handle ds(H5Dcreate2(parent, name.constData(), fileType(type), space, H5P_DEFAULT, properties,
                         H5P_DEFAULT),
              H5Dclose, name);
    for (qint64 i = 0; i < rows; i += block) {
        t.check();
        hsize_t origin[]{hsize_t(i), 0};
        hsize_t extent[]{hsize_t(std::min(block, rows - i)), hsize_t(cols)};
        ok(H5Sselect_hyperslab(space, H5S_SELECT_SET, origin, nullptr, extent, nullptr), "写选区");
        Handle mem(H5Screate_simple(2, extent, nullptr), H5Sclose, "内存选区");
        ok(H5Dwrite(ds, fileType(type), mem, space, H5P_DEFAULT, data + i * rowBytes),
           "写 " + QString::fromLatin1(name));
    }
    ds.close(name);
}

QByteArray get(hid_t parent, const QByteArray &name, Type type, qint64 rows, int cols, qint64 limit,
               const Task &t) {
    localLink(parent, name);
    Handle ds(H5Dopen2(parent, name.constData(), H5P_DEFAULT), H5Dclose, name);
    Handle properties(H5Dget_create_plist(ds), H5Pclose, "存储属性");
    if (H5Pget_external_count(properties) != 0 || H5Pget_layout(properties) == H5D_VIRTUAL)
        fail("不支持外部或虚拟 HDF5 数据");
    int filters = H5Pget_nfilters(properties);
    for (int i = 0; i < filters; ++i) {
        auto filter = H5Pget_filter2(properties, i, nullptr, nullptr, nullptr, 0, nullptr, nullptr);
        if (filter != H5Z_FILTER_SHUFFLE && filter != H5Z_FILTER_DEFLATE)
            fail("HDF5 过滤器不属于本模块 schema");
    }
    Handle ty(H5Dget_type(ds), H5Tclose, "数据类型");
    if (H5Tequal(ty, fileType(type)) <= 0)
        fail("HDF5 数组类型不符 " + QString::fromLatin1(name));
    Handle space(H5Dget_space(ds), H5Sclose, "dataspace");
    hsize_t dims[2]{};
    if (H5Sget_simple_extent_ndims(space) != 2 ||
        H5Sget_simple_extent_dims(space, dims, nullptr) != 2 || dims[0] > hsize_t(limit) ||
        dims[1] != hsize_t(cols) || (rows >= 0 && dims[0] != hsize_t(rows)))
        fail("HDF5 数组形状不符 " + QString::fromLatin1(name));
    rows = qint64(dims[0]);
    auto rowBytes = qint64(cols) * width(type);
    QByteArray data(checked(rows, rowBytes, limit), Qt::Uninitialized);
    qint64 block = std::max<qint64>(1, (1 << 20) / rowBytes);
    for (qint64 i = 0; i < rows; i += block) {
        t.check();
        hsize_t origin[]{hsize_t(i), 0};
        hsize_t extent[]{hsize_t(std::min(block, rows - i)), hsize_t(cols)};
        ok(H5Sselect_hyperslab(space, H5S_SELECT_SET, origin, nullptr, extent, nullptr), "读选区");
        Handle mem(H5Screate_simple(2, extent, nullptr), H5Sclose, "内存选区");
        ok(H5Dread(ds, fileType(type), mem, space, H5P_DEFAULT, data.data() + i * rowBytes),
           "读取 " + QString::fromLatin1(name));
    }
    return data;
}

QByteArray idName(int i) {
    return "a" + QByteArray::number(i).rightJustified(5, '0');
}

void metadata(hid_t g, const QByteArray &name, const QJsonObject &record, const Task &t) {
    auto bytes = json(record);
    put(g, name, Type::U8, bytes.size(), 1, bytes.constData(), t);
}
} // namespace

void archiveWrite(const QString &path, const Dataset &d, const QJsonObject &record, const Task &t) {
    std::lock_guard<std::mutex> lock(archiveLock);
    H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr);
    Handle file(H5Fcreate(path.toUtf8().constData(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT),
                H5Fclose, path);
    {
        auto root = group(file, "SimTransferRT", true);
        metadata(
            root, "schema",
            {{"application", "SimTransferRT/model-conversions"},
             {"version", 1},
             {"filters", "data >=4096 bytes: shuffle + DEFLATE 1, chunks <=256 KiB or one tuple"}},
            t);
        auto schema = manifest(d);
        metadata(root, "manifest", schema, t);
        metadata(root, "provenance", record, t);
        auto arrays = group(root, "arrays", true);
        QJsonArray hashes;
        for (size_t i = 0; i < d.fields.size(); ++i) {
            const auto &f = d.fields[i];
            auto g = group(arrays, idName(int(i)), true);
            put(g, "values", f.type, f.size(), f.components, f.data.constData(), t);
            if (f.list)
                put(g, "ends", Type::I64, f.ends.size(), 1,
                    reinterpret_cast<const char *>(f.ends.data()), t);
            hashes.append(fieldHash(f, t));
            g.close("数组");
        }
        if (d.tracking)
            put(root, "bx", Type::U8, d.bx.size(), 1, d.bx.constData(), t);
        metadata(root, "integrity",
                 {{"algorithm", "SHA-256"},
                  {"manifest", digest(json(schema))},
                  {"fields", hashes},
                  {"bx", digest(d.bx)}},
                 t);
        arrays.close("arrays");
        root.close("SimTransferRT");
    }
    ok(H5Fflush(file, H5F_SCOPE_GLOBAL), "flush");
    file.close(path);
}

Dataset archiveRead(const QString &path, const Task &t, QJsonObject *record) {
    std::lock_guard<std::mutex> lock(archiveLock);
    H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr);
    Handle file(H5Fopen(path.toUtf8().constData(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose, path);
    auto root = group(file, "SimTransferRT", false);
    auto readJson = [&](const char *name) {
        return object(get(root, name, Type::U8, -1, 1, 16 << 20, t));
    };
    auto schema = readJson("schema");
    if (schema["application"] != "SimTransferRT/model-conversions" ||
        schema["version"].toInt() != 1)
        fail("HDF5 标识或 schema version 不受支持");
    auto info = readJson("manifest");
    auto integrity = readJson("integrity");
    auto job = readJson("provenance");
    if (info["version"].toInt() != 1 || digest(json(info)) != integrity["manifest"].toString())
        fail("HDF5 manifest 版本或 SHA-256 不符");
    auto specs = info["fields"].toArray();
    auto hashes = integrity["fields"].toArray();
    if (specs.size() != hashes.size() || specs.size() > 8192)
        fail("HDF5 字段数量不符");
    Dataset d;
    d.descriptor = info["dataset"].toObject();
    auto arrays = group(root, "arrays", false);
    qint64 total = 0;
    for (int i = 0; i < specs.size(); ++i) {
        t.check();
        auto spec = specs.at(i).toObject();
        if (spec["id"].toInt(-1) != i)
            fail("HDF5 字段 ID 不连续");
        auto f = description(spec);
        auto rows = spec["values"].toInteger(-1);
        auto tuples = spec["tuples"].toInteger(-1);
        total += checked(rows, width(f.type) * f.components) + checked(tuples, f.list ? 8 : 0);
        if (total > budget)
            fail("HDF5 解压超过预算");
        auto g = group(arrays, idName(i), false);
        f.data = get(g, "values", f.type, rows, f.components, budget, t);
        if (f.list) {
            auto ends = get(g, "ends", Type::I64, tuples, 1, budget, t);
            f.ends.resize(tuples);
            if (tuples)
                std::memcpy(f.ends.data(), ends.constData(), ends.size());
        }
        if (f.tuples() != tuples || fieldHash(f, t) != hashes[i].toString())
            fail("HDF5 字段校验失败 " + f.name);
        d.fields.push_back(std::move(f));
    }
    if (d.descriptor["kind"] == "Tracking") {
        d.bx = get(root, "bx", Type::U8, -1, 1, aurora::MaxBxReplyBytes, t);
        d.tracking =
            aurora::decodeBx(reinterpret_cast<const uint8_t *>(d.bx.constData()), d.bx.size());
    }
    if (digest(d.bx) != integrity["bx"].toString())
        fail("HDF5 BX 校验不符");
    validate(d, t);
    if (record)
        *record = job;
    return d;
}
} // namespace rt::model
