#ifndef WOLVRIX_GRHSIM_PASS_REG_TO_MEM_HPP
#define WOLVRIX_GRHSIM_PASS_REG_TO_MEM_HPP

#include "grhsim/pass/pass.hpp"

#include <cstddef>
#include <filesystem>

namespace wolvrix::lib::grhsim {
    struct RegToMemOptions {
        std::size_t minElementCount = 4;
        bool enableReadRewrite = true;
        bool enableWriteMerge = true;
        bool enableSameAddressFusion = true;
        bool enableCostSelection = true;
        bool enableRowConstantFill = false;
        bool enableOrWriteMerge = false;
        bool analysisOnly = false;
        std::filesystem::path report;
    };

    class RegToMemPass final : public Pass {
    public:
        explicit RegToMemPass(RegToMemOptions options = {});
        PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override;
    private:
        RegToMemOptions options_;
    };
    void registerRegToMemPass(PassRegistry &registry);
}
#endif
