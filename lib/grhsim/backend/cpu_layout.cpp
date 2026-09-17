#include "grhsim/backend/cpu.hpp"

#include "grhsim/pass/pass.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>

namespace wolvrix::lib::grhsim
{
    namespace
    {
        uint64_t checkedAdd(uint64_t a, uint64_t b)
        {
            if (b > std::numeric_limits<uint64_t>::max() - a)
                throw std::runtime_error("CPU layout byte size overflow");
            return a + b;
        }

        uint64_t alignUp(uint64_t size, uint32_t alignment)
        {
            return checkedAdd(size, alignment - 1) & ~uint64_t(alignment - 1);
        }

        uint64_t allocate(uint64_t &end, const CpuType &type)
        {
            const auto offset = alignUp(end, type.alignment);
            end = checkedAdd(offset, type.size);
            return offset;
        }

        class TypeMapper
        {
        public:
            explicit TypeMapper(CpuDataLayout &layout) : layout_(layout) {}

            CpuTypeId scalar(CpuTypeKind kind, uint32_t width = 0)
            {
                const uint64_t size = kind == CpuTypeKind::String ? layout_.pointerBytes :
                                      kind == CpuTypeKind::Bool ? 1 :
                                      width > 64 ? ((uint64_t(width) + 63) / 64) * 8 : width / 8;
                return intern({{}, kind, width, {}, 0, size, static_cast<uint32_t>(std::min<uint64_t>(size, 8))});
            }

            CpuTypeId array(CpuTypeId element, uint64_t count)
            {
                const auto &type = layout_.types.at(element.index - 1);
                const auto stride = alignUp(type.size, type.alignment);
                if (!count || (stride && count > std::numeric_limits<uint64_t>::max() / stride))
                    throw std::runtime_error("CPU array count is zero or byte size overflows");
                return intern({{}, CpuTypeKind::Array, 0, element, count, stride * count, type.alignment});
            }

        private:
            CpuTypeId intern(CpuType type)
            {
                const auto key = std::tuple{type.kind, type.width, type.elementType.index, type.count};
                auto [it, inserted] = types_.try_emplace(key);
                if (inserted)
                {
                    if (layout_.types.size() >= std::numeric_limits<uint32_t>::max())
                        throw std::runtime_error("too many CPU types");
                    type.id = {static_cast<uint32_t>(layout_.types.size() + 1), 0};
                    it->second = type.id;
                    layout_.types.push_back(type);
                }
                return it->second;
            }

            CpuDataLayout &layout_;
            std::map<std::tuple<CpuTypeKind, uint32_t, uint32_t, uint64_t>, CpuTypeId> types_;
        };

        std::vector<CpuHelperReadCache> planHelperReadCaches(const GrhSimModel &model, const CpuPartitionTree &tree,
                                                          const CpuDataLayout &layout)
        {
            std::vector<bool> constants(model.values().size() + 1);
            for (const auto &op : model.operations())
                if (model.text(op.opType) == "core.compute.constant")
                    for (const auto result : model.results(op)) constants[result.index] = true;
            std::vector<CpuHelperReadCache> caches;
            for (const auto &partition : tree.partitions)
            {
                if (partition.attrs.kind != CpuPartitionKind::Supernode || partition.children.empty()) continue;
                std::vector<OpId> ops;
                for (const auto child : partition.children)
                {
                    const auto &node = tree.partitions[child.index - 1];
                    ops.insert(ops.end(), node.ops.begin(), node.ops.end());
                }
                auto chunks = partition.attrs.helperChunks;
                if (chunks.empty()) chunks.push_back({0, static_cast<uint32_t>(ops.size())});
                for (const auto chunk : chunks)
                {
                    const auto group = std::span<const OpId>(ops).subspan(chunk.offset, chunk.count);
                    if (group.empty()) continue;
                    std::map<uint32_t, uint32_t> uses;
                    for (const auto id : group)
                        for (const auto operand : model.operands(model.operations()[id.index - 1]))
                            ++uses[operand.index];
                    // Values produced in this helper can change during its body;
                    // only already-available boundary inputs may be snapshotted.
                    for (const auto id : group)
                        for (const auto result : model.results(model.operations()[id.index - 1]))
                            uses.erase(result.index);
                    CpuHelperReadCache cache{group.front(), {}};
                    for (const auto &[index, count] : uses)
                    {
                        const auto &type = model.types()[model.values()[index - 1].type.index - 1];
                        if (count > 1 && !constants[index] &&
                            layout.values[index - 1].kind == CpuStorageKind::Boundary &&
                            type.kind == TypeKind::Logic && type.domain == LogicDomain::TwoState &&
                            type.width > 0 && type.width <= 64)
                            cache.values.push_back({index, 0});
                    }
                    if (!cache.values.empty()) caches.push_back(std::move(cache));
                }
            }
            return caches;
        }

