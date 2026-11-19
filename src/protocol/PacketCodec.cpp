#include "protocol/PacketCodec.h"
#include "protocol/Checksum.h"
#include <algorithm>
#include <cstring>
namespace rt {
    void writeLe16(std::uint8_t* p,std::uint16_t v) noexcept {
        p[0]=std::uint8_t(v);
        p[1]=std::uint8_t(v>>8);
    }
    void writeLe32(std::uint8_t* p,std::uint32_t v) noexcept {
        for(int i=0;i<4;++i)p[i]=std::uint8_t(v>>(8*i));
    }
    std::uint16_t readLe16(const std::uint8_t* p) noexcept {
        return std::uint16_t(p[0]|std::uint16_t(p[1])<<8);
    }
    std::uint32_t readLe32(const std::uint8_t* p) noexcept {
        std::uint32_t v=0;
        for(int i=0;i<4;++i)v|=std::uint32_t(p[i])<<(8*i);
        return v;
    }
    namespace {
        void write64(std::uint8_t* p,std::uint64_t v) {
            for(int i=0;i<8;++i)p[i]=std::uint8_t(v>>(8*i));
        }
        std::uint64_t read64(const std::uint8_t* p) {
            std::uint64_t v=0;
            for(int i=0;i<8;++i)v|=std::uint64_t(p[i])<<(8*i);
            return v;
        }
        void header(Datagram& d,std::uint8_t type,std::uint8_t flags) {
            writeLe32(d.bytes.data(),Magic);
            writeLe16(d.bytes.data()+4,static_cast<std::uint16_t>(d.size));
            d.bytes[6]=type;
            d.bytes[7]=flags;
        }
        void finish(Datagram& d) {
            writeLe32(d.bytes.data()+d.size-4,crc32(d.bytes.data(),d.size-4));
        }
        void check(const std::uint8_t* p,std::size_t n,std::uint8_t type,std::size_t minimum) {
            if(!p || n<minimum || n>MaxDatagram)throw ProtocolError("invalid datagram size");
            if(readLe32(p)!=Magic || readLe16(p+4)!=n || p[6]!=type)throw ProtocolError("magic/length/type mismatch");
            if(crc32(p,n-4)!=readLe32(p+n-4))throw ProtocolError("CRC32 mismatch");
        }
    }
    Datagram encodeData(const Frame& f,std::uint16_t index,bool retry) {
        auto count=f.fragmentCount();
        if(index>=count)throw ProtocolError("frag_idx out of range");
        const auto offset=std::size_t(index)*PayloadStep;
        const auto len=std::min(PayloadStep,f.bytes.size()-offset);
        Datagram d;
        d.size=Overhead+len;
        header(d,DataType,retry?1:0);
        auto* p=d.bytes.data();
        writeLe32(p+8,f.logicId);
        writeLe32(p+12,static_cast<std::uint32_t>(f.bytes.size()));
        writeLe16(p+16,index);
        writeLe16(p+18,count);
        write64(p+20,f.timestampUs);
        writeLe32(p+28,f.moduleMask);
        std::memcpy(p+32,f.bytes.data()+offset,len);
        finish(d);
        return d;
    }
    DataPacket decodeData(const std::uint8_t* p,std::size_t n) {
        check(p,n,DataType,Overhead);
        if(p[7]&0xFEu)throw ProtocolError("unknown DATA flags");
        DataPacket d;
        d.logicId=readLe32(p+8);
        d.frameLength=readLe32(p+12);
        d.fragmentIndex=readLe16(p+16);
        d.fragmentCount=readLe16(p+18);
        d.timestampUs=read64(p+20);
        d.moduleMask=readLe32(p+28);
        d.retransmission=(p[7]&1)!=0;
        if(d.fragmentCount!=fragmentCount(d.frameLength) || d.fragmentIndex>=d.fragmentCount)throw ProtocolError("invalid fragment geometry");
        const auto len=std::min(PayloadStep,std::size_t(d.frameLength)-std::size_t(d.fragmentIndex)*PayloadStep);
        if(n!=Overhead+len)throw ProtocolError("payload length mismatch");
        d.payload.assign(p+HeaderSize,p+HeaderSize+len);
        return d;
    }
    Datagram encodeNack(const Nack& n) {
        if(n.ranges.empty()||n.ranges.size()>MaxRanges)throw ProtocolError("invalid range count");
        Datagram d;
        d.size=NackFixedSize+4*n.ranges.size();
        header(d,NackType,0);
        writeLe32(d.bytes.data()+8,n.logicId);
        writeLe16(d.bytes.data()+12,static_cast<std::uint16_t>(n.ranges.size()));
        for(std::size_t i=0;i<n.ranges.size();++i) {
            if(n.ranges[i].count==0 || std::uint32_t(n.ranges[i].start)+n.ranges[i].count>65535u)throw ProtocolError("invalid range");
            writeLe16(d.bytes.data()+16+4*i,n.ranges[i].start);
            writeLe16(d.bytes.data()+18+4*i,n.ranges[i].count);
        }
        finish(d);
        return d;
    }
    Nack decodeNack(const std::uint8_t* p,std::size_t size) {
        check(p,size,NackType,NackFixedSize+4);
        const auto count=readLe16(p+12);
        if(p[7]!=0||readLe16(p+14)!=0||count<1||count>MaxRanges||size!=NackFixedSize+4u*count)throw ProtocolError("invalid NACK fields");
        Nack n;
        n.logicId=readLe32(p+8);
        n.ranges.reserve(count);
        for(unsigned i=0;i<count;++i) {
            Range r {
                readLe16(p+16+4*i),readLe16(p+18+4*i)
            };
            if(!r.count||std::uint32_t(r.start)+r.count>65535u)throw ProtocolError("invalid NACK range");
            n.ranges.push_back(r);
        }
        return n;
    }
    void validateRanges(const Nack& n,std::uint16_t count) {
        if(n.ranges.empty()||n.ranges.size()>MaxRanges)throw ProtocolError("invalid ranges");
        for(auto r:n.ranges)if(!r.count||r.start>=count||std::uint32_t(r.start)+r.count>count)throw ProtocolError("NACK outside frame");
    }
    std::vector<Range> mergeIds(std::vector<std::uint16_t> ids) {
        std::sort(ids.begin(),ids.end());
        ids.erase(std::unique(ids.begin(),ids.end()),ids.end());
        std::vector<Range> out;
        for(auto id:ids) {
            if(!out.empty()&&std::uint32_t(out.back().start)+out.back().count==id&&out.back().count<65535)++out.back().count;
            else out.push_back( {
                id,1
            });
        }
        return out;
    }
}
