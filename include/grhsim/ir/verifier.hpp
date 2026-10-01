#ifndef WOLVRIX_GRHSIM_IR_VERIFIER_HPP
#define WOLVRIX_GRHSIM_IR_VERIFIER_HPP

#include "core/diagnostics.hpp"

namespace wolvrix::lib::grhsim
{
    class DialectRegistry;
    class GrhSimModel;

    bool verifyGrhSimModel(const GrhSimModel &model,
                           const DialectRegistry &registry,
                           wolvrix::lib::diag::Diagnostics &diagnostics);

    // B8 semantic seal (M5d-5): additional end-of-partition-stage checks on
    // top of verifyGrhSimModel — total phase attribution, lowered event form,
    // and Mem-phase operands produced in P_general. Invoked by
    // `grhsim.verify --seal semantic`; never runs implicitly mid-pipeline.
    bool verifyGrhSimSemanticSeal(const GrhSimModel &model,
                                  wolvrix::lib::diag::Diagnostics &diagnostics);
}

#endif // WOLVRIX_GRHSIM_IR_VERIFIER_HPP
