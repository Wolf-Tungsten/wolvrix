#ifndef WOLVRIX_TRANSFORM_MEMORY_WRITE_COALESCE_HPP
#define WOLVRIX_TRANSFORM_MEMORY_WRITE_COALESCE_HPP

#include "core/transform.hpp"

namespace wolvrix::lib::transform
{
    // Join independent constant-mask writes which select the same address on
    // the same event under equivalent, side-effect-free one-bit guards.
    class MemoryWriteCoalescePass : public Pass
    {
    public:
        MemoryWriteCoalescePass();
        PassResult run() override;
    };
}

#endif
