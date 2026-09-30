#include "grhsim/backend/cpu.hpp"
#include "grhsim/backend/cpu_emit.hpp"

#include "grhsim/pass/pass.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <stdexcept>
#include <tuple>

namespace wolvrix::lib::grhsim
{
    bool isCpuCommitOp(std::string_view type) noexcept
    {
        return type == "core.state.regWrite" || type == "core.state.latchWrite" ||
               type == "core.state.memWrite" || type == "core.state.memFill" ||
               type == "core.state.memAssign" || type == "core.state.memWriteSeq";
    }

    namespace
    {
        std::vector<bool> inputValues(const GrhSimModel &model)
        {
            std::vector<bool> result(model.values().size() + 1, false);
            for (const auto &op : model.operations())
                if (model.text(op.opType) == "core.input.read")
                    for (auto value : model.results(op)) result[value.index] = true;
            return result;
        }

        bool eventLess(const CpuEvent &lhs, const CpuEvent &rhs)
        {
            return std::tie(lhs.edge, lhs.value.index) < std::tie(rhs.edge, rhs.value.index);
        }

        std::optional<CpuEventGate> eventGate(const GrhSimModel &model, const SimOp &op,
                                             const std::vector<bool> &inputs)
        {
            const std::vector<std::string> *edges = nullptr;
            for (const auto &parameter : model.parameters(op))
            {
                if (model.text(parameter.name) != "event_edges") continue;
                edges = std::get_if<std::vector<std::string>>(&parameter.value);
                if (!edges) throw std::runtime_error("event_edges must be a string array");
            }
            const auto operands = model.operands(op);
            const auto type = model.text(op.opType);
            const auto refs = model.objectRefs(op);
            const std::size_t eventCount = edges ? edges->size() : 0;
            const std::size_t dataCount = (type == "core.state.regWrite" || type == "core.state.latchWrite") ? 3 :
                                          type == "core.state.memWrite" ? 4 : 2;
            if (type == "core.state.memWriteSeq")
            {
                if (operands.size() < eventCount + 3 || (operands.size() - eventCount) % 3 != 0)
                    throw std::runtime_error("invalid CPU ordered memory write operand count");
            }
            else if (operands.size() != eventCount + dataCount ||
                     (type == "core.state.latchWrite" && eventCount != 0))
                throw std::runtime_error("invalid event operand count in CPU commit op");
            if (refs.size() != eventCount + 1)
                throw std::runtime_error("CPU commit event history count does not match events");
            for (auto ref : refs)
                if (ref.kind != ObjectKind::State)
                    throw std::runtime_error("CPU commit target and histories must reference states");
            if (eventCount == 0) return std::nullopt;
            CpuEventGate gate;
            gate.source = CpuEventSource::Input;
            const auto events = operands.last(edges->size());
            for (std::size_t i = 0; i < events.size(); ++i)
            {
                CpuEventEdge edge;
                if ((*edges)[i] == "posedge") edge = CpuEventEdge::Posedge;
                else if ((*edges)[i] == "negedge") edge = CpuEventEdge::Negedge;
                else throw std::runtime_error("unsupported CPU event edge: " + (*edges)[i]);
                gate.events.push_back({events[i], edge});
                if (!inputs[events[i].index]) gate.source = CpuEventSource::Derived;
            }
            std::sort(gate.events.begin(), gate.events.end(), eventLess);
            gate.events.erase(std::unique(gate.events.begin(), gate.events.end()), gate.events.end());
            return gate;
        }

        PartitionId addPartition(CpuPartitionTree &tree, PartitionId parent,
                                 CpuPartitionKind kind, CpuPhase phase = CpuPhase::None)
        {
            const PartitionId id{static_cast<uint32_t>(tree.partitions.size() + 1), 0};
            tree.partitions.push_back({id, parent, {}, {}, {kind, phase, {}}});
            if (parent) tree.partitions[parent.index - 1].children.push_back(id);
            return id;
        }

