#pragma once
#include "network/IUdpTransport.h"
#include <array>
#include <cstddef>
#include <memory_resource>

namespace rt {
// The transport owner initiates and polls on one thread. Its usual single
// outstanding send can reuse this block; overlapping operations keep working.
class AsioOperationStorage final : public std::pmr::memory_resource {
  public:
    OperationStorageStats stats() const noexcept {
        return stats_;
    }

    AsioOperationStorage() = default;
    AsioOperationStorage(const AsioOperationStorage &) = delete;
    AsioOperationStorage &operator=(const AsioOperationStorage &) = delete;

  private:
    void *do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (!inUse_ && bytes <= storage_.size() && alignment <= alignof(std::max_align_t)) {
            inUse_ = true;
            ++stats_.hits;
            return storage_.data();
        }
        auto *allocation = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++stats_.fallbacks;
        return allocation;
    }

    void do_deallocate(void *ptr, std::size_t bytes, std::size_t alignment) override {
        if (ptr == storage_.data())
            inUse_ = false;
        else
            std::pmr::new_delete_resource()->deallocate(ptr, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
        return this == &other;
    }

    alignas(std::max_align_t) std::array<std::byte, 1024> storage_;
    bool inUse_ = false;
    OperationStorageStats stats_{true, 0, 0};
};
} // namespace rt
