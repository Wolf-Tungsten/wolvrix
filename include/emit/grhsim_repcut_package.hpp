#ifndef WOLVRIX_EMIT_GRHSIM_REPCUT_PACKAGE_HPP
#define WOLVRIX_EMIT_GRHSIM_REPCUT_PACKAGE_HPP

#include "emit/verilator_repcut_package.hpp"

namespace wolvrix::lib::emit
{
    // Backend selection is resolved at generation time. Both packages share
    // the RepCut phase scheduler; generated models never mix backends.
    class EmitGrhSimRepCutPackage : public EmitVerilatorRepCutPackage
    {
    public:
        using EmitVerilatorRepCutPackage::EmitVerilatorRepCutPackage;

    private:
        EmitResult emitImpl(const wolvrix::lib::grh::Design &design,
                            std::span<const wolvrix::lib::grh::Graph *const> topGraphs,
                            const EmitOptions &options) override
        {
            return emitPackage(design, topGraphs, options, true);
        }
    };
}

#endif
