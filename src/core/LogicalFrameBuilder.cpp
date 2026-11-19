#include "core/LogicalFrameBuilder.h"
#include "protocol/PacketCodec.h"
#include <algorithm>
#include <fstream>
#include <filesystem>
namespace rt {
    Frame buildFrame(std::uint32_t id,std::uint64_t ts,std::vector<ModuleData> modules) {
        std::sort(modules.begin(),modules.end(),[](const auto&a,const auto&b) {
            return a.id<b.id;
        });
        Frame f;
        f.logicId=id;
        f.timestampUs=ts;
        std::size_t len=0;
        for(const auto&m:modules) {
            if(m.id>=32||(f.moduleMask&(std::uint32_t(1)<<m.id)))throw ProtocolError("duplicate/invalid module ID");
            if(m.data.size()>MaxFrameBytes || len+ModuleHeaderSize+m.data.size()>MaxFrameBytes)throw ProtocolError("logical frame exceeds 2MiB");
            len+=ModuleHeaderSize+m.data.size();
            f.moduleMask|=std::uint32_t(1)<<m.id;
        }
        if(!len)throw ProtocolError("no modules; skip sample instead of emitting an empty frame");
        f.bytes.resize(len);
        std::size_t pos=0;
        for(const auto&m:modules) {
            f.bytes[pos]=m.id;
            writeLe32(f.bytes.data()+pos+1,static_cast<std::uint32_t>(m.data.size()));
            std::copy(m.data.begin(),m.data.end(),f.bytes.begin()+static_cast<std::ptrdiff_t>(pos+5));
            pos+=5+m.data.size();
        }
        return f;
    }
    std::vector<ModuleData> decodeModules(const Frame& f) {
        std::vector<ModuleData> out;
        std::size_t pos=0;
        for(unsigned id=0;id<32;++id)if(f.moduleMask&(std::uint32_t(1)<<id)) {
            if(f.bytes.size()-pos<5||f.bytes[pos]!=id)throw ProtocolError("module header/order mismatch");
            const auto len=readLe32(f.bytes.data()+pos+1);
            pos+=5;
            if(len>f.bytes.size()-pos)throw ProtocolError("module data_len overflow");
            ModuleData m;
            m.id=static_cast<std::uint8_t>(id);
            m.data.assign(f.bytes.begin()+static_cast<std::ptrdiff_t>(pos),f.bytes.begin()+static_cast<std::ptrdiff_t>(pos+len));
            out.push_back(std::move(m));
            pos+=len;
        }
        if(pos!=f.bytes.size()||out.empty())throw ProtocolError("module lengths/mask mismatch");
        return out;
    }
    ModuleDataSource::ModuleDataSource(const std::vector<ModuleConfig>& configs) {
        if(configs.empty()||configs.size()>32)throw std::invalid_argument("configure 1..32 module definitions");
        std::size_t total=0;
        for(const auto&c:configs) {
            if(c.id>=32||(available_&(std::uint32_t(1)<<c.id)))throw std::invalid_argument("module IDs must be unique 0..31");
            ModuleData m;
            m.id=c.id;
            if(!c.file.empty()) {
                std::ifstream in(std::filesystem::u8path(c.file),std::ios::binary|std::ios::ate);
                if(!in)throw std::runtime_error("cannot open module file: "+c.file);
                const auto end=in.tellg();
                if(end<0||static_cast<std::uint64_t>(end)>MaxFrameBytes)throw std::runtime_error("module file too large");
                m.data.resize(static_cast<std::size_t>(end));
                in.seekg(0);
                if(!m.data.empty()&&!in.read(reinterpret_cast<char*>(m.data.data()),static_cast<std::streamsize>(m.data.size())))throw std::runtime_error("module file read failed");
            }
            else {
                if(c.dataBytes>MaxFrameBytes)throw std::invalid_argument("module data too large");
                m.data.resize(c.dataBytes);
                for(std::size_t i=0;i<m.data.size();++i)m.data[i]=static_cast<std::uint8_t>((i+31u*c.id+c.seed)&255u);
            }
            total+=m.data.size()+5;
            if(total>32*MaxFrameBytes)throw std::invalid_argument("module templates exceed 64MiB resource cap");
            available_|=std::uint32_t(1)<<c.id;
            if(c.online)initial_|=std::uint32_t(1)<<c.id;
            modules_.push_back(std::move(m));
        }
        std::sort(modules_.begin(),modules_.end(),[](const auto&a,const auto&b) {
            return a.id<b.id;
        });
    }
    std::size_t ModuleDataSource::sizeForMask(std::uint32_t mask)const {
        if(mask&~available_)throw std::invalid_argument("mask references undefined module");
        std::size_t n=0;
        for(const auto&m:modules_)if(mask&(std::uint32_t(1)<<m.id))n+=5+m.data.size();
        return n;
    }
    std::optional<Frame> ModuleDataSource::sample(std::uint32_t id,std::uint64_t ts,std::uint32_t mask)const {
        auto n=sizeForMask(mask);
        if(!n)return std::nullopt;
        if(n>MaxFrameBytes)throw ProtocolError("logical frame exceeds 2MiB");
        Frame f;
        f.logicId=id;
        f.timestampUs=ts;
        f.moduleMask=mask;
        f.bytes.resize(n);
        std::size_t pos=0;
        // Snapshot templates are immutable. No filesystem I/O or per-module clock reads in the sampling loop.
        for(const auto&m:modules_)if(mask&(std::uint32_t(1)<<m.id)) {
            f.bytes[pos]=m.id;
            writeLe32(f.bytes.data()+pos+1,static_cast<std::uint32_t>(m.data.size()));
            std::copy(m.data.begin(),m.data.end(),f.bytes.begin()+static_cast<std::ptrdiff_t>(pos+5));
            pos+=5+m.data.size();
        }
        return f;
    }
}
