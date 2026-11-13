#include "application/StreamRunner.h"
#include "core/RealtimeStream.h"
#include "core/SamplingClock.h"
#include <sstream>
#include <iomanip>
namespace rt {
    Snapshot runStream(const StreamConfig& config,IUdpTransport& udp,CommandDispatcher& commands,const SnapshotCallback& publish,const LogCallback& log) {
        udp.open(config);
        // Module files/templates are prepared by the constructor BEFORE the first sampling deadline.
        // Reset origin after bounded preparation through a fresh initial timestamp in first tick below.
        RealtimeStream stream(config,monotonicUs());
        auto nextFeedback=monotonicUs(),nextPublish=nextFeedback;
        std::uint64_t lastSendErrors=0;
        log("UDP bound to "+config.bindIp+":"+std::to_string(udp.localPort())+"; DF enabled; no READY/FIN/SUCCESS messages");
        log("TS source: SIMULATED host monotonic microsecond counter; module bodies: opaque test bytes");
        if(config.cachePolicy==CachePolicy::FifoErratum)log("EXPLICIT ERRATUM MODE: FIFO cache replaces documented logic_id%3 indexing");
        if(config.allowOversubscribed)log("FORMAT-ONLY overload testing allowed; 15ms deadline is NOT assured");
        stream.resetSchedule(monotonicUs());
        while(stream.running()) {
            auto now=monotonicUs();
            if(now>=nextFeedback) {
                IncomingDatagram d;
                for(int i=0;i<32&&udp.receive(d);++i) {
                    if((!config.allowedNackIp.empty()&&config.allowedNackIp!="*"&&d.sourceIp!=config.allowedNackIp)||(config.allowedNackPort&&d.sourcePort!=config.allowedNackPort)) {
                        stream.rejectedSource();
                        continue;
                    }
                    try {
                        Command c;
                        c.kind=Command::Kind::NackFeedback;
                        c.nack=decodeNack(d.bytes.data(),d.bytes.size());
                        if(!commands.post(std::move(c)))stream.queueRejected();
                    }
                    catch(const ProtocolError&) {
                        stream.badNack();
                    }
                }
                // Bounded work prevents a feedback flood from starving initial DATA transmission.
                for(int i=0;i<32;++i) {
                    auto c=commands.take();
                    if(!c)break;
                    if(c->kind==Command::Kind::Stop) {
                        stream.stop();
                        break;
                    }
                    if(c->kind==Command::Kind::NackFeedback)stream.acceptNack(c->nack);
                    else if(c->kind==Command::Kind::SetMask) {
                        try {
                            stream.setModuleMask(c->mask);
                            log("Module mask applied at next sample: "+std::to_string(c->mask));
                        }
                        catch(const std::exception&e) {
                            log(std::string("Rejected mask: ")+e.what());
                        }
                    }
                }
                nextFeedback=monotonicUs()+(stream.initialPending()?100u:1000u);
            }
            now=monotonicUs();
            stream.tick(now,[&](const auto*p,std::size_t n) {
                return udp.send(p,n);
            });
            if(now>=nextPublish) {
                auto snap=stream.snapshot(now);
                snap.localPort=udp.localPort();
                snap.dfEnabled=udp.dfEnabled();
                publish(snap);
                if(snap.stats.sendErrors>lastSendErrors) {
                    log("Local send errors="+std::to_string(snap.stats.sendErrors)+" latest="+udp.lastError());
                    lastSendErrors=snap.stats.sendErrors;
                }
                nextPublish=now+100000;
            }
            // 100us NACK/command polling even during fine-grained pacing; no Qt event loop is blocked.
            waitBrieflyUntil(std::min(stream.nextWakeUs(monotonicUs()),nextFeedback));
        }
        auto result=stream.snapshot(monotonicUs());
        result.localPort=udp.localPort();
        result.dfEnabled=udp.dfEnabled();
        publish(result);
        log(result.detail);
        return result;
    }
    namespace {
        std::string escape(const std::string&s) {
            std::string o;
            for(unsigned char c:s) {
                if(c=='"'||c=='\\') {
                    o+='\\';
                    o+=char(c);
                }
                else if(c=='\n')o+="\\n";
                else if(c>=32)o+=char(c);
            }
            return o;
        }
    }
    std::string snapshotJson(const Snapshot& s) {
        std::ostringstream o;
        o<<"{\n  \"protocol\": \"UDP multi-module document, LE AA55A55A\",\n  \"receiver_delivery\": \"unknown: no ACK/SUCCESS in this protocol\",\n  \"timestamp_source\": \"SIMULATED host monotonic microseconds\",\n";
        o<<"  \"detail\": \""<<escape(s.detail)<<"\",\n  \"running\": "<<(s.running?"true":"false")<<",\n  \"local_port\": "<<s.localPort<<",\n  \"df_enabled\": "<<(s.dfEnabled?"true":"false")<<",\n  \"next_logic_id\": "<<s.nextLogicId<<",\n  \"stats\": {\n";
#define STAT(name) o<<"    \"" #name "\": "<<s.stats.name<<",\n";
        STAT(sampled) STAT(built) STAT(skippedNoModules) STAT(skippedOverrun) STAT(buildRejected)
        STAT(firstAttempts) STAT(datagrams) STAT(bytesSent) STAT(retransmissions) STAT(injectedDrops) STAT(injectedCorrupt)
        STAT(nacks) STAT(invalidNacks) STAT(cacheMisses) STAT(duplicatePending) STAT(queueDrops) STAT(sourceRejected)
        STAT(unsentExpired) STAT(sendErrors) STAT(deadlineMisses) STAT(maxSampleLatenessUs) STAT(maxSendLatenessUs) STAT(lastInitialDurationUs)
#undef STAT
        o<<"    \"elapsedUs\": "<<s.stats.elapsedUs<<"\n  }\n}\n";
        return o.str();
    }
}
