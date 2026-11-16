#pragma once

#include "core/transform.hpp"

namespace wolvrix::lib::transform
{
    struct SliceDemandOptions
    {
        bool twoState = false;
    };

    class SliceDemandPass : public Pass
    {
    public:
        explicit SliceDemandPass(SliceDemandOptions options = {});
        PassResult run() override;

    private:
        SliceDemandOptions options_;
    };
}
