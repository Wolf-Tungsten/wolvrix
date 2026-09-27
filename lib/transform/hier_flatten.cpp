#include "transform/hier_flatten.hpp"

#include "core/grh.hpp"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wolvrix::lib::transform
{

    namespace
    {
        using ValueMap = std::unordered_map<wolvrix::lib::grh::ValueId,
                                            wolvrix::lib::grh::ValueId,
                                            wolvrix::lib::grh::ValueIdHash>;

        struct PendingAttr
        {
            wolvrix::lib::grh::OperationId op;
            std::string key;
            std::string oldValue;
        };

        struct InstanceInfo
        {
            std::string moduleName;
            std::string instanceName;
            std::vector<wolvrix::lib::grh::ValueId> operands;
            std::vector<wolvrix::lib::grh::ValueId> results;
            std::optional<std::vector<std::string>> inputPortNames;
            std::optional<std::vector<std::string>> outputPortNames;
            std::optional<std::vector<std::string>> inoutPortNames;
        };

        struct InlineState
        {
            wolvrix::lib::grh::Design &design;
            std::unordered_set<const wolvrix::lib::grh::Graph *> stack;
            std::unordered_map<std::string, int> instanceCounts;
            std::unordered_map<wolvrix::lib::grh::ValueId, std::string,
                               wolvrix::lib::grh::ValueIdHash> renamedValues;
            std::function<void(const wolvrix::lib::grh::Graph &, std::string)> graphError;
            std::function<void(const wolvrix::lib::grh::Graph &, const wolvrix::lib::grh::Operation &,
                               std::string)> opError;
            HierFlattenOptions::SymProtectMode symProtect = HierFlattenOptions::SymProtectMode::All;
            bool failed = false;
            bool changed = false;
        };

        std::optional<std::string> getAttrString(const wolvrix::lib::grh::Operation &op,
                                                 std::string_view key)
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

        std::optional<std::vector<std::string>> getAttrStrings(const wolvrix::lib::grh::Operation &op,
                                                                std::string_view key)
        {
            auto attr = op.attr(key);
            if (!attr)
            {
                return std::nullopt;
            }
            if (auto values = std::get_if<std::vector<std::string>>(&*attr))
            {
                return *values;
            }
            return std::nullopt;
        }

        bool attrsEqual(const wolvrix::lib::grh::Operation &lhs,
                        const wolvrix::lib::grh::Operation &rhs)
        {
            const auto lhsAttrs = lhs.attrs();
            const auto rhsAttrs = rhs.attrs();
            if (lhsAttrs.size() != rhsAttrs.size())
            {
                return false;
            }
            for (const auto &attr : lhsAttrs)
            {
                auto rhsAttr = rhs.attr(attr.key);
                if (!rhsAttr || *rhsAttr != attr.value)
                {
                    return false;
                }
            }
            return true;
        }

        std::string normalizeComponent(std::string_view text)
        {
            std::string normalized = wolvrix::lib::grh::Graph::normalizeComponent(text);
            if (normalized.empty())
            {
                normalized = "inst";
            }
            if (!normalized.empty() && normalized.front() >= '0' && normalized.front() <= '9')
            {
                normalized.insert(normalized.begin(), '_');
            }
            return normalized;
        }

        std::string makeHierName(std::string_view prefix, std::string_view base)
        {
            if (prefix.empty())
            {
                return std::string(base);
            }
            std::string out(prefix);
            out.push_back('$');
            out.append(base);
            return out;
        }

        std::string uniqueInstanceComponent(InlineState &state,
                                             std::string_view parentPrefix,
                                             std::string base)
        {
            std::string key;
            key.reserve(parentPrefix.size() + base.size() + 1);
            key.append(parentPrefix);
            key.push_back('|');
            key.append(base);
            int &count = state.instanceCounts[key];
            count += 1;
            if (count == 1)
            {
                return base;
            }
            return base + "_" + std::to_string(count);
        }

        wolvrix::lib::grh::SymbolId internUniqueSymbol(wolvrix::lib::grh::Graph &graph, const std::string &base)
        {
            std::string candidate = base;
            int suffix = 0;
            while (true)
            {
                wolvrix::lib::grh::SymbolId sym = graph.internSymbol(candidate);
                if (sym.valid())
                {
                    return sym;
                }
                candidate = base + "_" + std::to_string(++suffix);
            }
        }

        bool addResultChecked(InlineState &state,
                              wolvrix::lib::grh::Graph &target,
                              wolvrix::lib::grh::OperationId op,
                              wolvrix::lib::grh::ValueId value,
                              const wolvrix::lib::grh::Graph &source,
                              const wolvrix::lib::grh::Operation &srcOp,
                              wolvrix::lib::grh::ValueId srcValue,
                              std::string_view prefix)
        {
            if (!value.valid())
            {
                state.opError(source, srcOp, "Flatten mapping produced invalid result value");
                return false;
            }
            auto describePort = [&](const wolvrix::lib::grh::Graph &graph,
                                    wolvrix::lib::grh::ValueId valueId) -> std::string {
                if (!valueId.valid())
                {
                    return std::string();
                }
                for (const auto &port : graph.inputPorts())
                {
                    if (port.value == valueId)
                    {
                        return "input:" + port.name;
                    }
                }
                for (const auto &port : graph.outputPorts())
                {
                    if (port.value == valueId)
                    {
                        return "output:" + port.name;
                    }
                }
                for (const auto &port : graph.inoutPorts())
                {
                    if (port.in == valueId)
                    {
                        return "inout.in:" + port.name;
                    }
                    if (port.out == valueId)
                    {
                        return "inout.out:" + port.name;
                    }
                    if (port.oe == valueId)
                    {
                        return "inout.oe:" + port.name;
                    }
                }
                return std::string();
            };
            const wolvrix::lib::grh::Value dstValue = target.getValue(value);
            if (dstValue.definingOp().valid())
            {
                const wolvrix::lib::grh::Operation existing = target.getOperation(dstValue.definingOp());
                std::string message = "Flatten result value already has defining op";
                message.append(" target_value=");
                message.append(dstValue.symbolText());
                message.append(" target_graph=");
                message.append(target.symbol());
                if (srcValue.valid())
                {
                    const wolvrix::lib::grh::Value srcVal = source.getValue(srcValue);
                    message.append(" source_value=");
                    message.append(srcVal.symbolText());
                    const std::string portDesc = describePort(source, srcValue);
                    if (!portDesc.empty())
                    {
                        message.append(" source_port=");
                        message.append(portDesc);
                    }
                }
                if (!prefix.empty())
                {
                    message.append(" instance_path=");
                    message.append(prefix);
                }
                message.append(" existing_def=");
                message.append(existing.symbolText());
                message.append(" existing_kind=");
                message.append(std::string(wolvrix::lib::grh::toString(existing.kind())));
                state.opError(source, srcOp, std::move(message));
                return false;
            }
            target.addResult(op, value);
            return true;
        }

        bool inlineGraphContents(const wolvrix::lib::grh::Graph &source,
                                 wolvrix::lib::grh::Graph &target,
                                 const std::string &prefix,
                                 const ValueMap &portMap,
                                 InlineState &state);

        bool inlineInstanceInfo(const wolvrix::lib::grh::Graph &sourceGraph,
                                const InstanceInfo &info,
                                const ValueMap &valueMap,
                                wolvrix::lib::grh::Graph &target,
                                const std::string &prefix,
                                InlineState &state)
        {
            if (info.moduleName.empty())
            {
                state.graphError(sourceGraph, "Instance missing moduleName");
                return false;
            }
            wolvrix::lib::grh::Graph *child = state.design.findGraph(info.moduleName);
            if (!child)
            {
                state.graphError(sourceGraph, "Instance module not found: " + info.moduleName);
                return false;
            }

            const std::size_t inputCount = info.inputPortNames
                                               ? info.inputPortNames->size()
                                               : child->inputPorts().size();
            const std::size_t outputCount = info.outputPortNames
                                                ? info.outputPortNames->size()
                                                : child->outputPorts().size();
            const std::size_t inoutCount = info.inoutPortNames
                                               ? info.inoutPortNames->size()
                                               : child->inoutPorts().size();

            if (child->inputPorts().size() != inputCount ||
                child->outputPorts().size() != outputCount ||
                child->inoutPorts().size() != inoutCount)
            {
                std::string message = "Instance port list does not match module ports";
                message.append(" module=");
                message.append(info.moduleName);
                if (!info.instanceName.empty())
                {
                    message.append(" instance=");
                    message.append(info.instanceName);
                }
                message.append(" child_in=");
                message.append(std::to_string(child->inputPorts().size()));
                message.append(" attr_in=");
                message.append(std::to_string(inputCount));
                message.append(" child_out=");
                message.append(std::to_string(child->outputPorts().size()));
                message.append(" attr_out=");
                message.append(std::to_string(outputCount));
                message.append(" child_inout=");
                message.append(std::to_string(child->inoutPorts().size()));
                message.append(" attr_inout=");
                message.append(std::to_string(inoutCount));
                state.graphError(sourceGraph, std::move(message));
                return false;
            }

            const std::size_t expectedOperands = inputCount + inoutCount;
            const std::size_t expectedResults = outputCount + inoutCount * 2;
            if (info.operands.size() != expectedOperands ||
                info.results.size() != expectedResults)
            {
                state.graphError(sourceGraph, "Instance operand/result count mismatch during flatten");
                return false;
            }

            std::string baseName = !info.instanceName.empty() ? info.instanceName : info.moduleName;
            std::string component = normalizeComponent(baseName);
            component = uniqueInstanceComponent(state, prefix, component);
            const std::string childPrefix = makeHierName(prefix, component);

            auto mapValue = [&](wolvrix::lib::grh::ValueId src) -> std::optional<wolvrix::lib::grh::ValueId> {
                auto it = valueMap.find(src);
                if (it == valueMap.end())
                {
                    return std::nullopt;
                }
                return it->second;
            };

            // Declared child symbols are preserved in every symProtect mode by
            // inlineGraphContents; the mode only controls whether an undeclared
            // parent-side port value adopts the child hierarchical port name.
            const bool renamePorts =
                state.symProtect == HierFlattenOptions::SymProtectMode::All ||
                state.symProtect == HierFlattenOptions::SymProtectMode::Hierarchy;

            auto maybeRenameMappedValue = [&](wolvrix::lib::grh::ValueId childValue,
                                              wolvrix::lib::grh::ValueId targetValue,
                                              std::string_view baseName) {
                if (!renamePorts)
                {
                    return;
                }
                if (!childValue.valid() || !targetValue.valid())
                {
                    return;
                }
                std::string base;
                if (!baseName.empty())
                {
                    base = std::string(baseName);
                }
                else
                {
                    const wolvrix::lib::grh::Value childVal = child->getValue(childValue);
                    base = std::string(childVal.symbolText());
                }
                if (base.empty())
                {
                    return;
                }
                const wolvrix::lib::grh::Value targetVal = target.getValue(targetValue);
                if (target.isDeclaredSymbol(targetVal.symbol()) &&
                    !targetVal.symbolText().starts_with("_val_"))
                {
                    return;
                }
                const std::string desiredName = makeHierName(childPrefix, base);
                if (desiredName.empty() || targetVal.symbolText() == desiredName)
                {
                    return;
                }
                auto renamedIt = state.renamedValues.find(targetValue);
                if (renamedIt != state.renamedValues.end())
                {
                    if (renamedIt->second != desiredName)
                    {
                        state.graphError(sourceGraph,
                                         "Flatten port maps share value with different names: " +
                                             renamedIt->second + " vs " + desiredName);
                        state.failed = true;
                    }
                    return;
                }
                wolvrix::lib::grh::SymbolId sym = internUniqueSymbol(target, desiredName);
                target.setValueSymbol(targetValue, sym);
                target.addDeclaredSymbol(sym);
                state.renamedValues.emplace(targetValue, std::string(target.symbolText(sym)));
                state.changed = true;
            };

            ValueMap childPortMap;
            childPortMap.reserve(inputCount + outputCount + inoutCount * 3);

            const auto childInputs = child->inputPorts();
            std::unordered_map<std::string_view, wolvrix::lib::grh::ValueId> childInputByName;
            if (info.inputPortNames)
            {
                childInputByName.reserve(childInputs.size());
                for (const auto &port : childInputs)
                {
                    childInputByName.emplace(port.name, port.value);
                }
            }
            for (std::size_t i = 0; i < inputCount; ++i)
            {
                wolvrix::lib::grh::ValueId childValue = wolvrix::lib::grh::ValueId::invalid();
                std::string_view portName;
                if (info.inputPortNames)
                {
                    portName = (*info.inputPortNames)[i];
                    auto it = childInputByName.find(portName);
                    if (it == childInputByName.end())
                    {
                        state.graphError(sourceGraph, "Flatten missing input port: " + std::string(portName));
                        return false;
                    }
                    childValue = it->second;
                }
                else
                {
                    portName = childInputs[i].name;
                    childValue = childInputs[i].value;
                }
                auto targetValue = mapValue(info.operands[i]);
                if (!targetValue)
                {
                    state.graphError(sourceGraph, "Flatten missing mapping for instance input operand");
                    return false;
                }
                maybeRenameMappedValue(childValue, *targetValue, portName);
                childPortMap.emplace(childValue, *targetValue);
            }

            const auto childOutputs = child->outputPorts();
            std::unordered_map<std::string_view, wolvrix::lib::grh::ValueId> childOutputByName;
            if (info.outputPortNames)
            {
                childOutputByName.reserve(childOutputs.size());
                for (const auto &port : childOutputs)
                {
                    childOutputByName.emplace(port.name, port.value);
                }
            }
            for (std::size_t i = 0; i < outputCount; ++i)
            {
                wolvrix::lib::grh::ValueId childValue = wolvrix::lib::grh::ValueId::invalid();
                std::string_view portName;
                if (info.outputPortNames)
                {
                    portName = (*info.outputPortNames)[i];
                    auto it = childOutputByName.find(portName);
                    if (it == childOutputByName.end())
                    {
                        state.graphError(sourceGraph, "Flatten missing output port: " + std::string(portName));
                        return false;
                    }
                    childValue = it->second;
                }
                else
                {
                    portName = childOutputs[i].name;
                    childValue = childOutputs[i].value;
                }
                auto targetValue = mapValue(info.results[i]);
                if (!targetValue)
                {
                    state.graphError(sourceGraph, "Flatten missing mapping for instance output result");
                    return false;
                }
                maybeRenameMappedValue(childValue, *targetValue, portName);
                auto [it, inserted] = childPortMap.emplace(childValue, *targetValue);
                if (!inserted && it->second != *targetValue)
                {
                    // Multiple output ports can legally alias the same child value (e.g. XMR read ports
                    // tied to an existing output). Preserve the first mapping and explicitly drive
                    // the duplicate output from it.
                    const wolvrix::lib::grh::Value dstValue = target.getValue(*targetValue);
                    if (dstValue.definingOp().valid())
                    {
                        state.graphError(sourceGraph,
                                         "Flatten duplicate output mapping already has defining op");
                        return false;
                    }
                    wolvrix::lib::grh::SymbolId opSym = target.makeInternalOpSym();
                    wolvrix::lib::grh::OperationId assign =
                        target.createOperation(wolvrix::lib::grh::OperationKind::kAssign, opSym);
                    target.addOperand(assign, it->second);
                    target.addResult(assign, *targetValue);
                    state.changed = true;
                }
            }

            const auto childInouts = child->inoutPorts();
            std::unordered_map<std::string_view, std::size_t> childInoutByName;
            if (info.inoutPortNames)
            {
                childInoutByName.reserve(childInouts.size());
                for (std::size_t i = 0; i < childInouts.size(); ++i)
                {
                    childInoutByName.emplace(childInouts[i].name, i);
                }
            }
            for (std::size_t i = 0; i < inoutCount; ++i)
            {
                std::size_t childIndex = i;
                if (info.inoutPortNames)
                {
                    const std::string_view portName = (*info.inoutPortNames)[i];
                    auto it = childInoutByName.find(portName);
                    if (it == childInoutByName.end())
                    {
                        state.graphError(sourceGraph, "Flatten missing inout port: " + std::string(portName));
                        return false;
                    }
                    childIndex = it->second;
                }
                const std::size_t inIndex = inputCount + i;
                const std::size_t outIndex = outputCount + i;
                const std::size_t oeIndex = outputCount + inoutCount + i;
                auto inValue = mapValue(info.operands[inIndex]);
                auto outValue = mapValue(info.results[outIndex]);
                auto oeValue = mapValue(info.results[oeIndex]);
                if (!outValue || !oeValue || !inValue)
                {
                    state.graphError(sourceGraph, "Flatten missing mapping for instance inout port");
                    return false;
                }
                const auto &childInout = childInouts[childIndex];
                maybeRenameMappedValue(childInout.out, *outValue, std::string_view());
                maybeRenameMappedValue(childInout.oe, *oeValue, std::string_view());
                maybeRenameMappedValue(childInout.in, *inValue, std::string_view());
                childPortMap.emplace(childInout.out, *outValue);
                childPortMap.emplace(childInout.oe, *oeValue);
                childPortMap.emplace(childInout.in, *inValue);
            }
            if (state.failed)
            {
                return false;
            }

            return inlineGraphContents(*child, target, childPrefix, childPortMap, state);
        }

        InstanceInfo buildInstanceInfo(const wolvrix::lib::grh::Operation &op)
        {
            InstanceInfo info;
            info.moduleName = getAttrString(op, "moduleName").value_or(std::string());
            info.instanceName = getAttrString(op, "instanceName").value_or(std::string());
            info.operands.assign(op.operands().begin(), op.operands().end());
            info.results.assign(op.results().begin(), op.results().end());
            info.inputPortNames = getAttrStrings(op, "inputPortName");
            info.outputPortNames = getAttrStrings(op, "outputPortName");
            info.inoutPortNames = getAttrStrings(op, "inoutPortName");
            return info;
        }

        // Propagate the inlined child graph's generate groups into the target:
        // members are remapped to the cloned/renamed symbols, the group scope
        // gains the instance path prefix (e.g. "gen_loop" -> "u_inst$gen_loop"),
        // and the bare declaration name is kept unchanged. Members whose symbols
        // were dropped during inlining (e.g. child port names superseded by the
        // parent-side declared name) are removed; groups left empty are skipped.
        bool propagateGenerateGroups(const wolvrix::lib::grh::Graph &source,
                                     wolvrix::lib::grh::Graph &target,
                                     const std::string &prefix,
                                     const std::unordered_map<std::string, std::string> &valueRename,
                                     const std::unordered_map<std::string, std::string> &opRename,
                                     InlineState &state)
        {
            auto lookupOrIntern = [&](std::string_view text) -> wolvrix::lib::grh::SymbolId {
                wolvrix::lib::grh::SymbolId sym = target.lookupSymbol(text);
                if (!sym.valid())
                {
                    sym = target.internSymbol(text);
                }
                return sym;
            };
            for (const auto &srcGroup : source.generateGroups())
            {
                const std::string_view srcScope = source.symbolText(srcGroup.scope);
                const std::string_view srcName = source.symbolText(srcGroup.name);
                if (srcScope.empty() || srcName.empty())
                {
                    continue;
                }
                std::vector<wolvrix::lib::grh::SymbolId> newMembers;
                newMembers.reserve(srcGroup.symbols.size());
                for (const wolvrix::lib::grh::SymbolId srcSym : srcGroup.symbols)
                {
                    if (!srcSym.valid())
                    {
                        continue;
                    }
                    const std::string oldText = std::string(source.symbolText(srcSym));
                    if (oldText.empty())
                    {
                        continue;
                    }
                    std::string_view newText;
                    if (auto it = valueRename.find(oldText); it != valueRename.end())
                    {
                        newText = it->second;
                    }
                    else if (auto it = opRename.find(oldText); it != opRename.end())
                    {
                        newText = it->second;
                    }
                    else
                    {
                        // Symbol was dropped while inlining (port mapping, etc.).
                        continue;
                    }
                    const wolvrix::lib::grh::SymbolId dstSym = target.lookupSymbol(newText);
                    if (!dstSym.valid())
                    {
                        state.graphError(source,
                                         "Flatten generate group member missing after rename: " +
                                             std::string(newText));
                        state.failed = true;
                        return false;
                    }
                    newMembers.push_back(dstSym);
                }
                if (newMembers.empty())
                {
                    continue;
                }
                const std::string newScope = makeHierName(prefix, srcScope);
                const wolvrix::lib::grh::SymbolId scopeSym = lookupOrIntern(newScope);
                const wolvrix::lib::grh::SymbolId nameSym = lookupOrIntern(srcName);
                if (!scopeSym.valid() || !nameSym.valid())
                {
                    state.graphError(source,
                                     "Flatten generate group scope/name collision: " + newScope);
                    state.failed = true;
                    return false;
                }
                const std::size_t dstGroup = target.addGenerateGroup(scopeSym, nameSym);
                for (const wolvrix::lib::grh::SymbolId sym : newMembers)
                {
                    target.addGenerateGroupSymbol(dstGroup, sym);
                }
            }
            return true;
        }

        bool inlineGraphContents(const wolvrix::lib::grh::Graph &source,
                                 wolvrix::lib::grh::Graph &target,
                                 const std::string &prefix,
                                 const ValueMap &portMap,
                                 InlineState &state)
        {
            if (state.failed)
            {
                return false;
            }
            if (state.stack.find(&source) != state.stack.end())
            {
                state.graphError(source, "Recursive instance detected during hier-flatten");
                state.failed = true;
                return false;
            }
            state.stack.insert(&source);

            std::unordered_set<uint32_t> declaredSet;
            declaredSet.reserve(source.declaredSymbols().size());
            for (const auto sym : source.declaredSymbols())
            {
                if (sym.valid())
                {
                    declaredSet.insert(sym.value);
                }
            }
            auto isDeclared = [&](wolvrix::lib::grh::SymbolId sym) -> bool {
                return sym.valid() && declaredSet.find(sym.value) != declaredSet.end();
            };
            // Declared symbols are always preserved under their hierarchical
            // name and re-registered as declared, independent of
            // HierFlattenOptions::symProtect (the mode now only gates
            // parent-side port renaming, see renamePorts).
            auto shouldProtectValue = [&](const wolvrix::lib::grh::Value &value) -> bool {
                return isDeclared(value.symbol());
            };
            auto shouldProtectOp = [&](const wolvrix::lib::grh::Operation &op) -> bool {
                return isDeclared(op.symbol());
            };

            ValueMap valueMap = portMap;
            auto requireMappedPort = [&](wolvrix::lib::grh::ValueId valueId, std::string_view role) -> bool {
                if (valueMap.find(valueId) == valueMap.end())
                {
                    state.graphError(source, "Flatten missing mapping for port value (" + std::string(role) + ")");
                    return false;
                }
                return true;
            };
            for (const auto &port : source.inputPorts())
            {
                if (!requireMappedPort(port.value, "input"))
                {
                    state.failed = true;
                    state.stack.erase(&source);
                    return false;
                }
            }
            for (const auto &port : source.outputPorts())
            {
                if (!requireMappedPort(port.value, "output"))
                {
                    state.failed = true;
                    state.stack.erase(&source);
                    return false;
                }
            }
            for (const auto &port : source.inoutPorts())
            {
                if (!requireMappedPort(port.in, "inout.in") ||
                    !requireMappedPort(port.out, "inout.out") ||
                    !requireMappedPort(port.oe, "inout.oe"))
                {
                    state.failed = true;
                    state.stack.erase(&source);
                    return false;
                }
            }

            std::unordered_map<std::string, std::string> valueRename;
            for (const auto valueId : source.values())
            {
                if (!valueId.valid())
                {
                    continue;
                }
                if (valueMap.find(valueId) != valueMap.end())
                {
                    continue;
                }
                const wolvrix::lib::grh::Value value = source.getValue(valueId);
                wolvrix::lib::grh::SymbolId newSym = wolvrix::lib::grh::SymbolId::invalid();
                if (shouldProtectValue(value))
                {
                    std::string base = std::string(value.symbolText());
                    if (!base.empty())
                    {
                        const std::string hierName = makeHierName(prefix, base);
                        newSym = internUniqueSymbol(target, hierName);
                        target.addDeclaredSymbol(newSym);
                    }
                }
                if (!newSym.valid())
                {
                    newSym = target.makeInternalValSym();
                }
                wolvrix::lib::grh::ValueId newValue =
                    target.createValue(newSym, value.width(), value.isSigned(), value.type());
                if (value.srcLoc())
                {
                    target.setValueSrcLoc(newValue, *value.srcLoc());
                }
                valueMap.emplace(valueId, newValue);
                if (!value.symbolText().empty())
                {
                    valueRename.emplace(std::string(value.symbolText()),
                                        std::string(target.symbolText(newSym)));
                }
                state.changed = true;
            }

            std::unordered_map<std::string, std::string> opRename;
            std::vector<PendingAttr> pendingAttrs;

            auto rewriteAttr = [&](wolvrix::lib::grh::OperationId newOp,
                                   const std::string &key,
                                   const wolvrix::lib::grh::AttributeValue &value) {
                target.setAttr(newOp, key, value);
                if (key != "regSymbol" && key != "memSymbol" &&
                    key != "latchSymbol" && key != "targetImportSymbol")
                {
                    return;
                }
                if (const auto *strValue = std::get_if<std::string>(&value))
                {
                    auto it = opRename.find(*strValue);
                    if (it != opRename.end())
                    {
                        target.setAttr(newOp, key, it->second);
                    }
                    else
                    {
                        pendingAttrs.push_back(PendingAttr{newOp, key, *strValue});
                    }
                }
            };

            for (const auto opId : source.operations())
            {
                if (!opId.valid())
                {
                    continue;
                }
                const wolvrix::lib::grh::Operation op = source.getOperation(opId);
                if (op.kind() == wolvrix::lib::grh::OperationKind::kInstance)
                {
                    const InstanceInfo info = buildInstanceInfo(op);
                    if (!inlineInstanceInfo(source, info, valueMap, target, prefix, state))
                    {
                        state.failed = true;
                        break;
                    }
                    state.changed = true;
                    continue;
                }

                if (op.kind() == wolvrix::lib::grh::OperationKind::kDpicImport)
                {
                    const std::string opName = std::string(op.symbolText());
                    if (opName.empty())
                    {
                        state.opError(source, op, "kDpicImport missing symbol");
                        state.failed = true;
                        break;
                    }
                    const wolvrix::lib::grh::OperationId existing = target.findOperation(opName);
                    if (existing.valid())
                    {
                        const wolvrix::lib::grh::Operation existingOp = target.getOperation(existing);
                        if (existingOp.kind() != wolvrix::lib::grh::OperationKind::kDpicImport)
                        {
                            state.opError(source, op,
                                          "kDpicImport symbol conflicts with existing operation");
                            state.failed = true;
                            break;
                        }
                        if (!attrsEqual(op, existingOp))
                        {
                            state.opError(source, op,
                                          "kDpicImport attributes conflict for symbol " + opName);
                            state.failed = true;
                            break;
                        }
                        opRename.emplace(opName, opName);
                        continue;
                    }
                    if (target.findValue(opName).valid())
                    {
                        state.opError(source, op,
                                      "kDpicImport symbol conflicts with existing value");
                        state.failed = true;
                        break;
                    }
                    wolvrix::lib::grh::SymbolId sym = target.internSymbol(opName);
                    if (!sym.valid())
                    {
                        state.opError(source, op,
                                      "kDpicImport symbol collision during flatten");
                        state.failed = true;
                        break;
                    }
                    wolvrix::lib::grh::OperationId newOp =
                        target.createOperation(wolvrix::lib::grh::OperationKind::kDpicImport, sym);
                    if (op.srcLoc())
                    {
                        target.setOpSrcLoc(newOp, *op.srcLoc());
                    }
                    for (const auto &attr : op.attrs())
                    {
                        target.setAttr(newOp, attr.key, attr.value);
                    }
                    opRename.emplace(opName, opName);
                    state.changed = true;
                    continue;
                }

                wolvrix::lib::grh::SymbolId newSym = wolvrix::lib::grh::SymbolId::invalid();
                const bool protectOp = shouldProtectOp(op);
                if (protectOp)
                {
                    const std::string base = std::string(op.symbolText());
                    if (!base.empty())
                    {
                        const std::string hierName = makeHierName(prefix, base);
                        newSym = internUniqueSymbol(target, hierName);
                        target.addDeclaredSymbol(newSym);
                    }
                }
                if (!newSym.valid())
                {
                    newSym = target.makeInternalOpSym();
                }

                const std::string oldOpName = std::string(op.symbolText());
                const std::string newOpName = std::string(target.symbolText(newSym));
                if (!oldOpName.empty() && !newOpName.empty())
                {
                    opRename.emplace(oldOpName, newOpName);
                }

                wolvrix::lib::grh::OperationId newOp =
                    target.createOperation(op.kind(), newSym);
                if (op.srcLoc())
                {
                    target.setOpSrcLoc(newOp, *op.srcLoc());
                }

                for (const auto &attr : op.attrs())
                {
                    rewriteAttr(newOp, attr.key, attr.value);
                }

                for (const auto operand : op.operands())
                {
                    auto it = valueMap.find(operand);
                    if (it == valueMap.end())
                    {
                        state.opError(source, op, "Flatten missing operand mapping");
                        state.failed = true;
                        break;
                    }
                    target.addOperand(newOp, it->second);
                }
                if (state.failed)
                {
                    break;
                }
                for (const auto result : op.results())
                {
                    auto it = valueMap.find(result);
                    if (it == valueMap.end())
                    {
                        state.opError(source, op, "Flatten missing result mapping");
                        state.failed = true;
                        break;
                    }
                    if (!addResultChecked(state, target, newOp, it->second, source, op, result, prefix))
                    {
                        state.failed = true;
                        break;
                    }
                }
                if (state.failed)
                {
                    break;
                }
                state.changed = true;
            }

            if (!state.failed && !pendingAttrs.empty())
            {
                for (const auto &pending : pendingAttrs)
                {
                    auto it = opRename.find(pending.oldValue);
                    if (it == opRename.end())
                    {
                        state.graphError(source, "Flatten unresolved symbol reference: " + pending.oldValue);
                        state.failed = true;
                        break;
                    }
                    target.setAttr(pending.op, pending.key, it->second);
                }
            }

            if (!state.failed && !source.generateGroups().empty())
            {
                propagateGenerateGroups(source, target, prefix, valueRename, opRename, state);
            }

            state.stack.erase(&source);
            return !state.failed;
        }

        bool hasXmrOps(const wolvrix::lib::grh::Graph &graph)
        {
            for (const auto opId : graph.operations())
            {
                if (!opId.valid())
                {
                    continue;
                }
                const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
                if (op.kind() == wolvrix::lib::grh::OperationKind::kXMRRead ||
                    op.kind() == wolvrix::lib::grh::OperationKind::kXMRWrite)
                {
                    return true;
                }
            }
            return false;
        }

    } // namespace

    HierFlattenPass::HierFlattenPass()
        : Pass("hier-flatten", "hier-flatten", "Flatten module hierarchy into a single graph"),
          options_({})
    {
    }

    HierFlattenPass::HierFlattenPass(HierFlattenOptions options)
        : Pass("hier-flatten", "hier-flatten", "Flatten module hierarchy into a single graph"),
          options_(options)
    {
    }

    PassResult HierFlattenPass::run()
    {
        PassResult result;
        const std::size_t graphCount = design().graphs().size();
        logDebug("begin graphs=" + std::to_string(graphCount));

        for (const auto &entry : design().graphs())
        {
            if (!entry.second)
            {
                continue;
            }
            if (hasXmrOps(*entry.second))
            {
                error(*entry.second, "hier-flatten requires xmr-resolve before flatten");
                result.failed = true;
                return result;
            }
        }

        wolvrix::lib::grh::Graph *top = nullptr;
        const auto &tops = design().topGraphs();
        if (tops.size() == 1)
        {
            top = design().findGraph(tops.front());
        }
        else if (tops.empty() && design().graphs().size() == 1)
        {
            top = design().graphs().begin()->second.get();
        }
        else
        {
            error("hier-flatten requires exactly one top module");
            result.failed = true;
            return result;
        }
        if (!top)
        {
            error("hier-flatten failed to resolve top module");
            result.failed = true;
            return result;
        }

        InlineState state{design()};
        state.symProtect = options_.symProtect;
        state.graphError = [this](const wolvrix::lib::grh::Graph &graph, std::string message) {
            error(graph, std::move(message));
        };
        state.opError = [this](const wolvrix::lib::grh::Graph &graph,
                               const wolvrix::lib::grh::Operation &op,
                               std::string message) {
            error(graph, op, std::move(message));
        };

        // Invariant: top-graph symbols are never renamed; they are the root of
        // every hierarchical path. Only symbols cloned from inlined child
        // graphs receive the `inst$...$` prefix (and only child graphs'
        // generate groups gain the scope prefix).
        ValueMap identity;
        identity.reserve(top->values().size());
        for (const auto valueId : top->values())
        {
            if (!valueId.valid())
            {
                continue;
            }
            identity.emplace(valueId, valueId);
        }

        // Prefixing happens only while inlining a kInstance op, and the op is
        // erased as it is inlined; re-running the pass on an already flattened
        // graph finds no instances and is a no-op, so already-prefixed symbols
        // never gain a second prefix.
        std::vector<wolvrix::lib::grh::OperationId> instanceOps;
        for (const auto opId : top->operations())
        {
            if (!opId.valid())
            {
                continue;
            }
            const wolvrix::lib::grh::Operation op = top->getOperation(opId);
            if (op.kind() == wolvrix::lib::grh::OperationKind::kInstance)
            {
                instanceOps.push_back(opId);
            }
        }

        for (const auto opId : instanceOps)
        {
            if (state.failed)
            {
                break;
            }
            const wolvrix::lib::grh::Operation op = top->getOperation(opId);
            const InstanceInfo info = buildInstanceInfo(op);
            top->eraseOpUnchecked(opId);
            state.changed = true;
            if (!inlineInstanceInfo(*top, info, identity, *top, std::string(), state))
            {
                state.failed = true;
                break;
            }
        }

        if (state.failed)
        {
            result.failed = true;
            return result;
        }

        if (!options_.preserveFlattenedModules)
        {
            const std::string topName = top->symbol();
            std::unordered_set<std::string> keepModules;
            for (const auto opId : top->operations())
            {
                const wolvrix::lib::grh::Operation op = top->getOperation(opId);
                if (op.kind() != wolvrix::lib::grh::OperationKind::kBlackbox)
                {
                    continue;
                }
                const auto attr = op.attr("moduleName");
                if (!attr)
                {
                    continue;
                }
                if (const auto value = std::get_if<std::string>(&*attr))
                {
                    if (!value->empty())
                    {
                        keepModules.insert(*value);
                    }
                }
            }
            std::vector<std::string> toDelete;
            toDelete.reserve(design().graphs().size());
            for (const auto &entry : design().graphs())
            {
                if (!entry.second)
                {
                    continue;
                }
                if (entry.first != topName && keepModules.find(entry.first) == keepModules.end())
                {
                    toDelete.push_back(entry.first);
                }
            }
            for (const auto &name : toDelete)
            {
                design().deleteGraph(name);
            }
        }

        result.changed = state.changed;
        return result;
    }

} // namespace wolvrix::lib::transform