        class SplitPhasePass final : public Pass
        {
        public:
            SplitPhasePass() : Pass("cpu.st.split-phase", PassKind::BackendMapping) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                CpuBackendMapping mapping;
                auto &tree = mapping.partitionTree;
                tree.root = addPartition(tree, {}, CpuPartitionKind::Root);
                const auto compute = addPartition(tree, tree.root, CpuPartitionKind::Phase, CpuPhase::Compute);
                const auto commit = addPartition(tree, tree.root, CpuPartitionKind::Phase, CpuPhase::Commit);
                for (const auto &op : model.operations())
                {
                    const bool write = isCpuCommitOp(model.text(op.opType));
                    if (write && !model.results(op).empty())
                        throw std::runtime_error("CPU commit op must not define graph values");
                    tree.partitions[(write ? commit : compute).index - 1].ops.push_back(op.id);
                }
                diagnostics.info("compute_ops=" + std::to_string(tree.partitions[compute.index - 1].ops.size()) +
                                 " commit_ops=" + std::to_string(tree.partitions[commit.index - 1].ops.size()), name());
                model.setCpuMapping(std::move(mapping));
                return {true, true, {}};
            }
        };

        class FormEventDomainsPass final : public Pass
        {
        public:
            explicit FormEventDomainsPass(uint32_t maxOps)
                : Pass("cpu.st.form-event-domains", PassKind::BackendMapping), maxOps_(maxOps) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                const auto *previous = model.cpuMapping();
                if (!previous || previous->stage != CpuMappingStage::SplitPhase)
                {
                    diagnostics.error("requires cpu.st.split-phase output", name());
                    return {false, false, {}};
                }
                CpuBackendMapping mapping = *previous;
                auto &tree = mapping.partitionTree;
                PartitionId commit;
                for (const auto &partition : tree.partitions)
                    if (partition.attrs.phase == CpuPhase::Commit) commit = partition.id;
                auto ops = std::move(tree.partitions[commit.index - 1].ops);
                const auto inputs = inputValues(model);
                // Keys contain only the canonical event disjunction, never data enables or masks.
                std::map<std::vector<std::pair<CpuEventEdge, uint32_t>>, PartitionId> domains;
                std::size_t inputDomains = 0, derivedDomains = 0, generalDomains = 0;
                std::vector<uint32_t> writerCounts(model.states().size() + 1);
                for (auto id : ops)
                {
                    const auto &op = model.operations()[id.index - 1];
                    auto gate = eventGate(model, op, inputs);
                    std::vector<std::pair<CpuEventEdge, uint32_t>> key;
                    if (gate)
                        for (auto event : gate->events) key.emplace_back(event.edge, event.value.index);
                    auto [it, inserted] = domains.try_emplace(std::move(key));
                    if (inserted)
                    {
                        it->second = addPartition(tree, commit, CpuPartitionKind::EventDomain);
                        tree.partitions[it->second.index - 1].attrs.eventGate = gate;
                        if (!gate) ++generalDomains;
                        else if (gate->source == CpuEventSource::Input) ++inputDomains;
                        else ++derivedDomains;
                    }
                    const auto domain = it->second;
                    const auto &children = tree.partitions[domain.index - 1].children;
                    PartitionId chunk = children.empty() ? PartitionId{} : children.back();
                    if (!chunk || tree.partitions[chunk.index - 1].ops.size() == maxOps_)
                        chunk = addPartition(tree, domain, CpuPartitionKind::Supernode);
                    tree.partitions[chunk.index - 1].ops.push_back(id);
                    const auto refs = model.objectRefs(op);
                    if (!refs.empty() && refs.front().kind == ObjectKind::State)
                        ++writerCounts[refs.front().index];
                }
                const auto multiwriters = std::count_if(writerCounts.begin(), writerCounts.end(),
                                                        [](uint32_t count) { return count > 1; });
                diagnostics.info("input_edge_domains=" + std::to_string(inputDomains) +
                                 " derived_edge_domains=" + std::to_string(derivedDomains) +
                                 " general_domains=" + std::to_string(generalDomains) +
                                 " multiwriter_states=" + std::to_string(multiwriters), name());
                mapping.stage = CpuMappingStage::EventDomains;
                model.setCpuMapping(std::move(mapping));
                return {true, true, {}};
            }

