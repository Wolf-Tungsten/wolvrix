#include "grhsim/ir/verifier.hpp"
#include "grhsim/backend/cpu.hpp"

#include "grhsim/dialect/registry.hpp"
#include "grhsim/ir/model.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

namespace wolvrix::lib::grhsim
{

    namespace
    {
        template <typename IdType>
        bool validId(IdType id, std::size_t size) noexcept
        {
            return id.generation == 0 && id.index != 0 && id.index <= size;
        }

        bool validOptionalString(const GrhSimModel &model, StringId id) noexcept
        {
            return !id.valid() || model.strings().valid(id);
        }

        bool parameterValueValid(const ParameterValue &value)
        {
            return std::visit([](const auto &entry) {
                using T = std::decay_t<decltype(entry)>;
                if constexpr (std::is_same_v<T, double>)
                {
                    return std::isfinite(entry);
                }
                else if constexpr (std::is_same_v<T, std::vector<double>>)
                {
                    for (double item : entry)
                    {
                        if (!std::isfinite(item)) return false;
                    }
                    return true;
                }
                else
                {
                    return true;
                }
            }, value);
        }

        template <typename ContextFactory>
        bool validParameters(const GrhSimModel &model,
                             std::span<const Parameter> parameters,
                             diag::Diagnostics &diagnostics,
                             ContextFactory context)
        {
            bool ok = true;
            for (std::size_t i = 0; i < parameters.size(); ++i)
            {
                const Parameter &parameter = parameters[i];
                if (!model.strings().valid(parameter.name))
                {
                    diagnostics.error("parameter has an invalid name StringId", context());
                    ok = false;
                }
                else
                {
                    for (std::size_t j = 0; j < i; ++j)
                    {
                        if (parameters[j].name != parameter.name) continue;
                        diagnostics.error("parameter name is duplicated: " +
                                          std::string(model.text(parameter.name)), context());
                        ok = false;
                        break;
                    }
                }
                if (!parameterValueValid(parameter.value))
                {
                    diagnostics.error("parameter contains a non-finite floating-point value",
                                      context());
                    ok = false;
                }
            }
            return ok;
        }

        const Parameter *findParameter(const GrhSimModel &model,
                                       std::span<const Parameter> parameters,
                                       std::string_view name)
        {
            for (const Parameter &parameter : parameters)
            {
                if (model.text(parameter.name) == name) return &parameter;
            }
            return nullptr;
        }

        // Linear bit size of a type: the width of a Logic type is already the
        // total across all shape dimensions (stage-06); non-logic scalars have
        // no bits. Returns false on an invalid type reference.
        bool linearBitSize(const GrhSimModel &model, TypeId typeId, uint64_t &out)
        {
            if (!validId(typeId, model.types().size())) return false;
            const Type &type = model.types()[typeId.index - 1];
            switch (type.kind)
            {
            case TypeKind::Logic:
                out = type.width;
                return true;
            case TypeKind::Real:
            case TypeKind::String:
                out = 0;
                return true;
            }
            return false;
        }

        // M5d-1 declaration provenance contract: records anchor declared
        // symbols to the entities that currently realize them. Structural
        // rules only — whether a fold/merge/split redirection is semantically
        // justified belongs to the pass that performs it.
        bool verifyDeclProvenances(const GrhSimModel &model, diag::Diagnostics &diagnostics)
        {
            bool ok = true;
            auto error = [&](const std::string &message, const std::string &context) {
                diagnostics.error(message, context);
                ok = false;
            };
            for (std::size_t i = 0; i < model.declProvenances().size(); ++i)
            {
                const DeclProvenance &record = model.declProvenances()[i];
                const std::string context = "declProvenances[" + std::to_string(i) + "]";
                if (!model.strings().valid(record.symbol))
                {
                    error("declaration provenance has an invalid symbol StringId", context);
                    continue;
                }
                if (!model.isDeclaredSymbol(record.symbol))
                    error("declaration provenance symbol is not a declared symbol", context);
                if (record.origin.valid() && !validId(record.origin, model.origins().size()))
                    error("declaration provenance origin is out of range", context);
                // Stage-06: the declaration shape lives in the type system;
                // record.width is the total linear bit width.
                const uint64_t declLinear = record.width;
                std::vector<std::pair<uint64_t, uint64_t>> declRanges;
                for (std::size_t j = 0; j < record.slices.size(); ++j)
                {
                    const DeclProvenanceSlice &slice = record.slices[j];
                    const std::string sliceContext = context + ".slices[" + std::to_string(j) + "]";
                    if (slice.target == DeclProvenanceTarget::Function)
                    {
                        if (!validId(FuncId{slice.targetIndex, 0}, model.functions().size()))
                        {
                            error("provenance slice function target is out of range", sliceContext);
                            continue;
                        }
                        if (slice.width != 0 || slice.targetOffset != 0 || slice.declOffset != 0)
                            error("provenance slice on a function must be a whole-object marker",
                                  sliceContext);
                        continue;
                    }
                    TypeId targetType;
                    if (slice.target == DeclProvenanceTarget::Value)
                    {
                        if (!validId(ValueId{slice.targetIndex, 0}, model.values().size()))
                        {
                            error("provenance slice value target is out of range", sliceContext);
                            continue;
                        }
                        targetType = model.values()[slice.targetIndex - 1].type;
                    }
                    else
                    {
                        if (!validId(StateId{slice.targetIndex, 0}, model.states().size()))
                        {
                            error("provenance slice state target is out of range", sliceContext);
                            continue;
                        }
                        targetType = model.states()[slice.targetIndex - 1].type;
                    }
                    uint64_t targetLinear = 0;
                    if (!linearBitSize(model, targetType, targetLinear))
                    {
                        error("provenance slice target type is invalid or overflows", sliceContext);
                        continue;
                    }
                    if (slice.width == 0)
                    {
                        // Whole-object marker: only meaningful for bit-less
                        // declarations/targets; bit-carrying slices name a range.
                        if (slice.targetOffset != 0 || slice.declOffset != 0)
                            error("whole-object provenance slice must have zero offsets",
                                  sliceContext);
                        if (targetLinear != 0 && declLinear != 0)
                            error("whole-object provenance slice on bit-carrying declaration and "
                                  "target",
                                  sliceContext);
                        continue;
                    }
                    if (targetLinear == 0)
                        error("provenance slice carries bits but its target has none", sliceContext);
                    else if (slice.width > targetLinear || slice.targetOffset > targetLinear - slice.width)
                        error("provenance slice range exceeds the target", sliceContext);
                    if (declLinear == 0)
                        error("provenance slice carries bits but its declaration has none",
                              sliceContext);
                    else if (slice.width > declLinear || slice.declOffset > declLinear - slice.width)
                        error("provenance slice range exceeds the declaration", sliceContext);
                    else
                        declRanges.emplace_back(slice.declOffset, slice.width);
                }
                std::sort(declRanges.begin(), declRanges.end());
                for (std::size_t j = 1; j < declRanges.size(); ++j)
                    if (declRanges[j].first < declRanges[j - 1].first + declRanges[j - 1].second)
                        error("provenance slices overlap within the declaration", context);
            }
            return ok;
        }

        // M5d-4 state store classification contract (pass
        // grhsim.select-state-stores): the classification is the single
        // decision point for smallSinkStore/largeSinkStore attribution; consumers
        // (split-phases, layout, emit) must read the annotation instead of
        // re-deriving it from the state's type. Structural rules only:
        // classification is total once present, and a large-sink state's
        // writes go through the class-aware phase rule so P_large_sink owns
        // the in-place commit. The per-class NBA contract
        // itself (commit timing, old-value reads, partial writes,
        // declaration-order multi-write priority, multi-round accumulation)
        // is defined by the select-state-stores pass doc.
        bool verifyStateStores(const GrhSimModel &model, diag::Diagnostics &diagnostics)
        {
            bool ok = true;
            auto error = [&](const std::string &message, const std::string &context) {
                diagnostics.error(message, context);
                ok = false;
            };
            std::size_t classified = 0;
            for (const auto &state : model.states())
                if (state.storeClass != StateStoreClass::None) ++classified;
            if (classified == 0) return true;
            for (std::size_t i = 0; i < model.states().size(); ++i)
            {
                const StateObject &state = model.states()[i];
                const std::string context = "states[" + std::to_string(i) + "]";
                if (state.storeClass == StateStoreClass::None)
                {
                    error("state store classification is not total: state is unclassified "
                          "while other states carry a class",
                          context);
                    continue;
                }
                if (state.storeClass == StateStoreClass::LargeSink)
                {
                    if (!validId(state.type, model.types().size())) continue;
                    const Type &type = model.types()[state.type.index - 1];
                    if (type.kind != TypeKind::Logic)
                        error("large-sink store class requires a logic state", context);
                }
            }
            return ok;
        }


