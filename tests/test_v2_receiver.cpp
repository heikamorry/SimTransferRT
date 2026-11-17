#include "v2/Receiver.h"
#include "protocol/Checksum.h"
#include <iostream>
#include <stdexcept>
using namespace rt::v2;
namespace {
void check(bool ok,const char*why){if(!ok)throw std::runtime_error(why);}
Frame make(std::uint8_t module,std::uint32_t id,std::size_t bytes,std::uint64_t ts=10){Frame f;f.moduleId=module;f.frameId=id;f.timestampUs=ts;f.bytes.resize(bytes,42);return f;}
void deliver(Receiver&r,const Frame&f,std::uint16_t i,std::uint64_t now,bool retry=false){auto p=encodeData(f,i,retry);r.receive(p.bytes.data(),p.size,now);}
Nack unpack(const rt::Datagram&p){return decodeNack(p.bytes.data(),p.size);}
}
int main(){try{
    std::uint64_t now=0;
    Receiver r(false,[&]{return now;});
    r.receive(nullptr,0,0);r.receive(nullptr,1472,0);
    check(r.snapshot(0).invalidPackets==2,"public receive rejects null input without dereference");r.reset();
    auto ndi=make(1,1,1437),ep=make(2,1,1);
    deliver(r,ndi,0,100);deliver(r,ep,0,101);
    check(r.snapshot(101).modules[1].consumed==1,"EP independent consumption");
    check(r.poll(2099).empty(),"tail timer early");
    auto packets=r.poll(2100);check(packets.size()==1,"tail NACK missing");
    auto nack=unpack(packets[0]);check(nack.moduleId==1&&nack.frameId==1&&nack.indices==std::vector<std::uint16_t>{1},"module keyed tail");
    deliver(r,ndi,1,2200,true);check(r.snapshot(2200).modules[0].completed==1,"recovery");
    check(r.poll(12100).empty(),"complete cancels retry");

    r.reset();ndi=make(1,7,3000);deliver(r,ndi,1,100);
    packets=r.poll(100);check(packets.size()==1&&unpack(packets[0]).indices==std::vector<std::uint16_t>{0},"gap immediate");
    check(r.poll(10099).empty(),"second round early");check(r.poll(10100).size()==1,"second round");
    check(r.poll(20099).empty(),"third round early");check(r.poll(20100).size()==1,"third round");
    r.poll(30099);check(r.snapshot(30099).modules[0].activeFrames==1,"premature expiry");
    r.poll(30100);check(r.snapshot(30100).modules[0].expired==1,"third round expiry");
    check(r.snapshot(30100).modules[0].maxRetryIntervalUs==10000,"retry interval");

    r.reset();ndi=make(1,1,1437);deliver(r,ndi,0,100);deliver(r,make(2,2,1),0,101);
    check(r.poll(101).empty(),"foreign module must not trigger tail");deliver(r,make(1,2,1,20),0,102);
    check(r.poll(102).size()==1,"same module next frame tail");deliver(r,ndi,1,103,true);
    check(r.snapshot(103).latest[0].frame->frameId==2,"old recovered frame must not roll display back");
    check(r.snapshot(103).modules[0].completed==2&&r.snapshot(103).modules[0].consumed==1,"delivery versus consumption");
    auto ledger=r.snapshot(103).modules[0].ledger;
    check(ledger.count==2&&ledger.idSum==3&&ledger.stampSum==30&&ledger.crcSum==std::uint64_t(rt::crc32(ndi.bytes))+rt::crc32(make(1,2,1,20).bytes),"completion ledger includes late recovered frame");

    r.reset();auto f=make(2,1,1437);deliver(r,f,0,0);deliver(r,f,0,1);
    check(r.snapshot(1).modules[1].duplicates==1,"duplicate");f.timestampUs++;deliver(r,f,1,2);
    check(r.snapshot(2).modules[1].conflicts==1&&r.snapshot(2).modules[1].activeFrames==0,"metadata conflict");

    r.reset();for(auto id:{0xfffffffeu,0xffffffffu,0u,1u})deliver(r,make(2,id,1),0,0);
    check(r.snapshot(0).modules[1].completed==4&&r.snapshot(0).modules[1].wholeMissing==0,"frame wrap");
    for(auto id:{0xffffffffu,0u,0xffffffffu,0u}){Status s;s.sequence=id;auto p=encodeStatus(s);r.receive(p.bytes.data(),p.size,0);}
    check(r.snapshot(0).statusApplied==2&&r.snapshot(0).statusOld==2,"status wrap latest only");
    for(unsigned i=2;i<20;++i)deliver(r,make(2,i,1437),0,0);
    check(r.snapshot(0).modules[1].activeFrames==4&&r.snapshot(0).modules[1].highWaterFrames<=4,"bounded assembly");

    r.reset();auto large=make(2,1,2097152);deliver(r,large,1460,0);packets=r.poll(0);
    std::size_t ids=0;for(const auto&p:packets)ids+=unpack(p).indices.size();
    check(packets.size()==3&&ids==1460&&r.snapshot(0).modules[1].nackRounds==1,"split NACK counts one round");

    Receiver bx(true,[&]{return now;});Frame b=make(1,1,1);b.bytes=rt::aurora::generateSample(3,8);now=150;deliver(bx,b,0,100);
    check(bx.snapshot(150).ndi.tools.size()==3&&bx.snapshot(150).latest[0].hostCompletedUs==150,"BX consumer completion clock");
    b.frameId=2;b.bytes.back()^=1;now=250;deliver(bx,b,0,200);
    check(bx.snapshot(250).modules[0].completed==2&&bx.snapshot(250).modules[0].businessErrors==1,"CRC16 distinct from UDP CRC32");
    check(bx.snapshot(250).modules[0].ledger.count==2,"UDP completion and business consumption ledgers remain distinct");
    check(bx.snapshot(20151).latest[0].stale,"stale data indicated");
    r.reset();deliver(r,make(2,1,1,80000),0,100);deliver(r,make(1,1,1,90000),0,101);deliver(r,make(2,2,1,105000),0,102);
    check(r.snapshot(102).associatedEpId==1&&r.snapshot(102).nearestTimestampDeltaUs==10000,"nearest neighbor uses history, not only latest");
    std::cout<<"V2 receiver: isolation, reassembly, retry timers, wrap, bounded state, BX consumption passed\n";
    return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
