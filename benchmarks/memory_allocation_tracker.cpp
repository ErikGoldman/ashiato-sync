#include "allocation_tracker.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

namespace {

struct alignas(std::max_align_t) AllocationHeader {
    void* allocation;
    std::size_t size;
    bool tracked;
};

struct AtomicAllocationCounts {
    std::atomic_size_t allocations = 0;
    std::atomic_size_t allocated_bytes = 0;
    std::atomic_size_t live_allocations = 0;
    std::atomic_size_t live_bytes = 0;
    std::atomic_size_t peak_live_allocations = 0;
    std::atomic_size_t peak_live_bytes = 0;
};

std::atomic_bool allocation_tracking_enabled = false;
AtomicAllocationCounts tracked_allocations;

void record_peak(std::atomic_size_t& peak, std::size_t value) noexcept {
    std::size_t previous = peak.load(std::memory_order_relaxed);
    while (previous < value &&
           !peak.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
    }
}

void record_allocation(std::size_t size) noexcept {
    tracked_allocations.allocations.fetch_add(1U, std::memory_order_relaxed);
    tracked_allocations.allocated_bytes.fetch_add(size, std::memory_order_relaxed);
    const std::size_t live_allocations =
        tracked_allocations.live_allocations.fetch_add(1U, std::memory_order_relaxed) + 1U;
    const std::size_t live_bytes =
        tracked_allocations.live_bytes.fetch_add(size, std::memory_order_relaxed) + size;
    record_peak(tracked_allocations.peak_live_allocations, live_allocations);
    record_peak(tracked_allocations.peak_live_bytes, live_bytes);
}

void record_deallocation(const AllocationHeader& header) noexcept {
    tracked_allocations.live_allocations.fetch_sub(1U, std::memory_order_relaxed);
    tracked_allocations.live_bytes.fetch_sub(header.size, std::memory_order_relaxed);
}

void* allocate(std::size_t size, std::size_t alignment) {
    const std::size_t allocated_size = size == 0U ? 1U : size;
    const std::size_t effective_alignment = std::max(alignment, alignof(AllocationHeader));
    constexpr std::size_t header_size = sizeof(AllocationHeader);
    if (allocated_size > std::numeric_limits<std::size_t>::max() - header_size - effective_alignment + 1U) {
        throw std::bad_alloc();
    }

    const std::size_t storage_size = allocated_size + header_size + effective_alignment - 1U;
    void* const allocation = std::malloc(storage_size);
    if (allocation == nullptr) {
        throw std::bad_alloc();
    }

    const auto unaligned = reinterpret_cast<std::uintptr_t>(allocation) + header_size;
    const auto aligned = (unaligned + effective_alignment - 1U) & ~(effective_alignment - 1U);
    auto* const header = reinterpret_cast<AllocationHeader*>(aligned - header_size);
    ::new (static_cast<void*>(header)) AllocationHeader{
        allocation,
        allocated_size,
        allocation_tracking_enabled.load(std::memory_order_relaxed),
    };
    if (header->tracked) {
        record_allocation(allocated_size);
    }
    return reinterpret_cast<void*>(aligned);
}

void deallocate(void* memory) noexcept {
    if (memory == nullptr) {
        return;
    }

    auto* const header = reinterpret_cast<AllocationHeader*>(
        reinterpret_cast<std::uintptr_t>(memory) - sizeof(AllocationHeader));
    if (header->tracked) {
        record_deallocation(*header);
    }
    std::free(header->allocation);
}

}  // namespace

void* operator new(std::size_t size) {
    return allocate(size, alignof(std::max_align_t));
}

void* operator new[](std::size_t size) {
    return allocate(size, alignof(std::max_align_t));
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocate(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocate(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* memory) noexcept {
    deallocate(memory);
}

void operator delete[](void* memory) noexcept {
    deallocate(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    deallocate(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    deallocate(memory);
}

void operator delete(void* memory, std::align_val_t) noexcept {
    deallocate(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept {
    deallocate(memory);
}

void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
    deallocate(memory);
}

void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
    deallocate(memory);
}

namespace ashiato::sync::benchmarks {

void reset_allocation_counts() noexcept {
    tracked_allocations.allocations.store(0U, std::memory_order_relaxed);
    tracked_allocations.allocated_bytes.store(0U, std::memory_order_relaxed);
    tracked_allocations.live_allocations.store(0U, std::memory_order_relaxed);
    tracked_allocations.live_bytes.store(0U, std::memory_order_relaxed);
    tracked_allocations.peak_live_allocations.store(0U, std::memory_order_relaxed);
    tracked_allocations.peak_live_bytes.store(0U, std::memory_order_relaxed);
}

void set_allocation_tracking(bool enabled) noexcept {
    allocation_tracking_enabled.store(enabled, std::memory_order_relaxed);
}

AllocationCounts allocation_counts() noexcept {
    return AllocationCounts{
        tracked_allocations.allocations.load(std::memory_order_relaxed),
        tracked_allocations.allocated_bytes.load(std::memory_order_relaxed),
        tracked_allocations.live_allocations.load(std::memory_order_relaxed),
        tracked_allocations.live_bytes.load(std::memory_order_relaxed),
        tracked_allocations.peak_live_allocations.load(std::memory_order_relaxed),
        tracked_allocations.peak_live_bytes.load(std::memory_order_relaxed),
    };
}

}  // namespace ashiato::sync::benchmarks
