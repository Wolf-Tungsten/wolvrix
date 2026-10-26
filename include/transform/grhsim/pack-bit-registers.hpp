#ifndef WOLVRIX_TRANSFORM_GRHSIM_PACK_BIT_REGISTERS_HPP
#define WOLVRIX_TRANSFORM_GRHSIM_PACK_BIT_REGISTERS_HPP

#include "core/transform.hpp"
#include <filesystem>

namespace wolvrix::lib::transform::grhsim {

struct PackBitRegistersOptions { std::string top; std::filesystem::path report; };

class PackBitRegistersPass final : public transform::Pass {
public:
    explicit PackBitRegistersPass(PackBitRegistersOptions options = {});
    PassResult run() override;
    bool supportsDialect(std::string_view dialect) const noexcept override;
private:
    PackBitRegistersOptions options_;
};

} // namespace wolvrix::lib::transform::grhsim
#endif
