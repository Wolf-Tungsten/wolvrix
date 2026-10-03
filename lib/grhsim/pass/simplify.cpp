#include "grhsim/pass/simplify.hpp"

#include "grhsim/ir/model.hpp"
#include "simplify_internal.hpp"

#include <array>
#include <map>
#include <optional>
#include <string>

namespace wolvrix::lib::grhsim
{
    namespace
    {
        // Unified simplify infrastructure (M5d-2): the fixed sub-pass sequence
        //    const-fold -> canonicalize-compute -> bitwise-predicates
        //    -> bitwise-muxes -> mux-chain-fold -> used-bits
        // iterated until a whole round reports no change or the round cap is
        // hit. scope=whole runs the sequence once per round over the entire
        // graph; scope=phase runs it per simulation phase (Event -> General ->
        // Mem -> Output), each restricted to its own ops, so partition
        // interfaces, side-effect roots and cross-partition references are
        // preserved and no cross-phase CSE happens. cpu.st.clone-shared-boundaries
        // (the C-segment boundary clone) is deliberately not part of this fixpoint.
        struct StepEntry
        {
            std::string_view name;
            SimplifyStepReport (*run)(GrhSimModel &, diag::Diagnostics &, SimplifyScope);
        };

        constexpr std::array<StepEntry, 6> kSteps{{
            {"grhsim.const-fold", &simplifyStepConstFold},
            {"grhsim.canonicalize-compute", &simplifyStepCanonicalizeCompute},
            {"grhsim.bitwise-predicates", &simplifyStepBitwisePredicates},
            {"grhsim.bitwise-muxes", &simplifyStepBitwiseMuxes},
            {"grhsim.mux-chain-fold", &simplifyStepMuxChainFold},
            {"grhsim.used-bits", &simplifyStepUsedBits},
        }};

        constexpr std::array<SimPhase, 4> kPhaseOrder{
            SimPhase::Event, SimPhase::General, SimPhase::Mem, SimPhase::Output};

        bool parsePositive(std::string_view text, uint32_t &value)
        {
            if (text.empty()) return false;
            uint64_t parsed = 0;
            for (const char c : text)
            {
                if (c < '0' || c > '9') return false;
                parsed = parsed * 10 + static_cast<uint64_t>(c - '0');
                if (parsed > UINT32_MAX) return false;
            }
            if (parsed == 0) return false;
            value = static_cast<uint32_t>(parsed);
            return true;
        }

        class SimplifyPass final : public Pass
        {
        public:
            SimplifyPass(bool phaseScope, std::optional<SimPhase> onlyPhase, uint32_t maxRounds)
                : Pass("grhsim.simplify", PassKind::SemanticTransform),
                  phaseScope_(phaseScope), onlyPhase_(onlyPhase), maxRounds_(maxRounds) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                // Per-step aggregated counters, in the fixed sequence order.
                std::map<std::string_view, std::map<std::string_view, uint64_t>> totals;
                std::map<std::string_view, uint64_t> stepChanges;
                const auto runSequence = [&](SimplifyScope scope) {
                    bool changed = false;
                    for (const auto &step : kSteps)
                    {
                        auto report = step.run(model, diagnostics, scope);
                        if (report.changed)
                        {
                            changed = true;
                            ++stepChanges[step.name];
                        }
                        for (const auto &[key, count] : report.counters) totals[step.name][key] += count;
                    }
                    return changed;
                };
                uint32_t rounds = 0;
                bool converged = false;
                bool changed = false;
                while (rounds < maxRounds_)
                {
                    ++rounds;
                    bool roundChanged = false;
                    if (!phaseScope_)
                    {
                        roundChanged = runSequence(SimplifyScope::whole());
                    }
                    else
                    {
                        for (const SimPhase phase : kPhaseOrder)
                        {
                            if (onlyPhase_ && phase != *onlyPhase_) continue;
                            roundChanged = runSequence(SimplifyScope::restricted(phase)) ||
                                             roundChanged;
                        }
                    }
                    changed = changed || roundChanged;
                    if (!roundChanged)
                    {
                        converged = true;
                        break;
                    }
                }
                std::string header = "simplify_scope=" + std::string(phaseScope_ ? "phase" : "whole");
                if (phaseScope_)
                {
                    header += " simplify_phases=";
                    bool first = true;
                    for (const SimPhase phase : kPhaseOrder)
                    {
                        if (onlyPhase_ && phase != *onlyPhase_) continue;
                        if (!first) header += ',';
                        header += toString(phase);
                        first = false;
                    }
                }
                header += " simplify_rounds=" + std::to_string(rounds) +
                          " simplify_converged=" + (converged ? "1" : "0");
                diagnostics.info(std::move(header), name());
                for (const auto &step : kSteps)
                {
                    const auto stepTotals = totals.find(step.name);
                    if (stepTotals == totals.end()) continue;
                    bool any = false;
                    for (const auto &[key, count] : stepTotals->second) any = any || count != 0;
                    if (!any) continue;
                    std::string line = "simplify_step=" + std::string(step.name) +
                        " step_changed_rounds=" + std::to_string(stepChanges[step.name]);
                    for (const auto &[key, count] : stepTotals->second)
                    {
                        line += ' ';
                        line += std::string(key) + '=' + std::to_string(count);
                    }
                    diagnostics.info(std::move(line), name());
                }
                if (!converged)
                    diagnostics.warning("grhsim.simplify reached the round cap without converging "
                                        "(max-rounds=" + std::to_string(maxRounds_) + ")",
                                        name());
                return {true, changed, {}};
            }

        private:
            bool phaseScope_;
            std::optional<SimPhase> onlyPhase_;
            uint32_t maxRounds_;
        };
    } // namespace

    void registerSimplifyPass(PassRegistry &registry)
    {
        std::string error;
        registry.registerPass(
            "grhsim.simplify", PassKind::SemanticTransform,
            [](std::span<const std::string_view> args, std::string &factoryError)
                -> std::unique_ptr<Pass> {
                if (args.size() % 2 != 0)
                {
                    factoryError = "grhsim.simplify options must be --key value pairs";
                    return {};
                }
                bool phaseScope = false;
                std::optional<SimPhase> onlyPhase;
                uint32_t maxRounds = 8;
                for (std::size_t i = 0; i < args.size(); i += 2)
                {
                    if (args[i] == "--scope")
                    {
                        if (args[i + 1] == "whole") phaseScope = false;
                        else if (args[i + 1] == "phase") phaseScope = true;
                        else
                        {
                            factoryError = "unknown grhsim.simplify scope: " + std::string(args[i + 1]);
                            return {};
                        }
                    }
                    else if (args[i] == "--max-rounds")
                    {
                        if (!parsePositive(args[i + 1], maxRounds))
                        {
                            factoryError = "grhsim.simplify --max-rounds requires a positive integer";
                            return {};
                        }
                    }
                    else if (args[i] == "--phase")
                    {
                        const auto phase = parseSimPhase(args[i + 1]);
                        if (!phase || *phase == SimPhase::None)
                        {
                            factoryError = "unknown grhsim.simplify phase: " + std::string(args[i + 1]);
                            return {};
                        }
                        onlyPhase = *phase;
                    }
                    else
                    {
                        factoryError = "unknown grhsim.simplify option: " + std::string(args[i]);
                        return {};
                    }
                }
                if (onlyPhase && !phaseScope)
                {
                    factoryError = "grhsim.simplify --phase requires --scope phase";
                    return {};
                }
                return std::make_unique<SimplifyPass>(phaseScope, onlyPhase, maxRounds);
            }, error);
    }

} // namespace wolvrix::lib::grhsim
