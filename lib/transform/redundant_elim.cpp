#include "transform/redundant_elim.hpp"

#include "core/grh.hpp"

#include "topo_guard.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace wolvrix::lib::transform
{

    namespace
    {
        std::size_t hashCombine(std::size_t seed, std::size_t value)
        {
            return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
        }

        std::size_t hashAttributeValue(const wolvrix::lib::grh::AttributeValue &value)
        {
            const std::size_t typeTag = value.index();
            const std::size_t payload = std::visit(
                [](const auto &val) -> std::size_t
                {
                    using T = std::decay_t<decltype(val)>;
                    if constexpr (std::is_same_v<T, bool>)
                    {
                        return std::hash<bool>{}(val);
                    }
                    else if constexpr (std::is_same_v<T, int64_t>)
                    {
                        return std::hash<int64_t>{}(val);
                    }
                    else if constexpr (std::is_same_v<T, double>)
                    {
                        return std::hash<double>{}(val);
                    }
                    else if constexpr (std::is_same_v<T, std::string>)
                    {
                        return std::hash<std::string>{}(val);
                    }
                    else if constexpr (std::is_same_v<T, std::vector<bool>>)
                    {
                        std::size_t seed = 0;
                        for (const bool bit : val)
                        {
                            seed = hashCombine(seed, std::hash<bool>{}(bit));
                        }
                        return seed;
                    }
                    else if constexpr (std::is_same_v<T, std::vector<int64_t>>)
                    {
                        std::size_t seed = 0;
                        for (const int64_t item : val)
                        {
                            seed = hashCombine(seed, std::hash<int64_t>{}(item));
                        }
                        return seed;
                    }
                    else if constexpr (std::is_same_v<T, std::vector<double>>)
                    {
                        std::size_t seed = 0;
                        for (const double item : val)
                        {
                            seed = hashCombine(seed, std::hash<double>{}(item));
                        }
                        return seed;
                    }
                    else if constexpr (std::is_same_v<T, std::vector<std::string>>)
                    {
                        std::size_t seed = 0;
                        for (const auto &item : val)
                        {
                            seed = hashCombine(seed, std::hash<std::string>{}(item));
                        }
                        return seed;
                    }
                    else
                    {
                        return 0U;
                    }
                },
                value);
            return hashCombine(typeTag, payload);
        }

        struct OpSignature
        {
            wolvrix::lib::grh::OperationKind kind = wolvrix::lib::grh::OperationKind::kConstant;
            std::vector<wolvrix::lib::grh::ValueId> operands;
            std::vector<wolvrix::lib::grh::AttrKV> attrs;
            int32_t width = 0;
            bool isSigned = false;

            bool operator==(const OpSignature &other) const
            {
                if (kind != other.kind || width != other.width || isSigned != other.isSigned)
                {
                    return false;
                }
                if (operands != other.operands)
                {
                    return false;
                }
                if (attrs.size() != other.attrs.size())
                {
                    return false;
                }
                for (std::size_t i = 0; i < attrs.size(); ++i)
                {
                    if (attrs[i].key != other.attrs[i].key || attrs[i].value != other.attrs[i].value)
                    {
                        return false;
                    }
                }
                return true;
            }
        };

        struct OpSignatureHash
        {
            std::size_t operator()(const OpSignature &sig) const
            {
                std::size_t seed = static_cast<std::size_t>(sig.kind);
                seed = hashCombine(seed, std::hash<int32_t>{}(sig.width));
                seed = hashCombine(seed, std::hash<bool>{}(sig.isSigned));
                seed = hashCombine(seed, std::hash<std::size_t>{}(sig.operands.size()));
                for (const auto &operand : sig.operands)
                {
                    seed = hashCombine(seed, wolvrix::lib::grh::ValueIdHash{}(operand));
                }
                seed = hashCombine(seed, std::hash<std::size_t>{}(sig.attrs.size()));
                for (const auto &attr : sig.attrs)
                {
                    seed = hashCombine(seed, std::hash<std::string>{}(attr.key));
                    seed = hashCombine(seed, hashAttributeValue(attr.value));
                }
                return seed;
            }
        };

        // Lean screening helpers (stage-07): all read through the Graph id/span
        // accessors instead of materializing fat Value/Operation copies.
        bool isOutputPortValue(const wolvrix::lib::grh::Graph &graph,
                               wolvrix::lib::grh::ValueId valueId)
        {
            return graph.valueIsOutput(valueId) && !graph.valueIsInput(valueId) &&
                   !graph.valueIsInout(valueId);
        }

        // Graph has no public lean users accessor, so this materializes the
        // Value; call sites reach it only for output-port values (rare).
        bool hasOtherUsers(const wolvrix::lib::grh::Graph &graph,
                           wolvrix::lib::grh::ValueId valueId)
        {
            const wolvrix::lib::grh::Value value = graph.getValue(valueId);
            return !value.users().empty();
        }

        bool isTemporarySymbol(const wolvrix::lib::grh::Graph &graph,
                               wolvrix::lib::grh::ValueId valueId)
        {
            if (graph.valueIsInput(valueId) || graph.valueIsOutput(valueId) ||
                graph.valueIsInout(valueId))
            {
                return false;
            }
            const wolvrix::lib::grh::SymbolId symbol = graph.valueSymbol(valueId);
            if (graph.isDeclaredSymbol(symbol))
            {
                return false;
            }
            const std::string_view name = graph.symbolText(symbol);
            if (name.rfind("_val_", 0) == 0)
            {
                return true;
            }
            return false;
        }

        bool isSideEffectFreeOp(wolvrix::lib::grh::OperationKind kind)
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kRegister:
            case wolvrix::lib::grh::OperationKind::kRegisterReadPort:
            case wolvrix::lib::grh::OperationKind::kRegisterReadArrayPort:
            case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
            case wolvrix::lib::grh::OperationKind::kRegisterWriteMaskPort:
            case wolvrix::lib::grh::OperationKind::kRegisterWriteArrayPort:
            case wolvrix::lib::grh::OperationKind::kRegisterWriteArrayMaskPort:
            case wolvrix::lib::grh::OperationKind::kRegisterFillPort:
            case wolvrix::lib::grh::OperationKind::kLatch:
            case wolvrix::lib::grh::OperationKind::kLatchReadPort:
            case wolvrix::lib::grh::OperationKind::kLatchWritePort:
            case wolvrix::lib::grh::OperationKind::kInstance:
            case wolvrix::lib::grh::OperationKind::kBlackbox:
            case wolvrix::lib::grh::OperationKind::kSystemFunction:
            case wolvrix::lib::grh::OperationKind::kSystemTask:
            case wolvrix::lib::grh::OperationKind::kDpicImport:
            case wolvrix::lib::grh::OperationKind::kDpicCall:
                return false;
            default:
                return true;
            }
        }

        bool isCseCandidate(const wolvrix::lib::grh::Graph &graph,
                            wolvrix::lib::grh::OperationId opId)
        {
            const wolvrix::lib::grh::OperationKind kind = graph.opKind(opId);
            if (!isSideEffectFreeOp(kind))
            {
                return false;
            }
            // stage-07: constants stay out of CSE — constant dedup is
            // const-fold's ConstantPool job (it runs before RE in the simplify
            // pipeline), and copying/hashing their constValue literals into
            // OpSignature (aggregate reset literals reach ~140KB) dominated
            // this pass. A standalone RE run simply loses constant dedup;
            // that semantic-safe trade-off is accepted.
            if (kind == wolvrix::lib::grh::OperationKind::kConstant)
            {
                return false;
            }
            const std::span<const wolvrix::lib::grh::ValueId> results = graph.opResults(opId);
            if (results.size() != 1)
            {
                return false;
            }
            const wolvrix::lib::grh::ValueId resultId = results[0];
            if (!resultId.valid())
            {
                return false;
            }
            if (graph.valueIsInput(resultId) || graph.valueIsOutput(resultId) ||
                graph.valueIsInout(resultId))
            {
                return false;
            }
            if (!isTemporarySymbol(graph, resultId))
            {
                return false;
            }
            for (const auto operandId : graph.opOperands(opId))
            {
                if (!operandId.valid())
                {
                    return false;
                }
            }
            return true;
        }

        std::optional<OpSignature> makeSignature(const wolvrix::lib::grh::Graph &graph,
                                                 wolvrix::lib::grh::OperationId opId)
        {
            OpSignature sig;
            sig.kind = graph.opKind(opId);
            const std::span<const wolvrix::lib::grh::ValueId> operands = graph.opOperands(opId);
            sig.operands.assign(operands.begin(), operands.end());
            const std::span<const wolvrix::lib::grh::AttrKV> attrs = graph.opAttrs(opId);
            // Defensive (stage-07): refuse to copy/hash a large string
            // attribute into the signature; the op simply skips CSE.
            // kConstant, the known offender, is already excluded upstream.
            for (const auto &kv : attrs)
            {
                if (const auto *text = std::get_if<std::string>(&kv.value);
                    text != nullptr && text->size() > 1024)
                {
                    return std::nullopt;
                }
            }
            sig.attrs.assign(attrs.begin(), attrs.end());
            std::stable_sort(sig.attrs.begin(), sig.attrs.end(),
                             [](const wolvrix::lib::grh::AttrKV &lhs, const wolvrix::lib::grh::AttrKV &rhs)
                             { return lhs.key < rhs.key; });
            const wolvrix::lib::grh::ValueId resultId = graph.opResults(opId)[0];
            sig.width = graph.valueWidth(resultId);
            sig.isSigned = graph.valueSigned(resultId);
            return sig;
        }

        // Graph has no public lean users accessor, so this materializes the
        // Value; call sites reach it only for screened rewrite candidates.
        bool isSingleUser(const wolvrix::lib::grh::Graph &graph,
                          wolvrix::lib::grh::ValueId valueId,
                          wolvrix::lib::grh::OperationId user)
        {
            const wolvrix::lib::grh::Value value = graph.getValue(valueId);
            std::size_t count = 0;
            for (const auto &use : value.users())
            {
                if (use.operation == user)
                {
                    ++count;
                }
                else
                {
                    return false;
                }
            }
            return count > 0;
        }

        bool isLogicNotOf(const wolvrix::lib::grh::Graph &graph,
                          wolvrix::lib::grh::ValueId maybeNot,
                          wolvrix::lib::grh::ValueId operand)
        {
            if (!maybeNot.valid() || !operand.valid())
            {
                return false;
            }
            const wolvrix::lib::grh::OperationId defOpId = graph.valueDef(maybeNot);
            if (!defOpId.valid())
            {
                return false;
            }
            const wolvrix::lib::grh::OperationKind defKind = graph.opKind(defOpId);
            if (defKind != wolvrix::lib::grh::OperationKind::kLogicNot &&
                defKind != wolvrix::lib::grh::OperationKind::kNot)
            {
                return false;
            }
            const std::span<const wolvrix::lib::grh::ValueId> ops = graph.opOperands(defOpId);
            return !ops.empty() && ops.front() == operand;
        }

        // Lean overload: reads the attribute span directly (no Operation
        // materialization) for hot screening loops.
        std::optional<int64_t> getIntAttr(const wolvrix::lib::grh::Graph &graph,
                                          wolvrix::lib::grh::OperationId op,
                                          std::string_view key)
        {
            for (const auto &kv : graph.opAttrs(op))
            {
                if (kv.key == key)
                {
                    if (const auto *value = std::get_if<int64_t>(&kv.value))
                    {
                        return *value;
                    }
                    return std::nullopt;
                }
            }
            return std::nullopt;
        }

        std::string makeInlineConstName(const wolvrix::lib::grh::Graph &graph,
                                        std::string_view kind,
                                        std::string_view baseName,
                                        uint32_t &counter)
        {
            (void)baseName;
            std::string base = wolvrix::lib::grh::Graph::makeInternalBase(kind);
            for (;;)
            {
                std::string candidate = base + "_" + std::to_string(counter++);
                if (!graph.findOperation(candidate).valid() &&
                    !graph.findValue(candidate).valid())
                {
                    return candidate;
                }
            }
        }

        wolvrix::lib::grh::ValueId createInlineConst(wolvrix::lib::grh::Graph &graph,
                                           std::string_view baseName,
                                           int32_t width,
                                           bool isSigned,
                                           std::string_view literal,
                                           uint32_t &counter)
        {
            std::string base;
            if (!baseName.empty())
            {
                base = std::string(baseName);
            }
            const std::string valueName = makeInlineConstName(graph, "val", base, counter);
            const std::string opName = makeInlineConstName(graph, "op", base, counter);
            const wolvrix::lib::grh::SymbolId valueSym = graph.internSymbol(valueName);
            const wolvrix::lib::grh::SymbolId opSym = graph.internSymbol(opName);
            const wolvrix::lib::grh::ValueId val =
                graph.createValue(valueSym, width, isSigned, wolvrix::lib::grh::ValueType::Logic);
            const wolvrix::lib::grh::OperationId op =
                graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant, opSym);
            graph.addResult(op, val);
            graph.setAttr(op, "constValue", std::string(literal));
            const wolvrix::lib::grh::SrcLoc genLoc = makeTransformSrcLoc("redundant-elim", "inline_const");
            graph.setValueSrcLoc(val, genLoc);
            graph.setOpSrcLoc(op, genLoc);
            return val;
        }

        void replaceUsers(wolvrix::lib::grh::Graph &graph,
                          wolvrix::lib::grh::ValueId from,
                          wolvrix::lib::grh::ValueId to,
                          const std::function<void(std::string)> &onError)
        {
            // Type-honest aliasing: when the replacement's type differs from
            // the replaced value's, a kAssignAs view keeps the retype explicit.
            wolvrix::lib::grh::ValueId aliased = to;
            try
            {
                aliased = graph.replaceAllUsesWithView(from, to);
            }
            catch (const std::exception &ex)
            {
                onError(std::string("Failed to replace operands: ") + ex.what());
                return;
            }

            std::vector<std::string> outputPortsToUpdate;
            for (const auto &port : graph.outputPorts())
            {
                if (port.value == from)
                {
                    outputPortsToUpdate.push_back(port.name);
                }
            }
            for (const auto &portName : outputPortsToUpdate)
            {
                try
                {
                    graph.bindOutputPort(portName, aliased);
                }
                catch (const std::exception &ex)
                {
                    onError(std::string("Failed to rebind output port: ") + ex.what());
                }
            }
        }

    } // namespace

    RedundantElimPass::RedundantElimPass()
        : Pass("redundant-elim", "redundant-elim",
               "Inline trivial assigns and eliminate redundant temps")
    {
    }

    PassResult RedundantElimPass::run()
    {
        const auto timingStart = std::chrono::steady_clock::now();
        PassResult result;
        bool anyChanged = false;
        const std::size_t graphCount = design().graphs().size();
        logDebug("begin graphs=" + std::to_string(graphCount));
        std::size_t changedGraphs = 0;
        std::size_t opsRemoved = 0;
        std::size_t valuesRemoved = 0;

        for (const auto &entry : design().graphs())
        {
            wolvrix::lib::grh::Graph &graph = *entry.second;
            bool graphChanged = false;
            uint32_t inlineConstCounter = 0;
            auto eraseOp = [&](auto opId, auto... args) -> bool {
                if (graph.eraseOp(opId, args...))
                {
                    ++opsRemoved;
                    return true;
                }
                return false;
            };

            // Stage-07 compile-time fix: one topological ranking per graph
            // sweep turns the per-candidate cycle guard into an O(1) check
            // (see lib/transform/topo_guard.hpp); the lean DFS remains as the
            // fallback for unranked (residual comb cycle) values.
            const topo_guard::TopoValueRanks topoRanks = topo_guard::computeTopoValueRanks(graph);
            topo_guard::ConeDfsScratch dfsScratch;
            std::size_t guardDfsTrue = 0;
            std::size_t guardDfsFalse = 0;
            auto dependsTransitivelyOn = [&](const wolvrix::lib::grh::Graph &graph_,
                                             wolvrix::lib::grh::ValueId valueId,
                                             wolvrix::lib::grh::ValueId targetId) {
                if (topoRanks.definitelyNotDependent(valueId, targetId))
                {
                    return false;
                }
                const bool dependent =
                    topo_guard::dependsTransitivelyOn(graph_, valueId, targetId, dfsScratch);
                if (dependent)
                {
                    ++guardDfsTrue;
                }
                else
                {
                    ++guardDfsFalse;
                }
                return dependent;
            };

            std::vector<wolvrix::lib::grh::OperationId> ops(graph.operations().begin(),
                                                  graph.operations().end());
            for (const auto opId : ops)
            {
                // Lean screening (stage-07): kind/operand/result/attr reads go
                // through the Graph id/span accessors; a fat Operation is
                // materialized only on rewrite error paths that need one for
                // diagnostics.
                const wolvrix::lib::grh::OperationKind opKind = graph.opKind(opId);
                const std::span<const wolvrix::lib::grh::ValueId> operands = graph.opOperands(opId);
                const std::span<const wolvrix::lib::grh::ValueId> results = graph.opResults(opId);

                if (opKind == wolvrix::lib::grh::OperationKind::kAssign &&
                    operands.size() == 1 && results.size() == 1)
                {
                    const wolvrix::lib::grh::ValueId srcId = operands[0];
                    const wolvrix::lib::grh::ValueId dstId = results[0];
                    if (!srcId.valid() || !dstId.valid())
                    {
                        continue;
                    }

                    if (isOutputPortValue(graph, dstId) && !hasOtherUsers(graph, dstId))
                    {
                        const wolvrix::lib::grh::OperationId constOpId = graph.valueDef(srcId);
                        if (constOpId.valid())
                        {
                            if (graph.opKind(constOpId) == wolvrix::lib::grh::OperationKind::kConstant &&
                                graph.opResults(constOpId).size() == 1)
                            {
                                // Copy the literal text up front: eraseOp
                                // mutates builder storage, so the borrowed
                                // attribute span is not held across it.
                                std::string literalText;
                                bool literalFound = false;
                                for (const auto &kv : graph.opAttrs(constOpId))
                                {
                                    if (kv.key == "constValue")
                                    {
                                        if (const auto *text = std::get_if<std::string>(&kv.value))
                                        {
                                            literalText = *text;
                                            literalFound = true;
                                        }
                                        break;
                                    }
                                }
                                if (!literalFound)
                                {
                                    continue;
                                }
                                if (!eraseOp(opId))
                                {
                                    continue;
                                }
                                if (isSingleUser(graph, srcId, opId) &&
                                    !graph.valueIsOutput(srcId))
                                {
                                    try
                                    {
                                        graph.replaceResult(constOpId, 0, dstId);
                                        graphChanged = true;
                                        continue;
                                    }
                                    catch (const std::exception &)
                                    {
                                        // Fall through to clone path.
                                    }
                                }

                                const std::string opName =
                                    makeInlineConstName(graph, "op",
                                                        graph.symbolText(graph.valueSymbol(dstId)),
                                                        inlineConstCounter);
                                wolvrix::lib::grh::SymbolId opSym = graph.internSymbol(opName);
                                wolvrix::lib::grh::OperationId newConst =
                                    graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant,
                                                          opSym);
                                graph.addResult(newConst, dstId);
                                graph.setAttr(newConst, "constValue", literalText);
                                graph.setOpSrcLoc(newConst,
                                                  makeTransformSrcLoc("redundant-elim", "clone_const"));
                                graphChanged = true;
                                continue;
                            }
                        }
                    }
                }

                if (opKind == wolvrix::lib::grh::OperationKind::kConcat &&
                    operands.size() == 1 && results.size() == 1)
                {
                    const wolvrix::lib::grh::ValueId operandId = operands[0];
                    const wolvrix::lib::grh::ValueId resultId = results[0];
                    if (!operandId.valid() || !resultId.valid())
                    {
                        continue;
                    }
                    if (!isTemporarySymbol(graph, resultId))
                    {
                        continue;
                    }
                    if (graph.valueWidth(operandId) != graph.valueWidth(resultId) ||
                        graph.valueSigned(operandId) != graph.valueSigned(resultId))
                    {
                        continue;
                    }
                    if (!topo_guard::shapesAliasable(graph, resultId, operandId))
                    {
                        continue;
                    }
                    auto onError = [&](const std::string &msg)
                    { this->error(graph, graph.getOperation(opId), msg); };
                    replaceUsers(graph, resultId, operandId, onError);
                    if (eraseOp(opId))
                    {
                        graphChanged = true;
                    }
                    continue;
                }

                if (opKind == wolvrix::lib::grh::OperationKind::kConcat &&
                    operands.size() >= 2 && results.size() == 1)
                {
                    const wolvrix::lib::grh::ValueId resultId = results[0];
                    if (!resultId.valid())
                    {
                        continue;
                    }

                    std::vector<wolvrix::lib::grh::ValueId> flattenedOperands;
                    flattenedOperands.reserve(operands.size());
                    bool changed = false;
                    bool failed = false;
                    int64_t flattenedWidth = 0;

                    for (const auto operandId : operands)
                    {
                        if (!operandId.valid())
                        {
                            failed = true;
                            break;
                        }
                        const wolvrix::lib::grh::OperationId operandDefId = graph.valueDef(operandId);
                        if (!operandDefId.valid())
                        {
                            flattenedOperands.push_back(operandId);
                            flattenedWidth += graph.valueWidth(operandId);
                            continue;
                        }

                        if (graph.opKind(operandDefId) != wolvrix::lib::grh::OperationKind::kConcat ||
                            !isTemporarySymbol(graph, operandId) ||
                            !isSingleUser(graph, operandId, opId))
                        {
                            flattenedOperands.push_back(operandId);
                            flattenedWidth += graph.valueWidth(operandId);
                            continue;
                        }

                        const std::span<const wolvrix::lib::grh::ValueId> innerOperands =
                            graph.opOperands(operandDefId);
                        if (innerOperands.empty())
                        {
                            flattenedOperands.push_back(operandId);
                            flattenedWidth += graph.valueWidth(operandId);
                            continue;
                        }

                        int64_t innerWidth = 0;
                        bool canFlatten = true;
                        for (const auto innerOperandId : innerOperands)
                        {
                            if (!innerOperandId.valid() ||
                                dependsTransitivelyOn(graph, innerOperandId, resultId))
                            {
                                canFlatten = false;
                                break;
                            }
                            const int64_t width = graph.valueWidth(innerOperandId);
                            if (width <= 0)
                            {
                                canFlatten = false;
                                break;
                            }
                            innerWidth += width;
                        }
                        if (!canFlatten || innerWidth != graph.valueWidth(operandId))
                        {
                            flattenedOperands.push_back(operandId);
                            flattenedWidth += graph.valueWidth(operandId);
                            continue;
                        }

                        flattenedOperands.insert(flattenedOperands.end(),
                                                 innerOperands.begin(),
                                                 innerOperands.end());
                        flattenedWidth += innerWidth;
                        changed = true;
                    }

                    if (!changed || failed)
                    {
                        continue;
                    }

                    if (flattenedWidth != graph.valueWidth(resultId))
                    {
                        continue;
                    }

                    try
                    {
                        for (std::size_t i = 0; i < flattenedOperands.size(); ++i)
                        {
                            if (graph.opOperands(opId).size() > i)
                            {
                                graph.replaceOperand(opId, i, flattenedOperands[i]);
                            }
                            else
                            {
                                graph.addOperand(opId, flattenedOperands[i]);
                            }
                        }
                        while (graph.opOperands(opId).size() > flattenedOperands.size())
                        {
                            graph.eraseOperand(opId, graph.opOperands(opId).size() - 1);
                        }
                        graphChanged = true;
                    }
                    catch (const std::exception &ex)
                    {
                        this->error(graph, graph.getOperation(opId),
                                    std::string("Failed to flatten nested concat: ") + ex.what());
                    }
                    continue;
                }

                if (opKind == wolvrix::lib::grh::OperationKind::kSliceStatic &&
                    operands.size() == 1 && results.size() == 1)
                {
                    const wolvrix::lib::grh::ValueId baseId = operands[0];
                    const wolvrix::lib::grh::ValueId resultId = results[0];
                    if (!baseId.valid() || !resultId.valid())
                    {
                        continue;
                    }
                    const auto sliceStart = getIntAttr(graph, opId, "sliceStart");
                    const auto sliceEnd = getIntAttr(graph, opId, "sliceEnd");
                    if (!sliceStart || !sliceEnd || *sliceStart < 0 || *sliceEnd < *sliceStart)
                    {
                        continue;
                    }

                    const bool isIdentitySlice =
                        *sliceStart == 0 &&
                        *sliceEnd + 1 == graph.valueWidth(baseId) &&
                        graph.valueWidth(baseId) == graph.valueWidth(resultId) &&
                        graph.valueSigned(baseId) == graph.valueSigned(resultId);
                    if (isIdentitySlice &&
                        !dependsTransitivelyOn(graph, baseId, resultId) &&
                        topo_guard::shapesAliasable(graph, resultId, baseId) &&
                        (isTemporarySymbol(graph, resultId) || isOutputPortValue(graph, resultId)))
                    {
                        auto onError = [&](const std::string &msg)
                        { this->error(graph, graph.getOperation(opId), msg); };
                        replaceUsers(graph, resultId, baseId, onError);
                        if (eraseOp(opId))
                        {
                            graphChanged = true;
                        }
                        continue;
                    }

                    const wolvrix::lib::grh::OperationId baseDefId = graph.valueDef(baseId);
                    if (!baseDefId.valid())
                    {
                        continue;
                    }
                    const wolvrix::lib::grh::OperationKind baseDefKind = graph.opKind(baseDefId);
                    if (baseDefKind == wolvrix::lib::grh::OperationKind::kSliceStatic &&
                        graph.opOperands(baseDefId).size() == 1)
                    {
                        const auto innerStart = getIntAttr(graph, baseDefId, "sliceStart");
                        const auto innerEnd = getIntAttr(graph, baseDefId, "sliceEnd");
                        if (innerStart && innerEnd && *innerStart >= 0 && *innerEnd >= *innerStart)
                        {
                            const int64_t combinedStart = *innerStart + *sliceStart;
                            const int64_t combinedEnd = *innerStart + *sliceEnd;
                            if (combinedEnd <= *innerEnd)
                            {
                                const wolvrix::lib::grh::ValueId innerBaseId =
                                    graph.opOperands(baseDefId)[0];
                                if (innerBaseId.valid() &&
                                    !dependsTransitivelyOn(graph, innerBaseId, resultId))
                                {
                                    try
                                    {
                                        graph.replaceOperand(opId, 0, innerBaseId);
                                        graph.setAttr(opId, "sliceStart", combinedStart);
                                        graph.setAttr(opId, "sliceEnd", combinedEnd);
                                        graphChanged = true;
                                        continue;
                                    }
                                    catch (const std::exception &ex)
                                    {
                                        this->error(graph, graph.getOperation(opId),
                                                    std::string("Failed to combine nested slice: ") + ex.what());
                                    }
                                }
                            }
                        }
                    }
                    if (baseDefKind != wolvrix::lib::grh::OperationKind::kConcat)
                    {
                        continue;
                    }
                    const std::span<const wolvrix::lib::grh::ValueId> concatOperands =
                        graph.opOperands(baseDefId);
                    if (concatOperands.empty())
                    {
                        continue;
                    }

                    int64_t totalWidth = 0;
                    std::vector<int64_t> operandWidths;
                    operandWidths.reserve(concatOperands.size());
                    bool widthsOk = true;
                    for (const auto operandId : concatOperands)
                    {
                        if (!operandId.valid())
                        {
                            widthsOk = false;
                            break;
                        }
                        const int64_t width = graph.valueWidth(operandId);
                        if (width <= 0)
                        {
                            widthsOk = false;
                            break;
                        }
                        operandWidths.push_back(width);
                        totalWidth += width;
                    }
                    if (!widthsOk || totalWidth <= 0 || totalWidth != graph.valueWidth(baseId) || *sliceEnd >= totalWidth)
                    {
                        continue;
                    }

                    int64_t cursor = totalWidth;
                    for (std::size_t i = 0; i < concatOperands.size(); ++i)
                    {
                        const int64_t width = operandWidths[i];
                        const int64_t operandHigh = cursor - 1;
                        const int64_t operandLow = cursor - width;
                        cursor = operandLow;
                        if (*sliceStart < operandLow || *sliceEnd > operandHigh)
                        {
                            continue;
                        }

                        const wolvrix::lib::grh::ValueId selectedOperandId = concatOperands[i];
                        if (!selectedOperandId.valid() ||
                            dependsTransitivelyOn(graph, selectedOperandId, resultId))
                        {
                            break;
                        }
                        const int64_t newLow = *sliceStart - operandLow;
                        const int64_t newHigh = *sliceEnd - operandLow;
                        const bool selectsWholeOperand =
                            newLow == 0 &&
                            newHigh + 1 == graph.valueWidth(selectedOperandId) &&
                            graph.valueWidth(selectedOperandId) == graph.valueWidth(resultId) &&
                            graph.valueSigned(selectedOperandId) == graph.valueSigned(resultId);

                        if (selectsWholeOperand &&
                            topo_guard::shapesAliasable(graph, resultId, selectedOperandId) &&
                            (isTemporarySymbol(graph, resultId) || isOutputPortValue(graph, resultId)))
                        {
                            auto onError = [&](const std::string &msg)
                            { this->error(graph, graph.getOperation(opId), msg); };
                            replaceUsers(graph, resultId, selectedOperandId, onError);
                            if (eraseOp(opId))
                            {
                                graphChanged = true;
                            }
                            break;
                        }

                        try
                        {
                            bool changed = false;
                            if (selectedOperandId != baseId)
                            {
                                graph.replaceOperand(opId, 0, selectedOperandId);
                                changed = true;
                            }
                            if (newLow != *sliceStart)
                            {
                                graph.setAttr(opId, "sliceStart", newLow);
                                changed = true;
                            }
                            if (newHigh != *sliceEnd)
                            {
                                graph.setAttr(opId, "sliceEnd", newHigh);
                                changed = true;
                            }
                            if (changed)
                            {
                                graphChanged = true;
                            }
                        }
                        catch (const std::exception &ex)
                        {
                            this->error(graph, graph.getOperation(opId),
                                        std::string("Failed to retarget slice through concat: ") + ex.what());
                        }
                        break;
                    }
                    continue;
                }

                if (opKind == wolvrix::lib::grh::OperationKind::kLogicOr &&
                    operands.size() >= 2 && results.size() == 1)
                {
                    const wolvrix::lib::grh::ValueId resultId = results[0];
                    if (!resultId.valid())
                    {
                        continue;
                    }
                    if (graph.valueWidth(resultId) != 1)
                    {
                        continue;
                    }
                    bool alwaysTrue = false;
                    for (std::size_t i = 0; i < operands.size() && !alwaysTrue; ++i)
                    {
                        const wolvrix::lib::grh::ValueId lhs = operands[i];
                        if (!lhs.valid())
                        {
                            continue;
                        }
                        for (std::size_t j = i + 1; j < operands.size(); ++j)
                        {
                            const wolvrix::lib::grh::ValueId rhs = operands[j];
                            if (!rhs.valid())
                            {
                                continue;
                            }
                            if (isLogicNotOf(graph, lhs, rhs) ||
                                isLogicNotOf(graph, rhs, lhs))
                            {
                                alwaysTrue = true;
                                break;
                            }
                        }
                    }
                    if (alwaysTrue &&
                        (isTemporarySymbol(graph, resultId) || isOutputPortValue(graph, resultId)))
                    {
                        auto onError = [&](const std::string &msg)
                        { this->error(graph, graph.getOperation(opId), msg); };
                        wolvrix::lib::grh::ValueId constOne =
                            createInlineConst(graph, graph.symbolText(graph.valueSymbol(resultId)),
                                              1, graph.valueSigned(resultId), "1'b1",
                                              inlineConstCounter);
                        replaceUsers(graph, resultId, constOne, onError);
                        eraseOp(opId);
                        graphChanged = true;
                        continue;
                    }
                }

                if (opKind == wolvrix::lib::grh::OperationKind::kAssign &&
                    operands.size() == 1 && results.size() == 1)
                {
                    const wolvrix::lib::grh::ValueId srcId = operands[0];
                    const wolvrix::lib::grh::ValueId dstId = results[0];
                    if (!srcId.valid() || !dstId.valid())
                    {
                        continue;
                    }
                    if (isOutputPortValue(graph, dstId) && !hasOtherUsers(graph, dstId))
                    {
                        if (graph.valueIsInput(srcId) || graph.valueIsOutput(srcId) ||
                            graph.valueIsInout(srcId))
                        {
                            continue;
                        }
                        const wolvrix::lib::grh::OperationId dstDefId = graph.valueDef(dstId);
                        if (!dstDefId.valid() || dstDefId != opId)
                        {
                            continue;
                        }
                        if (!isSingleUser(graph, srcId, opId))
                        {
                            continue;
                        }
                        if (graph.valueWidth(srcId) != graph.valueWidth(dstId) ||
                            graph.valueSigned(srcId) != graph.valueSigned(dstId))
                        {
                            continue;
                        }
                        // The assign may be an aggregate pack/unpack rather than
                        // a pure rename: a mismatched shape would backfill the
                        // aggregate shape onto the (flat) output port value via
                        // replaceResult, corrupting the port's declared type.
                        const std::span<const int64_t> srcShape = graph.valueShape(srcId);
                        const std::span<const int64_t> dstShape = graph.valueShape(dstId);
                        if (!std::equal(srcShape.begin(), srcShape.end(),
                                        dstShape.begin(), dstShape.end()))
                        {
                            continue;
                        }
                        const wolvrix::lib::grh::OperationId defOpId = graph.valueDef(srcId);
                        if (!defOpId.valid() || defOpId == opId)
                        {
                            continue;
                        }
                        const std::span<const wolvrix::lib::grh::ValueId> defResults =
                            graph.opResults(defOpId);
                        if (defResults.empty())
                        {
                            continue;
                        }
                        std::size_t defIndex = defResults.size();
                        for (std::size_t i = 0; i < defResults.size(); ++i)
                        {
                            if (defResults[i] == srcId)
                            {
                                defIndex = i;
                                break;
                            }
                        }
                        if (defIndex >= defResults.size())
                        {
                            continue;
                        }
                        if (!eraseOp(opId))
                        {
                            continue;
                        }
                        try
                        {
                            graph.replaceResult(defOpId, defIndex, dstId);
                            graphChanged = true;
                        }
                        catch (const std::exception &ex)
                        {
                            this->error(graph, graph.getOperation(defOpId),
                                        std::string("Failed to inline output assign: ") +
                                            ex.what());
                        }
                        continue;
                    }
                    if (graph.valueIsInput(dstId) || graph.valueIsOutput(dstId) ||
                        graph.valueIsInout(dstId))
                    {
                        continue;
                    }
                    if (!isTemporarySymbol(graph, srcId))
                    {
                        continue;
                    }
                    if (!isSingleUser(graph, srcId, opId))
                    {
                        continue;
                    }
                    if (graph.valueWidth(srcId) != graph.valueWidth(dstId) ||
                        graph.valueSigned(srcId) != graph.valueSigned(dstId))
                    {
                        continue;
                    }
                    // Same guard as the output-port branch above: a
                    // shape-mismatched assign is an aggregate pack/unpack, and
                    // inlining it backfills the aggregate shape onto a value
                    // that was declared flat.
                    const std::span<const int64_t> srcShape = graph.valueShape(srcId);
                    const std::span<const int64_t> dstShape = graph.valueShape(dstId);
                    if (!std::equal(srcShape.begin(), srcShape.end(),
                                    dstShape.begin(), dstShape.end()))
                    {
                        continue;
                    }
                    const wolvrix::lib::grh::OperationId defOpId = graph.valueDef(srcId);
                    if (!defOpId.valid() || defOpId == opId)
                    {
                        continue;
                    }
                    const std::span<const wolvrix::lib::grh::ValueId> defResults =
                        graph.opResults(defOpId);
                    if (defResults.empty())
                    {
                        continue;
                    }
                    std::size_t defIndex = defResults.size();
                    for (std::size_t i = 0; i < defResults.size(); ++i)
                    {
                        if (defResults[i] == srcId)
                        {
                            defIndex = i;
                            break;
                        }
                    }
                    if (defIndex >= defResults.size())
                    {
                        continue;
                    }
                    if (!eraseOp(opId, std::array<wolvrix::lib::grh::ValueId, 1>{dstId}))
                    {
                        continue;
                    }
                    try
                    {
                        graph.replaceResult(defOpId, defIndex, dstId);
                        graphChanged = true;
                    }
                    catch (const std::exception &ex)
                    {
                        this->error(graph, graph.getOperation(defOpId),
                                    std::string("Failed to inline assign: ") + ex.what());
                    }
                    continue;
                }

                if (opKind == wolvrix::lib::grh::OperationKind::kNot &&
                    operands.size() == 1 && results.size() == 1)
                {
                    const wolvrix::lib::grh::ValueId operandId = operands[0];
                    const wolvrix::lib::grh::ValueId resultId = results[0];
                    if (!operandId.valid() || !resultId.valid())
                    {
                        continue;
                    }
                    if (!isTemporarySymbol(graph, operandId))
                    {
                        continue;
                    }
                    if (!isSingleUser(graph, operandId, opId))
                    {
                        continue;
                    }
                    const wolvrix::lib::grh::OperationId defOpId = graph.valueDef(operandId);
                    if (!defOpId.valid())
                    {
                        continue;
                    }
                    if (graph.opKind(defOpId) != wolvrix::lib::grh::OperationKind::kXor)
                    {
                        continue;
                    }
                    const std::span<const wolvrix::lib::grh::ValueId> defResults =
                        graph.opResults(defOpId);
                    if (defResults.empty())
                    {
                        continue;
                    }
                    std::size_t defIndex = defResults.size();
                    for (std::size_t i = 0; i < defResults.size(); ++i)
                    {
                        if (defResults[i] == operandId)
                        {
                            defIndex = i;
                            break;
                        }
                    }
                    if (defIndex >= defResults.size())
                    {
                        continue;
                    }
                    if (!eraseOp(opId, std::array<wolvrix::lib::grh::ValueId, 1>{resultId}))
                    {
                        continue;
                    }
                    graph.setOpKind(defOpId, wolvrix::lib::grh::OperationKind::kXnor);
                    try
                    {
                        graph.replaceResult(defOpId, defIndex, resultId);
                        graphChanged = true;
                    }
                    catch (const std::exception &ex)
                    {
                        this->error(graph, graph.getOperation(defOpId),
                                    std::string("Failed to fold NOT/XOR: ") + ex.what());
                    }
                    continue;
                }
            }

            struct CseEntry
            {
                wolvrix::lib::grh::OperationId op;
                wolvrix::lib::grh::ValueId value;
                uint64_t revision = 0;
            };

            std::unordered_map<OpSignature, CseEntry, OpSignatureHash> seen;
            std::deque<wolvrix::lib::grh::OperationId> cseOps;
            enum : uint8_t { Idle, Queued, Erased };
            std::vector<uint8_t> queued;
            std::vector<uint64_t> revisions;
            auto enqueue = [&](wolvrix::lib::grh::OperationId opId) {
                if (opId.index >= queued.size())
                {
                    queued.resize(static_cast<std::size_t>(opId.index) + 1, Idle);
                    revisions.resize(queued.size(), 0);
                }
                if (queued[opId.index] == Idle && isCseCandidate(graph, opId))
                {
                    queued[opId.index] = Queued;
                    cseOps.push_back(opId);
                }
            };
            for (const auto opId : graph.operations())
            {
                enqueue(opId);
            }
            while (!cseOps.empty())
            {
                const auto opId = cseOps.front();
                cseOps.pop_front();
                if (queued[opId.index] == Erased)
                {
                    continue;
                }
                queued[opId.index] = Idle;
                // Lean screening (stage-07): candidate checks read id/span
                // accessors only; kConstant never enters CSE (see
                // isCseCandidate), so no mega constValue literal is copied
                // into a signature.
                if (!isCseCandidate(graph, opId))
                {
                    continue;
                }
                const wolvrix::lib::grh::ValueId resultId = graph.opResults(opId)[0];
                if (graph.valueType(resultId) != wolvrix::lib::grh::ValueType::Logic)
                {
                    continue;
                }
                std::optional<OpSignature> sig = makeSignature(graph, opId);
                if (!sig)
                {
                    continue;
                }
                auto [it, inserted] = seen.emplace(std::move(*sig), CseEntry{opId, resultId, revisions[opId.index]});
                if (inserted)
                {
                    continue;
                }
                // Only operand rewrites occur during CSE. Stamp their users so
                // stale entries are rejected without rebuilding the signature
                // of the representative on every collision.
                if (queued[it->second.op.index] == Erased || revisions[it->second.op.index] != it->second.revision)
                {
                    it->second = CseEntry{opId, resultId, revisions[opId.index]};
                    continue;
                }
                const wolvrix::lib::grh::ValueId canonicalValue = it->second.value;
                if (canonicalValue == resultId)
                {
                    continue;
                }
                // A flat value may also be used as an array index. Merging it
                // with a shaped result would backfill shape onto every user of
                // the representative, changing those index operands' types.
                const auto resultShape = graph.valueShape(resultId);
                const auto canonicalShape = graph.valueShape(canonicalValue);
                if (!std::equal(resultShape.begin(), resultShape.end(),
                                canonicalShape.begin(), canonicalShape.end()))
                {
                    continue;
                }
                auto onError = [&](const std::string &msg)
                { this->error(graph, graph.getOperation(opId), msg); };
                const auto resultValue = graph.getValue(resultId);
                for (const auto &use : resultValue.users())
                {
                    enqueue(use.operation);
                    // `enqueue` grows both state arrays for operation IDs
                    // discovered through a user edge.  A user need not be a
                    // CSE candidate (and can therefore have been absent from
                    // the initial queue), so the revision stamp must be
                    // written only after that growth has happened.
                    ++revisions[use.operation.index];
                }
                replaceUsers(graph, resultId, canonicalValue, onError);
                if (eraseOp(opId))
                {
                    queued[opId.index] = Erased;
                    graphChanged = true;
                }
            }

            for (const auto &port : graph.outputPorts())
            {
                if (port.name.empty() || !port.value.valid())
                {
                    continue;
                }
                if (graph.symbolText(graph.valueSymbol(port.value)) == port.name)
                {
                    continue;
                }
                const wolvrix::lib::grh::OperationId def = graph.valueDef(port.value);
                if (!def.valid())
                {
                    continue;
                }
                if (graph.opKind(def) != wolvrix::lib::grh::OperationKind::kConstant)
                {
                    continue;
                }
                wolvrix::lib::grh::ValueId existing = graph.findValue(port.name);
                if (existing.valid() && existing != port.value)
                {
                    // Cold path (per output port): keep the fat Value for the
                    // users emptiness check — no lean users accessor exists.
                    wolvrix::lib::grh::Value existingValue = graph.getValue(existing);
                    if (existingValue.isInput() || existingValue.isOutput() ||
                        existingValue.isInout() || existingValue.definingOp().valid() ||
                        !existingValue.users().empty())
                    {
                        continue;
                    }
                    graph.eraseValue(existing);
                    ++valuesRemoved;
                }
                if (graph.findOperation(port.name).valid())
                {
                    continue;
                }
                wolvrix::lib::grh::SymbolId portSym = graph.lookupSymbol(port.name);
                if (!portSym.valid())
                {
                    portSym = graph.internSymbol(port.name);
                }
                if (!portSym.valid())
                {
                    continue;
                }
                graph.setValueSymbol(port.value, portSym);
                graphChanged = true;
            }

            anyChanged = anyChanged || graphChanged;
            if (graphChanged)
            {
                ++changedGraphs;
            }
            if (std::getenv("WOLVRIX_SIMPLIFY_PHASE_TIMING") != nullptr)
            {
                std::fprintf(stderr,
                             "[re-guard] graph=%s values=%zu unranked=%zu fastSafe=%zu "
                             "fallbackUnranked=%zu fallbackOrdered=%zu dfsTrue=%zu dfsFalse=%zu "
                             "dfsNodes=%llu\n",
                             std::string(graph.symbol()).c_str(),
                             topoRanks.totalValues,
                             topoRanks.unrankedValues,
                             topoRanks.fastSafe,
                             topoRanks.fallbackUnranked,
                             topoRanks.fallbackOrdered,
                             guardDfsTrue,
                             guardDfsFalse,
                             static_cast<unsigned long long>(dfsScratch.nodesExpanded));
            }
        }

        result.changed = anyChanged;
        result.failed = false;
        std::string message = "graphs=" + std::to_string(graphCount);
        message.append(", changedGraphs=");
        message.append(std::to_string(changedGraphs));
        message.append(result.changed ? ", changed=true" : ", changed=false");
        message.append(", opsRemoved=");
        message.append(std::to_string(opsRemoved));
        message.append(", valuesRemoved=");
        message.append(std::to_string(valuesRemoved));
        logDebug(std::move(message));
        if (std::getenv("WOLVRIX_SIMPLIFY_PHASE_TIMING") != nullptr)
        {
            std::fprintf(stderr, "[simplify-phase] pass=redundant-elim %lldms\n",
                         static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    std::chrono::steady_clock::now() - timingStart)
                                                    .count()));
        }
        return result;
    }

} // namespace wolvrix::lib::transform
