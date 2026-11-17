#include "protocol/V2PacketCodec.h"
#include "protocol/PacketCodec.h"
#include "protocol/Checksum.h"
#include "sources/AuroraCodec.h"
#include <iostream>
#include <functional>
#include <limits>
#include <iomanip>
#include <sstream>
namespace {
void require(bool v,const char*m){if(!v)throw std::runtime_error(m);}
template<class F>void rejects(F f){bool bad=false;try{f();}catch(const rt::ProtocolError&){bad=true;}require(bad,"malformed packet accepted");}
void crc(rt::Datagram&d){rt::writeLe32(d.bytes.data()+d.size-4,rt::crc32(d.bytes.data(),d.size-4));}
std::string hex(const std::uint8_t*p,std::size_t n){std::ostringstream o;o<<std::hex<<std::setfill('0');for(std::size_t i=0;i<n;++i)o<<std::setw(2)<<unsigned(p[i]);return o.str();}
void echoGolden(){std::string line;while(std::getline(std::cin,line)){try{
    bool bx=line.rfind("bx:",0)==0;if(bx)line=line.substr(3);
    require(line.size()%2==0&&line.size()<=32768,"hex size");rt::Bytes b;
    for(std::size_t i=0;i<line.size();i+=2){std::size_t used=0;auto v=std::stoul(line.substr(i,2),&used,16);require(used==2,"hex syntax");b.push_back(std::uint8_t(v));}
    if(bx){auto reply=rt::aurora::decodeBx(b);auto encoded=rt::aurora::encodeBx(reply);std::cout<<"bx:"<<hex(encoded.data(),encoded.size())<<'\n';continue;}
    require(b.size()>=8,"short");rt::Datagram d;
    if(b[6]==1){auto p=rt::v2::decodeData(b.data(),b.size());rt::v2::Frame f;f.moduleId=p.moduleId;f.frameId=p.frameId;f.timestampUs=p.timestampUs;f.bytes.resize(p.frameLength);std::copy(p.payload.begin(),p.payload.end(),f.bytes.begin()+p.fragmentIndex*rt::PayloadStep);d=rt::v2::encodeData(f,p.fragmentIndex,p.retransmission);}
    else if(b[6]==2)d=rt::v2::encodeNack(rt::v2::decodeNack(b.data(),b.size()));
    else if(b[6]==3)d=rt::v2::encodeStatus(rt::v2::decodeStatus(b.data(),b.size()));
    else throw rt::ProtocolError("type");std::cout<<hex(d.bytes.data(),d.size)<<'\n';
}catch(const std::exception&){std::cout<<"ERROR\n";}}}
}
int main(int argc,char**argv){try{
    if(argc==2&&std::string(argv[1])=="--echo-golden"){echoGolden();return 0;}
    if(argc==2&&std::string(argv[1])=="--emit-golden"){
        rt::v2::Frame f;f.moduleId=1;f.frameId=0xffffffff;f.timestampUs=0x0102030405060708ull;f.bytes=rt::aurora::generateSample(2,8);
        std::cout<<"bx:"<<hex(f.bytes.data(),f.bytes.size())<<'\n';auto d=rt::v2::encodeData(f,0);std::cout<<hex(d.bytes.data(),d.size)<<'\n';
        rt::v2::Nack n;n.moduleId=2;n.frameId=5;n.indices={0,7,1460};d=rt::v2::encodeNack(n);std::cout<<hex(d.bytes.data(),d.size)<<'\n';
        rt::v2::Status s;s.sequence=0xffffffff;s.timestampUs=99;s.payload={0,1,255};d=rt::v2::encodeStatus(s);std::cout<<hex(d.bytes.data(),d.size)<<'\n';return 0;
    }
    for(auto size:{std::size_t(1),std::size_t(1436),std::size_t(1437),std::size_t(2097152)})for(auto module:{1,2}){
        rt::v2::Frame f;f.moduleId=std::uint8_t(module);f.frameId=0xfedcba98;f.timestampUs=0x0102030405060708ull;f.bytes.resize(size);for(std::size_t i=0;i<size;++i)f.bytes[i]=std::uint8_t(i*17);
        rt::Bytes assembled;for(unsigned i=0;i<f.fragmentCount();++i){auto d=rt::v2::encodeData(f,std::uint16_t(i));auto p=rt::v2::decodeData(d.bytes.data(),d.size);require(d.size<=1472&&p.moduleId==module&&p.frameId==f.frameId&&p.timestampUs==f.timestampUs,"boundary fields");assembled.insert(assembled.end(),p.payload.begin(),p.payload.end());auto retry=rt::v2::encodeData(f,std::uint16_t(i),true);auto r=rt::v2::decodeData(retry.bytes.data(),retry.size);require(r.retransmission&&r.payload==p.payload,"retry changes payload");}require(assembled==f.bytes,"boundary reassembly");
    }
    rt::v2::Frame f;f.bytes={0x11};f.frameId=0x78563412;f.timestampUs=0x0807060504030201ull;
    auto d=rt::v2::encodeData(f,0);const rt::Bytes prefix={0x5a,0xa5,0x55,0xaa,37,0,1,0,1,0,0,0,0x12,0x34,0x56,0x78,1,0,0,0,0,0,1,0,1,2,3,4,5,6,7,8,0x11};require(std::equal(prefix.begin(),prefix.end(),d.bytes.begin()),"V2 independent expected field offsets");
    for(auto off:{0,4,6,7,8,9,10,11,16,20,22}){auto bad=d;bad.bytes[off]=0xff;crc(bad);rejects([&]{rt::v2::decodeData(bad.bytes.data(),bad.size);});}
    auto bad=d;bad.bytes[32]^=1;rejects([&]{rt::v2::decodeData(bad.bytes.data(),bad.size);});
    rejects([&]{rt::v2::decodeData(nullptr,0);});rejects([&]{rt::v2::encodeData(f,1);});f.bytes.clear();rejects([&]{rt::v2::encodeData(f,0);});f.bytes.resize(2097153);rejects([&]{rt::v2::encodeData(f,0);});
    rt::v2::Nack n;n.moduleId=2;n.frameId=0x12345678;n.indices={7};auto nack=rt::v2::encodeNack(n);require(nack.size==26&&rt::readLe16(nack.bytes.data()+20)==7,"NACK single index erratum");auto nn=rt::v2::decodeNack(nack.bytes.data(),nack.size);require(nn.moduleId==2&&nn.frameId==n.frameId&&nn.indices==n.indices,"NACK roundtrip");rt::v2::validateIndices(nn,8);rejects([&]{rt::v2::validateIndices(nn,7);});
    n.indices.resize(724);for(unsigned i=0;i<724;++i)n.indices[i]=std::uint16_t(i);auto max=rt::v2::encodeNack(n);require(max.size==1472&&rt::v2::decodeNack(max.bytes.data(),max.size).indices==n.indices,"max NACK");n.indices.push_back(724);rejects([&]{rt::v2::encodeNack(n);});n.indices.clear();rejects([&]{rt::v2::encodeNack(n);});
    for(auto off:{7,8,9,16,18}){auto b=nack;b.bytes[off]=0xff;crc(b);rejects([&]{rt::v2::decodeNack(b.bytes.data(),b.size);});}
    rt::v2::Status s;s.sequence=0xffffffff;s.timestampUs=99;for(auto len:{0,1444}){s.payload.assign(len,0x0d);auto p=rt::v2::encodeStatus(s);auto r=rt::v2::decodeStatus(p.bytes.data(),p.size);require(p.size==28u+len&&r.payload==s.payload&&r.sequence==s.sequence&&r.timestampUs==99,"status boundaries");for(auto off:{7,8,9}){auto b=p;b.bytes[off]=0xff;crc(b);rejects([&]{rt::v2::decodeStatus(b.bytes.data(),b.size);});}}s.payload.resize(1445);rejects([&]{rt::v2::encodeStatus(s);});
    require(rt::newerId(0,0xffffffff)&&!rt::newerId(0xffffffff,0)&&!rt::newerId(0x80000000,0),"half range ordering");
    rt::Frame legacy;legacy.logicId=1;legacy.moduleMask=1;legacy.bytes={5,6};auto old=rt::encodeData(legacy,0);rejects([&]{rt::v2::decodeData(old.bytes.data(),old.size);});
    std::cout<<"V2 protocol boundary/erratum/malformed/wrap/legacy checks passed\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
