#ifndef WOLVRIX_EMIT_VERILATOR_REPCUT_PACKAGE_HPP
#define WOLVRIX_EMIT_VERILATOR_REPCUT_PACKAGE_HPP

#include "core/emit.hpp"

namespace wolvrix::lib::emit
{

    class EmitVerilatorRepCutPackage : public Emit
    {
    public:
        using Emit::Emit;

    protected:
        EmitResult emitPackage(const wolvrix::lib::grh::Design &design,
                               std::span<const wolvrix::lib::grh::Graph *const> topGraphs,
                               const EmitOptions &options, bool grhSim);

    private:
        EmitResult emitImpl(const wolvrix::lib::grh::Design &design,
                            std::span<const wolvrix::lib::grh::Graph *const> topGraphs,
                            const EmitOptions &options) override;
    };

} // namespace wolvrix::lib::emit

#endif // WOLVRIX_EMIT_VERILATOR_REPCUT_PACKAGE_HPP