        private:
            uint32_t maxOps_;
        };
        // Six-phase (M3) structural verification for the
        // SplitPhases..GeneralFunctions stages. The legacy two-phase checks
        // below do not apply; layout/schedule payloads are not part of the
        // six-phase pipeline until the M4 stages (LayoutNamedStores on),
        // which hand the payload to verifyCpuDataLayout/verifyCpuSchedule.
        bool verifyCpuPhases(const GrhSimModel &model, const CpuBackendMapping &cpu,
                             diag::Diagnostics &diagnostics)
        {
            const auto error = [&](std::string message) {
                diagnostics.error(std::move(message), "cpu.mapping");
                return false;
            };
            if (cpu.stage < CpuMappingStage::LayoutNamedStores && (cpu.dataLayout || cpu.schedule))
                return error("six-phase CPU mapping must not carry data layout or schedule payloads");
            const auto &tree = cpu.partitionTree;
            const auto validPartition = [&](PartitionId id) {
                return id.generation == 0 && id.index > 0 && id.index <= tree.partitions.size();
            };
            if (!validPartition(tree.root)) return error("partition root is invalid");
            const auto &root = tree.partitions[tree.root.index - 1];
            const std::array<CpuPhase, 4> phaseOrder{CpuPhase::Event, CpuPhase::General,
                                                     CpuPhase::Mem, CpuPhase::Output};
            if (root.parent || root.attrs.kind != CpuPartitionKind::Root || !root.ops.empty() ||
                root.children.size() != phaseOrder.size())
                return error("six-phase CPU root must contain exactly four phase branches");
            for (std::size_t i = 0; i < phaseOrder.size(); ++i)
            {
                if (!validPartition(root.children[i])) return error("invalid phase branch");
                const auto &branch = tree.partitions[root.children[i].index - 1];
                if (branch.parent != tree.root || branch.attrs.kind != CpuPartitionKind::Phase ||
                    branch.attrs.phase != phaseOrder[i])
                    return error("six-phase branch order must be event, general, mem, output");
            }
            const auto memWrite = [](std::string_view type) {
                return type == "core.state.memWrite" || type == "core.state.memFill" ||
                       type == "core.state.memAssign" || type == "core.state.memWriteSeq";
            };
            std::vector<bool> visited(tree.partitions.size() + 1);
            std::vector<bool> covered(model.operations().size() + 1);
            std::vector<OpId> eventOps, generalOps, memOps, outputOps;
            uint32_t eventFunctions = 0, memFunctions = 0, outputFunctions = 0;
            struct Visit { PartitionId id; CpuPhase phase; };
            std::vector<Visit> stack{{tree.root, CpuPhase::None}};
            while (!stack.empty())
            {
                auto [id, phase] = stack.back();
                stack.pop_back();
                if (!validPartition(id) || visited[id.index]) return error("invalid, repeated or cyclic partition");
                visited[id.index] = true;
                const auto &partition = tree.partitions[id.index - 1];
                const auto &attrs = partition.attrs;
                if (partition.id != id) return error("partition IDs must be dense and match table order");
                if (attrs.kind == CpuPartitionKind::Phase)
                {
                    if (partition.parent != tree.root || phase != CpuPhase::None)
                        return error("phase partition is not a root child");
                    phase = attrs.phase;
                    if (phase == CpuPhase::General && !partition.ops.empty())
                        return error("general branch holds its ops in nodes, not on the phase");
                }
                else if (attrs.phase != CpuPhase::None)
                    return error("phase annotation must occur only on phase branches");
                if (attrs.eventGate || attrs.activeId || attrs.activeWord)
                    return error("legacy activity annotations are not part of the six-phase mapping");
                if (attrs.kind == CpuPartitionKind::EventDomain || attrs.kind == CpuPartitionKind::ActiveWord ||
                    (attrs.kind == CpuPartitionKind::Root && id != tree.root))
                    return error("legacy partition kinds are not part of the six-phase mapping");
                const bool generalSupernode = phase == CpuPhase::General &&
                                              attrs.kind == CpuPartitionKind::Supernode;
                if (cpu.stage >= CpuMappingStage::GeneralSupernodes)
                {
                    if (generalSupernode != attrs.eventActs.has_value())
                        return error("event act annotations must cover exactly the general supernodes");
                }
                else if (attrs.eventActs)
                    return error("event act annotations require the general-supernode stage");
                if (!attrs.helperChunks.empty() &&
                    !(cpu.stage >= CpuMappingStage::GeneralFunctions && generalSupernode))
                    return error("helper chunks annotate only packed general supernodes");
                std::optional<CpuPartitionKind> childKind;
                switch (attrs.kind)
                {
                case CpuPartitionKind::Root:
                    childKind = CpuPartitionKind::Phase;
                    break;
                case CpuPartitionKind::Phase:
                    if (phase == CpuPhase::General)
                    {
                        if (cpu.stage >= CpuMappingStage::GeneralFunctions) childKind = CpuPartitionKind::EmitFunction;
                        else if (cpu.stage >= CpuMappingStage::GeneralSupernodes) childKind = CpuPartitionKind::Supernode;
                        else if (cpu.stage >= CpuMappingStage::GeneralNodes) childKind = CpuPartitionKind::Node;
                    }
                    else if (cpu.stage >= CpuMappingStage::GeneralFunctions)
                        childKind = CpuPartitionKind::EmitFunction;
                    break;
                case CpuPartitionKind::Node:
                    if (phase != CpuPhase::General || cpu.stage < CpuMappingStage::GeneralNodes ||
                        partition.ops.empty() || !partition.children.empty())
                        return error("general node must be a nonempty leaf in the general branch");
                    {
                        const auto parentKind = tree.partitions[partition.parent.index - 1].attrs.kind;
                        const auto expected = cpu.stage == CpuMappingStage::GeneralNodes ?
                                              CpuPartitionKind::Phase : CpuPartitionKind::Supernode;
                        if (parentKind != expected)
                            return error("general node parenting disagrees with the mapping stage");
                    }
                    break;
                case CpuPartitionKind::Supernode:
                    if (!generalSupernode || cpu.stage < CpuMappingStage::GeneralSupernodes ||
                        partition.children.empty())
                        return error("six-phase supernodes exist only in the general branch");
                    {
                        const auto parentKind = tree.partitions[partition.parent.index - 1].attrs.kind;
                        const auto expected = cpu.stage == CpuMappingStage::GeneralSupernodes ?
                                              CpuPartitionKind::Phase : CpuPartitionKind::EmitFunction;
                        if (parentKind != expected)
                            return error("general supernode parenting disagrees with the mapping stage");
                        uint64_t size = 0;
                        for (auto child : partition.children)
                        {
                            if (!validPartition(child)) return error("invalid general node child");
                            size += tree.partitions[child.index - 1].ops.size();
                        }
                        uint64_t end = 0;
                        for (auto chunk : attrs.helperChunks)
                        {
                            if (chunk.offset != end || chunk.count == 0) return error("invalid helper chunk range");
                            end += chunk.count;
                        }
                        if (!attrs.helperChunks.empty() && end != size)
                            return error("helper chunks do not cover supernode ops");
                    }
                    childKind = CpuPartitionKind::Node;
                    break;
                case CpuPartitionKind::EmitFunction:
                    if (cpu.stage < CpuMappingStage::GeneralFunctions ||
                        tree.partitions[partition.parent.index - 1].attrs.kind != CpuPartitionKind::Phase)
                        return error("emit functions require the function-packing stage under a phase branch");
                    if (phase == CpuPhase::General)
                    {
                        if (partition.children.empty())
                            return error("general emit function requires supernode children");
                        childKind = CpuPartitionKind::Supernode;
                    }
                    else
                    {
                        if (!partition.children.empty())
                            return error("flat emit function must be a leaf");
                        if (phase == CpuPhase::Event) ++eventFunctions;
                        else if (phase == CpuPhase::Mem) ++memFunctions;
                        else if (phase == CpuPhase::Output) ++outputFunctions;
                    }
                    break;
                default: return error("unknown partition kind");
                }
                if (!partition.children.empty() && !partition.ops.empty()) return error("non-leaf partition contains ops");
                if (childKind && !partition.ops.empty()) return error("structural partition must not contain ops");
                if (!childKind && !partition.children.empty()) return error("leaf partition must not contain children");
                for (auto it = partition.children.rbegin(); it != partition.children.rend(); ++it)
                {
                    const auto child = *it;
                    if (!validPartition(child) || tree.partitions[child.index - 1].parent != id)
                        return error("partition parent/child references disagree");
                    if (childKind && tree.partitions[child.index - 1].attrs.kind != *childKind)
                        return error("partition child kind disagrees with CPU mapping stage");
                    stack.push_back({child, phase});
                }
                for (auto opId : partition.ops)
                {
                    if (opId.generation != 0 || opId.index == 0 || opId.index > model.operations().size() ||
                        covered[opId.index])
                        return error("invalid or duplicate op ownership");
                    covered[opId.index] = true;
                    const auto &op = model.operations()[opId.index - 1];
                    if (phase == CpuPhase::None) return error("op outside any phase branch");
                    const auto required = phase == CpuPhase::Event ? SimPhase::Event :
                                          phase == CpuPhase::General ? SimPhase::General :
                                          phase == CpuPhase::Mem ? SimPhase::Mem : SimPhase::Output;
                    if (op.phase != required) return error("op belongs to the wrong phase branch");
                    if (phase == CpuPhase::Mem && !memWrite(model.text(op.opType)))
                        return error("mem branch holds only memory write ops");
                    (phase == CpuPhase::Event ? eventOps : phase == CpuPhase::General ? generalOps :
                     phase == CpuPhase::Mem ? memOps : outputOps).push_back(opId);
                }
            }
            if (std::count(visited.begin() + 1, visited.end(), false)) return error("unreachable partitions");
            for (const auto &op : model.operations())
                if (op.phase == SimPhase::None)
                    return error("six-phase mapping requires total phase attribution");
            if (cpu.stage == CpuMappingStage::SplitPhases)
            {
                for (std::size_t i = 1; i < covered.size(); ++i)
                    if (!covered[i] && model.operations()[i - 1].phase != SimPhase::General)
                        return error("flat branches do not cover every non-general op");
            }
            else if (std::count(covered.begin() + 1, covered.end(), false))
                return error("partition tree does not cover all ops");
            if (cpu.stage >= CpuMappingStage::GeneralFunctions &&
                (eventFunctions != 1 || memFunctions != 1 || outputFunctions != 1))
                return error("flat branches must each hold exactly one emit function");
            const auto checkOrder = [&](const std::vector<OpId> &ops, bool detectorsLast,
                                        const char *message) {
                std::vector<bool> defined(model.values().size() + 1);
                bool detectorSeen = false;
                for (auto opId : ops)
                {
                    const auto &op = model.operations()[opId.index - 1];
                    const bool detector = model.text(op.opType) == "core.event.edgeDet";
                    if (detectorsLast && detectorSeen && !detector)
                        return error("event edge detectors must trail the event cone");
                    detectorSeen = detectorSeen || detector;
                    for (auto value : model.operands(op))
                        if (!defined[value.index]) return error(message);
                    for (auto value : model.results(op)) defined[value.index] = true;
                }
                return true;
            };
            if (!checkOrder(eventOps, true, "event order uses a value before its definition")) return false;
            if (!checkOrder(outputOps, false, "output order uses a value before its definition")) return false;
            if (!checkOrder(generalOps, false, "general order uses a value before its definition")) return false;
            uint32_t lastMem = 0;
            for (auto opId : memOps)
            {
                if (opId.index <= lastMem) return error("mem write order must follow static op order");
                lastMem = opId.index;
            }
            if (cpu.stage >= CpuMappingStage::GeneralSupernodes)
            {
                const auto domainSets = computeCpuEventDomainSets(model);
                const auto unionInto = [](std::vector<int64_t> &target, const std::vector<int64_t> &source) {
                    if (source.empty()) return;
                    const auto middle = target.size();
                    target.insert(target.end(), source.begin(), source.end());
                    std::inplace_merge(target.begin(), target.begin() + middle, target.end());
                    target.erase(std::unique(target.begin(), target.end()), target.end());
                };
                for (const auto &partition : tree.partitions)
                {
                    if (partition.attrs.kind != CpuPartitionKind::Supernode) continue;
                    std::vector<int64_t> acts, influence, common;
                    bool sawEvent = false, uniform = true;
                    for (auto child : partition.children)
                        for (auto opId : tree.partitions[child.index - 1].ops)
                        {
                            unionInto(influence, domainSets.influence[opId.index]);
                            if (domainSets.acts[opId.index].empty()) continue;
                            if (!sawEvent) { common = domainSets.acts[opId.index]; sawEvent = true; }
                            else if (common != domainSets.acts[opId.index]) uniform = false;
                            unionInto(acts, domainSets.acts[opId.index]);
                        }
                    if (*partition.attrs.eventActs != acts)
                        return error("general supernode event acts disagree with its ops");
                    if (sawEvent && (!uniform || influence != acts))
                        return error("general supernode violates the event domain constraint");
                }
            }
            if (cpu.stage >= CpuMappingStage::LayoutNamedStores)
                return verifyCpuDataLayout(model, cpu, diagnostics) && verifyCpuSchedule(model, cpu, diagnostics);
            return true;
        }
    }

    bool verifyCpuMapping(const GrhSimModel &model, const BackendMapping &mapping,
                          diag::Diagnostics &diagnostics)
    {
        const auto error = [&](std::string message) {
            diagnostics.error(std::move(message), "cpu.mapping");
            return false;
        };
        if (model.text(mapping.backend) != "cpu" || model.text(mapping.schema) != "cpu.st.v1" || !mapping.cpu)
            return error("CPU mapping requires a typed cpu.st.v1 payload");
        const auto &cpu = *mapping.cpu;
        // The legacy pipeline completes at Schedule, the six-phase pipeline
        // at PhaseSchedule; both are valid complete terminals.
        if (mapping.complete != (cpu.stage == CpuMappingStage::Schedule ||
                                 cpu.stage == CpuMappingStage::PhaseSchedule))
            return error("CPU mapping completion requires the schedule stage");
        if (cpu.stage > CpuMappingStage::PhaseSchedule)
            return error("unknown CPU mapping stage");
        if (cpu.stage >= CpuMappingStage::SplitPhases)
            return verifyCpuPhases(model, cpu, diagnostics);
        const auto &tree = cpu.partitionTree;
        const auto validPartition = [&](PartitionId id) {
            return id.generation == 0 && id.index > 0 && id.index <= tree.partitions.size();
        };
        if (!validPartition(tree.root)) return error("partition root is invalid");
        const auto &root = tree.partitions[tree.root.index - 1];
        if (root.parent || root.attrs.kind != CpuPartitionKind::Root || root.children.size() != 2)
            return error("CPU root must contain exactly two phase branches");
        if (!validPartition(root.children[0]) || !validPartition(root.children[1]) ||
            tree.partitions[root.children[0].index - 1].attrs.phase != CpuPhase::Compute ||
            tree.partitions[root.children[1].index - 1].attrs.phase != CpuPhase::Commit)
            return error("CPU phase order must be compute then commit");
        std::vector<bool> visited(tree.partitions.size() + 1);
        std::vector<bool> covered(model.operations().size() + 1);
        std::vector<uint32_t> lastDomainOp(tree.partitions.size() + 1);
        std::map<std::vector<std::pair<CpuEventEdge, uint32_t>>, PartitionId> domains;
        struct Visit { PartitionId id; CpuPhase phase; PartitionId domain; };
        std::vector<Visit> stack{{tree.root, CpuPhase::None, {}}};
        const auto inputs = inputValues(model);
        std::vector<bool> defined(model.values().size() + 1);
        uint32_t nextActiveId = 0;
        uint32_t computePhases = 0, commitPhases = 0;
        while (!stack.empty())
        {
            auto [id, phase, domain] = stack.back();
            stack.pop_back();
            if (!validPartition(id) || visited[id.index]) return error("invalid, repeated or cyclic partition");
            visited[id.index] = true;
            const auto &partition = tree.partitions[id.index - 1];
            const auto &attrs = partition.attrs;
            if (partition.id != id) return error("partition IDs must be dense and match table order");
            if (attrs.kind == CpuPartitionKind::Phase)
            {
                if (partition.parent != tree.root || phase != CpuPhase::None)
                    return error("phase partition is not a root child");
                phase = attrs.phase;
                if (phase == CpuPhase::Compute) ++computePhases;
                else if (phase == CpuPhase::Commit) ++commitPhases;
                else return error("phase partition must specify compute or commit");
            }
            else if (attrs.phase != CpuPhase::None)
                return error("phase annotation must occur only on phase branches");
            if (attrs.kind == CpuPartitionKind::EventDomain)
            {
                if (cpu.stage < CpuMappingStage::EventDomains || phase != CpuPhase::Commit || domain ||
                    tree.partitions[partition.parent.index - 1].attrs.kind != CpuPartitionKind::Phase)
                    return error("event domain must be a commit-phase child");
                domain = id;
                if (partition.children.empty()) return error("event domain must contain commit supernodes");
                std::vector<std::pair<CpuEventEdge, uint32_t>> key;
                if (attrs.eventGate)
                {
                    if (attrs.eventGate->events.empty()) return error("edge domain has no events");
                    for (auto event : attrs.eventGate->events) key.emplace_back(event.edge, event.value.index);
                }
                if (!domains.emplace(std::move(key), id).second) return error("duplicate canonical event domains");
            }
            else if (attrs.eventGate) return error("event gate must annotate an event-domain partition");
            const bool activeSupernode = phase == CpuPhase::Compute && attrs.kind == CpuPartitionKind::Supernode &&
                                         cpu.stage >= CpuMappingStage::ActiveWords;
            if (activeSupernode)
            {
                if (!attrs.activeId || *attrs.activeId != nextActiveId++) return error("compute active IDs must be continuous in schedule order");
            }
            else if (attrs.activeId || !attrs.helperChunks.empty()) return error("activity/helper annotations require a compute supernode");
            if (attrs.activeWord && attrs.kind != CpuPartitionKind::ActiveWord) return error("active-word index on a non-word partition");
            std::optional<CpuPartitionKind> childKind;
            switch (attrs.kind)
            {
            case CpuPartitionKind::Root:
                if (id != tree.root) return error("multiple root partitions");
                childKind = CpuPartitionKind::Phase;
                break;
            case CpuPartitionKind::Phase:
                if (phase == CpuPhase::Compute && cpu.stage >= CpuMappingStage::ComputeNodes)
                    childKind = cpu.stage >= CpuMappingStage::EmitFunctions ? CpuPartitionKind::EmitFunction :
                                cpu.stage >= CpuMappingStage::ActiveWords ? CpuPartitionKind::ActiveWord :
                                cpu.stage >= CpuMappingStage::ComputeSupernodes ? CpuPartitionKind::Supernode : CpuPartitionKind::Node;
                if (phase == CpuPhase::Commit && cpu.stage >= CpuMappingStage::EventDomains)
                    childKind = CpuPartitionKind::EventDomain;
                break;
            case CpuPartitionKind::EventDomain:
                childKind = cpu.stage >= CpuMappingStage::EmitFunctions ? CpuPartitionKind::EmitFunction : CpuPartitionKind::Supernode;
                break;
            case CpuPartitionKind::Supernode:
                if (phase == CpuPhase::Compute)
                {
                    if (cpu.stage < CpuMappingStage::ComputeSupernodes || partition.children.empty())
                        return error("compute supernode requires nonempty node children");
                    childKind = CpuPartitionKind::Node;
                    uint64_t size = 0;
                    for (auto child : partition.children)
                    {
                        if (!validPartition(child)) return error("invalid compute node child");
                        size += tree.partitions[child.index - 1].ops.size();
                    }
                    uint64_t end = 0;
                    for (auto chunk : attrs.helperChunks)
                    {
                        if (chunk.offset != end || chunk.count == 0) return error("invalid helper chunk range");
                        end += chunk.count;
                    }
                    if (!attrs.helperChunks.empty() && end != size) return error("helper chunks do not cover supernode ops");
                }
                else if (!domain || !partition.children.empty() || partition.ops.empty())
                    return error("commit supernode must be a nonempty domain leaf");
                break;
            case CpuPartitionKind::Node:
                if (phase != CpuPhase::Compute || cpu.stage < CpuMappingStage::ComputeNodes || partition.ops.empty())
                    return error("compute node must be a nonempty compute leaf");
                break;
            case CpuPartitionKind::ActiveWord:
                if (phase != CpuPhase::Compute || cpu.stage < CpuMappingStage::ActiveWords || !attrs.activeWord ||
                    *attrs.activeWord != nextActiveId / 8 || nextActiveId % 8 != 0 ||
                    partition.children.empty() || partition.children.size() > 8)
                    return error("active word must contain up to eight aligned consecutive supernodes");
                childKind = CpuPartitionKind::Supernode;
                break;
            case CpuPartitionKind::EmitFunction:
                if (cpu.stage < CpuMappingStage::EmitFunctions || partition.children.empty())
                    return error("function partition requires nonempty packed children");
                childKind = phase == CpuPhase::Compute ? CpuPartitionKind::ActiveWord : CpuPartitionKind::Supernode;
                break;
            default: return error("unknown partition kind");
            }
            if (!partition.children.empty() && !partition.ops.empty()) return error("non-leaf partition contains ops");
            if (childKind && !partition.ops.empty()) return error("structural partition must not contain ops");
            if (!childKind && !partition.children.empty()) return error("leaf partition must not contain children");
            for (auto it = partition.children.rbegin(); it != partition.children.rend(); ++it)
            {
                const auto child = *it;
                if (!validPartition(child) || tree.partitions[child.index - 1].parent != id)
                    return error("partition parent/child references disagree");
                if (tree.partitions[child.index - 1].attrs.kind != *childKind)
                    return error("partition child kind disagrees with CPU mapping stage");
                stack.push_back({child, phase, domain});
            }
            for (auto opId : partition.ops)
            {
                if (opId.generation != 0 || opId.index == 0 || opId.index > model.operations().size() || covered[opId.index])
                    return error("invalid or duplicate op ownership");
                covered[opId.index] = true;
                const auto &op = model.operations()[opId.index - 1];
                if (phase == CpuPhase::None || (isCpuCommitOp(model.text(op.opType)) != (phase == CpuPhase::Commit)))
                    return error("op belongs to the wrong compute/commit phase");
                if (phase == CpuPhase::Compute && cpu.stage >= CpuMappingStage::ComputeNodes)
                {
                    for (auto value : model.operands(op))
                        if (!defined[value.index]) return error("CPU compute order uses a value before its definition");
                    for (auto value : model.results(op)) defined[value.index] = true;
                }
                if (phase == CpuPhase::Commit && !model.results(op).empty()) return error("CPU commit op defines a graph value");
                if (phase == CpuPhase::Commit && cpu.stage >= CpuMappingStage::EventDomains)
                {
                    if (!domain) return error("commit op is not covered by an event domain");
                    if (lastDomainOp[domain.index] >= opId.index) return error("commit domain changed stable write order");
                    lastDomainOp[domain.index] = opId.index;
                    if (eventGate(model, op, inputs) != tree.partitions[domain.index - 1].attrs.eventGate)
                        return error("commit op canonical event key disagrees with domain gate");
                }
            }
        }
        if (computePhases != 1 || commitPhases != 1) return error("CPU mapping requires one branch of each phase");
        if (std::count(visited.begin() + 1, visited.end(), false)) return error("unreachable partitions");
        if (std::count(covered.begin() + 1, covered.end(), false)) return error("partition tree does not cover all ops");
        return verifyCpuDataLayout(model, cpu, diagnostics) && verifyCpuSchedule(model, cpu, diagnostics);
    }

    void registerCpuPasses(PassRegistry &registry)
    {
        registerCpuPartitionPasses(registry);
        registerCpuLayoutPasses(registry);
        registerCpuSchedulePasses(registry);
        registerCpuEmitPasses(registry);
        std::string error;
        if (!registry.registerPass("cpu.st.split-phase", PassKind::BackendMapping,
            [](std::span<const std::string_view> args, std::string &error) -> std::unique_ptr<Pass> {
                if (!args.empty()) { error = "cpu.st.split-phase does not accept arguments"; return {}; }
                return std::make_unique<SplitPhasePass>();
            }, error)) throw std::logic_error(error);
        if (!registry.registerPass("cpu.st.form-event-domains", PassKind::BackendMapping,
            [](std::span<const std::string_view> args, std::string &error) -> std::unique_ptr<Pass> {
                uint32_t maxOps = 4096;
                constexpr std::string_view option = "--max-op-in-commit-supernode";
                if (!args.empty())
                {
                    if (args.size() != 2 || args[0] != option)
                    { error = "expected --max-op-in-commit-supernode <count>"; return {}; }
                    const auto value = args[1];
                    const auto result = std::from_chars(value.data(), value.data() + value.size(), maxOps);
                    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || maxOps == 0)
                    { error = "commit chunk limit must be a positive 32-bit integer"; return {}; }
                }
                return std::make_unique<FormEventDomainsPass>(maxOps);
            }, error)) throw std::logic_error(error);
    }
}
