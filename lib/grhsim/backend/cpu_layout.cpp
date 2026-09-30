#include "grhsim/backend/cpu.hpp"

#include "grhsim/backend/cpu_phase_common.hpp"

#include "grhsim/pass/pass.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

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

        // ===== Six-phase named-store layout (cpu.st.layout-named-stores) =====

        // Sanitize + global-uniquify engine for store field names. One
        // namespace spans every store; collisions resolve deterministically
        // with "_<id>" then "_<id>_<seq>" suffixes.
        class StoreNamer
        {
        public:
            explicit StoreNamer(GrhSimModel &model) : model_(model) {}

            StringId claim(std::string base, uint32_t id)
            {
                base = sanitize(base);
                if (base.empty()) base = "field_" + std::to_string(id);
                std::string name = base;
                if (!used_.insert(name).second)
                {
                    ++renamed_;
                    name = base + "_" + std::to_string(id);
                    uint64_t seq = 0;
                    while (!used_.insert(name).second)
                    {
                        ++renamed_;
                        name = base + "_" + std::to_string(id) + "_" + std::to_string(seq++);
                    }
                }
                return model_.intern(name);
            }

            uint64_t renamed() const { return renamed_; }

            static std::string sanitize(std::string_view text)
            {
                std::string out;
                out.reserve(text.size() + 2);
                for (const char c : text)
                {
                    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                    (c >= '0' && c <= '9') || c == '_';
                    out.push_back(ok ? c : '_');
                }
                if (!out.empty() && out.front() >= '0' && out.front() <= '9') out.insert(out.begin(), '_');
                if (keyword(out)) out.push_back('_');
                return out;
            }

        private:
            static bool keyword(std::string_view text)
            {
                static const std::unordered_set<std::string_view> words{
                    "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool",
                    "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t", "class",
                    "compl", "concept", "const", "consteval", "constexpr", "constinit", "continue",
                    "co_await", "co_return", "co_yield", "decltype", "default", "delete", "do",
                    "double", "else", "enum", "explicit", "export", "extern", "false", "float",
                    "for", "friend", "goto", "if", "inline", "int", "long", "namespace", "new",
                    "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private",
                    "protected", "public", "register", "reinterpret_cast", "requires", "return",
                    "short", "signed", "sizeof", "static", "static_assert", "struct", "switch",
                    "template", "this", "throw", "true", "try", "typedef", "typeid", "typename",
                    "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t",
                    "while", "xor", "xor_eq"};
                return words.count(text) != 0;
            }

            GrhSimModel &model_;
            std::unordered_set<std::string> used_;
            uint64_t renamed_ = 0;
        };

        // Physical field type for a semantic type: 2-state logic picks
        // Bool(1)/UInt(8/16/32/64) by width, wider values map to an array of
        // UInt64 words, four-state doubles into a two-element array.
        CpuTypeId namedStorePhysicalType(TypeMapper &mapper, const GrhSimModel &model, TypeId semantic)
        {
            const auto &type = model.types()[semantic.index - 1];
            switch (type.kind)
            {
            case TypeKind::Logic:
            {
                CpuTypeId base;
                if (type.width == 1 && !type.isSigned) base = mapper.scalar(CpuTypeKind::Bool);
                else if (type.width <= 64)
                {
                    const auto rounded = type.width <= 8 ? 8 : type.width <= 16 ? 16 :
                                         type.width <= 32 ? 32 : 64;
                    base = mapper.scalar(type.isSigned ? CpuTypeKind::SInt : CpuTypeKind::UInt, rounded);
                }
                else base = mapper.array(mapper.scalar(CpuTypeKind::UInt, 64),
                                         (uint64_t(type.width) + 63) / 64);
                if (type.domain == LogicDomain::FourState) return mapper.array(base, 2);
                return base;
            }
            case TypeKind::Real: return mapper.scalar(CpuTypeKind::F64, 64);
            case TypeKind::String: return mapper.scalar(CpuTypeKind::String);
            case TypeKind::Array:
                return mapper.array(namedStorePhysicalType(mapper, model, type.elementType), type.count);
            }
            throw std::logic_error("unknown semantic type kind");
        }

        // Fallback prefix for unnamed values: producer op category + value id.
        std::string opNamePrefix(std::string_view type)
        {
            if (type == "core.input.read") return "readR";
            if (type == "core.state.read") return "readS";
            if (type == "core.state.memRead") return "readM";
            if (type == "core.compute.sliceStatic") return "slice";
            if (type == "core.compute.constant") return "cnst";
            const auto at = type.find_last_of('.');
            return std::string(at == std::string_view::npos ? type : type.substr(at + 1));
        }

        std::string signalNameOf(const GrhSimModel &model, ValueId value,
                                 std::span<const OpId> producer)
        {
            const auto &entry = model.values()[value.index - 1];
            if (entry.name && !model.text(entry.name).empty()) return std::string(model.text(entry.name));
            const auto source = producer[value.index];
            const auto prefix = source ? opNamePrefix(model.text(model.operations()[source.index - 1].opType))
                                       : std::string("value");
            return prefix + "_" + std::to_string(value.index);
        }

        struct NamedStoreBuilder
        {
            const CpuDataLayout &layout;
            CpuNamedStore &store;
            uint64_t end = 0;
            uint32_t maxAlign = 1;
            void add(StringId name, CpuTypeId type, StateId state, ValueId value, uint32_t aux)
            {
                const auto &physical = layout.types[type.index - 1];
                store.fields.push_back({name, type, allocate(end, physical), state, value, aux});
                maxAlign = std::max(maxAlign, physical.alignment);
            }
            void finish() { store.sizeBytes = end == 0 ? 0 : alignUp(end, maxAlign); }
        };

        struct NamedStoreStats
        {
            uint64_t regLatchFields = 0, memFields = 0, boundaryFields = 0, prevEventFields = 0;
            uint64_t eventActBits = 0, timeslotFlags = 0, activeBytes = 0, renamed = 0;
        };

        // Fills the seven named stores (regLatch/mem/boundary/prevEvent/
        // eventAct/timeslotTrigger/activeFlags) and the layout type table.
        // The legacy layout fields stay empty on the six-phase pipeline.
        std::vector<CpuNamedStore> buildNamedStores(GrhSimModel &model, const CpuPartitionTree &tree,
                                                    CpuDataLayout &layout, NamedStoreStats &stats)
        {
            TypeMapper mapper(layout);
            StoreNamer namer(model);
            const auto supernodeOf = generalSupernodeOf(model, tree);
            const auto boundary = sixPhaseBoundaryValues(model, tree, supernodeOf);
            const auto supernodeCount = static_cast<uint32_t>(generalSupernodeOrder(tree).size());
            std::vector<OpId> producer(model.values().size() + 1);
            for (const auto &op : model.operations())
                for (const auto value : model.results(op)) producer[value.index] = op.id;

            std::vector<CpuNamedStore> stores(7);

            // regLatch / mem: one named field per state (array states go to
            // mem, everything else to regLatch).
            auto &regLatch = stores[0];
            regLatch.kind = CpuNamedStoreKind::RegLatch;
            auto &mem = stores[1];
            mem.kind = CpuNamedStoreKind::Mem;
            NamedStoreBuilder regBuilder{layout, regLatch}, memBuilder{layout, mem};
            for (const auto &state : model.states())
            {
                const auto physical = namedStorePhysicalType(mapper, model, state.type);
                const auto name = namer.claim(model.text(state.name).empty() ?
                                              "state_" + std::to_string(state.id.index) :
                                              std::string(model.text(state.name)), state.id.index);
                const bool isArray = model.types()[state.type.index - 1].kind == TypeKind::Array;
                (isArray ? memBuilder : regBuilder).add(name, physical, state.id, {}, 0);
            }
            regBuilder.finish();
            memBuilder.finish();
            stats.regLatchFields = regLatch.fields.size();
            stats.memFields = mem.fields.size();

            // boundary: input ports, then the cross-supernode / General->Mem
            // value set. Mem write operand slots carry the dedicated
            // <mem>__w<idx>__<role> names (first writer wins for a shared
            // operand value); everything else uses the source signal name.
            auto &boundaryStore = stores[2];
            boundaryStore.kind = CpuNamedStoreKind::Boundary;
            struct SlotName { StateId state; uint32_t write; std::string role; };
            std::vector<std::optional<SlotName>> slotNames(model.values().size() + 1);
            {
                std::vector<uint32_t> perMem(model.states().size() + 1, 0);
                for (const auto &op : model.operations())
                {
                    if (op.phase != SimPhase::Mem) continue;
                    const auto type = model.text(op.opType);
                    if (!isCpuPhaseMemWriteOp(type)) continue;
                    const auto refs = model.objectRefs(op);
                    if (refs.empty() || refs.front().kind != ObjectKind::State) continue;
                    const auto state = StateId{refs.front().index, refs.front().generation};
                    const auto writeIndex = perMem[state.index]++;
                    const auto operands = model.operands(op);
                    const auto slot = [&](uint32_t position, std::string role) {
                        if (position >= operands.size()) return;
                        const auto value = operands[position];
                        if (!slotNames[value.index])
                            slotNames[value.index] = SlotName{state, writeIndex, std::move(role)};
                    };
                    if (type == "core.state.memWrite")
                    {
                        slot(0, "enable"); slot(1, "addr"); slot(2, "data"); slot(3, "mask");
                    }
                    else if (type == "core.state.memFill" || type == "core.state.memAssign")
                    {
                        slot(0, "enable"); slot(1, "data");
                    }
                    else // memWriteSeq: (enable, addr, data) triples
                    {
                        for (uint32_t triple = 0; triple * 3 + 2 < operands.size(); ++triple)
                        {
                            slot(triple * 3, "enable" + std::to_string(triple));
                            slot(triple * 3 + 1, "addr" + std::to_string(triple));
                            slot(triple * 3 + 2, "data" + std::to_string(triple));
                        }
                    }
                }
            }
            NamedStoreBuilder boundaryBuilder{layout, boundaryStore};
            for (const auto &input : model.inputs())
            {
                const auto physical = namedStorePhysicalType(mapper, model, input.type);
                const auto name = namer.claim(model.text(input.name).empty() ?
                                              "input_" + std::to_string(input.id.index) :
                                              std::string(model.text(input.name)), input.id.index);
                boundaryBuilder.add(name, physical, {}, {}, input.id.index - 1);
            }
            for (const auto &value : model.values())
            {
                if (!boundary[value.id.index]) continue;
                std::string base;
                uint32_t claimId = value.id.index;
                if (slotNames[value.id.index])
                {
                    const auto &slot = *slotNames[value.id.index];
                    base = StoreNamer::sanitize(model.text(model.states()[slot.state.index - 1].name)) +
                           "__w" + std::to_string(slot.write) + "__" + slot.role;
                }
                else base = signalNameOf(model, value.id, producer);
                const auto physical = namedStorePhysicalType(mapper, model, value.type);
                boundaryBuilder.add(namer.claim(std::move(base), claimId), physical, {}, value.id, 0);
            }
            boundaryBuilder.finish();
            stats.boundaryFields = boundaryStore.fields.size();

            // prevEvent: one slot per (event,edge) cluster, named
            // <signal>__<edge>, typed after the event value, aux = cluster.
            // eventAct: one bit per cluster, same naming (uniquified), aux =
            // act index, byte-packed offsets.
            uint32_t edgeDetCount = 0;
            const auto acts = eventClusterActs(model, edgeDetCount);
            std::map<int64_t, OpId> detOfAct;
            for (const auto &op : model.operations())
            {
                if (model.text(op.opType) != "core.event.edgeDet") continue;
                const Parameter *act = findCpuPhaseParameter(model, model.parameters(op), "act");
                const auto index = std::get_if<int64_t>(&act->value);
                if (!detOfAct.count(*index)) detOfAct[*index] = op.id;
            }
            auto &prevEvent = stores[3];
            prevEvent.kind = CpuNamedStoreKind::PrevEvent;
            auto &eventAct = stores[4];
            eventAct.kind = CpuNamedStoreKind::EventAct;
            NamedStoreBuilder prevBuilder{layout, prevEvent};
            const auto boolType = mapper.scalar(CpuTypeKind::Bool);
            uint32_t maxAct = 0;
            for (const auto act : acts)
            {
                if (act > std::numeric_limits<uint32_t>::max())
                    throw std::runtime_error("event act index exceeds 32 bits");
                const auto &det = model.operations()[detOfAct.at(act).index - 1];
                const auto event = model.operands(det).front();
                const Parameter *edge = findCpuPhaseParameter(model, model.parameters(det), "edge");
                const auto *edgeText = edge ? std::get_if<std::string>(&edge->value) : nullptr;
                if (!edgeText) throw std::runtime_error("edgeDet edge must be a string");
                const auto base = signalNameOf(model, event, producer) + "__" + *edgeText;
                const auto physical = namedStorePhysicalType(mapper, model,
                                                             model.values()[event.index - 1].type);
                prevBuilder.add(namer.claim(base, event.index), physical, {}, event,
                                static_cast<uint32_t>(act));
                eventAct.fields.push_back({namer.claim(base, event.index), boolType,
                                           uint64_t(act) / 8, {}, {}, static_cast<uint32_t>(act)});
                maxAct = std::max(maxAct, static_cast<uint32_t>(act));
            }
            prevBuilder.finish();
            eventAct.sizeBytes = acts.empty() ? 0 : maxAct / 8 + 1;
            stats.prevEventFields = prevEvent.fields.size();
            stats.eventActBits = eventAct.fields.size();

            // timeslotTrigger: one byte per event-carrying timeslot task,
            // named after the task, aux = timeslotFlag index.
            auto &timeslot = stores[5];
            timeslot.kind = CpuNamedStoreKind::TimeslotTrigger;
            uint32_t maxFlag = 0;
            for (const auto &op : model.operations())
            {
                if (op.phase != SimPhase::Output || model.text(op.opType) != "core.system.task") continue;
                const Parameter *flag = findCpuPhaseParameter(model, model.parameters(op), "timeslotFlag");
                if (!flag) continue;
                const auto *index = std::get_if<int64_t>(&flag->value);
                if (!index || *index < 0 || *index > std::numeric_limits<uint32_t>::max())
                    throw std::runtime_error("timeslotFlag must be a non-negative int64");
                std::string base = "task_" + std::to_string(op.id.index);
                const Parameter *taskName = findCpuPhaseParameter(model, model.parameters(op), "name");
                if (const auto *text = taskName ? std::get_if<std::string>(&taskName->value) : nullptr)
                    if (!text->empty()) base = *text;
                timeslot.fields.push_back({namer.claim(std::move(base), op.id.index), boolType,
                                           uint64_t(*index), {}, {}, static_cast<uint32_t>(*index)});
                maxFlag = std::max(maxFlag, static_cast<uint32_t>(*index));
            }
            timeslot.sizeBytes = timeslot.fields.empty() ? 0 : maxFlag + 1;
            stats.timeslotFlags = timeslot.fields.size();

            // activeFlags: eventActiveFlag/dataActiveFlag/dataActiveFlagNext
            // byte arrays with one byte per General supernode. aux carries the
            // supernode count (the ordinal space of the M4 spec); bit/byte
            // index == supernode ordinal by definition.
            auto &active = stores[6];
            active.kind = CpuNamedStoreKind::ActiveFlags;
            NamedStoreBuilder activeBuilder{layout, active};
            const auto byteArray = mapper.array(mapper.scalar(CpuTypeKind::UInt, 8),
                                                std::max<uint32_t>(supernodeCount, 1));
            activeBuilder.add(namer.claim("eventActiveFlag", 0), byteArray, {}, {}, supernodeCount);
            activeBuilder.add(namer.claim("dataActiveFlag", 1), byteArray, {}, {}, supernodeCount);
            activeBuilder.add(namer.claim("dataActiveFlagNext", 2), byteArray, {}, {}, supernodeCount);
            activeBuilder.finish();
            stats.activeBytes = active.sizeBytes;

            stats.renamed = namer.renamed();
            return stores;
        }

        // Structural named-store verification (M4): name uniqueness, offset
        // alignment, coverage completeness, type validity. No rebuild
        // comparison — the payload is opaque beyond these checks.
        bool verifySixPhaseNamedStores(const GrhSimModel &model, const CpuBackendMapping &mapping,
                                       diag::Diagnostics &diagnostics)
        {
            const auto error = [&](std::string message) {
                diagnostics.error(std::move(message), "cpu.layout");
                return false;
            };
            if (!mapping.dataLayout || !mapping.dataLayout->namedStores)
                return error("six-phase CPU mapping requires the named-store layout payload");
            const auto &layout = *mapping.dataLayout;
            // helperReadCaches is positional in the v2 JSON tail: loading a
            // named-store layout materializes it as an engaged empty array,
            // which is semantically identical to absent.
            if (!layout.objects.empty() || !layout.values.empty() || !layout.localFrames.empty() ||
                !layout.runtime.empty() || layout.objectBytes != 0 || layout.boundaryBytes != 0 ||
                layout.runtimeBytes != 0 ||
                (layout.helperReadCaches && !layout.helperReadCaches->empty()))
                return error("six-phase layout must not carry legacy layout payload");
            const auto &types = layout.types;
            for (std::size_t i = 0; i < types.size(); ++i)
            {
                if (types[i].id.index != i + 1) return error("CPU type ids must be dense");
                if (types[i].kind == CpuTypeKind::Array &&
                    (!types[i].elementType || types[i].elementType.index > i))
                    return error("CPU array element type must precede the array");
            }
            const auto findType = [&](CpuTypeKind kind, uint32_t width, CpuTypeId element, uint64_t count) {
                CpuTypeId found;
                for (const auto &type : types)
                    if (type.kind == kind && type.width == width && type.elementType == element &&
                        type.count == count)
                        return type.id;
                return found;
            };
            const std::function<CpuTypeId(TypeId)> physicalType = [&](TypeId semantic) -> CpuTypeId {
                const auto &type = model.types()[semantic.index - 1];
                switch (type.kind)
                {
                case TypeKind::Logic:
                {
                    CpuTypeId base;
                    if (type.width == 1 && !type.isSigned) base = findType(CpuTypeKind::Bool, 0, {}, 0);
                    else if (type.width <= 64)
                    {
                        const auto rounded = type.width <= 8 ? 8 : type.width <= 16 ? 16 :
                                             type.width <= 32 ? 32 : 64;
                        base = findType(type.isSigned ? CpuTypeKind::SInt : CpuTypeKind::UInt,
                                        rounded, {}, 0);
                    }
                    else
                    {
                        const auto word = findType(CpuTypeKind::UInt, 64, {}, 0);
                        if (word) base = findType(CpuTypeKind::Array, 0, word,
                                                  (uint64_t(type.width) + 63) / 64);
                    }
                    if (base && type.domain == LogicDomain::FourState)
                        return findType(CpuTypeKind::Array, 0, base, 2);
                    return base;
                }
                case TypeKind::Real: return findType(CpuTypeKind::F64, 64, {}, 0);
                case TypeKind::String: return findType(CpuTypeKind::String, 0, {}, 0);
                case TypeKind::Array:
                {
                    const auto element = physicalType(type.elementType);
                    return element ? findType(CpuTypeKind::Array, 0, element, type.count) : CpuTypeId{};
                }
                }
                return {};
            };
            const auto &stores = *layout.namedStores;
            constexpr std::array<CpuNamedStoreKind, 7> kinds{
                CpuNamedStoreKind::RegLatch, CpuNamedStoreKind::Mem, CpuNamedStoreKind::Boundary,
                CpuNamedStoreKind::PrevEvent, CpuNamedStoreKind::EventAct,
                CpuNamedStoreKind::TimeslotTrigger, CpuNamedStoreKind::ActiveFlags};
            if (stores.size() != kinds.size())
                return error("named-store layout must hold all seven stores");
            for (std::size_t i = 0; i < kinds.size(); ++i)
                if (stores[i].kind != kinds[i])
                    return error("named stores must be ordered regLatch, mem, boundary, prevEvent, eventAct, timeslotTrigger, activeFlags");
            {
                std::unordered_set<std::string_view> names;
                for (const auto &store : stores)
                    for (const auto &field : store.fields)
                    {
                        if (!field.name || model.text(field.name).empty())
                            return error("named-store field must have a name");
                        if (!names.insert(model.text(field.name)).second)
                            return error("named-store field names must be globally unique");
                    }
            }
            const auto checkStructStore = [&](const CpuNamedStore &store) {
                auto fields = store.fields;
                std::sort(fields.begin(), fields.end(),
                          [](const auto &a, const auto &b) { return a.offset < b.offset; });
                uint64_t end = 0;
                uint32_t maxAlign = 1;
                for (const auto &field : fields)
                {
                    if (!field.type || field.type.index > types.size())
                        return error("named-store field type is invalid");
                    const auto &type = types[field.type.index - 1];
                    if (field.offset % type.alignment != 0 || field.offset < end)
                        return error("named-store field offsets must be aligned and non-overlapping");
                    end = field.offset + type.size;
                    maxAlign = std::max(maxAlign, type.alignment);
                }
                const auto expected = end == 0 ? 0 : alignUp(end, maxAlign);
                if (store.sizeBytes != expected)
                    return error("named-store size bytes disagree with its fields");
                return true;
            };
            const auto checkStateStore = [&](const CpuNamedStore &store, bool arrayStates) {
                if (!checkStructStore(store)) return false;
                std::vector<bool> seen(model.states().size() + 1, false);
                for (const auto &field : store.fields)
                {
                    if (!field.state || field.state.index > model.states().size() || field.value)
                        return error("state store field must reference exactly one state");
                    const auto &state = model.states()[field.state.index - 1];
                    const bool isArray = model.types()[state.type.index - 1].kind == TypeKind::Array;
                    if (isArray != arrayStates) return error("state store field is in the wrong store");
                    if (seen[field.state.index]) return error("state store covers a state twice");
                    seen[field.state.index] = true;
                    if (field.type != physicalType(state.type))
                        return error("state store field type disagrees with its state");
                }
                for (const auto &state : model.states())
                {
                    const bool isArray = model.types()[state.type.index - 1].kind == TypeKind::Array;
                    if (isArray == arrayStates && !seen[state.id.index])
                        return error("state store misses a state");
                }
                return true;
            };
            if (!checkStateStore(stores[0], false) || !checkStateStore(stores[1], true)) return false;
            // boundary: every input port (aux = port index) plus exactly the
            // recomputed cross-supernode / General->Mem value set.
            {
                const auto &store = stores[2];
                if (!checkStructStore(store)) return false;
                std::vector<bool> seenPorts(model.inputs().size(), false), seenValues(model.values().size() + 1, false);
                for (const auto &field : store.fields)
                {
                    if (field.state) return error("boundary field must not reference a state");
                    if (field.value)
                    {
                        if (field.value.index > model.values().size() || seenValues[field.value.index])
                            return error("boundary value field is invalid or duplicated");
                        seenValues[field.value.index] = true;
                        if (field.type != physicalType(model.values()[field.value.index - 1].type))
                            return error("boundary value field type disagrees with its value");
                    }
                    else
                    {
                        if (field.aux >= model.inputs().size() || seenPorts[field.aux])
                            return error("boundary port field is invalid or duplicated");
                        seenPorts[field.aux] = true;
                        if (field.type != physicalType(model.inputs()[field.aux].type))
                            return error("boundary port field type disagrees with its input");
                    }
                }
                for (std::size_t i = 0; i < seenPorts.size(); ++i)
                    if (!seenPorts[i]) return error("boundary store misses an input port");
                const auto expected = sixPhaseBoundaryValues(model, mapping.partitionTree,
                                                             generalSupernodeOf(model, mapping.partitionTree));
                for (std::size_t i = 1; i < expected.size(); ++i)
                    if (expected[i] != seenValues[i])
                        return error("boundary store coverage disagrees with the cross-supernode value set");
            }
            // prevEvent / eventAct: one entry per (event,edge) cluster.
            uint32_t edgeDetCount = 0;
            const auto acts = eventClusterActs(model, edgeDetCount);
            std::map<int64_t, OpId> detOfAct;
            for (const auto &op : model.operations())
            {
                if (model.text(op.opType) != "core.event.edgeDet") continue;
                const Parameter *act = findCpuPhaseParameter(model, model.parameters(op), "act");
                const auto index = std::get_if<int64_t>(&act->value);
                if (!detOfAct.count(*index)) detOfAct[*index] = op.id;
            }
            const auto boolType = findType(CpuTypeKind::Bool, 0, {}, 0);
            if (!boolType) return error("named-store layout lacks the Bool type");
            {
                const auto &store = stores[3];
                if (!checkStructStore(store)) return false;
                if (store.fields.size() != acts.size())
                    return error("prev event store must hold one slot per event cluster");
                std::set<uint32_t> seen;
                for (const auto &field : store.fields)
                {
                    if (!detOfAct.count(field.aux) || !seen.insert(field.aux).second || field.state)
                        return error("prev event field cluster is invalid or duplicated");
                    const auto &det = model.operations()[detOfAct.at(field.aux).index - 1];
                    const auto event = model.operands(det).front();
                    if (field.value != event)
                        return error("prev event field value disagrees with its cluster");
                    if (field.type != physicalType(model.values()[event.index - 1].type))
                        return error("prev event field type disagrees with its event value");
                }
            }
            {
                const auto &store = stores[4];
                if (store.fields.size() != acts.size())
                    return error("event act store must hold one bit per event cluster");
                std::set<uint32_t> seen;
                uint32_t maxAct = 0;
                for (const auto &field : store.fields)
                {
                    if (!detOfAct.count(field.aux) || !seen.insert(field.aux).second ||
                        field.state || field.value)
                        return error("event act field cluster is invalid or duplicated");
                    if (field.type != boolType || field.offset != field.aux / 8)
                        return error("event act field must be a byte-packed Bool bit");
                    maxAct = std::max(maxAct, field.aux);
                }
                if (store.sizeBytes != (acts.empty() ? 0 : uint64_t(maxAct) / 8 + 1))
                    return error("event act store size bytes disagree with its bits");
            }
            // timeslotTrigger: one byte per event-carrying timeslot task.
            {
                const auto &store = stores[5];
                std::set<uint32_t> expected;
                for (const auto &op : model.operations())
                {
                    if (op.phase != SimPhase::Output || model.text(op.opType) != "core.system.task")
                        continue;
                    const Parameter *flag = findCpuPhaseParameter(model, model.parameters(op), "timeslotFlag");
                    if (!flag) continue;
                    const auto *index = std::get_if<int64_t>(&flag->value);
                    if (!index || *index < 0 || *index > std::numeric_limits<uint32_t>::max())
                        return error("timeslotFlag must be a non-negative int64");
                    expected.insert(static_cast<uint32_t>(*index));
                }
                if (store.fields.size() != expected.size())
                    return error("timeslot trigger store must hold one byte per timeslot task");
                uint32_t maxFlag = 0;
                std::set<uint32_t> seen;
                for (const auto &field : store.fields)
                {
                    if (!expected.count(field.aux) || !seen.insert(field.aux).second ||
                        field.state || field.value)
                        return error("timeslot trigger field flag is invalid or duplicated");
                    if (field.type != boolType || field.offset != field.aux)
                        return error("timeslot trigger field must be a Bool byte at its flag index");
                    maxFlag = std::max(maxFlag, field.aux);
                }
                if (store.sizeBytes != (store.fields.empty() ? 0 : uint64_t(maxFlag) + 1))
                    return error("timeslot trigger store size bytes disagree with its flags");
            }
            // activeFlags: the three byte arrays sized by the supernode count.
            {
                const auto &store = stores[6];
                const auto supernodeCount = static_cast<uint32_t>(
                    generalSupernodeOrder(mapping.partitionTree).size());
                const std::array<std::string_view, 3> names{"eventActiveFlag", "dataActiveFlag",
                                                            "dataActiveFlagNext"};
                if (store.fields.size() != names.size())
                    return error("active flags store must hold exactly three fields");
                const auto word = findType(CpuTypeKind::UInt, 8, {}, 0);
                const auto byteArray = word ? findType(CpuTypeKind::Array, 0, word,
                                                       std::max<uint32_t>(supernodeCount, 1))
                                            : CpuTypeId{};
                for (std::size_t i = 0; i < names.size(); ++i)
                {
                    const auto &field = store.fields[i];
                    if (model.text(field.name) != names[i])
                        return error("active flags fields must be eventActiveFlag, dataActiveFlag, dataActiveFlagNext");
                    if (field.aux != supernodeCount)
                        return error("active flags field aux must be the supernode count");
                    if (field.type != byteArray || field.offset != i * uint64_t(std::max<uint32_t>(supernodeCount, 1)))
                        return error("active flags field type or offset is wrong");
                }
                if (store.sizeBytes != 3 * uint64_t(std::max<uint32_t>(supernodeCount, 1)))
                    return error("active flags store size bytes disagree with its fields");
            }
            return true;
        }

        class LayoutNamedStoresPass final : public Pass
        {
        public:
            LayoutNamedStoresPass() : Pass("cpu.st.layout-named-stores", PassKind::BackendMapping) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                const auto *previous = model.cpuMapping();
                if (!previous || previous->stage != CpuMappingStage::GeneralFunctions)
                {
                    diagnostics.error("requires cpu.st.pack-general-functions output", name());
                    return {false, false, {}};
                }
                CpuBackendMapping mapping = *previous;
                CpuDataLayout layout;
                NamedStoreStats stats;
                layout.namedStores = buildNamedStores(model, mapping.partitionTree, layout, stats);
                diagnostics.info("cpu_types=" + std::to_string(layout.types.size()) +
                                 " reg_latch_fields=" + std::to_string(stats.regLatchFields) +
                                 " mem_fields=" + std::to_string(stats.memFields) +
                                 " boundary_fields=" + std::to_string(stats.boundaryFields) +
                                 " prev_event_fields=" + std::to_string(stats.prevEventFields) +
                                 " event_act_bits=" + std::to_string(stats.eventActBits) +
                                 " timeslot_flags=" + std::to_string(stats.timeslotFlags) +
                                 " active_flag_bytes=" + std::to_string(stats.activeBytes) +
                                 " renamed_count=" + std::to_string(stats.renamed), name());
                mapping.dataLayout = std::move(layout);
                mapping.stage = CpuMappingStage::LayoutNamedStores;
                model.setCpuMapping(std::move(mapping));
                return {true, true, {}};
            }
        };

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
        // M4 six-phase fork: a named-store payload switches verification to
        // the structural checks; legacy stages keep the canonical rebuild.
        if (mapping.stage >= CpuMappingStage::LayoutNamedStores)
            return verifySixPhaseNamedStores(model, mapping, diagnostics);
        if (mapping.stage < CpuMappingStage::DataLayout && !mapping.dataLayout) return true;
        if (mapping.stage < CpuMappingStage::DataLayout || !mapping.dataLayout)
        {
            diagnostics.error("CPU layout payload disagrees with mapping stage", "cpu.layout");
            return false;
        }
        // v1 uses a canonical layout, checking both coverage and every required lifetime boundary.
        const bool readCaches = mapping.dataLayout->helperReadCaches.has_value();
        // The M1 named-store shell has no canonical producer until the M4
        // named-store layout rework; treat it as opaque payload over the rebuild.
        auto canonical = buildLayout(model, mapping.partitionTree, readCaches);
        canonical.namedStores = mapping.dataLayout->namedStores;
        if (*mapping.dataLayout == canonical) return true;
        // Archived checkpoints written before event-gated endpoint boundary
        // inputs were densified keep the legacy canonical form; accept it so
        // re-emit flows can upgrade the mapping by rerunning cpu.st.layout-data.
        auto legacy = buildLayout(model, mapping.partitionTree, readCaches, false);
        legacy.namedStores = mapping.dataLayout->namedStores;
        if (*mapping.dataLayout == legacy) return true;
        diagnostics.error("CPU data layout differs from canonical types, storage or runtime slots", "cpu.layout");
        return false;
    }

    bool refreshCpuDataLayout(GrhSimModel &model, diag::Diagnostics &diagnostics)
    {
        const auto *previous = model.cpuMapping();
        if (!previous || previous->stage < CpuMappingStage::DataLayout || !previous->dataLayout)
        {
            diagnostics.error("CPU data layout refresh requires a data-layout stage mapping", "cpu.layout");
            return false;
        }
        const bool readCaches = previous->dataLayout->helperReadCaches.has_value();
        auto mapping = *previous;
        mapping.dataLayout = buildLayout(model, mapping.partitionTree, readCaches);
        model.setCpuMapping(std::move(mapping));
        return true;
    }

    void registerCpuLayoutPasses(PassRegistry &registry)
    {
        std::string error;
        if (!registry.registerPass("cpu.st.layout-data", PassKind::BackendMapping,
            [](std::span<const std::string_view> args, std::string &error) -> std::unique_ptr<Pass> {
                if (!args.empty()) { error = "cpu.st.layout-data does not accept arguments"; return {}; }
                return std::make_unique<LayoutDataPass>();
            }, error)) throw std::logic_error(error);
        if (!registry.registerPass("cpu.st.layout-named-stores", PassKind::BackendMapping,
            [](std::span<const std::string_view> args, std::string &error) -> std::unique_ptr<Pass> {
                if (!args.empty()) { error = "cpu.st.layout-named-stores does not accept arguments"; return {}; }
                return std::make_unique<LayoutNamedStoresPass>();
            }, error)) throw std::logic_error(error);
    }
}
