#include "grhsim/pass/demonitor_edge_completion.hpp"
#include "grhsim/backend/cpu.hpp"
#include "grhsim/ir/model.hpp"

#include <charconv>
#include <fstream>
#include <stdexcept>
#include <string>

namespace wolvrix::lib::grhsim {
    namespace {
        // Edge-completion de-monitoring (NO00015). A monitored boundary value v
        // whose producer's non-constant operands almost cover v's consumer units
        // is made redundant by ADDING the missing operand->consumer activation
        // edges (w is a monitored boundary value whose fanout row already
        // activates the producer unit, giving the same freshness proof as
        // grhsim.demonitor-redundant), then removing v's fanout row. The added
        // edges widen consumer unit activations (idempotent extra fires), so the
        // removal set is priced with a dynamic per-value change profile and only
        // profit>0 candidates are taken; eligibility itself is fully static and
        // re-validated on application inside buildSchedule (see cpu_schedule.cpp
        // for the rule and its safety argument).
        //
        // The pass loads the flat vchg profile ("<value> <writes> <changes>"
        // rows with a "# grhsim-vchg-profile v1 cycles=..." header), computes
        // the selection, and stores it on the schedule plan; the rule inside
        // buildSchedule applies it so verifyCpuSchedule's rebuild reproduces
        // the same plan deterministically.
        class DemonitorEdgeCompletionPass final : public Pass {
        public:
            explicit DemonitorEdgeCompletionPass(std::string profilePath)
                : Pass("grhsim.demonitor-edge-completion", PassKind::BackendMapping),
                  profilePath_(std::move(profilePath)) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override {
                const auto *previous = model.cpuMapping();
                if (!previous || previous->stage != CpuMappingStage::Schedule ||
                    !previous->dataLayout || !previous->schedule)
                    throw std::runtime_error(
                        "grhsim.demonitor-edge-completion requires a complete CPU schedule mapping");
                if (previous->schedule->demonitorEdgeCompletion) {
                    diagnostics.info("demonitor_edge_completion already applied", name());
                    return {true, false, {}};
                }
                std::vector<uint64_t> writes(model.values().size() + 1, 0);
                std::vector<uint64_t> changes(model.values().size() + 1, 0);
                loadProfile(writes, changes);
                auto selection = computeDemonitorEdgeCompletionSelection(model, *previous, writes, changes);
                if (selection.removed.empty()) {
                    diagnostics.info("demonitor_edge_completion no profitable candidates", name());
                    return {true, false, {}};
                }
                const std::size_t rowsBefore = previous->schedule->computeSupernodeFanout.size();
                CpuBackendMapping mapping = *previous;
                mapping.schedule->demonitorEdgeCompletion = true;
                mapping.schedule->demonitorEdgeCompletionRemoved = std::move(selection.removed);
                model.setCpuMapping(std::move(mapping));
                if (!refreshCpuSchedule(model, diagnostics)) return {false, true, {}};
                const auto *current = model.cpuMapping();
                diagnostics.info("demonitor_edge_completion_rows=" +
                    std::to_string(rowsBefore - current->schedule->computeSupernodeFanout.size()) +
                    " remaining=" + std::to_string(current->schedule->computeSupernodeFanout.size()) +
                    " eligible=" + std::to_string(selection.eligible) +
                    " profitable=" + std::to_string(selection.profitable) +
                    " cascade_trimmed=" + std::to_string(selection.cascadeTrimmed) +
                    " added_edges=" + std::to_string(selection.addedEdges) +
                    " save_x4=" + std::to_string(selection.saveX4) +
                    " widen_x4=" + std::to_string(selection.widenX4), name());
                return {true, true, {}};
            }

        private:
            void loadProfile(std::vector<uint64_t> &writes, std::vector<uint64_t> &changes) const {
                std::ifstream input(profilePath_);
                if (!input)
                    throw std::runtime_error("grhsim.demonitor-edge-completion cannot open profile: " + profilePath_);
                const std::string header = "# grhsim-vchg-profile v1";
                std::string line;
                bool sawHeader = false;
                const auto parseUint = [](const char *&cursor, const char *end, uint64_t &out) {
                    while (cursor != end && *cursor == ' ') ++cursor;
                    const auto *begin = cursor;
                    while (cursor != end && *cursor != ' ') ++cursor;
                    if (begin == cursor) return false;
                    return std::from_chars(begin, cursor, out).ec == std::errc();
                };
                while (std::getline(input, line)) {
                    if (line.empty()) continue;
                    if (line.front() == '#') {
                        if (line.compare(0, header.size(), header) != 0)
                            throw std::runtime_error("grhsim.demonitor-edge-completion bad profile header: " + line);
                        sawHeader = true;
                        continue;
                    }
                    const char *cursor = line.data();
                    const char *end = cursor + line.size();
                    uint64_t id = 0, wr = 0, ch = 0;
                    if (!parseUint(cursor, end, id) || !parseUint(cursor, end, wr) || !parseUint(cursor, end, ch))
                        throw std::runtime_error("grhsim.demonitor-edge-completion bad profile row: " + line);
                    if (!id || id >= writes.size())
                        throw std::runtime_error("grhsim.demonitor-edge-completion profile value id out of range: " +
                                                 std::to_string(id) + " (stale profile?)");
                    writes[id] = wr;
                    changes[id] = ch;
                }
                if (!sawHeader)
                    throw std::runtime_error("grhsim.demonitor-edge-completion profile missing header: " + profilePath_);
            }

            std::string profilePath_;
        };
    }

    void registerDemonitorEdgeCompletionPass(PassRegistry &registry) {
        std::string error;
        if (!registry.registerPass("grhsim.demonitor-edge-completion", PassKind::BackendMapping,
            [](std::span<const std::string_view> args, std::string &factoryError) -> std::unique_ptr<Pass> {
                std::string profile;
                for (std::size_t i = 0; i < args.size(); ++i) {
                    if (args[i] == "--profile" && i + 1 < args.size()) {
                        profile = std::string(args[++i]);
                        continue;
                    }
                    factoryError = "grhsim.demonitor-edge-completion expects --profile <path>";
                    return {};
                }
                if (profile.empty()) {
                    factoryError = "grhsim.demonitor-edge-completion requires --profile <path>";
                    return {};
                }
                return std::make_unique<DemonitorEdgeCompletionPass>(std::move(profile));
            }, error))
            throw std::logic_error(error);
    }
}
