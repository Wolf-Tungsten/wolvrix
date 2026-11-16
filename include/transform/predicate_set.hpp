#pragma once

#include "core/transform.hpp"

namespace wolvrix::lib::transform
{
    struct PredicateSetOptions
    {
        // Interval comparisons do not preserve general four-state equality.
        bool twoState = false;
    };

    class PredicateSetPass : public Pass
    {
    public:
        explicit PredicateSetPass(PredicateSetOptions options = {});
        PassResult run() override;

    private:
        PredicateSetOptions options_;
    };
}