        // Stage-06 state access op helpers. The checks compare type GEOMETRY
        // (kind + total width + shape) and deliberately ignore sign/domain:
        // assignment moves bits, and the GRH side does not pin the signedness
        // of a port's data/result value to the declaration's (widths and
        // shapes are the contract).
        const Type *modelType(const GrhSimModel &model, TypeId id) noexcept
        {
            if (!validId(id, model.types().size())) return nullptr;
            return &model.types()[id.index - 1];
        }
        const Type *valueType(const GrhSimModel &model, ValueId value) noexcept
        {
            if (!validId(value, model.values().size())) return nullptr;
            return modelType(model, model.values()[value.index - 1].type);
        }
        const Type *stateRefType(const GrhSimModel &model, ObjectRef ref) noexcept
        {
            if (ref.kind != ObjectKind::State ||
                !validId(StateId{ref.index, 0}, model.states().size()))
                return nullptr;
            return modelType(model, model.states()[ref.index - 1].type);
        }
        bool scalarLogic(const Type *type) noexcept
        {
            return type && type->kind == TypeKind::Logic && type->shape.empty();
        }
        // Packed-first contract: shape is a structural view over a logic
        // value.  State conditions, masks, and array indices consume the
        // flattened bits, so a shaped logic value is valid here as well.
        bool logicAnyShape(const Type *type) noexcept
        {
            return type && type->kind == TypeKind::Logic;
        }
        // State-boundary value contract (stage-06): a value read from or
        // written to a state must carry the same TOTAL bit width; the shape
        // of the value may differ from the state's (or be absent) — shape is
        // a structural view over the flat bits (stage-01), and GRH legitimately
        // hands flat values to array-state ports. Non-logic states keep exact
        // type identity (they are interned singletons).
        bool stateValueGeometryOk(const Type *actual, const Type &expected) noexcept
        {
            if (!actual) return false;
            if (expected.kind != TypeKind::Logic) return actual->kind == expected.kind;
            return actual->kind == TypeKind::Logic && actual->width == expected.width;
        }
        // Geometry of a k-index access (1 <= k <= rank) into a shaped type:
        // the result/data width is elementWidth * product(shape[k:]) and its
        // shape is the shape[k:] suffix (empty at k == rank).
        bool indexedAccessGeometry(const Type &arrayType, std::size_t indexCount,
                                   uint32_t &width, std::span<const int64_t> &shape) noexcept
        {
            const std::optional<uint32_t> elementWidth = typeElementWidth(arrayType);
            if (!elementWidth || indexCount < 1 || indexCount > arrayType.shape.size())
                return false;
            uint64_t slots = 1;
            for (std::size_t dim = indexCount; dim < arrayType.shape.size(); ++dim)
                slots *= static_cast<uint64_t>(arrayType.shape[dim]);
            const uint64_t total = slots * *elementWidth;
            if (total == 0 || total > UINT32_MAX) return false;
            width = static_cast<uint32_t>(total);
            shape = std::span<const int64_t>(arrayType.shape).subspan(indexCount);
            return true;
        }
        // Indexed access consumes/produces packed bits.  The shape on the
        // value is only a view and need not match the state's remaining
        // dimensions (the GRH contract checks the addressed total width).
        bool indexedAccessTypeMatches(const Type *actual, uint32_t width) noexcept
        {
            return actual && actual->kind == TypeKind::Logic && actual->width == width;
        }
        // Trailing event operand count of a pre-lowering write op (the
        // event_edges parameter); 0 for the lowered event_acts form.
        std::size_t eventOperandCount(const GrhSimModel &model,
                                      std::span<const Parameter> parameters)
        {
            const Parameter *edges = findParameter(model, parameters, "event_edges");
            if (!edges) return 0;
            const auto *texts = std::get_if<std::vector<std::string>>(&edges->value);
            return texts ? texts->size() : 0;
        }

        // M2b exception: latchWrite may also carry Output — the timeslot
        // __tslot_prev_* write-backs live in P_output (verifyOutputLowering
        // constrains those states to Output-phase latchWrites).
        //
        // M5d-5 class-aware P_mem duty (pass grhsim.split-phases): on a
        // classified model a mem write's phase follows the target state's
        // store class — mem-class arrays commit in P_mem, regLatch-class
        // states (including small arrays) take the General next-buffer path.
        // Unclassified models keep the legacy all-Mem attribution, and an
        // unresolvable target ref falls back to it too (the structural checks
        // report the malformed ref separately).
        bool verifyPhaseAttribution(const GrhSimModel &model, diag::Diagnostics &diagnostics)
        {
            bool ok = true;
            const bool classified = model.hasStateStoreClassification();
            for (std::size_t i = 0; i < model.operations().size(); ++i)
            {
                const SimOp &op = model.operations()[i];
                if (!model.strings().valid(op.opType)) continue;
                const std::string_view name = model.text(op.opType);
                std::optional<SimPhase> required;
                bool unconditional = false;
                bool latchWrite = false;
                if (name == "core.event.edgeDet")
                {
                    required = SimPhase::Event;
                    unconditional = true;
                }
                else if (name == "core.output.write") required = SimPhase::Output;
                else if (name == "core.state.write" || name == "core.state.writeMask" ||
                         name == "core.state.writeArray" || name == "core.state.writeArrayMask" ||
                         name == "core.state.fill" || name == "core.state.latchWrite")
                {
                    latchWrite = name == "core.state.latchWrite";
                    // Stage-06 class-aware routing: writes to mem-class states
                    // commit in P_mem; writes to regLatch-class states take
                    // the General NBA path. On an unclassified model the
                    // target's shape decides (shaped = array = legacy Mem
                    // default); an unresolvable ref falls back the same way
                    // (the structural checks report it separately).
                    required = SimPhase::LargeSink;
                    std::span<const ObjectRef> refs;
                    try
                    {
                        refs = model.objectRefs(op);
                    }
                    catch (const std::exception &)
                    {
                        refs = {};
                    }
                    if (!refs.empty() && refs[0].kind == ObjectKind::State &&
                        validId(StateId{refs[0].index, 0}, model.states().size()))
                    {
                        const auto &state = model.states()[refs[0].index - 1];
                        if (classified)
                        {
                            if (state.storeClass == StateStoreClass::SmallSink)
                                required = SimPhase::General;
                        }
                        else if (validId(state.type, model.types().size()) &&
                                 model.types()[state.type.index - 1].shape.empty())
                            required = SimPhase::General;
                    }
                    else if (name == "core.state.write" || name == "core.state.writeMask")
                        required = SimPhase::General;
                }
                if (!required || (op.phase == SimPhase::None && !unconditional)) continue;
                if (latchWrite && op.phase == SimPhase::Output) continue;
                if (op.phase != *required)
                {
                    diagnostics.error(std::string(name) + " belongs to phase " +
                                      std::string(toString(*required)) + " but carries phase " +
                                      std::string(toString(op.phase)),
                                      "operations[" + std::to_string(i) + "]");
                    ok = false;
                }
            }
            return ok;
        }

