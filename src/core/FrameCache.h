#pragma once
#include "core/Types.h"
#include <memory>
namespace rt {
    class FrameCache {
        public:
        explicit FrameCache(CachePolicy policy=CachePolicy::SpecModulo):policy_(policy) {
        }
        void put(Frame f) {
            const auto slot=policy_==CachePolicy::SpecModulo?std::size_t(f.logicId%3u):next_++%3u;
            slots_[slot]=std::make_unique<Frame>(std::move(f));
        }
        const Frame* find(std::uint32_t id)const noexcept {
            for(const auto&f:slots_)if(f&&f->logicId==id)return f.get();
            return nullptr;
        }
        std::vector<const Frame*> frames()const {
            std::vector<const Frame*> v;
            for(const auto&f:slots_)if(f)v.push_back(f.get());
            return v;
        }
        private:
        CachePolicy policy_;
        std::size_t next_=0;
        std::array<std::unique_ptr<Frame>,3> slots_;
    };
}
