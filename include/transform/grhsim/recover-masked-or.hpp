#ifndef WOLVRIX_TRANSFORM_GRHSIM_RECOVER_MASKED_OR_HPP
#define WOLVRIX_TRANSFORM_GRHSIM_RECOVER_MASKED_OR_HPP

#include "core/transform.hpp"
#include <filesystem>

namespace wolvrix::lib::transform::grhsim {
struct RecoverMaskedOrOptions { std::string top; uint32_t minTerms = 16; std::filesystem::path report; };
class RecoverMaskedOrPass final : public transform::Pass {
public:
    explicit RecoverMaskedOrPass(RecoverMaskedOrOptions options = {});
    PassResult run() override;
    bool supportsDialect(std::string_view dialect) const noexcept override;
private:
    RecoverMaskedOrOptions options_;
};
} // namespace wolvrix::lib::transform::grhsim
#endif