        // M5d-5 semantic seal (B8, grhsim.verify --seal semantic): certifies
        // that the partition stage has completed and the semantic layer is
        // final. On top of the standing checks (which tolerate a partially
        // attributed mid-pipeline model) the seal requires:
        //  - total phase attribution (grhsim.split-phases has run);
        //  - no surviving raw event_edges parameters (grhsim.lower-edge-detect
        //    has lowered every event consumer to the event_acts form);
        //  - every Mem-phase write operand produced by a General-phase op, so
        //    P_mem only samples values that exist before it in a round
        //    (Event cone values never leave P_event; Output runs after the
        //    round loop).
        bool verifySemanticSeal(const GrhSimModel &model, diag::Diagnostics &diagnostics)
        {
            bool ok = true;
            auto error = [&](const std::string &message, const std::string &context) {
                diagnostics.error(message, context);
                ok = false;
            };
            std::vector<OpId> producer(model.values().size() + 1);
            for (const auto &op : model.operations())
                for (auto value : model.results(op)) producer[value.index] = op.id;
            for (std::size_t i = 0; i < model.operations().size(); ++i)
            {
                const SimOp &op = model.operations()[i];
                const std::string context = "operations[" + std::to_string(i) + "]";
                if (op.phase == SimPhase::None)
                    error("semantic seal requires total phase attribution "
                          "(grhsim.split-phases)", context);
                if (!model.strings().valid(op.opType)) continue;
                const std::string_view name = model.text(op.opType);
                std::span<const Parameter> params;
                std::span<const ValueId> operands;
                try
                {
                    params = model.parameters(op);
                    operands = model.operands(op);
                }
                catch (const std::exception &)
                {
                    continue; // malformed ranges are reported by the per-op check
                }
                for (const auto &param : params)
                    if (model.strings().valid(param.name) &&
                        model.text(param.name) == "event_edges")
                        error("semantic seal requires the lowered event_acts form; "
                              "event_edges survived (grhsim.lower-edge-detect)", context);
                if (op.phase != SimPhase::LargeSink) continue;
                for (const auto operand : operands)
                {
                    if (operand.generation != 0 || !operand.valid() ||
                        operand.index >= producer.size())
                        continue; // dangling operands are reported structurally
                    const auto source = producer[operand.index];
                    if (!source) continue;
                    const auto sourcePhase = model.operations()[source.index - 1].phase;
                    if (sourcePhase != SimPhase::General)
                        error(std::string(name) +
                                  " operand is not produced in P_general; P_mem samples "
                                  "General-phase values (producer carries phase " +
                                  std::string(toString(sourcePhase)) + ")",
                              context);
                }
            }
            return ok;
        }

        // Cross-model edgeDet clustering checks: act/prev are unique indices
        // into eventActStore/prevEventStore, and (event value, edge) clusters
        // are deduplicated so every consumer of one cluster shares a detector.
        bool verifyEdgeDetUniqueness(const GrhSimModel &model, diag::Diagnostics &diagnostics)
        {
            bool ok = true;
            std::unordered_set<int64_t> acts;
            std::unordered_set<int64_t> prevs;
            std::unordered_set<std::string> clusters;
            for (std::size_t i = 0; i < model.operations().size(); ++i)
            {
                const SimOp &op = model.operations()[i];
                if (!model.strings().valid(op.opType) ||
                    model.text(op.opType) != "core.event.edgeDet") continue;
                const std::string context = "operations[" + std::to_string(i) + "]";
                // Malformed signatures are reported by the per-op check; skip.
                std::span<const ValueId> operands;
                std::span<const Parameter> parameters;
                try
                {
                    operands = model.operands(op);
                    parameters = model.parameters(op);
                }
                catch (const std::exception &)
                {
                    continue;
                }
                if (operands.size() != 1) continue;
                const Parameter *edge = findParameter(model, parameters, "edge");
                const Parameter *act = findParameter(model, parameters, "act");
                const Parameter *prev = findParameter(model, parameters, "prev");
                if (act && std::holds_alternative<int64_t>(act->value) &&
                    std::get<int64_t>(act->value) >= 0 &&
                    !acts.insert(std::get<int64_t>(act->value)).second)
                {
                    diagnostics.error("core.event.edgeDet act index is not unique", context);
                    ok = false;
                }
                if (prev && std::holds_alternative<int64_t>(prev->value) &&
                    std::get<int64_t>(prev->value) >= 0 &&
                    !prevs.insert(std::get<int64_t>(prev->value)).second)
                {
                    diagnostics.error("core.event.edgeDet prev index is not unique", context);
                    ok = false;
                }
                if (!edge || !std::holds_alternative<std::string>(edge->value)) continue;
                const std::string &edgeText = std::get<std::string>(edge->value);
                if (edgeText != "posedge" && edgeText != "negedge" && edgeText != "both") continue;
                if (!clusters.insert(std::to_string(operands.front().index) + '\x1f' + edgeText).second)
                {
                    diagnostics.error("core.event.edgeDet (event, edge) cluster is duplicated", context);
                    ok = false;
                }
            }
            return ok;
        }

        // M2 event-lowering invariants (enabled once the model carries the
        // lowered event_acts form, i.e. after grhsim.lower-edge-detect ran):
        // no event_edges may survive; event_acts entries must resolve into the
        // edgeDet cluster (act) set; rewritten consumers must have their
        // event-free operand/ref shape; the P_event cone must be
        // self-contained; and General ops must not read Event-phase values.
        bool verifyEventLowering(const GrhSimModel &model, diag::Diagnostics &diagnostics)
        {
            std::unordered_set<int64_t> acts;
            bool lowered = false;
            for (const auto &op : model.operations())
            {
                if (!model.strings().valid(op.opType)) continue;
                if (model.text(op.opType) == "core.event.edgeDet")
                {
                    const Parameter *act = findParameter(model, model.parameters(op), "act");
                    if (act && std::holds_alternative<int64_t>(act->value))
                        acts.insert(std::get<int64_t>(act->value));
                }
                if (findParameter(model, model.parameters(op), "event_acts")) lowered = true;
            }
            if (!lowered) return true;

            bool ok = true;
            std::vector<uint32_t> producers(model.values().size() + 1, 0);
            for (const auto &op : model.operations())
                for (const auto result : model.results(op))
                    if (result.generation == 0 && result.index < producers.size())
                        producers[result.index] = op.id.index;
            for (std::size_t i = 0; i < model.operations().size(); ++i)
            {
                const SimOp &op = model.operations()[i];
                const std::string context = "operations[" + std::to_string(i) + "]";
                std::span<const ValueId> operands;
                std::span<const ObjectRef> refs;
                std::span<const Parameter> parameters;
                try
                {
                    operands = model.operands(op);
                    refs = model.objectRefs(op);
                    parameters = model.parameters(op);
                }
                catch (const std::exception &)
                {
                    continue;
                }
                if (findParameter(model, parameters, "event_edges"))
                {
                    diagnostics.error("event_edges survives the edge-detect lowering",
                                      context);
                    ok = false;
                }
                const Parameter *eventActs = findParameter(model, parameters, "event_acts");
                if (eventActs)
                {
                    const auto *indices = std::get_if<std::vector<int64_t>>(&eventActs->value);
                    if (!indices)
                    {
                        diagnostics.error("event_acts must be an int64 array", context);
                        ok = false;
                    }
                    else
                    {
                        for (const int64_t index : *indices)
                        {
                            if (index < 0 || !acts.contains(index))
                            {
                                diagnostics.error("event_acts index is not an edgeDet act "
                                                  "cluster index",
                                                  context);
                                ok = false;
                            }
                        }
                    }
                    const std::string_view name = model.strings().valid(op.opType)
                                                      ? model.text(op.opType)
                                                      : std::string_view{};
                    bool shapeOk = true;
                    if (name == "core.state.write" || name == "core.state.writeMask")
                        shapeOk = operands.size() >= 2;
                    else if (name == "core.state.writeArray" ||
                             name == "core.state.writeArrayMask")
                        shapeOk = operands.size() >= 3;
                    else if (name == "core.state.fill")
                        shapeOk = operands.size() == 2;
                    else if (name == "core.system.task") shapeOk = refs.empty();
                    else if (name == "core.dpi.call") shapeOk = refs.size() == 1;
                    else shapeOk = false;
                    if (!shapeOk)
                    {
                        diagnostics.error(std::string(name) +
                                              " with event_acts must use its event-free "
                                              "operand/object-ref shape",
                                          context);
                        ok = false;
                    }
                }
                if (op.phase != SimPhase::Event && op.phase != SimPhase::General) continue;
                for (const auto operand : operands)
                {
                    if (operand.generation != 0 || !operand.valid() ||
                        operand.index >= producers.size())
                        continue;
                    const uint32_t producer = producers[operand.index];
                    if (producer == 0) continue;
                    const SimPhase producerPhase =
                        model.operations()[producer - 1].phase;
                    if (op.phase == SimPhase::Event && producerPhase != SimPhase::Event)
                    {
                        diagnostics.error("Event-phase op operand is produced by a "
                                          "non-Event-phase op",
                                          context);
                        ok = false;
                    }
                    if (op.phase == SimPhase::General && producerPhase == SimPhase::Event)
                    {
                        diagnostics.error("General-phase op operand is produced by an "
                                          "Event-phase op",
                                          context);
                        ok = false;
                    }
                }
            }
            return ok;
        }

