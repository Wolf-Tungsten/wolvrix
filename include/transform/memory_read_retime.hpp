#ifndef WOLVRIX_TRANSFORM_MEMORY_READ_RETIME_HPP
#define WOLVRIX_TRANSFORM_MEMORY_READ_RETIME_HPP

#include "core/transform.hpp"

namespace wolvrix::lib::transform
{

    class MemoryReadRetimePass : public Pass
    {
    public:
        MemoryReadRetimePass();

        PassResult run() override;
        bool preservesDialects() const noexcept override { return true; }
        std::unique_ptr<Pass> cloneGraphKernel() const override {
            return std::make_unique<MemoryReadRetimePass>();
        }
        void finishGraphKernels(std::span<const PassResult> results) override;
    };

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_MEMORY_READ_RETIME_HPP
