# 文件格式与严格转换能力

适配器根据文件内容、数据集类型、每个字段的声明和实际取值判断可转换范围。对任何文件名执行同一组检查。下表中的“可转换”要求源数据可完整解析、目标能承载其内容，且写出后通过逐字段回读比较。具体文件的判定见转换对话框。

| 源数据 | 可读取、归档或恢复的内容 | 模型目标与主要拒绝条件 |
|---|---|---|
| PLY 1.0 | ASCII、二进制小端和大端。保留头部、注释、元素/属性的声明顺序、坐标、法向量、RGB/RGBA、UV、其他定长数值及变长数值列表。支持点云、三角形与多边形面。 | 可按源布局写回 PLY，或转换可映射的字段到 VTP / legacy VTK。自定义元素、非拓扑变长属性或异类型坐标可能要求源 PLY 或 HDF5。文件声明外部贴图时，模型输出拒绝，档案保存资源状态。 |
| VTP XML PolyData | 读取版本 0.1、1.0、2.0、2.1、2.2、2.3。一或多个 Piece，各自的 Points、Verts、Lines、Polys、Strips、PointData、CellData，以及数据集级 FieldData。支持 Int/UInt 8、16、32、64 和 Float32/64。读取 ASCII、内联 Base64 binary、appended raw/Base64、两种字节序、UInt32/UInt64 长度头。支持无压缩及 VTK zlib、LZ4、LZMA 块。 | 可输出 VTP。多 Piece 转单块格式会丢失分块，拒绝 PLY/legacy。单块转 legacy 要求属性均可表示。转标准 PLY 时拒绝非 Polys 单元、FieldData、64 位整数属性及超范围索引。并行 PPolyData、时间序列、字符串或其他未实现节点会报错。 |
| legacy VTK POLYDATA | ASCII 和大端二进制，1.0 至 5.1 格式头。经典单元列表及 5.1 的 OFFSETS / CONNECTIVITY。支持四类单元、POINT_DATA、CELL_DATA、FIELD、SCALARS、VECTORS、NORMALS、TEXTURE_COORDINATES、TENSORS、COLOR_SCALARS 和 LOOKUP_TABLE。 | 可输出支持的 legacy、VTP 或 PLY。显式查找表可保存到 legacy/HDF5。legacy 的颜色要求编码一致，同一文件混合 Float32 颜色与 UInt8 颜色时拒绝严格 legacy 导出。读到 STRUCTURED_POINTS、STRUCTURED_GRID、RECTILINEAR_GRID、UNSTRUCTURED_GRID 等非 POLYDATA 时报告实际 DATASET 类别。位宽不明的 long 等类型拒绝解析。 |
| Aurora BX 帧序列 | 严格校验起始标记、声明长度、头/正文 CRC16 和工具记录边界。按帧顺序、帧内工具记录顺序保留全部 Valid 位置；重复 Handle、重复坐标和 UInt32 帧号回绕都不合并。Missing/Disabled 只计数，不产生坐标。可写 HDF5 点云档案。 | 模型可单向导出为标准 BX 帧序列：每个顶点一帧、单工具 Valid、坐标 mm、帧号 8 起每帧 +8；颜色、面连接、纹理和法向不写入。BX 不支持反向导出为 PLY/VTP/VTK，也不恢复模型属性；该路径在界面和后端都拒绝。 |
| SimTransferRT HDF5 schema v1 | 可恢复已存入的源数据集及任务记录，包括被拒或被取消的任务。检查版本、物理类型、形状、长度、SHA-256 和拓扑。BX 会再次调用协议解析器。 | 以恢复后实际字段重新做能力检查。允许输出新 HDF5 任务档案并建立 parentJobId 关联。其他应用的 HDF5 schema 需单独适配。 |

### 数值、声明及互操作

- 模型保留点、单元、顶点索引、Piece 和数组的顺序。源数组逐分量保存整数和 IEEE 754 浮点的位模式。符号零、NaN 载荷和无穷值均参与最终比较。ASCII 输入保存解析后的数值。若目标文本编码不能恢复同一位模式，目标回读失败。文件字节哈希用于辨认某一次的输入或输出，重新编码可改变字节序、压缩、空白或文件长度。
- 同一 Piece 内 PointData 和 CellData 可分别有同名字段。在同一 Piece、同一关联内有重名属性时，常规 VTK 读者可能合并它们，严格 VTP / legacy 导出会拒绝。多个属性共享 VTP 活动角色时，保留各数值数组，首组选为活动数组，其余角色写入公开的字段映射。生成 PLY 时，优先为活动颜色、法向和 UV 分配标准属性名，避免同名普通标量抢占通道。重新读入带映射的 VTP 时会核对 XML 中的活动属性。其他软件若已修改或删除 Scalars 等活动属性而保留旧映射，程序会报告矛盾并拒绝打开。legacy 5.1 连接索引和累计偏移必须同时为 Int32 或同时为 Int64。源声明不兼容时输出 VTP 或 HDF5。
- VTK 单元按有符号 64 位连接索引和累计终点保存，保留源声明。生成标准 PLY 面连接前检查范围。跨格式输出将真实数值写在原生数组中。必要时用附加描述恢复逻辑字段的关联、名称、原类型和顺序。PLY 使用 `comment SimTransferRT1` 和字段绑定，VTP 使用 XML 注释和 `sim_id`，legacy 使用 `FIELD` 数组 `SimTransferRT_schema_v1`。第三方软件可读取其支持的原生数据。若它重写文件并删除扩展，完整源声明可能丢失。HDF5 源快照适合后续恢复。
- PLY 的 `x/y/z`、`nx/ny/nz`、`red/green/blue[/alpha]`，及成对的 `u/v`、`s/t`、`texture_u/texture_v` 可按名称识别。带 `TextureFile` 注释时，记录原引用、资源状态，并将模型目录内可读的资源按原字节归档。找不到或解析到模型目录之外的引用会记录相应状态。目前模型输出无法作为一组文件共同提交贴图，遇到该引用时选择 HDF5。
- 预览创建单独的 VTK 显示数组。着色、相机、曲面栅格化和渲染用的四元数归一化均只影响显示。外部格式没有声明单位或坐标系时，程序保留未知值。

### 输入边界

普通模型源文件上限 768 MiB；BX 帧序列文件同样上限 768 MiB，单帧正文不超过 16 KiB，逐帧检查长度和 CRC。字段最多 8192 个，Piece 最多 4096 个，单字段分量上限 4096，常规数量声明限制为 67,108,864。解析检查乘法溢出、界限、无符号索引范围、单元累计偏移和属性数。规范数据数组预算 2 GiB。输入缓冲、渲染副本和回读校验可使进程工作集超过此值。PLY 头上限 8 MiB，VTP XML 元数据限制 256 MiB，单项 HDF5 JSON 元数据限制 16 MiB，外部贴图总量限制 256 MiB。超过支持范围时应保留原文件，按界面报错定位格式、资源或字段。