        // M2b output-lowering invariants. The __tslot_prev_* write rule and
        // the timeslotFlag shape rule are unconditional (they vacuously pass
        // when no such states/ops exist); the P_output cone rules gate on the
        // presence of any Output-phase op, mirroring the event_acts gate in
        // verifyEventLowering.
        bool verifyOutputLowering(const GrhSimModel &model, diag::Diagnostics &diagnostics)
        {
            bool ok = true;
            bool hasOutput = false;
            for (const auto &op : model.operations())
                if (op.phase == SimPhase::Output) hasOutput = true;

            const auto stateWriteType = [](std::string_view name) {
                return name == "core.state.write" || name == "core.state.writeMask" ||
                       name == "core.state.writeArray" || name == "core.state.writeArrayMask" ||
                       name == "core.state.fill" || name == "core.state.latchWrite";
            };

            for (std::size_t i = 0; i < model.operations().size(); ++i)
            {
                const SimOp &op = model.operations()[i];
                if (!model.strings().valid(op.opType)) continue;
                const std::string_view name = model.text(op.opType);
                const std::string context = "operations[" + std::to_string(i) + "]";
                std::span<const ObjectRef> refs;
                std::span<const Parameter> parameters;
                try
                {
                    refs = model.objectRefs(op);
                    parameters = model.parameters(op);
                }
                catch (const std::exception &)
                {
                    continue;
                }
                // A __tslot_prev_* state may only be written by an
                // Output-phase core.state.latchWrite.
                if (stateWriteType(name))
                {
                    for (const auto ref : refs)
                    {
                        if (ref.kind != ObjectKind::State || ref.index == 0 ||
                            ref.index > model.states().size())
                            continue;
                        const auto &state = model.states()[ref.index - 1];
                        if (!model.strings().valid(state.name) ||
                            !model.text(state.name).starts_with("__tslot_prev_"))
                            continue;
                        if (name != "core.state.latchWrite" || op.phase != SimPhase::Output)
                        {
                            diagnostics.error("__tslot_prev_* states may only be written "
                                              "by an Output-phase core.state.latchWrite",
                                              context);
                            ok = false;
                        }
                    }
                }
                // timeslotFlag is only valid as a non-negative int64 on an
                // Output-phase core.system.task that carries event_acts.
                if (const Parameter *flag = findParameter(model, parameters, "timeslotFlag"))
                {
                    const auto *value = std::get_if<int64_t>(&flag->value);
                    if (!value || *value < 0)
                    {
                        diagnostics.error("timeslotFlag must be a non-negative int64",
                                          context);
                        ok = false;
                    }
                    if (name != "core.system.task" || op.phase != SimPhase::Output ||
                        !findParameter(model, parameters, "event_acts"))
                    {
                        diagnostics.error("timeslotFlag requires an Output-phase "
                                          "core.system.task with event_acts",
                                          context);
                        ok = false;
                    }
                }
            }
            if (!hasOutput) return ok;

            // P_output cone self-containment plus the General/Output barrier.
            std::vector<uint32_t> producers(model.values().size() + 1, 0);
            for (const auto &op : model.operations())
                for (const auto result : model.results(op))
                    if (result.generation == 0 && result.index < producers.size())
                        producers[result.index] = op.id.index;
            for (std::size_t i = 0; i < model.operations().size(); ++i)
            {
                const SimOp &op = model.operations()[i];
                if (op.phase != SimPhase::Output && op.phase != SimPhase::General) continue;
                const std::string context = "operations[" + std::to_string(i) + "]";
                std::span<const ValueId> operands;
                try
                {
                    operands = model.operands(op);
                }
                catch (const std::exception &)
                {
                    continue;
                }
                for (const auto operand : operands)
                {
                    if (operand.generation != 0 || !operand.valid() ||
                        operand.index >= producers.size())
                        continue;
                    const uint32_t producer = producers[operand.index];
                    if (producer == 0) continue;
                    const SimPhase producerPhase =
                        model.operations()[producer - 1].phase;
                    if (op.phase == SimPhase::Output && producerPhase != SimPhase::Output)
                    {
                        diagnostics.error("Output-phase op operand is produced by a "
                                          "non-Output-phase op",
                                          context);
                        ok = false;
                    }
                    if (op.phase == SimPhase::General && producerPhase == SimPhase::Output)
                    {
                        diagnostics.error("General-phase op operand is produced by an "
                                          "Output-phase op",
                                          context);
                        ok = false;
                    }
                }
            }
            return ok;
        }
    } // namespace

