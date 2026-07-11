#pragma once

#include <cstddef>

namespace ashiato::sync::benchmarks {

struct AllocationCounts {
    std::size_t allocations = 0;
    std::size_t allocated_bytes = 0;
};

void reset_allocation_counts() noexcept;
void set_allocation_tracking(bool enabled) noexcept;
AllocationCounts allocation_counts() noexcept;

}  // namespace ashiato::sync::benchmarks
