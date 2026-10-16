#ifndef WOLVRIX_TRANSFORM_REGISTER_WRITE_NORMALIZE_HPP
#define WOLVRIX_TRANSFORM_REGISTER_WRITE_NORMALIZE_HPP

#include "core/transform.hpp"

namespace wolvrix::lib::transform
{
    // Opt-in two-state rewrite: extract guarded element assignments from
    // mux trees without increasing write-port count, removing identity leaves
    // only when no earlier write can
    // be exposed by removing that higher-priority identity assignment.
    class RegisterWriteNormalizePass : public Pass
    {
    public:
        explicit RegisterWriteNormalizePass(bool twoState = false);
        PassResult run() override;

    private:
        bool twoState_;
    };
}

#endif
