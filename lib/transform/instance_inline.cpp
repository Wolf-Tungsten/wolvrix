#include "transform/instance_inline.hpp"

#include "core/grh.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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

        struct AliasOutput
        {
            wolvrix::lib::grh::ValueId source;
            wolvrix::lib::grh::ValueId target;
        };

        struct ResolvedTarget
        {
            wolvrix::lib::grh::Graph *rootGraph = nullptr;
            wolvrix::lib::grh::Graph *parentGraph = nullptr;
            wolvrix::lib::grh::Graph *childGraph = nullptr;
            wolvrix::lib::grh::OperationId instanceOp = wolvrix::lib::grh::OperationId::invalid();
            std::vector<std::string> segments;
            std::string prefix;
        };

        struct CloneStats
        {
            std::size_t valuesCloned = 0;
            std::size_t opsCloned = 0;
            std::size_t attrsRewritten = 0;
            std::size_t aliasAssigns = 0;
        };

        struct Reporter
        {
            std::function<void(const wolvrix::lib::grh::Graph &, std::string)> graphError;
            std::function<void(const wolvrix::lib::grh::Graph &, const wolvrix::lib::grh::Operation &, std::string)> opError;
        };

        std::optional<std::string> getAttrString(const wolvrix::lib::grh::Operation &op,
                                                 std::string_view key)
        {
            auto attr = op.attr(key);
            if (!attr)
            {
                return std::nullopt;
            }
            if (const auto *value = std::get_if<std::string>(&*attr))
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
            if (const auto *values = std::get_if<std::vector<std::string>>(&*attr))
            {
                return *values;
            }
            return std::nullopt;
        }

        std::vector<std::string> splitPath(std::string_view path)
        {
            std::vector<std::string> out;
            std::string current;
            for (const char ch : path)
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

        bool hasXmrOps(const wolvrix::lib::grh::Graph &graph)
        {
            for (const auto opId : graph.operations())
            {
                if (!opId.valid())
                {
                    continue;
                }
                const auto op = graph.getOperation(opId);
                if (op.kind() == wolvrix::lib::grh::OperationKind::kXMRRead ||
                    op.kind() == wolvrix::lib::grh::OperationKind::kXMRWrite)
                {
                    return true;
                }
            }
            return false;
        }

        bool isStatefulKind(wolvrix::lib::grh::OperationKind kind)
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kRegister:
            case wolvrix::lib::grh::OperationKind::kLatch:
                return true;
            default:
                return false;
            }
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

        wolvrix::lib::grh::SymbolId internUniqueSymbol(wolvrix::lib::grh::Graph &graph,
                                                       const std::string &base)
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

        std::string joinPrefix(const std::vector<std::string> &segments)
        {
            std::string prefix;
            for (std::size_t i = 1; i < segments.size(); ++i)
            {
                const std::string part = normalizeComponent(segments[i]);
                if (part.empty())
                {
                    continue;
                }
                if (!prefix.empty())
                {
                    prefix.push_back('$');
                }
                prefix.append(part);
            }
            return prefix;
        }

        wolvrix::lib::grh::OperationId findUniqueInstance(const wolvrix::lib::grh::Graph &graph,
                                                          std::string_view instanceName,
                                                          std::string &error)
        {
            wolvrix::lib::grh::OperationId found = wolvrix::lib::grh::OperationId::invalid();
            for (const auto opId : graph.operations())
            {
                if (!opId.valid())
                {
                    continue;
                }
                const auto op = graph.getOperation(opId);
                if (op.kind() != wolvrix::lib::grh::OperationKind::kInstance)
                {
                    continue;
                }
                const auto name = getAttrString(op, "instanceName");
                if (!name || *name != instanceName)
                {
                    continue;
                }
                if (found.valid())
                {
                    error = "duplicate instanceName in graph: " + std::string(instanceName);
                    return wolvrix::lib::grh::OperationId::invalid();
                }
                found = opId;
            }
            if (!found.valid())
            {
                error = "instance not found: " + std::string(instanceName);
            }
            return found;
        }

        std::optional<ResolvedTarget> resolveTargetPath(wolvrix::lib::grh::Design &design,
                                                        std::string_view path,
                                                        std::string &error)
        {
            std::vector<std::string> segments = splitPath(path);
            if (segments.size() < 2)
            {
                error = "instance-inline path must be <root>.<inst>...";
                return std::nullopt;
            }

            wolvrix::lib::grh::Graph *root = design.findGraph(segments.front());
            if (root == nullptr)
            {
                error = "instance-inline root graph not found: " + segments.front();
                return std::nullopt;
            }

            wolvrix::lib::grh::Graph *current = root;
            for (std::size_t i = 1; i < segments.size(); ++i)
            {
                std::string hopError;
                const auto instOp = findUniqueInstance(*current, segments[i], hopError);
                if (!instOp.valid())
                {
                    error = "instance-inline path resolution failed at " + segments[i] + ": " + hopError;
                    return std::nullopt;
                }
                const auto op = current->getOperation(instOp);
                const auto moduleName = getAttrString(op, "moduleName");
                if (!moduleName || moduleName->empty())
                {
                    error = "instance-inline instance missing moduleName: " + segments[i];
                    return std::nullopt;
                }
                wolvrix::lib::grh::Graph *child = design.findGraph(*moduleName);
                if (child == nullptr)
                {
                    error = "instance-inline child graph not found: " + *moduleName;
                    return std::nullopt;
                }
                if (i + 1 == segments.size())
                {
                    const std::string prefix = joinPrefix(segments);
                    return ResolvedTarget{root, current, child, instOp, std::move(segments), prefix};
                }
                current = child;
            }

            error = "instance-inline internal resolution error";
            return std::nullopt;
        }

        bool addResultChecked(const Reporter &reporter,
                              const wolvrix::lib::grh::Graph &source,
                              wolvrix::lib::grh::Graph &target,
                              wolvrix::lib::grh::OperationId newOp,
                              wolvrix::lib::grh::ValueId newValue,
                              const wolvrix::lib::grh::Operation &srcOp,
                              std::string_view path)
        {
            if (!newValue.valid())
            {
                reporter.opError(source, srcOp, "instance-inline produced invalid result value");
                return false;
            }
            const auto dstValue = target.getValue(newValue);
            if (dstValue.definingOp().valid())
            {
                std::string message = "instance-inline result already has defining op";
                if (!path.empty())
                {
                    message.append(" path=");
                    message.append(path);
                }
                reporter.opError(source, srcOp, std::move(message));
                return false;
            }
            target.addResult(newOp, newValue);
            return true;
        }

        // stage-04: when the child port value carries an array shape and the
        // parent-side connection value has none (same flat width), propagate
        // the shape so child-internal array accesses survive the inline.
        void propagatePortShape(wolvrix::lib::grh::Graph &parent,
                                const wolvrix::lib::grh::Graph &child,
                                wolvrix::lib::grh::ValueId childValue,
                                wolvrix::lib::grh::ValueId parentValue)
        {
            const std::span<const int64_t> childShape = child.valueShape(childValue);
            if (childShape.empty() || !parent.valueShape(parentValue).empty())
            {
                return;
            }
            if (parent.getValue(parentValue).width() != child.getValue(childValue).width())
            {
                return;
            }
            parent.setValueShape(parentValue, childShape);
        }

        bool buildPortMap(const wolvrix::lib::grh::Graph &child,
                          wolvrix::lib::grh::Graph &parent,
                          const wolvrix::lib::grh::Operation &instOp,
                          ValueMap &portMap,
                          std::vector<AliasOutput> &aliasOutputs,
                          std::string &error)
        {
            const auto inputNames = getAttrStrings(instOp, "inputPortName");
            const auto outputNames = getAttrStrings(instOp, "outputPortName");
            const auto inoutNames = getAttrStrings(instOp, "inoutPortName");

            const std::size_t inputCount = inputNames ? inputNames->size() : child.inputPorts().size();
            const std::size_t outputCount = outputNames ? outputNames->size() : child.outputPorts().size();
            const std::size_t inoutCount = inoutNames ? inoutNames->size() : child.inoutPorts().size();

            if (child.inputPorts().size() != inputCount ||
                child.outputPorts().size() != outputCount ||
                child.inoutPorts().size() != inoutCount)
            {
                error = "instance-inline port list does not match child graph ports";
                return false;
            }

            if (instOp.operands().size() != inputCount + inoutCount)
            {
                error = "instance-inline operand count mismatch";
                return false;
            }
            if (instOp.results().size() != outputCount + inoutCount * 2)
            {
                error = "instance-inline result count mismatch";
                return false;
            }

            std::unordered_map<std::string_view, wolvrix::lib::grh::ValueId> childInputsByName;
            if (inputNames)
            {
                childInputsByName.reserve(child.inputPorts().size());
                for (const auto &port : child.inputPorts())
                {
                    childInputsByName.emplace(port.name, port.value);
                }
            }

            for (std::size_t i = 0; i < inputCount; ++i)
            {
                wolvrix::lib::grh::ValueId childValue = wolvrix::lib::grh::ValueId::invalid();
                if (inputNames)
                {
                    auto it = childInputsByName.find((*inputNames)[i]);
                    if (it == childInputsByName.end())
                    {
                        error = "instance-inline missing child input port: " + (*inputNames)[i];
                        return false;
                    }
                    childValue = it->second;
                }
                else
                {
                    childValue = child.inputPorts()[i].value;
                }
                portMap.emplace(childValue, instOp.operands()[i]);
                propagatePortShape(parent, child, childValue, instOp.operands()[i]);
            }

            std::unordered_map<std::string_view, wolvrix::lib::grh::ValueId> childOutputsByName;
            if (outputNames)
            {
                childOutputsByName.reserve(child.outputPorts().size());
                for (const auto &port : child.outputPorts())
                {
                    childOutputsByName.emplace(port.name, port.value);
                }
            }

            for (std::size_t i = 0; i < outputCount; ++i)
            {
                wolvrix::lib::grh::ValueId childValue = wolvrix::lib::grh::ValueId::invalid();
                if (outputNames)
                {
                    auto it = childOutputsByName.find((*outputNames)[i]);
                    if (it == childOutputsByName.end())
                    {
                        error = "instance-inline missing child output port: " + (*outputNames)[i];
                        return false;
                    }
                    childValue = it->second;
                }
                else
                {
                    childValue = child.outputPorts()[i].value;
                }
                const auto mapped = instOp.results()[i];
                propagatePortShape(parent, child, childValue, mapped);
                auto [it, inserted] = portMap.emplace(childValue, mapped);
                if (!inserted && it->second != mapped)
                {
                    aliasOutputs.push_back(AliasOutput{it->second, mapped});
                }
            }

            std::unordered_map<std::string_view, std::size_t> childInoutsByName;
            if (inoutNames)
            {
                childInoutsByName.reserve(child.inoutPorts().size());
                for (std::size_t i = 0; i < child.inoutPorts().size(); ++i)
                {
                    childInoutsByName.emplace(child.inoutPorts()[i].name, i);
                }
            }

            for (std::size_t i = 0; i < inoutCount; ++i)
            {
                std::size_t childIndex = i;
                if (inoutNames)
                {
                    auto it = childInoutsByName.find((*inoutNames)[i]);
                    if (it == childInoutsByName.end())
                    {
                        error = "instance-inline missing child inout port: " + (*inoutNames)[i];
                        return false;
                    }
                    childIndex = it->second;
                }
                const auto &port = child.inoutPorts()[childIndex];
                const auto operandIndex = inputCount + i;
                const auto outIndex = outputCount + i;
                const auto oeIndex = outputCount + inoutCount + i;
                portMap.emplace(port.in, instOp.operands()[operandIndex]);
                portMap.emplace(port.out, instOp.results()[outIndex]);
                portMap.emplace(port.oe, instOp.results()[oeIndex]);
                propagatePortShape(parent, child, port.in, instOp.operands()[operandIndex]);
                propagatePortShape(parent, child, port.out, instOp.results()[outIndex]);
                propagatePortShape(parent, child, port.oe, instOp.results()[oeIndex]);
            }

            return true;
        }

        bool cloneChildOneLevel(const Reporter &reporter,
                                const ResolvedTarget &targetInfo,
                                ValueMap &portMap,
                                const std::vector<AliasOutput> &aliasOutputs,
                                CloneStats &stats)
        {
            auto &source = *targetInfo.childGraph;
            auto &target = *targetInfo.parentGraph;

            auto requireMappedPort = [&](wolvrix::lib::grh::ValueId valueId, std::string_view role) {
                if (portMap.find(valueId) == portMap.end())
                {
                    reporter.graphError(source, "instance-inline missing mapping for port value (" + std::string(role) + ")");
                    return false;
                }
                return true;
            };

            for (const auto &port : source.inputPorts())
            {
                if (!requireMappedPort(port.value, "input"))
                {
                    return false;
                }
            }
            for (const auto &port : source.outputPorts())
            {
                if (!requireMappedPort(port.value, "output"))
                {
                    return false;
                }
            }
            for (const auto &port : source.inoutPorts())
            {
                if (!requireMappedPort(port.in, "inout.in") ||
                    !requireMappedPort(port.out, "inout.out") ||
                    !requireMappedPort(port.oe, "inout.oe"))
                {
                    return false;
                }
            }

            for (const auto valueId : source.values())
            {
                if (!valueId.valid() || portMap.find(valueId) != portMap.end())
                {
                    continue;
                }
                const auto value = source.getValue(valueId);
                wolvrix::lib::grh::SymbolId newSym = wolvrix::lib::grh::SymbolId::invalid();
                if (source.isDeclaredSymbol(value.symbol()))
                {
                    const std::string base = std::string(value.symbolText());
                    if (!base.empty())
                    {
                        newSym = internUniqueSymbol(target, makeHierName(targetInfo.prefix, base));
                        target.addDeclaredSymbol(newSym);
                    }
                }
                if (!newSym.valid())
                {
                    newSym = target.makeInternalValSym();
                }
                // stage-04: preserve the array view on cloned values.
                const auto newValue = target.createValue(newSym, value.width(), value.isSigned(),
                                                         value.type(), value.shape());
                if (value.srcLoc())
                {
                    target.setValueSrcLoc(newValue, *value.srcLoc());
                }
                portMap.emplace(valueId, newValue);
                ++stats.valuesCloned;
            }

            std::unordered_map<std::string, std::string> opRename;
            std::vector<PendingAttr> pendingAttrs;

            auto rewriteAttr = [&](wolvrix::lib::grh::OperationId newOp,
                                   const std::string &key,
                                   const wolvrix::lib::grh::AttributeValue &value) {
                target.setAttr(newOp, key, value);
                if (key != "regSymbol" &&
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
                        ++stats.attrsRewritten;
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
                const auto op = source.getOperation(opId);

                if (op.kind() == wolvrix::lib::grh::OperationKind::kDpicImport)
                {
                    const std::string opName = std::string(op.symbolText());
                    if (opName.empty())
                    {
                        reporter.opError(source, op, "instance-inline encountered kDpicImport without symbol");
                        return false;
                    }
                    const auto existingId = target.findOperation(opName);
                    if (existingId.valid())
                    {
                        const auto existing = target.getOperation(existingId);
                        if (existing.kind() != wolvrix::lib::grh::OperationKind::kDpicImport ||
                            !attrsEqual(existing, op))
                        {
                            reporter.opError(source, op, "instance-inline DPI import conflicts with existing operation");
                            return false;
                        }
                        opRename.emplace(opName, opName);
                        continue;
                    }
                    if (target.findValue(opName).valid())
                    {
                        reporter.opError(source, op, "instance-inline DPI import conflicts with existing value");
                        return false;
                    }
                    const auto sym = target.internSymbol(opName);
                    if (!sym.valid())
                    {
                        reporter.opError(source, op, "instance-inline DPI import symbol collision");
                        return false;
                    }
                    const auto newOp = target.createOperation(wolvrix::lib::grh::OperationKind::kDpicImport, sym);
                    if (op.srcLoc())
                    {
                        target.setOpSrcLoc(newOp, *op.srcLoc());
                    }
                    for (const auto &attr : op.attrs())
                    {
                        target.setAttr(newOp, attr.key, attr.value);
                    }
                    opRename.emplace(opName, opName);
                    ++stats.opsCloned;
                    continue;
                }

                wolvrix::lib::grh::SymbolId newSym = wolvrix::lib::grh::SymbolId::invalid();
                if ((op.symbol().valid() && source.isDeclaredSymbol(op.symbol())) || isStatefulKind(op.kind()))
                {
                    const std::string base = std::string(op.symbolText());
                    if (!base.empty())
                    {
                        newSym = internUniqueSymbol(target, makeHierName(targetInfo.prefix, base));
                        target.addDeclaredSymbol(newSym);
                    }
                }
                if (!newSym.valid())
                {
                    newSym = target.makeInternalOpSym();
                }

                const std::string oldName = std::string(op.symbolText());
                const std::string newName = std::string(target.symbolText(newSym));
                if (!oldName.empty() && !newName.empty())
                {
                    opRename.emplace(oldName, newName);
                }

                const auto newOp = target.createOperation(op.kind(), newSym);
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
                    auto it = portMap.find(operand);
                    if (it == portMap.end())
                    {
                        reporter.opError(source, op, "instance-inline missing operand mapping");
                        return false;
                    }
                    target.addOperand(newOp, it->second);
                }
                for (const auto result : op.results())
                {
                    auto it = portMap.find(result);
                    if (it == portMap.end())
                    {
                        reporter.opError(source, op, "instance-inline missing result mapping");
                        return false;
                    }
                    if (!addResultChecked(reporter, source, target, newOp, it->second, op, targetInfo.prefix))
                    {
                        return false;
                    }
                }
                ++stats.opsCloned;
            }

            for (const auto &pending : pendingAttrs)
            {
                auto it = opRename.find(pending.oldValue);
                if (it == opRename.end())
                {
                    reporter.graphError(source, "instance-inline unresolved symbol reference: " + pending.oldValue);
                    return false;
                }
                target.setAttr(pending.op, pending.key, it->second);
                ++stats.attrsRewritten;
            }

            for (const auto &alias : aliasOutputs)
            {
                if (!alias.source.valid() || !alias.target.valid())
                {
                    reporter.graphError(source, "instance-inline invalid output alias mapping");
                    return false;
                }
                if (target.getValue(alias.target).definingOp().valid())
                {
                    reporter.graphError(source, "instance-inline duplicate output already defined");
                    return false;
                }
                const auto assignOp =
                    target.createOperation(wolvrix::lib::grh::OperationKind::kAssign, target.makeInternalOpSym());
                target.addOperand(assignOp, alias.source);
                target.addResult(assignOp, alias.target);
                target.setOpSrcLoc(assignOp, makeTransformSrcLoc("instance-inline", "alias_output"));
                ++stats.aliasAssigns;
                ++stats.opsCloned;
            }

            return true;
        }

    } // namespace

    InstanceInlinePass::InstanceInlinePass()
        : InstanceInlinePass(InstanceInlineOptions{})
    {
    }

    InstanceInlinePass::InstanceInlinePass(InstanceInlineOptions options)
        : Pass("instance-inline", "instance-inline", "Inline one selected instance without recursive flatten"),
          options_(std::move(options))
    {
    }

    PassResult InstanceInlinePass::run()
    {
        PassResult result;

        if (options_.path.empty())
        {
            error("instance-inline requires -path");
            result.failed = true;
            return result;
        }

        for (const auto &entry : design().graphs())
        {
            if (entry.second && hasXmrOps(*entry.second))
            {
                error(*entry.second, "instance-inline requires xmr-resolve before inline");
                result.failed = true;
                return result;
            }
        }

        std::string resolveError;
        auto resolved = resolveTargetPath(design(), options_.path, resolveError);
        if (!resolved)
        {
            error(std::move(resolveError));
            result.failed = true;
            return result;
        }

        const auto instOp = resolved->parentGraph->getOperation(resolved->instanceOp);
        ValueMap portMap;
        std::vector<AliasOutput> aliasOutputs;
        std::string mapError;
        if (!buildPortMap(*resolved->childGraph, *resolved->parentGraph, instOp, portMap,
                          aliasOutputs, mapError))
        {
            error(*resolved->parentGraph, instOp, std::move(mapError));
            result.failed = true;
            return result;
        }

        if (!resolved->parentGraph->eraseOpUnchecked(resolved->instanceOp))
        {
            error(*resolved->parentGraph, instOp, "instance-inline failed to remove target instance");
            result.failed = true;
            return result;
        }

        CloneStats stats;
        const Reporter reporter{
            [this](const wolvrix::lib::grh::Graph &graph, std::string message) {
                error(graph, std::move(message));
            },
            [this](const wolvrix::lib::grh::Graph &graph,
                   const wolvrix::lib::grh::Operation &op,
                   std::string message) {
                error(graph, op, std::move(message));
            }};

        if (!cloneChildOneLevel(reporter, *resolved, portMap, aliasOutputs, stats))
        {
            result.failed = true;
            return result;
        }

        result.changed = true;
        logInfo("instance-inline: path=" + options_.path +
                " values_cloned=" + std::to_string(stats.valuesCloned) +
                " ops_cloned=" + std::to_string(stats.opsCloned) +
                " attrs_rewritten=" + std::to_string(stats.attrsRewritten) +
                " alias_assigns=" + std::to_string(stats.aliasAssigns));
        return result;
    }

} // namespace wolvrix::lib::transform
