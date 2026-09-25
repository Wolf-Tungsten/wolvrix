#include "grhsim/pass/demonitor_redundant.hpp"
#include "grhsim/backend/cpu.hpp"
#include "grhsim/ir/model.hpp"

#include <stdexcept>
#include <string>

namespace wolvrix::lib::grhsim {
    namespace {
        // Drop activation-redundant compute fanout rows (NO00014). A monitored
        // boundary value v is redundantly monitored when every consumer unit in
        // its fanout activate list is already activated by every non-constant
        // operand of v's producer op: v-change implies one of those operands
        // changed, and that operand's own publish activates the unit in the
        // same round (topological order fires the producer unit first, so the
        // unit reads the fresh v). v's detection/publish can then only re-fire
        // already fired, idempotent units. Only the fanout row is removed —
        // the value keeps its boundary slot and store; the emitter's existing
        // no-targets degenerate path emits a plain store instead of the
        // detect/publish sequence.
        //
        // The pass itself only sets the schedule plan's demonitorRedundant
        // flag; the rule lives inside buildSchedule so that
        // verifyCpuSchedule's rebuild reproduces the same plan deterministically
        // (see cpu_schedule.cpp for the full rule and its safety argument).
        class DemonitorRedundantPass final : public Pass {
        public:
            DemonitorRedundantPass()
                : Pass("grhsim.demonitor-redundant", PassKind::BackendMapping) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override {
                const auto *previous = model.cpuMapping();
                if (!previous || previous->stage != CpuMappingStage::Schedule ||
                    !previous->dataLayout || !previous->schedule)
                    throw std::runtime_error(
                        "grhsim.demonitor-redundant requires a complete CPU schedule mapping");
                if (previous->schedule->demonitorRedundant) {
                    diagnostics.info("demonitor_redundant already applied", name());
                    return {true, false, {}};
                }
                const std::size_t rowsBefore = previous->schedule->computeSupernodeFanout.size();
                CpuBackendMapping mapping = *previous;
                mapping.schedule->demonitorRedundant = true;
                model.setCpuMapping(std::move(mapping));
                if (!refreshCpuSchedule(model, diagnostics)) return {false, true, {}};
                const auto *current = model.cpuMapping();
                diagnostics.info("demonitor_redundant_rows=" +
                    std::to_string(rowsBefore - current->schedule->computeSupernodeFanout.size()) +
                    " remaining=" + std::to_string(current->schedule->computeSupernodeFanout.size()), name());
                return {true, true, {}};
            }
        };
    }

    void registerDemonitorRedundantPass(PassRegistry &registry) {
        std::string error;
        if (!registry.registerPass("grhsim.demonitor-redundant", PassKind::BackendMapping,
            [](std::span<const std::string_view> args, std::string &factoryError) -> std::unique_ptr<Pass> {
                if (!args.empty()) {
                    factoryError = "grhsim.demonitor-redundant does not accept arguments";
                    return {};
                }
                return std::make_unique<DemonitorRedundantPass>();
            }, error))
            throw std::logic_error(error);
    }
}
