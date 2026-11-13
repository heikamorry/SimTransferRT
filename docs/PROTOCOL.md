# 字节级协议映射

依据《UDP 多模块实时数据传输协议》第2节；所有多字节整数小端。

## DATA(type=1)

| offset | bytes | field |
|---:|---:|---|
|0|4|magic 0xAA55A55A → 5A A5 55 AA|
|4|2|pkt_len = 36 + payload长度|
|6|1|type=1|
|7|1|flags：初发0，重传1|
|8|4|logic_id uint32|
|12|4|frame_len，1..2097152|
|16|2|frag_idx，从0开始|
|18|2|frag_cnt=ceil(frame_len/1436)|
|20|8|ts，μs|
|28|4|module_mask|
|32|M|逻辑帧偏移 frag_idx*1436 的载荷|
|32+M|4|CRC32（覆盖前32+M字节）|

非末片M=1436；末片M=frame_len-(frag_cnt-1)*1436。有效逻辑帧的最小模块子结构5B，因此数据生成器不会生成小于5B的业务帧；解析器保留原文frame_len>=1的线上检查。
CRC32/IEEE反射多项式0xEDB88320，init/xorout=0xFFFFFFFF，known check("123456789")=0xCBF43926。
模块流：[module_id:uint8][data_len:uint32 LE][data]，按位号从小到大，允许跨片。

## NACK(type=2)

公共头8B，flags=0；logic_id(4B)；range_cnt(2B)；reserved(2B，0)；ranges每项start:uint16+count:uint16；最后CRC32。
总长=20+4*N，N=1..363。区间为[start,start+count)，count不得为0，加法使用32位检查，必须落在缓存帧片数范围内。
允许同一帧同一轮多包NACK，没有requestId/round字段。CRC非法、字段非法或区间越界整包拒绝；已被覆盖的帧忽略并计数。

## 网络设置

UDP IPv4，DF置位。默认DATA发往127.0.0.1:5650，Sim在127.0.0.1:5500绑定同一个数据/命令socket；接收端net.cmd_dst对应5500。可以改成实际网卡单播地址；跨机器不能继续绑定127.0.0.1。
仅Windows/Linux实现了原生DF选项；设置失败拒绝启动，不静默允许IP分片。

## 不存在的字段/报文

没有版本号、128位transferId、SHA256、START/READY/FIN/SUCCESS/FAIL/CANCEL。不能用旧SimUDP/1反馈端测试本版。
