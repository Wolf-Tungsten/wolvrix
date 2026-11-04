#pragma once

#include "core/transform.hpp"

namespace wolvrix::lib::transform
{

    class LatchTransparentReadPass : public Pass
    {
    public:
        LatchTransparentReadPass();

        PassResult run() override;
        bool preservesDialects() const noexcept override { return true; }
        std::unique_ptr<Pass> cloneGraphKernel() const override {
            return std::make_unique<LatchTransparentReadPass>();
        }
    };

} // namespace wolvrix::lib::transform
