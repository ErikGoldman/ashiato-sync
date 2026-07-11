#include "allocation_tracker.hpp"

#include <cstdlib>
#include <new>

namespace {

thread_local bool allocation_tracking_enabled = false;
thread_local ashiato::sync::benchmarks::AllocationCounts tracked_allocations;

void record_allocation(std::size_t size) noexcept {
    if (allocation_tracking_enabled) {
        ++tracked_allocations.allocations;
        tracked_allocations.allocated_bytes += size;
    }
}

void* allocate(std::size_t size) {
    const std::size_t allocated_size = size == 0U ? 1U : size;
    if (void* memory = std::malloc(allocated_size)) {
        record_allocation(allocated_size);
        return memory;
    }
    throw std::bad_alloc();
}

}  // namespace

void* operator new(std::size_t size) {
    return allocate(size);
}

void* operator new[](std::size_t size) {
    return allocate(size);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace ashiato::sync::benchmarks {

void reset_allocation_counts() noexcept {
    tracked_allocations = AllocationCounts{};
}

void set_allocation_tracking(bool enabled) noexcept {
    allocation_tracking_enabled = enabled;
}

AllocationCounts allocation_counts() noexcept {
    return tracked_allocations;
}

}  // namespace ashiato::sync::benchmarks
