#include "support/CliOptions.h"
#include <limits>
namespace rt {
    CliOptions parseOptions(int argc,char**argv) {
        CliOptions o;
        bool nackIpExplicit=false;
        auto number=[](const std::string&s,std::uint64_t max) {
            std::size_t used=0;
            if(s.empty()||s[0]=='-')throw std::invalid_argument("invalid unsigned integer");
            auto n=std::stoull(s,&used,0);
            if(used!=s.size()||n>max)throw std::invalid_argument("integer out of range");
            return n;
        };
        for(int i=1;i<argc;++i) {
            std::string a=argv[i];
            auto value=[&]() {
                if(i+1>=argc)throw std::invalid_argument("missing value for "+a);
                return std::string(argv[++i]);
            };
            if(a=="--help"||a=="-h")o.help=true;
            else if(a=="--target")o.config.targetIp=value();
            else if(a=="--port")o.config.targetPort=static_cast<std::uint16_t>(number(value(),65535));
            else if(a=="--bind")o.config.bindIp=value();
            else if(a=="--cmd-port")o.config.commandPort=static_cast<std::uint16_t>(number(value(),65535));
            else if(a=="--nack-ip") {
                o.config.allowedNackIp=value();
                nackIpExplicit=true;
            }
            else if(a=="--nack-port")o.config.allowedNackPort=static_cast<std::uint16_t>(number(value(),65535));
            else if(a=="--frames")o.config.frameLimit=number(value(),1000000000ull);
            else if(a=="--start-id")o.config.initialLogicId=static_cast<std::uint32_t>(number(value(),0xFFFFFFFFull));
            else if(a=="--frame-bytes") {
                auto n=number(value(),MaxFrameBytes);
                if(n<5)throw std::invalid_argument("a one-module frame needs at least 5 bytes");
                o.config.modules[0].dataBytes=static_cast<std::uint32_t>(n-5);
            }
            else if(a=="--file")o.config.modules[0].file=value();
            else if(a=="--link-mbps")o.config.linkMbps=static_cast<unsigned>(number(value(),1000000));
            else if(a=="--allow-oversubscribed")o.config.allowOversubscribed=true;
            else if(a=="--cache-policy") {
                auto v=value();
                if(v!="modulo"&&v!="fifo")throw std::invalid_argument("cache-policy must be modulo or fifo");
                o.config.cachePolicy=v=="fifo"?CachePolicy::FifoErratum:CachePolicy::SpecModulo;
            }
            else if(a=="--drop-first")o.config.faults.dropFirst=parseFragmentIds(value());
            else if(a=="--drop-always")o.config.faults.dropAlways=parseFragmentIds(value());
            else if(a=="--corrupt-first")o.config.faults.corruptFirst=parseFragmentIds(value());
            else if(a=="--duplicate-first")o.config.faults.duplicateFirst=parseFragmentIds(value());
            else if(a=="--reverse")o.config.faults.reverseInitial=true;
            else if(a=="--report")o.report=value();
            else throw std::invalid_argument("unknown option: "+a);
        }
        if(!nackIpExplicit)o.config.allowedNackIp=o.config.targetIp;
        return o;
    }
    std::string cliHelp() {
        return
        "SimTransfer RT sender (DATA/NACK only; never waits for READY)\n"
        "  --target 127.0.0.1 --port 5650 --bind 127.0.0.1 --cmd-port 5500\n"
        "  --nack-ip 127.0.0.1 (or *) --nack-port 0 (0=any source port)\n"
        "  --frames 0 (continuous) --frame-bytes 65536 (INCLUDES 5B module header)\n"
        "  --file module_body.bin --start-id 1 --link-mbps 1000\n"
        "  --allow-oversubscribed (format testing only; not a timing guarantee)\n"
        "  --cache-policy modulo|fifo (fifo is an explicit proposed erratum)\n"
        "  --drop-first 2,4 --drop-always 2 --corrupt-first 4 --duplicate-first 3 --reverse\n"
        "  --report report.json\n";
    }
}
