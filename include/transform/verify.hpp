#ifndef WOLVRIX_TRANSFORM_VERIFY_HPP
#define WOLVRIX_TRANSFORM_VERIFY_HPP

#include "core/transform.hpp"

namespace wolvrix::lib::transform
{

    // Structural verifier for GRH graphs, including array types. Read-only: it
    // never mutates the design and reports every violation through
    // PassDiagnostics.
    class GrhVerifyPass : public Pass
    {
    public:
        GrhVerifyPass();

        PassResult run() override;
        bool supportsDialect(std::string_view) const noexcept override { return true; }
    };

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_VERIFY_HPP
