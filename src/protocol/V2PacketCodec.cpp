#include "protocol/V2PacketCodec.h"
#include "protocol/PacketCodec.h"
#include "protocol/Checksum.h"
#include <algorithm>
#include <cstring>
namespace rt::v2 {
namespace {
void put64(std::uint8_t*p,std::uint64_t v){for(unsigned i=0;i<8;++i)p[i]=std::uint8_t(v>>(8*i));}
std::uint64_t get64(const std::uint8_t*p){std::uint64_t v=0;for(unsigned i=0;i<8;++i)v|=std::uint64_t(p[i])<<(8*i);return v;}
void module(std::uint8_t m){if(m!=NdiModule&&m!=EpModule)throw ProtocolError("V2 invalid data module");}
void header(Datagram&d,std::uint8_t type,std::uint8_t flags,std::uint8_t m){auto*p=d.bytes.data();writeLe32(p,Magic);writeLe16(p+4,std::uint16_t(d.size));p[6]=type;p[7]=flags;p[8]=m;}
void finish(Datagram&d){writeLe32(d.bytes.data()+d.size-4,crc32(d.bytes.data(),d.size-4));}
void check(const std::uint8_t*p,std::size_t n,std::uint8_t type,std::size_t minimum){
    if(!p||n<minimum||n>MaxDatagram)throw ProtocolError("V2 invalid datagram length");
    if(readLe32(p)!=Magic||readLe16(p+4)!=n||p[6]!=type)throw ProtocolError("V2 magic/length/type mismatch");
    if(p[9]||p[10]||p[11])throw ProtocolError("V2 reserved bytes must be zero");
    if(crc32(p,n-4)!=readLe32(p+n-4))throw ProtocolError("V2 CRC32 mismatch");
}
}
Datagram encodeData(const Frame&f,std::uint16_t index,bool retry){
    module(f.moduleId);auto count=f.fragmentCount();if(index>=count)throw ProtocolError("V2 fragment index outside frame");
    auto off=std::size_t(index)*PayloadStep;auto len=std::min(PayloadStep,f.bytes.size()-off);
    Datagram d;d.size=Overhead+len;header(d,DataType,retry?1:0,f.moduleId);auto*p=d.bytes.data();
    writeLe32(p+12,f.frameId);writeLe32(p+16,std::uint32_t(f.bytes.size()));writeLe16(p+20,index);writeLe16(p+22,count);put64(p+24,f.timestampUs);
    std::memcpy(p+HeaderSize,f.bytes.data()+off,len);finish(d);return d;
}
DataPacket decodeData(const std::uint8_t*p,std::size_t n){
    check(p,n,DataType,Overhead);module(p[8]);if(p[7]&0xfe)throw ProtocolError("V2 invalid DATA flags");
    DataPacket d;d.moduleId=p[8];d.frameId=readLe32(p+12);d.frameLength=readLe32(p+16);d.fragmentIndex=readLe16(p+20);d.fragmentCount=readLe16(p+22);d.timestampUs=get64(p+24);d.retransmission=(p[7]&1)!=0;
    if(d.fragmentCount!=rt::fragmentCount(d.frameLength)||d.fragmentIndex>=d.fragmentCount)throw ProtocolError("V2 invalid fragment geometry");
    const auto len=std::min(PayloadStep,std::size_t(d.frameLength)-std::size_t(d.fragmentIndex)*PayloadStep);
    if(n!=Overhead+len)throw ProtocolError("V2 invalid fragment payload size");d.payload.assign(p+HeaderSize,p+HeaderSize+len);return d;
}
Datagram encodeNack(const Nack&n){
    module(n.moduleId);if(n.indices.empty()||n.indices.size()>MaxNackIndices)throw ProtocolError("V2 invalid NACK index count");
    Datagram d;d.size=NackFixedSize+2*n.indices.size();header(d,NackType,0,n.moduleId);auto*p=d.bytes.data();writeLe32(p+12,n.frameId);writeLe16(p+16,std::uint16_t(n.indices.size()));
    for(std::size_t i=0;i<n.indices.size();++i){if(n.indices[i]>=rt::fragmentCount(MaxFrameBytes))throw ProtocolError("V2 NACK index beyond maximum frame");writeLe16(p+20+2*i,n.indices[i]);}finish(d);return d;
}
Nack decodeNack(const std::uint8_t*p,std::size_t n){
    check(p,n,NackType,NackFixedSize+2);module(p[8]);auto count=readLe16(p+16);
    if(p[7]||readLe16(p+18)||!count||count>MaxNackIndices||n!=NackFixedSize+2u*count)throw ProtocolError("V2 invalid NACK fields");
    Nack out;out.moduleId=p[8];out.frameId=readLe32(p+12);out.indices.reserve(count);for(unsigned i=0;i<count;++i){auto index=readLe16(p+20+2*i);if(index>=rt::fragmentCount(MaxFrameBytes))throw ProtocolError("V2 NACK index beyond maximum frame");out.indices.push_back(index);}return out;
}
void validateIndices(const Nack&n,std::uint16_t count){module(n.moduleId);if(!count||n.indices.empty()||n.indices.size()>MaxNackIndices)throw ProtocolError("V2 invalid NACK geometry");for(auto index:n.indices)if(index>=count)throw ProtocolError("V2 NACK index outside cached frame");}
Datagram encodeStatus(const Status&s){
    if(s.payload.size()>MaxStatusPayload)throw ProtocolError("V2 status cannot be fragmented");Datagram d;d.size=StatusOverhead+s.payload.size();header(d,StatusType,0,StatusModule);auto*p=d.bytes.data();writeLe32(p+12,s.sequence);put64(p+16,s.timestampUs);if(!s.payload.empty())std::memcpy(p+StatusHeaderSize,s.payload.data(),s.payload.size());finish(d);return d;
}
Status decodeStatus(const std::uint8_t*p,std::size_t n){check(p,n,StatusType,StatusOverhead);if(p[7]||p[8]!=StatusModule)throw ProtocolError("V2 invalid STATUS flags/module");Status s;s.sequence=readLe32(p+12);s.timestampUs=get64(p+16);s.payload.assign(p+StatusHeaderSize,p+n-4);return s;}
}
