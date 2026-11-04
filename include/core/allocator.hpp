#pragma once

#include <cstdint>

namespace wolvrix::lib::allocator {

struct Configuration {
    bool mimalloc;
    bool projectDefaults;
    long genericCollect;
};

// A zero interval means that this allocator does not expose the option.
Configuration configuration() noexcept;

struct Statistics {
    bool enabled = false;
    uint64_t allocations = 0;
    uint64_t normalAllocations = 0;
    uint64_t hugeAllocations = 0;
    int64_t liveThreads = 0;
};

// Includes this thread and threads that have exited. Other live threads must
// merge their counters before a complete process snapshot can be taken.
Statistics statistics() noexcept;

} // namespace wolvrix::lib::allocator
