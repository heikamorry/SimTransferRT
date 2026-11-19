#pragma once
#include "core/Types.h"
namespace rt {
    struct ModuleData {
        std::uint8_t id=0;
        Bytes data;
    };
    Frame buildFrame(std::uint32_t id,std::uint64_t ts,std::vector<ModuleData> modules);
    std::vector<ModuleData> decodeModules(const Frame& f);
    class ModuleDataSource {
        public:
        explicit ModuleDataSource(const std::vector<ModuleConfig>& config);
        std::optional<Frame> sample(std::uint32_t id,std::uint64_t ts,std::uint32_t mask)const;
        std::uint32_t availableMask()const noexcept {
            return available_;
        }
        std::uint32_t initialMask()const noexcept {
            return initial_;
        }
        std::size_t sizeForMask(std::uint32_t mask)const;
        private:
        std::vector<ModuleData> modules_;
        std::uint32_t available_=0,initial_=0;
    };
}
