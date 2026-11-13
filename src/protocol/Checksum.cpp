#include "protocol/Checksum.h"
namespace rt {
    namespace {
        constexpr auto makeTables() {
            std::array<std::array<std::uint32_t,256>,8> t {
            };
            for(std::uint32_t i=0;i<256;++i) {
                auto c=i;
                for(unsigned k=0;k<8;++k)c=(c>>1)^((c&1u)?0xEDB88320u:0u);
                t[0][i]=c;
            }
            for(unsigned k=1;k<8;++k)for(unsigned i=0;i<256;++i)t[k][i]=(t[k-1][i]>>8)^t[0][t[k-1][i]&255u];
            return t;
        }
        constexpr auto table=makeTables();
        std::uint32_t le32(const std::uint8_t*p)noexcept {
            return std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16)|(std::uint32_t(p[3])<<24);
        }
    }
    std::uint32_t crc32(const std::uint8_t*p,std::size_t n)noexcept {
        std::uint32_t c=0xFFFFFFFFu;
        // Slicing-by-8 CRC32/IEEE, NOT SSE4.2 CRC32C. No unaligned native loads.
        while(n>=8) {
            const auto a=c^le32(p),b=le32(p+4);
            c=table[7][a&255u]^table[6][(a>>8)&255u]^table[5][(a>>16)&255u]^table[4][a>>24]
            ^table[3][b&255u]^table[2][(b>>8)&255u]^table[1][(b>>16)&255u]^table[0][b>>24];
            p+=8;
            n-=8;
        }
        while(n--) {
            c=table[0][(c^*p++)&255u]^(c>>8);
        }
        return c^0xFFFFFFFFu;
    }
}
