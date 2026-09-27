#include "transform/strip_debug.hpp"

#include "core/grh.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wolvrix::lib::transform
{

    namespace
    {
        using wolvrix::lib::grh::Graph;
        using wolvrix::lib::grh::Operation;
        using wolvrix::lib::grh::OperationId;
        using wolvrix::lib::grh::OperationIdHash;
        using wolvrix::lib::grh::OperationKind;
        using wolvrix::lib::grh::Port;
        using wolvrix::lib::grh::InoutPort;
        using wolvrix::lib::grh::SymbolId;
        using wolvrix::lib::grh::Value;
        using wolvrix::lib::grh::ValueId;
        using wolvrix::lib::grh::ValueIdHash;
        using wolvrix::lib::grh::ValueType;
        using wolvrix::lib::grh::SrcLoc;

        struct ValueInfo
        {
            std::string symbol;
            int32_t width = 0;
            bool isSigned = false;
            ValueType type = ValueType::Logic;
            std::optional<SrcLoc> srcLoc;
        };

        struct PortInfo
        {
            std::string name;
            ValueInfo value;
        };

        struct InoutPortInfo
        {
            std::string name;
            ValueInfo in;
            ValueInfo out;
            ValueInfo oe;
        };

        struct PortSnapshot
        {
            std::vector<PortInfo> inputs;
            std::vector<PortInfo> outputs;
            std::vector<InoutPortInfo> inouts;
        };

        struct StripPlan
        {
            std::vector<OperationId> stripOps;
            std::unordered_set<OperationId, OperationIdHash> stripSet;
            std::vector<OperationId> constOps;
            std::unordered_set<OperationId, OperationIdHash> constSet;
            std::vector<ValueId> boundaryInputs;
            std::unordered_set<ValueId, ValueIdHash> boundaryInputSet;
            std::vector<ValueId> boundaryResults;
            std::unordered_set<ValueId, ValueIdHash> boundaryResultSet;
        };

        struct BoundaryPort
        {
            ValueId value;
            ValueInfo info;
            std::string extPort;
            std::string intPort;
        };

        struct BoundaryPorts
        {
            std::vector<BoundaryPort> inputs;
            std::vector<BoundaryPort> results;
            std::unordered_map<ValueId, std::string, ValueIdHash> extInByValue;
            std::unordered_map<ValueId, std::string, ValueIdHash> extOutByValue;
            std::unordered_map<ValueId, std::string, ValueIdHash> intOutByValue;
            std::unordered_map<ValueId, std::string, ValueIdHash> intInByValue;
            std::unordered_map<ValueId, ValueId, ValueIdHash> intPortValueByValue;
        };

        ValueInfo captureValueInfo(const Graph &graph, ValueId valueId)
        {
            const Value value = graph.getValue(valueId);
            ValueInfo info;
            info.symbol = std::string(value.symbolText());
            info.width = value.width();
            info.isSigned = value.isSigned();
            info.type = value.type();
            info.srcLoc = value.srcLoc();
            return info;
        }

        PortSnapshot snapshotPorts(const Graph &graph)
        {
            PortSnapshot snapshot;
            for (const auto &port : graph.inputPorts())
            {
                snapshot.inputs.push_back(PortInfo{port.name, captureValueInfo(graph, port.value)});
            }
            for (const auto &port : graph.outputPorts())
            {
                snapshot.outputs.push_back(PortInfo{port.name, captureValueInfo(graph, port.value)});
            }
            for (const auto &port : graph.inoutPorts())
            {
                InoutPortInfo info;
                info.name = port.name;
                info.in = captureValueInfo(graph, port.in);
                info.out = captureValueInfo(graph, port.out);
                info.oe = captureValueInfo(graph, port.oe);
                snapshot.inouts.push_back(std::move(info));
            }
            return snapshot;
        }

        bool isStripKind(OperationKind kind)
        {
            switch (kind)
            {
            case OperationKind::kDpicCall:
            case OperationKind::kSystemTask:
            case OperationKind::kInstance:
            case OperationKind::kBlackbox:
                return true;
            default:
                return false;
            }
        }

        std::optional<std::string> getAttrString(const Operation &op, std::string_view key)
        {
            auto attr = op.attr(key);
            if (!attr)
            {
                return std::nullopt;
            }
            if (auto value = std::get_if<std::string>(&*attr))
            {
                return *value;
            }
            return std::nullopt;
        }

        std::string normalizePortBase(std::string_view text, std::string_view fallback)
        {
            std::string normalized = Graph::normalizeComponent(text);
            if (normalized.empty())
            {
                normalized = std::string(fallback);
            }
            if (!normalized.empty() && normalized.front() >= '0' && normalized.front() <= '9')
            {
                normalized.insert(normalized.begin(), '_');
            }
            return normalized;
        }

        std::string uniquePortName(std::unordered_set<std::string> &used, std::string base, bool &conflict)
        {
            conflict = false;
            std::string candidate = std::move(base);
            if (used.insert(candidate).second)
            {
                return candidate;
            }
            conflict = true;
            std::string root = candidate;
            int suffix = 0;
            while (true)
            {
                candidate = root + "_" + std::to_string(++suffix);
                if (used.insert(candidate).second)
                {
                    return candidate;
                }
            }
        }

        std::string uniqueGraphName(wolvrix::lib::grh::Design &design, std::string base)
        {
            std::string candidate = base;
            int suffix = 0;
            while (design.findGraph(candidate) != nullptr)
            {
                candidate = base + "_" + std::to_string(++suffix);
            }
            return candidate;
        }

        SymbolId internUniqueSymbol(Graph &graph, std::string base)
        {
            if (base.empty())
            {
                base = "value";
            }
            std::string candidate = base;
            int suffix = 0;
            while (true)
            {
                SymbolId sym = graph.internSymbol(candidate);
                if (!sym.valid())
                {
                    candidate = base + "_" + std::to_string(++suffix);
                    continue;
                }
                if (!graph.findValue(sym).valid() && !graph.findOperation(sym).valid())
                {
                    return sym;
                }
                candidate = base + "_" + std::to_string(++suffix);
            }
        }

        SymbolId internSymbolOrUnique(Graph &graph, std::string_view base)
        {
            SymbolId sym = graph.internSymbol(base);
            if (!sym.valid() || graph.findValue(sym).valid() || graph.findOperation(sym).valid())
            {
                sym = internUniqueSymbol(graph, std::string(base));
            }
            return sym;
        }

        ValueId mapValueToClone(const Graph &source, Graph &clone, ValueId valueId)
        {
            const Value value = source.getValue(valueId);
            std::string_view sym = value.symbolText();
            if (sym.empty())
            {
                return ValueId::invalid();
            }
            return clone.findValue(sym);
        }

        OperationId mapOpToClone(const Graph &source, Graph &clone, OperationId opId)
        {
            const Operation op = source.getOperation(opId);
            std::string_view sym = op.symbolText();
            if (sym.empty())
            {
                return OperationId::invalid();
            }
            return clone.findOperation(sym);
        }

        void copyAttrs(Graph &target, OperationId dstOp, const Operation &srcOp)
        {
            for (const auto &attr : srcOp.attrs())
            {
                target.setAttr(dstOp, attr.key, attr.value);
            }
        }

        ValueId createValueClone(Graph &target, const ValueInfo &info)
        {
            SymbolId sym = internSymbolOrUnique(target, info.symbol);
            ValueId valueId = target.createValue(sym, info.width, info.isSigned, info.type);
            if (info.srcLoc)
            {
                target.setValueSrcLoc(valueId, *info.srcLoc);
            }
            return valueId;
        }

        ValueId createBridgeValue(Graph &target, const ValueInfo &info, std::string_view base)
        {
            std::string normalized = normalizePortBase(base, "bridge");
            SymbolId sym = internUniqueSymbol(target, normalized);
            ValueId valueId = target.createValue(sym, info.width, info.isSigned, info.type);
            if (info.srcLoc)
            {
                target.setValueSrcLoc(valueId, *info.srcLoc);
            }
            return valueId;
        }

        OperationId addAssign(Graph &graph, ValueId src, ValueId dst)
        {
            SymbolId opSym = graph.makeInternalOpSym();
            OperationId op = graph.createOperation(OperationKind::kAssign, opSym);
            graph.addOperand(op, src);
            graph.addResult(op, dst);
            graph.setOpSrcLoc(op, makeTransformSrcLoc("strip-debug", "bridge_assign"));
            return op;
        }

        std::unordered_set<std::string> collectPortNames(const Graph &graph)
        {
            std::unordered_set<std::string> names;
            for (const auto &port : graph.inputPorts())
            {
                if (!port.name.empty())
                {
                    names.insert(port.name);
                }
            }
            for (const auto &port : graph.outputPorts())
            {
                if (!port.name.empty())
                {
                    names.insert(port.name);
                }
            }
            for (const auto &port : graph.inoutPorts())
            {
                if (!port.name.empty())
                {
                    names.insert(port.name);
                }
            }
            return names;
        }

        std::vector<std::string> splitPath(std::string_view path)
        {
            std::vector<std::string> out;
            std::string current;
            for (char ch : path)
            {
                if (ch == '.')
                {
                    if (!current.empty())
                    {
                        out.push_back(current);
                        current.clear();
                    }
                    continue;
                }
                current.push_back(ch);
            }
            if (!current.empty())
            {
                out.push_back(current);
            }
            return out;
        }

        OperationId findUniqueInstance(const Graph &graph, std::string_view instanceName)
        {
            OperationId found = OperationId::invalid();
            for (const auto opId : graph.operations())
            {
                if (!opId.valid())
                {
                    continue;
                }
                const Operation op = graph.getOperation(opId);
                if (op.kind() != OperationKind::kInstance)
                {
                    continue;
                }
                auto name = getAttrString(op, "instanceName");
                if (!name || *name != instanceName)
                {
                    continue;
                }
                if (found.valid())
                {
                    return OperationId::invalid();
                }
                found = opId;
            }
            return found;
        }

        std::optional<std::string> resolveTargetGraphName(wolvrix::lib::grh::Design &design,
                                                          std::string_view path,
                                                          std::string &error)
        {
            const std::vector<std::string> segments = splitPath(path);
            if (segments.empty())
            {
                error = "strip-debug path must not be empty";
                return std::nullopt;
            }
            if (segments.size() == 1)
            {
                if (design.findGraph(segments.front()) == nullptr)
                {
                    error = "strip-debug graph not found: " + segments.front();
                    return std::nullopt;
                }
                return segments.front();
            }

            Graph *current = design.findGraph(segments.front());
            if (current == nullptr)
            {
                error = "strip-debug root graph not found: " + segments.front();
                return std::nullopt;
            }
            for (std::size_t i = 1; i < segments.size(); ++i)
            {
                const OperationId instOp = findUniqueInstance(*current, segments[i]);
                if (!instOp.valid())
                {
                    error = "strip-debug instance not found or not unique: " + segments[i];
                    return std::nullopt;
                }
                const Operation op = current->getOperation(instOp);
                auto moduleName = getAttrString(op, "moduleName");
                if (!moduleName || moduleName->empty())
                {
                    error = "strip-debug instance missing moduleName: " + segments[i];
                    return std::nullopt;
                }
                Graph *child = design.findGraph(*moduleName);
                if (child == nullptr)
                {
                    error = "strip-debug child graph not found: " + *moduleName;
                    return std::nullopt;
                }
                current = child;
            }
            return current->symbol();
        }

    } // namespace

    StripDebugPass::StripDebugPass()
        : StripDebugPass(StripDebugOptions{})
    {
    }

    StripDebugPass::StripDebugPass(StripDebugOptions options)
        : Pass("strip-debug", "strip-debug", "Split selected modules into *_debug_part and *_logic_part"),
          options_(std::move(options))
    {
    }

    PassResult StripDebugPass::run()
    {
        PassResult result;
        std::vector<std::string> targetNames;
        if (options_.path.empty())
        {
            targetNames = design().topGraphs();
        }
        else
        {
            std::string resolveError;
            auto targetName = resolveTargetGraphName(design(), options_.path, resolveError);
            if (!targetName)
            {
                error(std::move(resolveError));
                result.failed = true;
                return result;
            }
            targetNames.push_back(*targetName);
        }

        if (targetNames.empty())
        {
            info("strip-debug: no target graphs to process");
            return result;
        }

        for (const auto &topName : targetNames)
        {
            Graph *top = design().findGraph(topName);
            if (!top)
            {
                warning("strip-debug: top graph not found", topName);
                continue;
            }

            StripPlan plan;
            const auto opCount = top->operations().size();
            plan.stripOps.reserve(opCount);
            plan.stripSet.reserve(opCount);
            for (const auto opId : top->operations())
            {
                const Operation op = top->getOperation(opId);
                if (!isStripKind(op.kind()))
                {
                    continue;
                }
                if (plan.stripSet.insert(opId).second)
                {
                    plan.stripOps.push_back(opId);
                }
                if (op.kind() == OperationKind::kDpicCall)
                {
                    auto importSym = getAttrString(op, "targetImportSymbol");
                    if (importSym && !importSym->empty())
                    {
                        OperationId importOpId = top->findOperation(*importSym);
                        if (importOpId.valid())
                        {
                            const Operation importOp = top->getOperation(importOpId);
                            if (importOp.kind() == OperationKind::kDpicImport)
                            {
                                if (plan.stripSet.insert(importOpId).second)
                                {
                                    plan.stripOps.push_back(importOpId);
                                }
                            }
                        }
                        else
                        {
                            warning(*top, "strip-debug: kDpicCall missing kDpicImport for symbol " + *importSym);
                        }
                    }
                }
            }

            if (plan.stripOps.empty())
            {
                info("strip-debug: no strip operations in top", topName);
                continue;
            }

            PortSnapshot portSnapshot = snapshotPorts(*top);

            std::unordered_set<ValueId, ValueIdHash> outputBound;
            outputBound.reserve(top->outputPorts().size() + top->inoutPorts().size() * 2);
            for (const auto &port : top->outputPorts())
            {
                outputBound.insert(port.value);
            }
            for (const auto &port : top->inoutPorts())
            {
                outputBound.insert(port.out);
                outputBound.insert(port.oe);
            }

            for (const auto opId : plan.stripOps)
            {
                const Operation op = top->getOperation(opId);
                for (const auto valueId : op.operands())
                {
                    const Value value = top->getValue(valueId);
                    OperationId defOp = value.definingOp();
                    if (defOp.valid())
                    {
                        const Operation def = top->getOperation(defOp);
                        if (def.kind() == OperationKind::kConstant)
                        {
                            if (plan.constSet.insert(defOp).second)
                            {
                                plan.constOps.push_back(defOp);
                            }
                            continue;
                        }
                        if (plan.stripSet.find(defOp) != plan.stripSet.end())
                        {
                            continue;
                        }
                    }
                    if (plan.boundaryInputSet.insert(valueId).second)
                    {
                        plan.boundaryInputs.push_back(valueId);
                    }
                }
            }

            for (const auto opId : plan.stripOps)
            {
                const Operation op = top->getOperation(opId);
                for (const auto valueId : op.results())
                {
                    bool usedOutside = outputBound.find(valueId) != outputBound.end();
                    if (!usedOutside)
                    {
                        const Value value = top->getValue(valueId);
                        for (const auto &user : value.users())
                        {
                            if (plan.stripSet.find(user.operation) == plan.stripSet.end())
                            {
                                usedOutside = true;
                                break;
                            }
                        }
                    }
                    if (usedOutside)
                    {
                        if (plan.boundaryResultSet.insert(valueId).second)
                        {
                            plan.boundaryResults.push_back(valueId);
                        }
                    }
                }
            }

            const bool wasTop =
                std::find(design().topGraphs().begin(), design().topGraphs().end(), topName) !=
                design().topGraphs().end();
            std::string topIntName = uniqueGraphName(design(), topName + "_logic_part");
            std::string topExtName = uniqueGraphName(design(), topName + "_debug_part");

            Graph &topInt = design().cloneGraph(topName, topIntName);
            Graph &topExt = design().createGraph(topExtName);

            bool topFailed = false;
            auto failTop = [&](std::string message) {
                error(*top, std::move(message));
                topFailed = true;
            };

            std::unordered_set<std::string> intPortNames = collectPortNames(topInt);
            std::unordered_set<std::string> extPortNames = collectPortNames(topExt);

            std::vector<Port> extInputPorts;
            std::vector<Port> extOutputPorts;
            std::vector<Port> intOutputPorts;
            std::vector<Port> intInputPorts;
            extInputPorts.reserve(plan.boundaryInputs.size());
            extOutputPorts.reserve(plan.boundaryResults.size());
            intOutputPorts.reserve(plan.boundaryInputs.size());
            intInputPorts.reserve(plan.boundaryResults.size());

            for (const auto opId : plan.stripOps)
            {
                OperationId intOpId = mapOpToClone(*top, topInt, opId);
                if (!intOpId.valid())
                {
                    failTop("strip-debug: failed to map strip op for removal");
                    break;
                }
                if (!topInt.eraseOpUnchecked(intOpId))
                {
                    failTop("strip-debug: failed to remove strip op");
                    break;
                }
            }

            if (topFailed)
            {
                design().deleteGraph(topIntName);
                design().deleteGraph(topExtName);
                result.failed = true;
                continue;
            }

            BoundaryPorts ports;
            std::unordered_map<ValueId, ValueId, ValueIdHash> extValueMap;
            auto getExtValue = [&](ValueId origValue) -> ValueId {
                auto it = extValueMap.find(origValue);
                if (it != extValueMap.end())
                {
                    return it->second;
                }
                ValueInfo info = captureValueInfo(*top, origValue);
                ValueId extValue = createValueClone(topExt, info);
                extValueMap.emplace(origValue, extValue);
                return extValue;
            };

            auto ensureIntPortValue = [&](ValueId origValue, std::string_view portBase,
                                          bool isInputPort) -> ValueId {
                auto it = ports.intPortValueByValue.find(origValue);
                if (it != ports.intPortValueByValue.end())
                {
                    return it->second;
                }
                ValueId intValue = mapValueToClone(*top, topInt, origValue);
                if (!intValue.valid())
                {
                    return ValueId::invalid();
                }
                const Value value = topInt.getValue(intValue);
                if (!value.isInout())
                {
                    ports.intPortValueByValue.emplace(origValue, intValue);
                    return intValue;
                }
                ValueInfo info = captureValueInfo(*top, origValue);
                ValueId bridge = createBridgeValue(topInt, info, portBase);
                if (isInputPort)
                {
                    addAssign(topInt, bridge, intValue);
                }
                else
                {
                    addAssign(topInt, intValue, bridge);
                }
                ports.intPortValueByValue.emplace(origValue, bridge);
                return bridge;
            };

            for (const auto valueId : plan.boundaryInputs)
            {
                ValueInfo valueInfo = captureValueInfo(*top, valueId);
                std::string fallback = std::string("value") + std::to_string(valueId.index);
                std::string base = normalizePortBase(valueInfo.symbol, fallback);

                bool extConflict = false;
                std::string extPort = uniquePortName(extPortNames, "ext_in_" + base, extConflict);
                if (extConflict)
                {
                    info(*top, "strip-debug: ext port name conflict for " + base + " -> " + extPort);
                }
                ValueId extValue = getExtValue(valueId);
                extInputPorts.push_back(Port{extPort, extValue});

                bool intConflict = false;
                std::string intPort = uniquePortName(intPortNames, "int_out_" + base, intConflict);
                if (intConflict)
                {
                    info(*top, "strip-debug: int port name conflict for " + base + " -> " + intPort);
                }
                ValueId intPortValue = ensureIntPortValue(valueId, intPort, /* isInputPort */ false);
                if (!intPortValue.valid())
                {
                    failTop("strip-debug: failed to map boundary input value for " + valueInfo.symbol);
                    break;
                }
                intOutputPorts.push_back(Port{intPort, intPortValue});
                ports.inputs.push_back(BoundaryPort{valueId, valueInfo, extPort, intPort});
                ports.extInByValue.emplace(valueId, extPort);
                ports.intOutByValue.emplace(valueId, intPort);
            }

            if (topFailed)
            {
                design().deleteGraph(topIntName);
                design().deleteGraph(topExtName);
                result.failed = true;
                continue;
            }

            for (const auto valueId : plan.boundaryResults)
            {
                ValueInfo valueInfo = captureValueInfo(*top, valueId);
                std::string fallback = std::string("value") + std::to_string(valueId.index);
                std::string base = normalizePortBase(valueInfo.symbol, fallback);

                bool extConflict = false;
                std::string extPort = uniquePortName(extPortNames, "ext_out_" + base, extConflict);
                if (extConflict)
                {
                    info(*top, "strip-debug: ext port name conflict for " + base + " -> " + extPort);
                }
                ValueId extValue = getExtValue(valueId);
                extOutputPorts.push_back(Port{extPort, extValue});

                bool intConflict = false;
                std::string intPort = uniquePortName(intPortNames, "int_in_" + base, intConflict);
                if (intConflict)
                {
                    info(*top, "strip-debug: int port name conflict for " + base + " -> " + intPort);
                }
                ValueId intPortValue = ensureIntPortValue(valueId, intPort, /* isInputPort */ true);
                if (!intPortValue.valid())
                {
                    failTop("strip-debug: failed to map boundary result value for " + valueInfo.symbol);
                    break;
                }
                intInputPorts.push_back(Port{intPort, intPortValue});
                ports.results.push_back(BoundaryPort{valueId, valueInfo, extPort, intPort});
                ports.extOutByValue.emplace(valueId, extPort);
                ports.intInByValue.emplace(valueId, intPort);
            }

            if (topFailed)
            {
                design().deleteGraph(topIntName);
                design().deleteGraph(topExtName);
                result.failed = true;
                continue;
            }

            if (!extInputPorts.empty())
            {
                topExt.bindInputPorts(extInputPorts);
            }
            if (!extOutputPorts.empty())
            {
                topExt.bindOutputPorts(extOutputPorts);
            }
            if (!intOutputPorts.empty())
            {
                topInt.bindOutputPorts(intOutputPorts);
            }
            if (!intInputPorts.empty())
            {
                topInt.bindInputPorts(intInputPorts);
            }

            for (const auto opId : plan.constOps)
            {
                const Operation op = top->getOperation(opId);
                std::string symText(op.symbolText());
                SymbolId opSym = internSymbolOrUnique(topExt, symText);
                OperationId newOp = topExt.createOperation(op.kind(), opSym);
                copyAttrs(topExt, newOp, op);
                if (op.srcLoc())
                {
                    topExt.setOpSrcLoc(newOp, *op.srcLoc());
                }
                for (const auto resultId : op.results())
                {
                    ValueId extValue = getExtValue(resultId);
                    topExt.addResult(newOp, extValue);
                }
            }

            std::unordered_map<std::string, std::string> importSymbolRemap;
            importSymbolRemap.reserve(plan.stripOps.size());

            for (const auto opId : plan.stripOps)
            {
                const Operation op = top->getOperation(opId);
                std::string symText(op.symbolText());
                SymbolId opSym = internSymbolOrUnique(topExt, symText);
                std::string newSymText = std::string(topExt.symbolText(opSym));
                if (op.kind() == OperationKind::kDpicImport && newSymText != symText)
                {
                    importSymbolRemap.emplace(symText, newSymText);
                }
                OperationId newOp = topExt.createOperation(op.kind(), opSym);
                copyAttrs(topExt, newOp, op);
                if (op.srcLoc())
                {
                    topExt.setOpSrcLoc(newOp, *op.srcLoc());
                }

                for (const auto operandId : op.operands())
                {
                    if (!operandId.valid())
                    {
                        continue;
                    }
                    ValueId mapped = getExtValue(operandId);
                    topExt.addOperand(newOp, mapped);
                }
                for (const auto resultId : op.results())
                {
                    ValueId mapped = getExtValue(resultId);
                    topExt.addResult(newOp, mapped);
                }
            }

            if (!importSymbolRemap.empty())
            {
                for (const auto opId : topExt.operations())
                {
                    const Operation op = topExt.getOperation(opId);
                    if (op.kind() != OperationKind::kDpicCall)
                    {
                        continue;
                    }
                    auto target = getAttrString(op, "targetImportSymbol");
                    if (!target)
                    {
                        continue;
                    }
                    auto it = importSymbolRemap.find(*target);
                    if (it == importSymbolRemap.end())
                    {
                        continue;
                    }
                    topExt.setAttr(opId, "targetImportSymbol", it->second);
                }
            }

            for (const auto opId : plan.constOps)
            {
                OperationId intOpId = mapOpToClone(*top, topInt, opId);
                if (!intOpId.valid())
                {
                    continue;
                }
                const Operation op = topInt.getOperation(intOpId);
                bool removable = true;
                for (const auto resultId : op.results())
                {
                    const Value value = topInt.getValue(resultId);
                    if (!value.users().empty())
                    {
                        removable = false;
                        break;
                    }
                    if (value.isInput() || value.isOutput() || value.isInout())
                    {
                        removable = false;
                        break;
                    }
                }
                if (!removable)
                {
                    continue;
                }
                for (const auto resultId : op.results())
                {
                    topInt.eraseValue(resultId);
                }
                topInt.eraseOp(intOpId);
            }

            std::vector<std::string> topDeclaredSymbols;
            topDeclaredSymbols.reserve(top->declaredSymbols().size());
            for (const SymbolId sym : top->declaredSymbols())
            {
                topDeclaredSymbols.emplace_back(top->symbolText(sym));
            }
            std::vector<std::string> topAliases = design().aliasesForGraph(topName);
            design().deleteGraph(topName);

            Graph &newTop = design().createGraph(topName);
            std::unordered_map<std::string, ValueId> topValueBySymbol;
            std::unordered_map<std::string, ValueId> topPortValueByName;
            std::vector<Port> topInputPorts;
            std::vector<Port> topOutputPorts;
            std::vector<InoutPort> topInoutPorts;
            topInputPorts.reserve(portSnapshot.inputs.size());
            topOutputPorts.reserve(portSnapshot.outputs.size());
            topInoutPorts.reserve(portSnapshot.inouts.size());

            auto ensureTopValue = [&](const ValueInfo &info) -> ValueId {
                auto it = topValueBySymbol.find(info.symbol);
                if (it != topValueBySymbol.end())
                {
                    return it->second;
                }
                SymbolId sym = internSymbolOrUnique(newTop, info.symbol);
                ValueId valueId = newTop.createValue(sym, info.width, info.isSigned, info.type);
                if (info.srcLoc)
                {
                    newTop.setValueSrcLoc(valueId, *info.srcLoc);
                }
                topValueBySymbol.emplace(info.symbol, valueId);
                return valueId;
            };

            for (const auto &port : portSnapshot.inputs)
            {
                ValueId valueId = ensureTopValue(port.value);
                topInputPorts.push_back(Port{port.name, valueId});
                topPortValueByName.emplace(port.name, valueId);
            }
            for (const auto &port : portSnapshot.outputs)
            {
                ValueId valueId = ensureTopValue(port.value);
                topOutputPorts.push_back(Port{port.name, valueId});
                topPortValueByName.emplace(port.name, valueId);
            }
            for (const auto &port : portSnapshot.inouts)
            {
                ValueId inValue = ensureTopValue(port.in);
                ValueId outValue = ensureTopValue(port.out);
                ValueId oeValue = ensureTopValue(port.oe);
                topInoutPorts.push_back(InoutPort{port.name, inValue, outValue, oeValue});
                topPortValueByName.emplace(port.name + ".in", inValue);
                topPortValueByName.emplace(port.name + ".out", outValue);
                topPortValueByName.emplace(port.name + ".oe", oeValue);
            }
            if (!topInputPorts.empty())
            {
                newTop.bindInputPorts(topInputPorts);
            }
            if (!topOutputPorts.empty())
            {
                newTop.bindOutputPorts(topOutputPorts);
            }
            if (!topInoutPorts.empty())
            {
                newTop.bindInoutPorts(topInoutPorts);
            }
            for (const std::string &text : topDeclaredSymbols)
            {
                const SymbolId sym = newTop.lookupSymbol(text);
                if (sym.valid() &&
                    (newTop.findValue(sym).valid() || newTop.findOperation(sym).valid()))
                {
                    newTop.addDeclaredSymbol(sym);
                }
            }

            std::unordered_map<ValueId, ValueId, ValueIdHash> linkValuesIn;
            std::unordered_map<ValueId, ValueId, ValueIdHash> linkValuesOut;

            auto makeLinkValue = [&](const ValueInfo &info, std::string_view base) -> ValueId {
                std::string normalized = normalizePortBase(base, "link");
                std::string symbol = "link_" + normalized;
                SymbolId sym = internUniqueSymbol(newTop, symbol);
                ValueId valueId = newTop.createValue(sym, info.width, info.isSigned, info.type);
                return valueId;
            };

            for (const auto &entry : ports.inputs)
            {
                ValueId link = makeLinkValue(entry.info, entry.info.symbol);
                linkValuesIn.emplace(entry.value, link);
            }
            for (const auto &entry : ports.results)
            {
                ValueId link = makeLinkValue(entry.info, entry.info.symbol);
                linkValuesOut.emplace(entry.value, link);
            }

            auto buildInstance = [&](Graph &parent,
                                     std::string_view moduleName,
                                     std::string_view instanceBase,
                                     const Graph &target,
                                     const std::unordered_map<std::string, ValueId> &inputMapping,
                                     const std::unordered_map<std::string, ValueId> &outputMapping,
                                     const std::unordered_map<std::string, std::tuple<ValueId, ValueId, ValueId>> &inoutMapping)
                -> OperationId {
                std::string opBase = std::string("inst_") + std::string(instanceBase);
                SymbolId opSym = internUniqueSymbol(parent, opBase);
                OperationId inst = parent.createOperation(OperationKind::kInstance, opSym);
                parent.setAttr(inst, "moduleName", std::string(moduleName));

                std::vector<std::string> inputNames;
                inputNames.reserve(target.inputPorts().size());
                for (const auto &port : target.inputPorts())
                {
                    inputNames.push_back(port.name);
                }
                std::vector<std::string> outputNames;
                outputNames.reserve(target.outputPorts().size());
                for (const auto &port : target.outputPorts())
                {
                    outputNames.push_back(port.name);
                }
                std::vector<std::string> inoutNames;
                inoutNames.reserve(target.inoutPorts().size());
                for (const auto &port : target.inoutPorts())
                {
                    inoutNames.push_back(port.name);
                }
                parent.setAttr(inst, "inputPortName", inputNames);
                parent.setAttr(inst, "outputPortName", outputNames);
                if (!inoutNames.empty())
                {
                    parent.setAttr(inst, "inoutPortName", inoutNames);
                }
                parent.setAttr(inst, "instanceName", std::string(instanceBase));

                for (const auto &portName : inputNames)
                {
                    auto it = inputMapping.find(portName);
                    if (it == inputMapping.end())
                    {
                        continue;
                    }
                    parent.addOperand(inst, it->second);
                }
                for (const auto &portName : inoutNames)
                {
                    auto it = inoutMapping.find(portName);
                    if (it == inoutMapping.end())
                    {
                        continue;
                    }
                    parent.addOperand(inst, std::get<0>(it->second));
                }
                for (const auto &portName : outputNames)
                {
                    auto it = outputMapping.find(portName);
                    if (it == outputMapping.end())
                    {
                        continue;
                    }
                    parent.addResult(inst, it->second);
                }
                for (const auto &portName : inoutNames)
                {
                    auto it = inoutMapping.find(portName);
                    if (it == inoutMapping.end())
                    {
                        continue;
                    }
                    parent.addResult(inst, std::get<1>(it->second));
                }
                for (const auto &portName : inoutNames)
                {
                    auto it = inoutMapping.find(portName);
                    if (it == inoutMapping.end())
                    {
                        continue;
                    }
                    parent.addResult(inst, std::get<2>(it->second));
                }
                return inst;
            };

            std::unordered_map<std::string, ValueId> intInputMapping;
            std::unordered_map<std::string, ValueId> intOutputMapping;
            std::unordered_map<std::string, std::tuple<ValueId, ValueId, ValueId>> intInoutMapping;

            for (const auto &port : portSnapshot.inputs)
            {
                auto it = topPortValueByName.find(port.name);
                if (it != topPortValueByName.end())
                {
                    intInputMapping.emplace(port.name, it->second);
                }
            }
            for (const auto &port : portSnapshot.outputs)
            {
                auto it = topPortValueByName.find(port.name);
                if (it != topPortValueByName.end())
                {
                    intOutputMapping.emplace(port.name, it->second);
                }
            }
            for (const auto &port : portSnapshot.inouts)
            {
                auto itIn = topPortValueByName.find(port.name + ".in");
                auto itOut = topPortValueByName.find(port.name + ".out");
                auto itOe = topPortValueByName.find(port.name + ".oe");
                if (itIn != topPortValueByName.end() && itOut != topPortValueByName.end() && itOe != topPortValueByName.end())
                {
                    intInoutMapping.emplace(port.name, std::make_tuple(itIn->second, itOut->second, itOe->second));
                }
            }

            for (const auto &entry : ports.inputs)
            {
                auto linkIt = linkValuesIn.find(entry.value);
                if (linkIt == linkValuesIn.end())
                {
                    continue;
                }
                intOutputMapping.emplace(entry.intPort, linkIt->second);
            }
            for (const auto &entry : ports.results)
            {
                auto linkIt = linkValuesOut.find(entry.value);
                if (linkIt == linkValuesOut.end())
                {
                    continue;
                }
                intInputMapping.emplace(entry.intPort, linkIt->second);
            }

            std::unordered_map<std::string, ValueId> extInputMapping;
            std::unordered_map<std::string, ValueId> extOutputMapping;
            std::unordered_map<std::string, std::tuple<ValueId, ValueId, ValueId>> extInoutMapping;

            for (const auto &entry : ports.inputs)
            {
                auto linkIt = linkValuesIn.find(entry.value);
                if (linkIt == linkValuesIn.end())
                {
                    continue;
                }
                extInputMapping.emplace(entry.extPort, linkIt->second);
            }
            for (const auto &entry : ports.results)
            {
                auto linkIt = linkValuesOut.find(entry.value);
                if (linkIt == linkValuesOut.end())
                {
                    continue;
                }
                extOutputMapping.emplace(entry.extPort, linkIt->second);
            }

            buildInstance(newTop, topInt.symbol(), "logic_part", topInt, intInputMapping, intOutputMapping, intInoutMapping);
            buildInstance(newTop, topExt.symbol(), "debug_part", topExt, extInputMapping, extOutputMapping, extInoutMapping);

            for (const auto &alias : topAliases)
            {
                design().registerGraphAlias(alias, newTop);
            }

            if (wasTop)
            {
                design().markAsTop(topName);
            }
            result.changed = true;

            std::string summary = "strip-debug: graph=" + topName +
                                  " strip_ops=" + std::to_string(plan.stripOps.size()) +
                                  " consts=" + std::to_string(plan.constOps.size()) +
                                  " boundary_in=" + std::to_string(plan.boundaryInputs.size()) +
                                  " boundary_out=" + std::to_string(plan.boundaryResults.size());
            info(summary);
        }

        return result;
    }

} // namespace wolvrix::lib::transform
