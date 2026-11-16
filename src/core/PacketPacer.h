#pragma once
#include "protocol/ProtocolTypes.h"
#include <algorithm>
namespace rt {
    // Absolute planned slots, with a minimum gap after an actually emitted initial packet.
    // Late callbacks recover phase with a bounded half-slot minimum gap, never a whole-frame burst.
    class PacketPacer {
        public:
        void begin(std::uint64_t start,std::uint16_t count) {
            start_=start;
            count_=count;
            last_=0;
            hasLast_=false;
        }
        std::uint64_t due(std::uint16_t ordinal)const {
            const auto gap=std::max<std::uint64_t>(1,std::min<std::uint64_t>(20,InitialWindowUs/count_));
            const auto planned=plannedDue(ordinal);
            return hasLast_?std::max(planned,last_+std::max<std::uint64_t>(1,gap/2)):planned;
        }
        std::uint64_t plannedDue(std::uint16_t ordinal)const {
            const auto gap=std::max<std::uint64_t>(1,std::min<std::uint64_t>(20,InitialWindowUs/count_));
            return start_+std::uint64_t(ordinal)*gap;
        }
        void emitted(std::uint64_t now) {
            last_=now;
            hasLast_=true;
        }
        private: std::uint64_t start_=0,last_=0;
        std::uint16_t count_=1;
        bool hasLast_=false;
    };
}
