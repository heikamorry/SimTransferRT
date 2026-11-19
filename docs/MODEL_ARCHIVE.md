# HDF5 过程档案 v1

每个转换任务生成一个独立 HDF5 文件，默认文件名为 `<jobId>.h5`。模型目标文件和本次已加载的完整源快照分别发布。用户直接选 `.h5` / `.hdf5` 作为目标时，选定的文件同时承担两者。GUI 的“打开过程档案”可还原数据集，并显示这次任务的结果。

## 文件层次

所有路径以 `/SimTransferRT/` 为起点。四项 JSON 元数据均以无 BOM 的 UTF-8 紧凑文本存放在二维 `UInt8[N,1]` 数据集中。

| 路径 | 数据类型和形状 | 含义 |
|---|---|---|
| `schema` | JSON | `application="SimTransferRT/model-conversions"`、`version=1`、分块与压缩策略 `filters`。读取器检查标识和版本。 |
| `manifest` | JSON | `version=1`，`dataset` 含 kind、源格式、Piece 数、可得单位和坐标系、源格式描述及资源引用，`fields` 是有序字段数组。 |
| `arrays/a00000/values` 等 | `[values,components]`，带符号/无符号 8/16/32/64 位整数或 IEEE Float32/64 | 五位十进制字段序号从零开始。一行包含一个定长值元组的全部分量。整数使用 HDF5 标准小端类型，浮点使用 IEEE 小端类型。 |
| `arrays/a00000/ends` 等 | `Int64[tuples,1]`，小端 | 仅用于变长字段。存放每个列表右侧的累计开区间终点，不附加起始零。终点须单调且最终等于 values 行数。例如，两个列表长度为 3、4，存为 `[3,7]`。连续相同终点表示空列表。 |
| `bx` | `UInt8[N,1]` | 跟踪数据集包含原始完整 BX 应答。恢复时校验并重新解码，与归档的跟踪字段交叉检查。 |
| `provenance` | JSON | UUID、源/目标路径与格式、起止 UTC、文件 SHA-256、能力检查、结果及失败原因。 |
| `integrity` | JSON | `algorithm="SHA-256"`，`manifest` 是 Qt 紧凑序列化 manifest 的摘要，`fields` 是依字段顺序存放的摘要，`bx` 是原始 BX 字节摘要（对模型为空字节串的摘要）。 |

一个字段的 manifest 条目含 `id, section, name, role, type, components, piece, list, tuples, values, declaration`。名称允许相同，以字段序号和关联定位。定长字段 `values=tuples`。变长字段 `tuples` 表示列表数，`values` 为扁平化后的行数。字段哈希先处理 `values` 原始小端字节，再处理存在的 `ends` 小端字节。用户可独立读取原生 HDF5 数组、索引和摘要。

点位保存于 Points 或按源声明分离的坐标字段，单元按 Verts、Lines、Polys、Strips 保留连接和边界。PointData 和 CellData 可指示 Scalars、Colors、Normals、TCoords 等角色，FieldData 保存整体关联。自定义 PLY 元素、数值列表和属性声明也列在 manifest。引用的贴图路径与状态记录在 `dataset.resourceStatus`，已归档文件的字节存放于 Resource 字段。

跟踪记录的 `handle` / `status` 按输入顺序覆盖全部工具。`transform_record` 把存在的姿态映射回工具行，对应 `position_mm`、`quaternion_wxyz` 和 `quality`。`port_record` 对应 `port_status` 和 `device_frame`。Missing 工具仍可有端口与帧号，Disabled 工具的缺失分量保留为空。系统状态位于 `dataset.systemStatus`。

## 发布状态与校验

- `sourceFileSHA256` 是本次实际打开的文件摘要。非 HDF5 目标通过回读比较后，`outputFileSHA256` 记录本次目标文件摘要。由旧档案重新转换时 `parentJobId` 指向旧任务，源文件哈希对应本次打开的档案。
- `status=success`、`complete=true` 表示该次转换完成。已解析的完整源快照用 `sourceDatasetComplete=true` 标记。`status=rejected / cancelled / failed` 配合 `complete=false` 表示该次请求的目标未完整交付。此时档案仍可包含完整可恢复的源数据，`detail` 给出原因，`publishedOutput` 标记已发布的目标路径。保存失败且档案目录不可写时，具体错误直接显示在 GUI 中。
- 目标与档案各自在目的目录生成临时文件，验证后分别重命名，已存在的目标拒绝覆盖。两个目录无法共同原子提交。若在两次发布之间发生断电，可有单独的目标文件。验收目标应同时核对成功记录和输出摘要。
- 每个数据集达 4096 字节时采用 shuffle 和 DEFLATE level 1。块按完整行分隔，通常不超过 256 KiB，超宽单行单独成块。读取器检查真实 HDF5 数据类型、维度及过滤器，拒绝外部数据、虚拟数据集和软链接/外部链接，再验证字段摘要和数据结构。

## 用 HDF5 工具检查

在仓库根目录的 Windows CMD 中，将最后一个参数替换为实际档案名。第一条可列出全部对象，第二条显示类型、维度、块和过滤器。

```bat
build\model-deps\hdf-sdk\HDF5-1.14.6-win64\bin\h5dump.exe -n "D:\models\sample.h5"
build\model-deps\hdf-sdk\HDF5-1.14.6-win64\bin\h5dump.exe -pH -d /SimTransferRT/arrays/a00000/values "D:\models\sample.h5"
```

模型转 BX 的任务档案保存源模型；由 BX 帧序列构建的点云归档保存 `Points` 与 `PointData` 字段（`kind=BxPointCloud`），不重复保存原始帧字节。旧版跟踪档案仍使用 `bx` 字段。

已装有 h5py 的分析环境可直接解码任务记录。以下代码列出字段名、关联和物理形状，适用于模型及 BX 档案。

```python
import json
import h5py

with h5py.File(r"D:\models\sample.h5", "r") as archive:
    root = archive["SimTransferRT"]
    job = json.loads(root["provenance"][:, 0].tobytes().decode("utf-8"))
    manifest = json.loads(root["manifest"][:, 0].tobytes().decode("utf-8"))
    print(job["jobId"], job["status"], job["sourceFileSHA256"])
    for field in manifest["fields"]:
        values = root[f"arrays/a{field['id']:05d}/values"]
        print(field["section"], field["name"], values.shape, values.dtype)
```
