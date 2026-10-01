#ifndef WOLVRIX_GRHSIM_CONVERT_GRH_TO_GRHSIM_HPP
#define WOLVRIX_GRHSIM_CONVERT_GRH_TO_GRHSIM_HPP

#include "core/diagnostics.hpp"
#include "core/grh.hpp"
#include "grhsim/ir/model.hpp"

#include <memory>
#include <string>

namespace wolvrix::lib::grhsim
{

    struct GrhToGrhSimOptions
    {
        std::string top;
        LogicDomain logicDomain = LogicDomain::FourState;
        bool keepOrigins = true;
        // Carry GRH declaredSymbols/generateGroups into the model as read-only
        // metadata, and resolve every declared symbol that lowers to a live
        // value/state/function into a DeclProvenance record (Direct full-range
        // slice; array declarations keep their shape). Independent of
        // keepOrigins: the name lists and the association are small.
        bool keepDeclaredSymbols = true;
    };

    std::unique_ptr<GrhSimModel> lowerGrhToGrhSim(
        const wolvrix::lib::grh::Design &design,
        const GrhToGrhSimOptions &options,
        wolvrix::lib::diag::Diagnostics &diagnostics);

} // namespace wolvrix::lib::grhsim

#endif // WOLVRIX_GRHSIM_CONVERT_GRH_TO_GRHSIM_HPP
