#include "core/Types.h"
#include <sstream>
#include <limits>
namespace rt {
    std::set<std::uint16_t> parseFragmentIds(const std::string& s) {
        std::set<std::uint16_t> out;
        std::istringstream in(s);
        std::string token;
        while(std::getline(in,token,',')) {
            if(token.find_first_not_of(" \t")==std::string::npos)continue;
            std::size_t used=0;
            auto v=std::stoul(token,&used);
            if(token.find_first_not_of(" \t",used)!=std::string::npos||v>1460)throw std::invalid_argument("fragment ID must be 0..1460");
            out.insert(static_cast<std::uint16_t>(v));
        }
        return out;
    }
    std::uint64_t conservativeWireBytes(std::size_t n) {
        // Each frame fragment adds 36 application + 28 IPv4/UDP + 38 Ethernet/preamble/IFG.
        // VLAN/tunnels not included. This is a feasibility estimate, not measured throughput.
        return n+std::uint64_t(fragmentCount(n))*102u;
    }
}
