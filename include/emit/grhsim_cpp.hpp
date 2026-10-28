#ifndef WOLVRIX_EMIT_GRHSIM_CPP_HPP
#define WOLVRIX_EMIT_GRHSIM_CPP_HPP

#include "core/emit.hpp"
#include "emit/grhsim_execution_plan.hpp"

namespace wolvrix::lib::emit
{

    class EmitGrhSimCpp : public Emit
    {
    public:
        using Emit::Emit;

        // Reuses the normal planner, but stops before creating/removing files or
        // emitting C++ bodies. A failed analysis leaves plan empty. No runtime
        // measurements or generated/compiled code are inputs to this API.
        EmitResult analyzeExecutionPlan(const wolvrix::lib::grh::Design &design,
                                        const EmitOptions &options,
                                        GrhSimExecutionPlan &plan);

    private:
        EmitResult emitImpl(const wolvrix::lib::grh::Design &design,
                            std::span<const wolvrix::lib::grh::Graph *const> topGraphs,
                            const EmitOptions &options) override;
        EmitResult prepareAndEmit(const wolvrix::lib::grh::Design &design,
                                  std::span<const wolvrix::lib::grh::Graph *const> topGraphs,
                                  const EmitOptions &options,
                                  GrhSimExecutionPlan *plan);
    };

} // namespace wolvrix::lib::emit

#endif // WOLVRIX_EMIT_GRHSIM_CPP_HPP