        struct DensifyBoundaryStats
        {
            uint64_t values = 0;
            uint64_t bytes = 0;
            uint64_t groups = 0;
        };

        CpuDataLayout buildLayout(const GrhSimModel &model, const CpuPartitionTree &tree, bool readCaches = true,
                                  bool densifyBoundary = true, DensifyBoundaryStats *densifyStats = nullptr)
        {
            CpuDataLayout layout;
            TypeMapper mapper(layout);
            std::vector<CpuTypeId> types(model.types().size() + 1);
            for (const auto &type : model.types())
            {
                CpuTypeId physical;
                switch (type.kind)
                {
                case TypeKind::Logic:
                {
                    const auto width = type.width <= 8 ? 8 : type.width <= 16 ? 16 :
                                       type.width <= 32 ? 32 : type.width <= 64 ? 64 : type.width;
                    physical = type.width == 1 && !type.isSigned ? mapper.scalar(CpuTypeKind::Bool) :
                               mapper.scalar(type.isSigned ? CpuTypeKind::SInt : CpuTypeKind::UInt, width);
                    if (type.domain == LogicDomain::FourState) physical = mapper.array(physical, 2);
                    break;
                }
                case TypeKind::Real: physical = mapper.scalar(CpuTypeKind::F64, 64); break;
                case TypeKind::String: physical = mapper.scalar(CpuTypeKind::String); break;
                case TypeKind::Array:
                    if (type.elementType.index >= type.id.index || !types[type.elementType.index])
                        throw std::runtime_error("CPU layout requires earlier array element types");
                    physical = mapper.array(types[type.elementType.index], type.count);
                    break;
                }
                types[type.id.index] = physical;
            }
            const auto addObject = [&](ObjectRef object, TypeId semantic) {
                const auto type = types[semantic.index];
                layout.objects.push_back({object, {type, CpuStorageKind::Object, {},
                    allocate(layout.objectBytes, layout.types[type.index - 1])}});
            };
            layout.objects.reserve(model.inputs().size() + model.outputs().size() + model.states().size());
            for (const auto &object : model.inputs()) addObject(ObjectRef::input(object.id), object.type);
            for (const auto &object : model.outputs()) addObject(ObjectRef::output(object.id), object.type);
            // State entries stay in id order for positional lookup; only the byte
            // offsets are assigned in tiers.  Edge-committed scalar states are
            // touched every cycle while array states sit in multi-megabyte
            // regions, so their offsets form one small front tier that keeps the
            // per-cycle commit working set cache-resident instead of scattered
            // across the full object arena.
            std::vector<uint32_t> stateRefs(model.states().size() + 1), stateAllowed(model.states().size() + 1),
                stateWriters(model.states().size() + 1);
            for (auto ref : model.objectRefPool())
                if (ref.kind == ObjectKind::State) ++stateRefs[ref.index];
            for (const auto &op : model.operations())
            {
                const auto name = model.text(op.opType);
                const auto refs = model.objectRefs(op);
                if (name == "core.state.read") ++stateAllowed[refs[0].index];
                else if (name == "core.state.regWrite" || name == "core.state.latchWrite")
                { ++stateAllowed[refs[0].index]; ++stateWriters[refs[0].index]; }
            }
            const auto hotCommitScalar = [&](const StateObject &state) {
                const auto &type = model.types()[state.type.index - 1];
                return type.kind == TypeKind::Logic && type.domain == LogicDomain::TwoState && type.width > 0 &&
                    type.width <= 64 && stateWriters[state.id.index] == 1 && stateRefs[state.id.index] == stateAllowed[state.id.index];
            };
            const auto stateBase = layout.objects.size();
            for (const auto &object : model.states())
                layout.objects.push_back({ObjectRef::state(object.id), {types[object.type.index], CpuStorageKind::Object, {}, 0}});
            for (unsigned tier = 0; tier < 3; ++tier)
                for (const auto &state : model.states())
                {
                    const bool isArray = model.types()[state.type.index - 1].kind == TypeKind::Array;
                    const unsigned stateTier = hotCommitScalar(state) ? 0 : isArray ? 2 : 1;
                    if (stateTier != tier) continue;
                    auto &slot = layout.objects[stateBase + state.id.index - 1].slot;
                    slot.offset = allocate(layout.objectBytes, layout.types[slot.type.index - 1]);
                }

            std::vector<PartitionId> opOwners(model.operations().size() + 1);
            std::vector<uint32_t> frames(tree.partitions.size() + 1);
            for (const auto &partition : tree.partitions)
            {
                if (partition.attrs.activeId)
                {
                    frames[partition.id.index] = static_cast<uint32_t>(layout.localFrames.size());
                    layout.localFrames.push_back({partition.id});
                }
                const auto owner = partition.attrs.kind == CpuPartitionKind::Node ? partition.parent : partition.id;
                for (auto op : partition.ops) opOwners[op.index] = owner;
            }
            layout.values.resize(model.values().size());
            for (const auto &op : model.operations())
            {
                for (auto result : model.results(op))
                {
                    auto &slot = layout.values[result.index - 1];
                    slot.owner = opOwners[op.id.index];
                    // A gated DPI call may leave its old result intact across supernode invocations.
                    slot.kind = model.text(op.opType) == "core.dpi.call" ?
                                CpuStorageKind::Boundary : CpuStorageKind::PartitionLocal;
                }
            }
            for (const auto &op : model.operations())
                for (auto operand : model.operands(op))
                {
                    auto &slot = layout.values[operand.index - 1];
                    if (slot.owner != opOwners[op.id.index]) slot.kind = CpuStorageKind::Boundary;
                }
            for (const auto &partition : tree.partitions)
            {
                if (partition.attrs.activeWord)
                    layout.runtime.push_back({CpuRuntimeKind::ActiveWord, partition.id, {},
                                              CpuEventEdge::Posedge, layout.runtimeBytes++});
                if (partition.attrs.eventGate)
                {
                    layout.runtime.push_back({CpuRuntimeKind::DomainArm, partition.id, {},
                                              CpuEventEdge::Posedge, layout.runtimeBytes++});
                    for (const auto &event : partition.attrs.eventGate->events)
                    {
                        layout.values[event.value.index - 1].kind = CpuStorageKind::Boundary;
                        layout.runtime.push_back({CpuRuntimeKind::EventEdge, partition.id, event.value,
                                                  event.edge, layout.runtimeBytes++});
                    }
                }
            }
            // Boundary operands of edge-commit ports are read every cycle by the
            // commit tasks; assign them the tier right after the densified
            // endpoint inputs (allocated below).
            std::vector<uint8_t> commitOperands(model.values().size() + 1);
            for (const auto &op : model.operations())
            {
                const auto name = model.text(op.opType);
                if (name != "core.state.regWrite" && name != "core.state.latchWrite") continue;
                const auto refs = model.objectRefs(op);
                if (refs.empty() || refs[0].kind != ObjectKind::State ||
                    !hotCommitScalar(model.states()[refs[0].index - 1])) continue;
                for (auto operand : model.operands(op)) commitOperands[operand.index] = 1;
            }
            for (const auto &value : model.values())
            {
                auto &slot = layout.values[value.id.index - 1];
                slot.type = types[value.type.index];
                if (!slot.owner) throw std::runtime_error("CPU layout value has no producer");
                if (slot.kind == CpuStorageKind::Boundary) continue;
                auto &frame = layout.localFrames.at(frames[slot.owner.index]);
                slot.offset = allocate(frame.size, layout.types[slot.type.index - 1]);
                frame.alignment = std::max(frame.alignment, layout.types[slot.type.index - 1].alignment);
            }
            // Compute tasks containing event-gated system/DPI endpoint ops read a
            // fan of narrow boundary conditions on every (non-quiesced) activation.
            // Scattered across the whole boundary arena, each condition costs a
            // separate cache line per activation; group those endpoint inputs per
            // consuming emit-function task and assign the boundary arena front
            // tier so an activation touches only a few dense lines.  The set is a
            // deterministic function of the model and partition tree, keeping the
            // canonical layout reproducible; verification also accepts the legacy
            // pre-densification canonical form for archived checkpoints.
            constexpr uint64_t densifyBudgetBytes = 16 * 1024;
            std::vector<uint8_t> densified(model.values().size() + 1, 0);
            std::vector<std::vector<uint32_t>> densifyGroups;
            uint64_t densifiedBytes = 0;
            if (densifyBoundary)
            {
                std::vector<uint8_t> claimed(model.values().size() + 1, 0);
                std::vector<std::pair<uint32_t, std::vector<uint32_t>>> candidates;
                for (const auto &partition : tree.partitions)
                {
                    if (partition.attrs.kind != CpuPartitionKind::EmitFunction) continue;
                    const auto &parent = tree.partitions[partition.parent.index - 1];
                    if (parent.attrs.kind == CpuPartitionKind::EventDomain) continue;
                    std::vector<uint32_t> group;
                    for (auto word : partition.children)
                    for (auto unit : tree.partitions[word.index - 1].children)
                    {
                        const auto &unitPartition = tree.partitions[unit.index - 1];
                        std::vector<OpId> ops;
                        for (auto node : unitPartition.children)
                            ops.insert(ops.end(), tree.partitions[node.index - 1].ops.begin(),
                                       tree.partitions[node.index - 1].ops.end());
                        bool gated = false;
                        for (auto id : ops)
                        {
                            if (gated) break;
                            const auto &op = model.operations()[id.index - 1];
                            const auto name = model.text(op.opType);
                            if (name != "core.system.task" && name != "core.dpi.call") continue;
                            for (const auto &parameter : model.parameters(op))
                                if (model.text(parameter.name) == "event_edges")
                                {
                                    const auto *edges = std::get_if<std::vector<std::string>>(&parameter.value);
                                    if (edges && !edges->empty()) gated = true;
                                }
                        }
                        if (!gated) continue;
                        for (auto id : ops)
                            for (auto operand : model.operands(model.operations()[id.index - 1]))
                            {
                                if (claimed[operand.index]) continue;
                                if (layout.values[operand.index - 1].kind != CpuStorageKind::Boundary) continue;
                                const auto &type = model.types()[model.values()[operand.index - 1].type.index - 1];
                                if (type.kind != TypeKind::Logic || type.domain != LogicDomain::TwoState ||
                                    !type.width || type.width > 8) continue;
                                claimed[operand.index] = 1;
                                group.push_back(operand.index);
                            }
                    }
                    if (!group.empty()) candidates.push_back({partition.id.index, std::move(group)});
                }
                std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
                    return a.second.size() != b.second.size() ? a.second.size() > b.second.size() : a.first < b.first; });
                for (auto &[task, group] : candidates)
                {
                    // Whole groups only: an over-budget group keeps the legacy tiers.
                    uint64_t groupBytes = 0;
                    for (auto index : group)
                        groupBytes += layout.types[layout.values[index - 1].type.index - 1].size;
                    if (densifiedBytes + groupBytes > densifyBudgetBytes) continue;
                    std::sort(group.begin(), group.end());
                    densifiedBytes += groupBytes;
                    if (densifyStats)
                    {
                        densifyStats->bytes += groupBytes;
                        densifyStats->values += group.size();
                        ++densifyStats->groups;
                    }
                    for (auto index : group) densified[index] = 1;
                    densifyGroups.push_back(std::move(group));
                }
            }
            for (const auto &group : densifyGroups)
                for (auto index : group)
                {
                    auto &slot = layout.values[index - 1];
                    slot.offset = allocate(layout.boundaryBytes, layout.types[slot.type.index - 1]);
                }
            for (unsigned tier = 0; tier < 2; ++tier)
                for (const auto &value : model.values())
                {
                    auto &slot = layout.values[value.id.index - 1];
                    if (slot.kind != CpuStorageKind::Boundary || densified[value.id.index] ||
                        (tier == 0) != (commitOperands[value.id.index] != 0)) continue;
                    slot.offset = allocate(layout.boundaryBytes, layout.types[slot.type.index - 1]);
                }
            for (auto &frame : layout.localFrames) frame.size = alignUp(frame.size, frame.alignment);
            layout.objectBytes = alignUp(layout.objectBytes, 8);
            layout.boundaryBytes = alignUp(layout.boundaryBytes, 8);
            if (readCaches) layout.helperReadCaches = planHelperReadCaches(model, tree, layout);
            return layout;
        }

        class LayoutDataPass final : public Pass
        {
        public:
            LayoutDataPass() : Pass("cpu.st.layout-data", PassKind::BackendMapping) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                const auto *previous = model.cpuMapping();
                if (!previous || previous->stage != CpuMappingStage::EmitFunctions)
                {
                    diagnostics.error("requires cpu.st.pack-emit-functions output", name());
                    return {false, false, {}};
                }
                DensifyBoundaryStats densifyStats;
                auto layout = buildLayout(model, previous->partitionTree, true, true, &densifyStats);
                diagnostics.info("cpu_types=" + std::to_string(layout.types.size()) +
                                 " object_bytes=" + std::to_string(layout.objectBytes) +
                                 " boundary_bytes=" + std::to_string(layout.boundaryBytes) +
                                 " runtime_bytes=" + std::to_string(layout.runtimeBytes) +
                                 " helper_read_caches=" + std::to_string(layout.helperReadCaches->size()) +
                                 " densified_boundary_values=" + std::to_string(densifyStats.values) +
                                 " densified_bytes=" + std::to_string(densifyStats.bytes) +
                                 " densified_groups=" + std::to_string(densifyStats.groups), name());
                auto mapping = *previous;
                mapping.dataLayout = std::move(layout);
                mapping.stage = CpuMappingStage::DataLayout;
                model.setCpuMapping(std::move(mapping));
                return {true, true, {}};
            }
        };
    }

    bool verifyCpuDataLayout(const GrhSimModel &model, const CpuBackendMapping &mapping,
                             diag::Diagnostics &diagnostics)
    {
        if (mapping.stage < CpuMappingStage::DataLayout && !mapping.dataLayout) return true;
        if (mapping.stage < CpuMappingStage::DataLayout || !mapping.dataLayout)
        {
            diagnostics.error("CPU layout payload disagrees with mapping stage", "cpu.layout");
            return false;
        }
        // v1 uses a canonical layout, checking both coverage and every required lifetime boundary.
        const bool readCaches = mapping.dataLayout->helperReadCaches.has_value();
        if (*mapping.dataLayout == buildLayout(model, mapping.partitionTree, readCaches)) return true;
        // Archived checkpoints written before event-gated endpoint boundary
        // inputs were densified keep the legacy canonical form; accept it so
        // re-emit flows can upgrade the mapping by rerunning cpu.st.layout-data.
        if (*mapping.dataLayout == buildLayout(model, mapping.partitionTree, readCaches, false)) return true;
        diagnostics.error("CPU data layout differs from canonical types, storage or runtime slots", "cpu.layout");
        return false;
    }

    void registerCpuLayoutPasses(PassRegistry &registry)
    {
        std::string error;
        if (!registry.registerPass("cpu.st.layout-data", PassKind::BackendMapping,
            [](std::span<const std::string_view> args, std::string &error) -> std::unique_ptr<Pass> {
                if (!args.empty()) { error = "cpu.st.layout-data does not accept arguments"; return {}; }
                return std::make_unique<LayoutDataPass>();
            }, error)) throw std::logic_error(error);
    }
}
