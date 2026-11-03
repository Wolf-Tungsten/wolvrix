#pragma once

namespace wolvrix::lib::allocator {

struct Configuration {
    bool mimalloc;
    bool projectDefaults;
    long genericCollect;
};

// A zero interval means that this allocator does not expose the option.
Configuration configuration() noexcept;

} // namespace wolvrix::lib::allocator