    bool verifyGrhSimModel(const GrhSimModel &model,
                           const DialectRegistry &registry,
                           diag::Diagnostics &diagnostics)
    {
        bool ok = true;
        auto error = [&](std::string message, std::string context = {}) {
            diagnostics.error(std::move(message), std::move(context));
            ok = false;
        };

        if (model.poisoned())
        {
            error("model is poisoned by a failed in-place pass", "model");
            return false;
        }
        if (!model.strings().valid(model.name()))
        {
            error("model name must be a valid non-empty StringId", "model");
        }

        std::unordered_set<std::string> manifestNames;
        for (const DialectUse &use : model.dialects())
        {
            if (!model.strings().valid(use.name) || !model.strings().valid(use.version))
            {
                error("dialect manifest contains invalid string references", "dialects");
                continue;
            }
            const std::string name(model.text(use.name));
            if (!manifestNames.insert(name).second)
            {
                error("dialect manifest contains duplicate entry: " + name, "dialects");
                continue;
            }
            const DialectDefinition *definition = registry.findDialect(name);
            if (!definition)
            {
                error("required dialect is not registered: " + name, "dialects");
                continue;
            }
            if (definition->version != model.text(use.version))
            {
                error("dialect version mismatch for " + name + ": model=" +
                      std::string(model.text(use.version)) + " registry=" + definition->version,
                      "dialects");
            }
            if (use.schemaFingerprint.valid() &&
                definition->schemaFingerprint != model.text(use.schemaFingerprint))
            {
                error("dialect schema fingerprint mismatch for " + name, "dialects");
            }
        }
        if (!manifestNames.contains("core"))
        {
            error("core dialect is missing from the manifest", "dialects");
        }

        for (std::size_t i = 0; i < model.types().size(); ++i)
        {
            const Type &type = model.types()[i];
            const std::string context = "types[" + std::to_string(i) + "]";
            if (type.id != TypeId{static_cast<uint32_t>(i + 1), 0})
                error("type ID does not match stable table position", context);
            if (!model.strings().valid(type.typeRef) || !registry.hasType(model.text(type.typeRef)))
                error("type has an unknown dialect reference", context);
            else if (model.text(type.typeRef).starts_with("cpu."))
                error("CPU types may only occur in backend mappings", context);
            // Stage-06 shape rules (isomorphic to the GRH verify pass): every
            // dimension is positive, the dimension product does not overflow
            // and divides the total width; only logic types carry a shape.
            switch (type.kind)
            {
            case TypeKind::Logic:
                if (type.width == 0) error("core.logic width must be greater than zero", context);
                if (!type.shape.empty())
                {
                    const std::optional<uint64_t> elements = shapeElementCount(type.shape);
                    if (!elements)
                        error("core.logic shape dimensions must be positive and their product "
                              "must not overflow",
                              context);
                    else if (*elements > type.width || type.width % *elements != 0)
                        error("core.logic shape dimension product must divide the total width",
                              context);
                }
                break;
            case TypeKind::Real:
            case TypeKind::String:
                if (type.width != 0 || !type.shape.empty())
                    error("scalar non-logic type has incompatible fields", context);
                break;
            }
        }

        auto verifyOriginRef = [&](OriginId id, std::string_view context) {
            if (id.valid() && !validId(id, model.origins().size()))
                error("origin ID is out of range", std::string(context));
        };
        for (std::size_t i = 0; i < model.origins().size(); ++i)
        {
            const Origin &origin = model.origins()[i];
            const std::string context = "origins[" + std::to_string(i) + "]";
            if (origin.id != OriginId{static_cast<uint32_t>(i + 1), 0})
                error("origin ID does not match stable table position", context);
            for (StringId id : {origin.sourceKind, origin.symbol, origin.file, origin.pass, origin.note})
            {
                if (!validOptionalString(model, id)) error("origin has an invalid StringId", context);
            }
        }

        for (std::size_t i = 0; i < model.inputs().size(); ++i)
        {
            const InputObject &object = model.inputs()[i];
            const std::string context = "inputs[" + std::to_string(i) + "]";
            if (object.id != InputId{static_cast<uint32_t>(i + 1), 0})
                error("input ID does not match stable table position", context);
            if (!model.strings().valid(object.name)) error("input name is invalid", context);
            if (!validId(object.type, model.types().size())) error("input type is invalid", context);
            verifyOriginRef(object.origin, context);
        }
        for (std::size_t i = 0; i < model.outputs().size(); ++i)
        {
            const OutputObject &object = model.outputs()[i];
            const std::string context = "outputs[" + std::to_string(i) + "]";
            if (object.id != OutputId{static_cast<uint32_t>(i + 1), 0})
                error("output ID does not match stable table position", context);
            if (!model.strings().valid(object.name)) error("output name is invalid", context);
            if (!validId(object.type, model.types().size())) error("output type is invalid", context);
            verifyOriginRef(object.origin, context);
        }
        for (std::size_t i = 0; i < model.states().size(); ++i)
        {
            const StateObject &object = model.states()[i];
            const auto context = [i] { return "states[" + std::to_string(i) + "]"; };
            if (object.id != StateId{static_cast<uint32_t>(i + 1), 0})
                error("state ID does not match stable table position", context());
            if (!model.strings().valid(object.name)) error("state name is invalid", context());
            if (!validId(object.type, model.types().size())) error("state type is invalid", context());
            if (object.origin.valid()) verifyOriginRef(object.origin, context());
        }

        for (std::size_t i = 0; i < model.functions().size(); ++i)
        {
            const ExternFunction &function = model.functions()[i];
            const std::string context = "functions[" + std::to_string(i) + "]";
            if (function.id != FuncId{static_cast<uint32_t>(i + 1), 0})
                error("function ID does not match stable table position", context);
            if (!model.strings().valid(function.name) || !model.strings().valid(function.symbol))
                error("function name or symbol is invalid", context);
            if (!model.strings().valid(function.declRef) ||
                !registry.hasFunctionDecl(model.text(function.declRef)))
                error("function declaration reference is unknown", context);
            if (function.returnType.valid() && !validId(function.returnType, model.types().size()))
                error("function return type is invalid", context);
            verifyOriginRef(function.origin, context);
            try
            {
                for (const DpiArgument &argument : model.arguments(function))
                {
                    if (!model.strings().valid(argument.name) ||
                        !validId(argument.type, model.types().size()))
                        error("function argument name or type is invalid", context);
                }
            }
            catch (const std::exception &ex)
            {
                error(ex.what(), context);
            }
        }

        for (std::size_t i = 0; i < model.interfacePorts().size(); ++i)
        {
            const InterfacePort &port = model.interfacePorts()[i];
            const std::string context = "interface[" + std::to_string(i) + "]";
            if (!model.strings().valid(port.name)) error("interface port name is invalid", context);
            if (port.direction == InterfaceDirection::Input)
            {
                if (!validId(port.input, model.inputs().size()) || port.output.valid() ||
                    port.outputEnable.valid())
                    error("input interface port has invalid object bindings", context);
            }
            else if (port.direction == InterfaceDirection::Output)
            {
                if (!validId(port.output, model.outputs().size()) || port.input.valid() ||
                    port.outputEnable.valid())
                    error("output interface port has invalid object bindings", context);
            }
            else if (!validId(port.input, model.inputs().size()) ||
                     !validId(port.output, model.outputs().size()) ||
                     !validId(port.outputEnable, model.outputs().size()))
            {
                error("inout interface port must bind input, output, and output-enable objects", context);
            }
        }

        std::vector<uint32_t> producers(model.values().size(), 0);
        for (std::size_t i = 0; i < model.values().size(); ++i)
        {
            const SimValue &value = model.values()[i];
            const auto context = [i] { return "values[" + std::to_string(i) + "]"; };
            if (value.id != ValueId{static_cast<uint32_t>(i + 1), 0})
                error("value ID does not match stable table position", context());
            if (!validId(value.type, model.types().size())) error("value type is invalid", context());
            if (!validOptionalString(model, value.name)) error("value name is invalid", context());
            if (value.origin.valid()) verifyOriginRef(value.origin, context());
        }

        auto objectRefValid = [&](ObjectRef ref) {
            if (ref.generation != 0 || ref.index == 0) return false;
            switch (ref.kind)
            {
            case ObjectKind::Input: return ref.index <= model.inputs().size();
            case ObjectKind::Output: return ref.index <= model.outputs().size();
            case ObjectKind::State: return ref.index <= model.states().size();
            case ObjectKind::Function: return ref.index <= model.functions().size();
            }
            return false;
        };

        std::vector<int8_t> opReferenceValidity(model.strings().size() + 1, 0);
        for (std::size_t i = 0; i < model.operations().size(); ++i)
        {
            const SimOp &op = model.operations()[i];
            const auto context = [i] { return "operations[" + std::to_string(i) + "]"; };
            if (op.id != OpId{static_cast<uint32_t>(i + 1), 0})
                error("operation ID does not match stable table position", context());
            if (!model.strings().valid(op.opType))
                error("operation has an invalid dialect reference", context());
            else
            {
                int8_t &validity = opReferenceValidity[op.opType.index];
                if (validity == 0) validity = registry.hasOp(model.text(op.opType)) ? 1 : -1;
                if (validity < 0)
                    error("operation has an unknown dialect reference: " +
                          std::string(model.text(op.opType)), context());
            }
            if (!validOptionalString(model, op.name)) error("operation name is invalid", context());
            if (op.origin.valid()) verifyOriginRef(op.origin, context());
            try
            {
                const auto operands = model.operands(op);
                const auto results = model.results(op);
                const auto refs = model.objectRefs(op);
                const auto parameters = model.parameters(op);
                for (ValueId value : operands)
                    if (!validId(value, model.values().size())) error("operand value ID is invalid", context());
                for (ValueId value : results)
                {
                    if (!validId(value, model.values().size()))
                        error("result value ID is invalid", context());
                    else
                        ++producers[value.index - 1];
                }
                for (ObjectRef ref : refs)
                    if (!objectRefValid(ref)) error("object reference is invalid", context());
                validParameters(model, parameters, diagnostics, context);

                const std::string_view opName = model.text(op.opType);
                if (opName == "core.input.read")
                {
                    if (!operands.empty() || results.size() != 1 || refs.size() != 1 ||
                        refs[0].kind != ObjectKind::Input)
                        error("core.input.read must have 0 operands, 1 result and 1 input ref", context());
                }
                else if (opName == "core.output.write")
                {
                    if (operands.size() != 1 || !results.empty() || refs.size() != 1 ||
                        refs[0].kind != ObjectKind::Output)
                        error("core.output.write must have 1 operand, 0 results and 1 output ref", context());
                }
                else if (opName == "core.state.read")
                {
                    // Whole read (stage-06 Q4): the result carries the state's
                    // full type, shape included for array states.
                    if (!operands.empty() || results.size() != 1 || refs.size() != 1 ||
                        refs[0].kind != ObjectKind::State)
                        error("core.state.read must have 0 operands, 1 result and 1 state ref",
                              context());
                    else
                    {
                        const Type *resultType = valueType(model, results[0]);
                        const Type *stateT = stateRefType(model, refs[0]);
                        if (!stateT || !stateValueGeometryOk(resultType, *stateT))
                            error("core.state.read result type must match the state type "
                                  "(result=" +
                                      (resultType
                                           ? std::to_string(resultType->width) + "b shape=[" +
                                                 [&] {
                                                     std::string s;
                                                     for (const int64_t d : resultType->shape)
                                                         s += (s.empty() ? "" : ",") +
                                                              std::to_string(d);
                                                     return s;
                                                 }() +
                                                 "]"
                                           : std::string("?")) +
                                      " state=" +
                                      (stateT ? std::to_string(stateT->width) + "b shape=[" +
                                                    [&] {
                                                        std::string s;
                                                        for (const int64_t d : stateT->shape)
                                                            s += (s.empty() ? "" : ",") +
                                                                 std::to_string(d);
                                                        return s;
                                                    }() +
                                                    "]"
                                              : std::string("?")) +
                                      ")",
                                  context());
                    }
                }
                else if (opName == "core.state.readArray")
                {
                    // Indexed read: 1..rank index operands; the result is the
                    // element (k == rank) or the remaining-dims sub-array.
                    const Type *target =
                        refs.size() == 1 ? stateRefType(model, refs[0]) : nullptr;
                    uint32_t accessWidth = 0;
                    std::span<const int64_t> accessShape;
                    if (results.size() != 1 || refs.size() != 1 ||
                        refs[0].kind != ObjectKind::State || !target || target->shape.empty())
                        error("core.state.readArray must have 1 result and 1 shaped state ref",
                              context());
                    else if (!indexedAccessGeometry(*target, operands.size(), accessWidth,
                                                    accessShape))
                        error("core.state.readArray index count is out of the state rank range",
                              context());
                    else
                    {
                        for (const ValueId index : operands)
                        {
                            const Type *indexType = valueType(model, index);
                            if (!logicAnyShape(indexType))
                            {
                                error("core.state.readArray indices must be logic values",
                                      context());
                                break;
                            }
                        }
                        if (!indexedAccessTypeMatches(valueType(model, results[0]), accessWidth))
                            error("core.state.readArray result type must be the addressed "
                                  "element / remaining-dims sub-array type",
                                  context());
                    }
                }
                else if (opName == "core.state.write" ||
                         opName == "core.state.writeMask" ||
                         opName == "core.state.writeArray" ||
                         opName == "core.state.writeArrayMask" ||
                         opName == "core.state.fill" ||
                         opName == "core.state.latchWrite")
                {
                    // write:      [updateCond, nextValue, events...]
                    // writeMask: [updateCond, nextValue, mask, events...]
                    // writeArray: [updateCond, i0..ik-1, data, events...]
                    // writeArrayMask: [updateCond, i0..ik-1, data, mask, events...]
                    // A legacy masked write/writeArray form is accepted while
                    // old v3 checkpoints are being migrated.
                    // fill:       [updateCond, data, events...]
                    // latchWrite: [updateCond, nextValue, mask] (level-sensitive)
                    const std::size_t events =
                        opName == "core.state.latchWrite" ? 0
                                                          : eventOperandCount(model, parameters);
                    const Type *target =
                        refs.size() == 1 ? stateRefType(model, refs[0]) : nullptr;
                    if (!results.empty() || refs.size() != 1 ||
                        refs[0].kind != ObjectKind::State || !target)
                    {
                        error("core.state write op must have 0 results and 1 state ref",
                              context());
                    }
                    else if (opName == "core.state.write" || opName == "core.state.writeMask")
                    {
                        const bool masked = opName == "core.state.writeMask" ||
                                            (opName == "core.state.write" &&
                                             operands.size() == 3 + events);
                        const std::size_t expected = 2 + events + (masked ? 1 : 0);
                        bool valid = operands.size() == expected &&
                                     logicAnyShape(valueType(model, operands[0])) &&
                                     stateValueGeometryOk(valueType(model, operands[1]),
                                                          *target);
                        const Type *mask = masked && operands.size() >= 3 + events
                                               ? valueType(model, operands[2])
                                               : nullptr;
                        valid = valid && (!masked || (logicAnyShape(mask) &&
                                                       mask->width == target->width));
                        if (!valid)
                        {
                            const Type *data = operands.size() >= 2 ? valueType(model, operands[1])
                                                                    : nullptr;
                            error(std::string(opName) +
                                      " must be [updateCond, nextValue(state type)" +
                                      (masked ? ", mask(full-width scalar)" : "") +
                                      ", events...] (operands=" +
                                      std::to_string(operands.size()) + " events=" +
                                      std::to_string(events) + " data=" +
                                      (data ? std::to_string(data->width) + "b shape=" +
                                                  std::to_string(data->shape.size()) + "d"
                                            : std::string("?")) +
                                      " state=" + std::to_string(target->width) + "b shape=" +
                                      std::to_string(target->shape.size()) + "d mask=" +
                                      (mask ? std::to_string(mask->width) : std::string("?")) +
                                      ")",
                                  context());
                        }
                    }
                    else if (opName == "core.state.writeArray" ||
                             opName == "core.state.writeArrayMask")
                    {
                        auto validIndexedLayout = [&](bool masked) {
                            if (target->shape.empty()) return false;
                            const std::size_t base = 2 + events + (masked ? 1 : 0);
                            if (operands.size() < base + 1) return false;
                            const std::size_t indexCount = operands.size() - base;
                            uint32_t accessWidth = 0;
                            std::span<const int64_t> accessShape;
                            if (!indexedAccessGeometry(*target, indexCount, accessWidth,
                                                       accessShape))
                                return false;
                            if (!logicAnyShape(valueType(model, operands[0]))) return false;
                            for (std::size_t i = 1; i <= indexCount; ++i)
                                if (!logicAnyShape(valueType(model, operands[i]))) return false;
                            if (!indexedAccessTypeMatches(
                                    valueType(model, operands[1 + indexCount]), accessWidth))
                                return false;
                            if (!masked) return true;
                            const Type *mask = valueType(model, operands[2 + indexCount]);
                            return logicAnyShape(mask) && mask->width == accessWidth;
                        };
                        const bool masked = opName == "core.state.writeArrayMask";
                        const bool valid = masked ? validIndexedLayout(true)
                                                  // Compatibility: old writeArray carried a
                                                  // synthesized/explicit full mask.
                                                  : (validIndexedLayout(false) ||
                                                     validIndexedLayout(true));
                        if (!valid)
                        {
                            error(std::string(opName) +
                                      " must target a shaped state with [updateCond, "
                                      "i0..ik-1, data" +
                                      (masked ? ", mask" : ", [mask]") +
                                      ", events...]; indices must be scalar logic, data "
                                      "must be the addressed element / remaining-dims "
                                      "sub-array type and mask its full-width scalar",
                                  context());
                        }
                    }
                    else if (opName == "core.state.fill")
                    {
                        bool valid = operands.size() == 2 + events && !target->shape.empty() &&
                                     logicAnyShape(valueType(model, operands[0]));
                        if (valid)
                        {
                            // Broadcast (element-width data) or packed
                            // (full-width data sliced row-major) fill.
                            const Type *data = valueType(model, operands[1]);
                            const std::optional<uint32_t> elementWidth =
                                typeElementWidth(*target);
                            valid = data && data->kind == TypeKind::Logic && elementWidth &&
                                    (data->width == *elementWidth ||
                                     data->width == target->width);
                        }
                        if (!valid)
                            error("core.state.fill must be [updateCond, data(element-width or "
                                  "full-width), events...] on a shaped state",
                                  context());
                    }
                    else // latchWrite
                    {
                        const Type *mask = operands.size() >= 3 ? valueType(model, operands[2])
                                                                : nullptr;
                        const bool valid = operands.size() == 3 && target->shape.empty() &&
                                           logicAnyShape(valueType(model, operands[0])) &&
                                           stateValueGeometryOk(valueType(model, operands[1]),
                                                                *target) &&
                                           logicAnyShape(mask) && mask->width == target->width;
                        if (!valid)
                            error("core.state.latchWrite must be [updateCond, nextValue(state "
                                  "type), mask(full-width scalar)] on a scalar state",
                                  context());
                    }
                }
                else if (opName == "core.compute.sliceArray")
                {
                    // Array value access (stage-02/stage-06): [base, i0..ik-1],
                    // 1 <= k <= rank(base); the result is the element or the
                    // remaining-dims sub-array.
                    const Type *base =
                        operands.empty() ? nullptr : valueType(model, operands.front());
                    uint32_t accessWidth = 0;
                    std::span<const int64_t> accessShape;
                    if (operands.size() < 2 || results.size() != 1 || !refs.empty() || !base ||
                        base->kind != TypeKind::Logic || base->shape.empty())
                        error("core.compute.sliceArray requires a shaped logic base value, "
                              "1..rank index operands and 1 result",
                              context());
                    else if (!indexedAccessGeometry(*base, operands.size() - 1, accessWidth,
                                                    accessShape))
                        error("core.compute.sliceArray index count is out of the base rank range",
                              context());
                    else
                    {
                        for (std::size_t i = 1; i < operands.size(); ++i)
                        {
                            const Type *indexType = valueType(model, operands[i]);
                            if (!logicAnyShape(indexType))
                            {
                                error("core.compute.sliceArray indices must be logic values",
                                      context());
                                break;
                            }
                        }
                        if (!indexedAccessTypeMatches(valueType(model, results[0]), accessWidth))
                            error("core.compute.sliceArray result type must be the addressed "
                                  "element / remaining-dims sub-array type",
                                  context());
                    }
                }
                else if (opName == "core.system.function" && results.size() != 1)
                {
                    error("core.system.function must have exactly one result", context());
                }
                else if (opName == "core.dpi.call" &&
                         (refs.empty() || refs.front().kind != ObjectKind::Function))
                {
                    error("core.dpi.call must reference its function first", context());
                }
                else if (opName == "core.compute.bitSelect") {
                    bool valid = operands.size() == 3 && results.size() == 1 &&
                                 refs.empty() && parameters.empty();
                    TypeId typeId;
                    const auto scalar = [&](ValueId value) {
                        if (!validId(value, model.values().size())) return false;
                        const auto id = model.values()[value.index - 1].type;
                        if (!validId(id, model.types().size())) return false;
                        const auto &type = model.types()[id.index - 1];
                        if (!typeId) typeId = id;
                        return id == typeId && type.kind == TypeKind::Logic &&
                               type.domain == LogicDomain::TwoState && type.width > 0 && type.width <= 64;
                    };
                    for (const auto value : operands) valid &= scalar(value);
                    for (const auto value : results) valid &= scalar(value);
                    if (!valid) error("bitSelect requires three operands and one result of the same scalar two-state type", context());
                }
                else if (opName == "core.compute.prioritySelect") {
                    // [c0..cN-1, a0..aN-1, default]: the first true condition wins.
                    bool valid = results.size() == 1 && refs.empty() && parameters.empty() &&
                                 operands.size() >= 7 && operands.size() % 2 == 1;
                    if (valid) {
                        const std::size_t count = (operands.size() - 1) / 2;
                        valid = count <= 64;
                        const auto valueType = [&](ValueId value) -> const Type * {
                            if (!validId(value, model.values().size())) return nullptr;
                            const auto id = model.values()[value.index - 1].type;
                            if (!validId(id, model.types().size())) return nullptr;
                            return &model.types()[id.index - 1];
                        };
                        const auto *resultType = valueType(results[0]);
                        valid &= resultType && resultType->kind == TypeKind::Logic &&
                                 resultType->domain == LogicDomain::TwoState &&
                                 resultType->width > 0 && resultType->width <= 64;
                        for (std::size_t i = 0; i < count && valid; ++i) {
                            const auto *condType = valueType(operands[i]);
                            valid &= condType && condType->kind == TypeKind::Logic &&
                                     condType->domain == LogicDomain::TwoState &&
                                     condType->width == 1 && !condType->isSigned;
                        }
                        for (std::size_t i = count; i < operands.size() && valid; ++i) {
                            const auto *armType = valueType(operands[i]);
                            valid &= armType && armType->kind == TypeKind::Logic &&
                                     armType->domain == LogicDomain::TwoState &&
                                     armType->width > 0 && armType->width <= 64;
                        }
                    }
                    if (!valid) error("prioritySelect requires 2N+1 operands (3<=N<=64): N one-bit unsigned two-state conditions, N arms and one default of scalar two-state types, and one result", context());
                }
                else if (opName == "core.compute.expr") {
                    // Fused expression tree (grhsim.fuse-expr-chains): operands are the
                    // tree's leaf values; the "tree" parameter holds postfix tokens —
                    // leaf "l<k>" (operand k) or node
                    // "n;<kind>;<width>;<signed01>;<arity>;<opId>[;key=int64...]" — and
                    // "rk" names the root's original op kind for dynamic accounting.
                    // The root token is last and the stack must reduce to one value.
                    bool valid = results.size() == 1 && refs.empty();
                    const Parameter *tree = findParameter(model, parameters, "tree");
                    const Parameter *rk = findParameter(model, parameters, "rk");
                    const std::vector<std::string> *tokens = nullptr;
                    if (!tree || !std::holds_alternative<std::vector<std::string>>(tree->value) ||
                        (tokens = &std::get<std::vector<std::string>>(tree->value))->empty())
                        valid = false;
                    if (!rk || !std::holds_alternative<std::string>(rk->value) ||
                        !std::get<std::string>(rk->value).starts_with("core.compute.") ||
                        std::get<std::string>(rk->value) == "core.compute.expr")
                        valid = false;
                    if (valid) {
                        const auto parseUnsigned = [](std::string_view text, std::uint64_t &out) {
                            if (text.empty()) return false;
                            for (const char ch : text) if (ch < '0' || ch > '9') return false;
                            out = std::stoull(std::string(text));
                            return true;
                        };
                        std::size_t depth = 0;
                        for (const auto &token : *tokens) {
                            if (!valid) break;
                            if (token.size() > 1 && token[0] == 'l') {
                                std::uint64_t leaf = 0;
                                if (!parseUnsigned(std::string_view(token).substr(1), leaf) ||
                                    leaf >= operands.size()) { valid = false; break; }
                                ++depth;
                                continue;
                            }
                            std::vector<std::string_view> fields;
                            std::size_t begin = 0;
                            for (std::size_t i = 0; i <= token.size(); ++i)
                                if (i == token.size() || token[i] == ';') {
                                    fields.push_back(std::string_view(token).substr(begin, i - begin));
                                    begin = i + 1;
                                }
                            std::uint64_t width = 0, arity = 0, origin = 0;
                            if (fields.size() < 6 || fields[0] != "n" || fields[1].empty() ||
                                fields[1] == "expr" || fields[1].find_first_of(";= ") != std::string_view::npos ||
                                !parseUnsigned(fields[2], width) || width == 0 || width > 64 ||
                                (fields[3] != "0" && fields[3] != "1") ||
                                !parseUnsigned(fields[4], arity) || arity == 0 || arity > depth ||
                                !parseUnsigned(fields[5], origin) || origin == 0 ||
                                origin > model.operations().size()) { valid = false; break; }
                            for (std::size_t i = 6; i < fields.size() && valid; ++i) {
                                const auto eq = fields[i].find('=');
                                std::string_view number = eq == std::string_view::npos ? std::string_view{} :
                                    fields[i].substr(eq + 1);
                                if (!number.empty() && number.front() == '-') number.remove_prefix(1);
                                std::uint64_t ignored = 0;
                                if (eq == std::string_view::npos || eq == 0 || !parseUnsigned(number, ignored))
                                    valid = false;
                            }
                            depth = depth - arity + 1;
                        }
                        if (depth != 1 || (*tokens).back()[0] != 'n') valid = false;
                        const auto scalar = [&](ValueId value) {
                            if (!validId(value, model.values().size())) return false;
                            const auto id = model.values()[value.index - 1].type;
                            if (!validId(id, model.types().size())) return false;
                            const auto &type = model.types()[id.index - 1];
                            return type.kind == TypeKind::Logic && type.domain == LogicDomain::TwoState &&
                                   type.width > 0 && type.width <= 64;
                        };
                        for (const auto value : operands) valid &= scalar(value);
                        for (const auto value : results) valid &= scalar(value);
                    }
                    if (!valid) error("core.compute.expr requires one scalar two-state result, scalar two-state leaf operands, a postfix \"tree\" string-array parameter reducing to one value and a \"rk\" string parameter naming the root's original op kind", context());
                }

                else if (opName == "core.event.edgeDet") {
                    // P_event edge detector with no data result: executing the op
                    // compares its prevEventStore slot against the current event
                    // value, pulses the act bit in eventActStore for the current
                    // round only, and immediately updates prev. The prev slot
                    // initializes to prevInit (the event signal's init value), so
                    // power-up evaluation reports no edge.
                    bool valid = operands.size() == 1 && results.empty() && refs.empty();
                    if (valid) {
                        const ValueId event = operands.front();
                        if (!validId(event, model.values().size())) valid = false;
                        else {
                            const auto typeId = model.values()[event.index - 1].type;
                            if (!validId(typeId, model.types().size())) valid = false;
                            else {
                                const auto &eventType = model.types()[typeId.index - 1];
                                if (eventType.kind != TypeKind::Logic || eventType.width != 1 ||
                                    !eventType.shape.empty()) valid = false;
                            }
                        }
                    }
                    const Parameter *edge = findParameter(model, parameters, "edge");
                    const Parameter *act = findParameter(model, parameters, "act");
                    const Parameter *prev = findParameter(model, parameters, "prev");
                    const Parameter *prevInit = findParameter(model, parameters, "prevInit");
                    if (parameters.size() != 4 || !edge || !act || !prev || !prevInit) valid = false;
                    else {
                        if (!std::holds_alternative<std::string>(edge->value)) valid = false;
                        else {
                            const auto &edgeText = std::get<std::string>(edge->value);
                            if (edgeText != "posedge" && edgeText != "negedge" && edgeText != "both")
                                valid = false;
                        }
                        if (!std::holds_alternative<int64_t>(act->value) ||
                            std::get<int64_t>(act->value) < 0) valid = false;
                        if (!std::holds_alternative<int64_t>(prev->value) ||
                            std::get<int64_t>(prev->value) < 0) valid = false;
                        if (!std::holds_alternative<std::string>(prevInit->value) ||
                            std::get<std::string>(prevInit->value).empty()) valid = false;
                    }
                    if (!valid) error("core.event.edgeDet requires exactly one unshaped 1-bit logic event operand, no results/object refs and four parameters: edge (posedge|negedge|both string), act/prev (non-negative int64 store indices) and prevInit (non-empty constant literal string)", context());
                }

                const Parameter *edges = findParameter(model, parameters, "event_edges");
                if (edges && !std::holds_alternative<std::vector<std::string>>(edges->value))
                    error("event_edges must be a string array", context());
            }
            catch (const std::exception &ex)
            {
                error(ex.what(), context());
            }
        }
        for (std::size_t i = 0; i < producers.size(); ++i)
        {
            if (producers[i] != 1)
                error("value must have exactly one producer; observed " +
                      std::to_string(producers[i]), "values[" + std::to_string(i) + "]");
        }

        if (!verifyPhaseAttribution(model, diagnostics)) ok = false;
        if (!verifyEdgeDetUniqueness(model, diagnostics)) ok = false;
        if (!verifyEventLowering(model, diagnostics)) ok = false;
        if (!verifyOutputLowering(model, diagnostics)) ok = false;

        std::vector<uint32_t> initCount(model.states().size(), 0);
        for (std::size_t i = 0; i < model.initRecords().size(); ++i)
        {
            const InitRecord &record = model.initRecords()[i];
            const auto context = [i] { return "init[" + std::to_string(i) + "]"; };
            if (!validId(record.state, model.states().size()))
            {
                error("init record state ID is invalid", context());
                continue;
            }
            ++initCount[record.state.index - 1];
            try
            {
                const auto steps = model.steps(record);
                if (steps.empty()) error("InitSpec must contain at least one step", context());
                for (const InitStep &step : steps)
                {
                    if (!model.strings().valid(step.kind) ||
                        !registry.hasInitStep(model.text(step.kind)))
                        error("init step has an unknown dialect reference", context());
                    const auto parameters = model.parameters(step);
                    validParameters(model, parameters, diagnostics, context);
                    if (model.text(step.kind) == "core.init.const" &&
                        validId(model.states()[record.state.index - 1].type, model.types().size()) &&
                        model.types()[model.states()[record.state.index - 1].type.index - 1].kind == TypeKind::Logic)
                    {
                        const Parameter *value = findParameter(model, parameters, "value");
                        if (parameters.size() != 1 || !value ||
                            !std::holds_alternative<std::string>(value->value))
                            error("logic core.init.const requires one total-width value literal string", context());
                    }
                }
            }
            catch (const std::exception &ex)
            {
                error(ex.what(), context());
            }
        }
        for (std::size_t i = 0; i < initCount.size(); ++i)
        {
            if (initCount[i] != 1)
                error("state must have exactly one InitSpec; observed " +
                      std::to_string(initCount[i]), "states[" + std::to_string(i) + "]");
        }

        for (std::size_t i = 0; i < model.declaredSymbols().size(); ++i)
        {
            if (!model.strings().valid(model.declaredSymbols()[i]))
                error("declared symbol has an invalid StringId",
                      "declaredSymbols[" + std::to_string(i) + "]");
        }
        for (std::size_t i = 0; i < model.generateGroups().size(); ++i)
        {
            const GenerateGroup &group = model.generateGroups()[i];
            const std::string context = "generateGroups[" + std::to_string(i) + "]";
            if (!model.strings().valid(group.scope) || !model.strings().valid(group.name))
                error("generate group scope/name has an invalid StringId", context);
            for (std::size_t j = 0; j < group.symbols.size(); ++j)
                if (!model.strings().valid(group.symbols[j]))
                    error("generate group member has an invalid StringId",
                          context + ".symbols[" + std::to_string(j) + "]");
        }
        if (!verifyDeclProvenances(model, diagnostics)) ok = false;
        if (!verifyStateStores(model, diagnostics)) ok = false;

        std::unordered_set<uint32_t> mappingBackends;
        const bool validModel = ok && !diagnostics.hasError();
        for (std::size_t i = 0; i < model.mappings().size(); ++i)
        {
            const BackendMapping &mapping = model.mappings()[i];
            const std::string context = "mappings[" + std::to_string(i) + "]";
            if (!model.strings().valid(mapping.backend) || !model.strings().valid(mapping.schema))
                error("mapping backend or schema is invalid", context);
            if (!mappingBackends.insert(mapping.backend.index).second)
                error("mapping backend is duplicated", context);
            if (mapping.sourceIdentity != model.identity() ||
                mapping.sourceSemanticRevision != model.semanticRevision())
                error("mapping is stale for the current model revision", context);
            try
            {
                validParameters(model, model.parameters(mapping), diagnostics,
                                [&context] { return context; });
                if (validModel && (mapping.cpu || model.text(mapping.backend) == "cpu"))
                    if (!verifyCpuMapping(model, mapping, diagnostics)) ok = false;
            }
            catch (const std::exception &ex)
            {
                error(ex.what(), context);
            }
        }

        return ok && !diagnostics.hasError();
    }

    bool verifyGrhSimSemanticSeal(const GrhSimModel &model, diag::Diagnostics &diagnostics)
    {
        return verifySemanticSeal(model, diagnostics);
    }

} // namespace wolvrix::lib::grhsim
