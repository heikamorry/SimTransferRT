#include "protocol/PacketCodec.h"
#include "protocol/Checksum.h"
#include "core/LogicalFrameBuilder.h"
#include "core/RealtimeStream.h"
#include "application/CommandDispatcher.h"
#include "application/StreamRunner.h"
#include <iostream>
#include <functional>
#include <fstream>
#include <thread>
using namespace rt;
namespace {
    void require(bool b,const char*m="assertion failed") {
        if(!b)throw std::runtime_error(m);
    }
    template<class F>void throws(F f) {
        bool yes=false;
        try {
            f();
        }
        catch(const std::exception&) {
            yes=true;
        }
        require(yes,"expected exception");
    }
    Frame frame(std::size_t n,std::uint32_t id=1) {
        Frame f;
        f.logicId=id;
        f.timestampUs=0x0102030405060708ull;
        f.moduleMask=1;
        f.bytes.resize(n);
        for(std::size_t i=0;i<n;++i)f.bytes[i]=std::uint8_t(i);
        return f;
    }
    StreamConfig small() {
        StreamConfig c;
        c.modules[0].dataBytes=5000;
        return c;
    }
    std::vector<DataPacket> advance(RealtimeStream&s,std::uint64_t from,std::uint64_t to) {
        std::vector<DataPacket> v;
        for(auto t=from;t<=to;t+=10)s.tick(t,[&](const auto*p,std::size_t n) {
            v.push_back(decodeData(p,n));return true;
        });
        return v;
    }
    void recalc(Datagram&d) {
        writeLe32(d.bytes.data()+d.size-4,crc32(d.bytes.data(),d.size-4));
    }
}
int main(int argc,char**argv) {
    using Test=std::pair<const char*,std::function<void()>>;
    const std::vector<Test> tests= {
        {
            "feedback flood remains bounded, new samples and stop progress",[] {
                struct Flood final: IUdpTransport {
                    Datagram packet;
                    std::uint64_t sent=0;
                    Flood(){Nack n;n.logicId=1;n.ranges.assign(363,Range{0,4});packet=encodeNack(n);}
                    void open(const StreamConfig&)override{}
                    bool send(const std::uint8_t*,std::size_t)override{++sent;return true;}
                    bool receive(IncomingDatagram& d)override{d.bytes.assign(packet.bytes.begin(),packet.bytes.begin()+packet.size);d.sourceIp="127.0.0.1";return true;}
                    std::uint16_t localPort()const override{return 1;}
                    bool dfEnabled()const override{return true;}
                    std::string lastError()const override{return {};}
                } flood;
                auto c=small();c.frameLimit=3;CommandDispatcher commands;
                const auto a=runStream(c,flood,commands,[](const auto&){},[](const auto&){});
                require(a.stats.sampled==3&&flood.sent>=12&&a.stats.elapsedUs<2000000);
                require(a.stats.nackParsed>0&&a.stats.retryQueueHighWater<=12);
                c.frameLimit=0;bool posted=false;
                const auto b=runStream(c,flood,commands,[&](const auto& snap){
                    if(!posted&&snap.stats.sampled){Command stop;commands.post(stop);posted=true;}
                },[](const auto&){});
                require(posted&&!b.running&&b.stats.elapsedUs<2000000);
            }
        },
        {
            "planned send lateness includes encode time before socket call",[] {
                auto c=small();c.modules[0].dataBytes=10;unsigned reads=0;
                RealtimeStream s(c,0,[&]{return ++reads<=4?0ull:100ull;});
                s.tick(0,[](const auto*,auto){return true;});
                require(s.snapshot(100).stats.maxPlannedSendLatenessUs==100);
            }
        },
        {
            "actual send completion uses injected clock; TS and legacy span unchanged",[] {
                auto c=small();c.modules[0].dataBytes=10;
                std::uint64_t now=100;
                RealtimeStream s(c,100,[&]{return now;});
                s.tick(now,[&](const auto* p,auto n){require(decodeData(p,n).timestampUs==100);now+=16000;return true;});
                const auto a=s.snapshot(now);
                require(a.stats.lastInitialDurationUs==0&&a.stats.lastSampleToInitialEndUs==16000);
                require(a.stats.deadlineMisses==0&&a.stats.actualInitialDeadlineMisses==1);
                require(a.initialDuration.count==1&&a.initialDuration.maxUs==16000&&a.initialDuration.overLimit==1);
                require(a.initialCallSpan.maxUs==16000&&a.stats.sendProfile.totalUs==16000);
            }
        },
        {
            "cancellation and replacement conserve planned first fragments",[] {
                RealtimeStream s(small(),0);
                auto send=[](const auto*,auto){return true;};
                s.tick(0,send);s.tick(20000,send);s.stop();s.stop();
                const auto a=s.snapshot(20000).stats;
                require(a.initialFragmentsPlanned==8&&a.firstAttempts==2&&a.unsentExpired==3&&a.unsentAtStop==3);
                require(a.initialIncompleteFrames==2&&a.initialFinishedFrames==0&&a.initialPendingFragments==0);
                require(a.initialFragmentsPlanned==a.firstAttempts+a.unsentExpired+a.unsentAtStop+a.initialPendingFragments);
            }
        },
        {
            "failed or injected first sends excluded from submitted distribution",[] {
                auto c=small();c.modules[0].dataBytes=10;
                RealtimeStream s(c,0);s.tick(0,[](const auto*,auto){return false;});
                require(s.snapshot(0).stats.initialFinishedFrames==1&&s.snapshot(0).initialDuration.count==0);
                c.faults.dropFirst.insert(0);RealtimeStream dropped(c,0);
                dropped.tick(0,[](const auto*,auto){return true;});
                require(dropped.snapshot(0).stats.initialFinishedFrames==1&&dropped.snapshot(0).initialDuration.count==0);
            }
        },
        {
            "sampling interval deviation is independent of absolute lateness",[] {
                RealtimeStream s(small(),0);auto send=[](const auto*,auto){return true;};
                s.tick(1000,send);s.tick(21000,send);s.tick(41500,send);
                const auto a=s.snapshot(41500);
                require(a.samplingIntervals.count==2&&a.samplingIntervals.maxUs==20500);
                require(a.samplingDeviation.maxUs==500&&a.stats.maxSampleLatenessUs==1500);
                require(a.stats.samplingSpanUs==40500&&a.stats.samplingActiveUs==60500);
            }
        },
        {
            "planned versus adjusted pacing lateness remains visible",[] {
                PacketPacer p;p.begin(0,731);p.emitted(1000);
                require(p.plannedDue(1)==20&&p.due(1)==1010);
                RealtimeStream s(small(),0);auto send=[](const auto*,auto){return true;};
                s.tick(0,send);s.tick(1000,send);s.tick(1010,send);
                require(s.snapshot(1010).stats.maxPlannedSendLatenessUs>=970);
            }
        },
        {
            "overlapping NACK worst case bounded and later repeat still eligible",[] {
                RealtimeStream s(small(),0);advance(s,0,100);
                Nack n;n.logicId=1;n.ranges.assign(363,Range{0,4});
                s.acceptNack(n);
                auto a=s.snapshot(100).stats;
                require(a.requestedFragments==1452&&a.duplicatePending==1448&&a.retryQueueHighWater==4);
                advance(s,110,1000);s.acceptNack(n);advance(s,1010,2000);
                require(s.snapshot(2000).stats.retransmissions==8);
            }
        },
        {
            "cache miss diagnostics bounded and wrap-safe",[] {
                auto c=small();c.initialLogicId=0xFFFFFFFFu;RealtimeStream s(c,0);advance(s,0,21000);
                for(unsigned i=0;i<40;++i)s.acceptNack(Nack{0xFFFFFFFEu,{{0,1}}});
                const auto a=s.snapshot(21000);
                require(a.cacheMissEvents.size()==32&&a.cacheMissEventsDropped==8);
                require(a.cacheMissEvents.back().distance==2&&a.cacheMissEvents.back().latest==0);
            }
        },
        {
            "clock reversal detected both between and during ticks",[] {
                RealtimeStream s(small(),0);s.tick(100,[](const auto*,auto){return true;});
                throws([&]{s.tick(99,[](const auto*,auto){return true;});});
                RealtimeStream bad(small(),100,[]{return 99;});
                throws([&]{bad.tick(100,[](const auto*,auto){return true;});});
            }
        },
        {
            "CRC IEEE known vector",[] {
                std::string s="123456789";
                require(crc32(reinterpret_cast<const std::uint8_t*>(s.data()),s.size())==0xCBF43926u);
            }
        },
        {
            "CRC fast path matches reference",[] {
                Bytes b(4096);
                for(std::size_t i=0;i<b.size();++i)b[i]=std::uint8_t(i*73u+11u);
                for(std::size_t n=0;n<=b.size();++n) {
                    std::uint32_t c=0xFFFFFFFFu;
                    for(std::size_t j=0;j<n;++j) {
                        c^=b[j];
                        for(int k=0;k<8;++k)c=(c>>1)^((c&1u)?0xEDB88320u:0u);
                    }
                    require(crc32(b.data(),n)==(c^0xFFFFFFFFu));
                }
            }
        },
        {
            "CRC empty",[] {
                require(crc32(nullptr,0)==0);
            }
        },
        {
            "DATA LE offsets",[] {
                auto d=encodeData(frame(1436,0x12345678),0);
                require(d.size==1472);
                require(d.bytes[0]==0x5A&&d.bytes[1]==0xA5&&d.bytes[2]==0x55&&d.bytes[3]==0xAA);
                require(d.bytes[4]==0xC0&&d.bytes[5]==5);
                require(d.bytes[8]==0x78&&d.bytes[20]==8&&d.bytes[27]==1);
            }
        },
        {
            "DATA roundtrip",[] {
                auto f=frame(5000);
                auto d=encodeData(f,2);
                auto p=decodeData(d.bytes.data(),d.size);
                require(p.fragmentIndex==2&&p.fragmentCount==4&&p.timestampUs==f.timestampUs&&p.payload.front()==f.bytes[2872]);
            }
        },
        {
            "1MiB geometry",[] {
                auto f=frame(1048576);
                require(f.fragmentCount()==731);
                require(encodeData(f,730).size==332);
            }
        },
        {
            "2MiB geometry",[] {
                auto f=frame(2097152);
                require(f.fragmentCount()==1461);
                require(encodeData(f,1460).size==628);
            }
        },
        {
            "size bounds",[] {
                throws([] {
                    fragmentCount(0);
                });
                throws([] {
                    fragmentCount(MaxFrameBytes+1);
                });
                require(fragmentCount(1436)==1&&fragmentCount(1437)==2);
            }
        },
        {
            "fragment bounds",[] {
                throws([] {
                    encodeData(frame(10),1);
                });
            }
        },
        {
            "payload CRC corruption",[] {
                auto d=encodeData(frame(10),0);
                d.bytes[32]^=1;
                throws([&] {
                    decodeData(d.bytes.data(),d.size);
                });
            }
        },
        {
            "header CRC corruption",[] {
                auto d=encodeData(frame(10),0);
                d.bytes[20]^=1;
                throws([&] {
                    decodeData(d.bytes.data(),d.size);
                });
            }
        },
        {
            "wrong length and magic",[] {
                auto d=encodeData(frame(10),0);
                d.bytes[4]^=1;
                recalc(d);
                throws([&] {
                    decodeData(d.bytes.data(),d.size);
                });
                d.bytes[0]^=1;
                throws([&] {
                    decodeData(d.bytes.data(),d.size);
                });
            }
        },
        {
            "unknown flags",[] {
                auto d=encodeData(frame(10),0);
                d.bytes[7]=2;
                recalc(d);
                throws([&] {
                    decodeData(d.bytes.data(),d.size);
                });
            }
        },
        {
            "retry preserves fields recomputes CRC",[] {
                auto f=frame(5000);
                auto a=encodeData(f,2),b=encodeData(f,2,true);
                require(b.bytes[7]==1&&a.bytes[7]==0);
                require(std::equal(a.bytes.begin()+8,a.bytes.begin()+static_cast<std::ptrdiff_t>(a.size-4),b.bytes.begin()+8));
                require(readLe32(a.bytes.data()+a.size-4)!=readLe32(b.bytes.data()+b.size-4));
                require(decodeData(b.bytes.data(),b.size).retransmission);
            }
        },
        {
            "NACK roundtrip",[] {
                Nack n {
                    42, {
                        {
                            2,3
                        }, {
                            9,2
                        }
                    }
                };
                auto d=encodeNack(n);
                auto p=decodeNack(d.bytes.data(),d.size);
                require(d.size==28&&p.logicId==42&&p.ranges[1].start==9&&p.ranges[0].count==3);
            }
        },
        {
            "NACK max ranges",[] {
                Nack n;
                for(unsigned i=0;i<363;++i)n.ranges.push_back( {
                    std::uint16_t(2*i),1
                });
                auto d=encodeNack(n);
                require(d.size==1472);
                require(decodeNack(d.bytes.data(),d.size).ranges.size()==363);
                n.ranges.push_back( {
                    800,1
                });
                throws([&] {
                    encodeNack(n);
                });
            }
        },
        {
            "NACK invalid reserved and CRC",[] {
                auto d=encodeNack( {
                    1, {
                        {
                            1,1
                        }
                    }
                });
                d.bytes[14]=1;
                recalc(d);
                throws([&] {
                    decodeNack(d.bytes.data(),d.size);
                });
                d.bytes[14]=0;
                throws([&] {
                    decodeNack(d.bytes.data(),d.size);
                });
            }
        },
        {
            "NACK range overflow",[] {
                throws([] {
                    encodeNack( {
                        1, {
                            {
                                65534,2
                            }
                        }
                    });
                });
                throws([] {
                    validateRanges( {
                        1, {
                            {
                                2,4
                            }
                        }
                    },5);
                });
                throws([] {
                    encodeNack( {
                        1, {
                            {
                                2,0
                            }
                        }
                    });
                });
            }
        },
        {
            "merge missing IDs",[] {
                auto r=mergeIds( {
                    10,2,3,4,9,3
                });
                require(r.size()==2&&r[0].start==2&&r[0].count==3&&r[1].count==2);
            }
        },
        {
            "module order and mask",[] {
                auto f=buildFrame(1,99, {
                    {
                        2,Bytes {
                            4,5
                        }
                    }, {
                        0,Bytes {
                            1,2,3
                        }
                    }
                });
                require(f.moduleMask==5&&f.bytes.size()==15);
                auto m=decodeModules(f);
                require(m.size()==2&&m[0].id==0&&m[1].id==2);
            }
        },
        {
            "module duplicates rejected",[] {
                throws([] {
                    buildFrame(1,1, {
                        {
                            0, {
                            }
                        }, {
                            0, {
                            }
                        }
                    });
                });
                throws([] {
                    buildFrame(1,1, {
                        {
                            32, {
                            }
                        }
                    });
                });
            }
        },
        {
            "modules cross fragment boundary and >64K",[] {
                auto f=buildFrame(1,1, {
                    {
                        0,Bytes(1430,1)
                    }, {
                        2,Bytes(70000,2)
                    }
                });
                Bytes reconstructed;
                for(unsigned i=0;i<f.fragmentCount();++i) {
                    auto d=encodeData(f,static_cast<std::uint16_t>(i));
                    auto p=decodeData(d.bytes.data(),d.size);
                    reconstructed.insert(reconstructed.end(),p.payload.begin(),p.payload.end());
                }
                require(reconstructed==f.bytes);
                require(decodeModules(f)[1].data.size()==70000);
            }
        },
        {
            "oversize sample skipped not stream failed",[] {
                auto c=small();
                c.modules= {
                    {
                        0,true,1500000,0,""
                    }, {
                        2,true,1500000,0,""
                    }
                };
                RealtimeStream s(c,0);
                advance(s,0,40000);
                require(s.snapshot(40000).stats.buildRejected==3&&s.snapshot(40000).stats.built==0&&s.running());
            }
        },
        {
            "module bounds and trailing bytes",[] {
                auto f=buildFrame(1,1, {
                    {
                        0, {
                            1
                        }
                    }
                });
                f.bytes.push_back(0);
                throws([&] {
                    decodeModules(f);
                });
                throws([] {
                    buildFrame(1,1, {
                        {
                            0,Bytes(MaxFrameBytes,0)
                        }
                    });
                });
            }
        },
        {
            "module hotplug snapshot immutable",[] {
                ModuleDataSource s( {
                    {
                        0,true,10,0,""
                    }, {
                        2,true,12,0,""
                    }
                });
                auto a=s.sample(1,100,5),b=s.sample(2,200,4);
                require(a->moduleMask==5&&b->moduleMask==4&&a->bytes.size()==32);
                require(!s.sample(3,300,0));
            }
        },
        {
            "three-slot cache exact ID",[] {
                FrameCache c;
                c.put(frame(5,1));
                c.put(frame(5,2));
                c.put(frame(5,3));
                require(c.find(1)&&c.find(2)&&c.find(3));
                c.put(frame(5,4));
                require(!c.find(1)&&c.find(4));
            }
        },
        {
            "document modulo wrap caveat explicit",[] {
                FrameCache c;
                c.put(frame(5,0xFFFFFFFEu));
                c.put(frame(5,0xFFFFFFFFu));
                c.put(frame(5,0));
                require(!c.find(0xFFFFFFFFu)&&c.find(0));
            }
        },
        {
            "opt-in FIFO preserves three across wrap",[] {
                FrameCache c(CachePolicy::FifoErratum);
                c.put(frame(5,0xFFFFFFFEu));
                c.put(frame(5,0xFFFFFFFFu));
                c.put(frame(5,0));
                require(c.find(0xFFFFFFFEu)&&c.find(0xFFFFFFFFu)&&c.find(0));
            }
        },
        {
            "modular ID comparison",[] {
                require(newerId(0,0xFFFFFFFFu)&&!newerId(0xFFFFFFFFu,0)&&!newerId(1,1));
            }
        },
        {
            "no READY needed",[] {
                RealtimeStream s(small(),0);
                auto v=advance(s,0,15000);
                require(v.size()==4&&s.snapshot(15000).stats.built==1);
            }
        },
        {
            "NACK during first send",[] {
                RealtimeStream s(small(),0);
                advance(s,0,1000);
                s.acceptNack( {
                    1, {
                        {
                            0,1
                        }
                    }
                });
                auto v=advance(s,1010,2000);
                require(!v.empty()&&v[0].retransmission);
            }
        },
        {
            "same round multiple NACK and pending dedup",[] {
                RealtimeStream s(small(),0);
                advance(s,0,15000);
                s.acceptNack( {
                    1, {
                        {
                            0,1
                        }
                    }
                });
                s.acceptNack( {
                    1, {
                        {
                            1,1
                        }
                    }
                });
                s.acceptNack( {
                    1, {
                        {
                            1,1
                        }
                    }
                });
                auto v=advance(s,15010,16000);
                require(v.size()==2&&s.snapshot(16000).stats.duplicatePending==1);
            }
        },
        {
            "later repeated requests not permanently suppressed",[] {
                RealtimeStream s(small(),0);
                advance(s,0,12000);
                for(int i=0;i<4;++i) {
                    s.acceptNack( {
                        1, {
                            {
                                0,1
                            }
                        }
                    });
                    advance(s,12010+i*500,12500+i*500);
                }
                require(s.snapshot(14000).stats.retransmissions==4&&s.running());
            }
        },
        {
            "atomic NACK validation",[] {
                RealtimeStream s(small(),0);
                advance(s,0,12000);
                s.acceptNack( {
                    1, {
                        {
                            0,1
                        }, {
                            3,2
                        }
                    }
                });
                auto v=advance(s,12010,14000);
                require(v.empty()&&s.snapshot(14000).stats.invalidNacks==1);
            }
        },
        {
            "expired cache NACK ignored",[] {
                RealtimeStream s(small(),0);
                advance(s,0,65000);
                s.acceptNack( {
                    1, {
                        {
                            0,1
                        }
                    }
                });
                require(s.snapshot(65000).stats.cacheMisses==1&&s.running());
            }
        },
        {
            "persistent loss does not stop stream",[] {
                auto c=small();
                c.faults.dropAlways= {
                    2
                };
                RealtimeStream s(c,0);
                advance(s,0,90000);
                require(s.snapshot(90000).stats.built==5&&s.running()&&s.snapshot(90000).stats.injectedDrops>=4);
            }
        },
        {
            "hotplug applies only next sampling frame",[] {
                auto c=small();
                c.modules.push_back( {
                    2,false,10,0,""
                });
                RealtimeStream s(c,0);
                advance(s,0,1000);
                s.setModuleMask(5);
                advance(s,1010,21000);
                require(s.cache().find(1)->moduleMask==1&&s.cache().find(2)->moduleMask==5);
            }
        },
        {
            "all-off skip explicit policy",[] {
                auto c=small();
                c.modules[0].online=false;
                RealtimeStream s(c,0);
                advance(s,0,40000);
                require(s.snapshot(40000).stats.built==0&&s.snapshot(40000).stats.skippedNoModules==3);
            }
        },
        {
            "sampling late skips burst catchup",[] {
                RealtimeStream s(small(),0);
                s.tick(65000,[](const auto*,auto) {
                    return true;
                });
                require(s.snapshot(65000).stats.built==1&&s.snapshot(65000).stats.skippedOverrun==3);
            }
        },
        {
            "pacer no burst flush",[] {
                PacketPacer p;
                p.begin(0,731);
                require(p.due(0)==0);
                p.emitted(1000);
                require(p.due(1)>=1010);
            }
        },
        {
            "2MiB@1Gb15ms rejected unless explicit lab override",[] {
                auto c=small();
                c.modules[0].dataBytes=MaxFrameBytes-5;
                throws([&] {
                    RealtimeStream s(c,0);
                });
                c.allowOversubscribed=true;
                RealtimeStream s(c,0);
                require(s.running());
            }
        },
        {
            "frame limit drains without FIN",[] {
                auto c=small();
                c.frameLimit=2;
                RealtimeStream s(c,0);
                advance(s,0,81000);
                auto snap=s.snapshot(81000);
                require(!s.running()&&snap.stats.sampled==2);
            }
        },
        {
            "dispatcher bounded and stop priority",[] {
                CommandDispatcher d;
                for(int i=0;i<512;++i) {
                    Command c;
                    c.kind=Command::Kind::NackFeedback;
                    require(d.post(c));
                }
                Command c;
                c.kind=Command::Kind::NackFeedback;
                require(!d.post(c));
                c.kind=Command::Kind::Stop;
                require(d.post(c));
                const auto stop=d.take();
                require(stop->kind==Command::Kind::Stop&&stop->queuedUs>0);
            }
        },
        {
            "dispatcher coalesces module mask",[] {
                CommandDispatcher d;
                Command c;
                c.kind=Command::Kind::SetMask;
                c.mask=1;
                d.post(c);
                c.mask=5;
                d.post(c);
                require(d.size()==1&&d.take()->mask==5);
            }
        },
        {
            "stop local no control packet",[] {
                RealtimeStream s(small(),0);
                s.stop();
                int sent=0;
                s.tick(0,[&](const auto*,auto) {
                    ++sent;return true;
                });
                require(sent==0&&!s.running());
            }
        }
    };
    int failed=0;
    for(const auto&t:tests) {
        try {
            t.second();
            std::cout<<"PASS "<<t.first<<'\n';
        }
        catch(const std::exception&e) {
            ++failed;
            std::cerr<<"FAIL "<<t.first<<": "<<e.what()<<'\n';
        }
    }
    std::cout<<tests.size()-failed<<"/"<<tests.size()<<" passed\n";
    if(argc==3&&std::string(argv[1])=="--vector") {
        auto f=buildFrame(0x12345678,0x0102030405060708ull, {
            {
                0, {
                    1,2,3
                }
            }, {
                2, {
                    9,8
                }
            }
        });
        auto d=encodeData(f,0);
        std::ofstream out(argv[2],std::ios::binary);
        out.write(reinterpret_cast<const char*>(d.bytes.data()),static_cast<std::streamsize>(d.size));
    }
    return failed?1:0;
}
