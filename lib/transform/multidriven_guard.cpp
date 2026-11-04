#include "transform/multidriven_guard.hpp"

#include "core/grh.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wolvrix::lib::transform
{

    namespace
    {
        struct DriveSource
        {
            const wolvrix::lib::grh::Graph *graph = nullptr;
            wolvrix::lib::grh::OperationId op;
            std::string path;
        };

        struct DriveInfo
        {
            std::unordered_map<wolvrix::lib::grh::ValueId, DriveSource,
                               wolvrix::lib::grh::ValueIdHash>
                driven;
            bool inProgress = false;
            bool valid = false;
        };

        std::optional<std::string_view> getAttrString(wolvrix::lib::grh::AttrRange attrs,
                                                      std::string_view key)
        {
            for (const auto &entry : attrs)
            {
                if (entry.key() == key)
                {
                    if (const auto value = entry.value().get<std::string>())
                    {
                        return *value;
                    }
                    return std::nullopt;
                }
            }
            return std::nullopt;
        }

        std::optional<wolvrix::lib::grh::StringListView> getAttrStrings(wolvrix::lib::grh::AttrRange attrs,
                                                       std::string_view key)
        {
            for (const auto &entry : attrs)
            {
                if (entry.key() == key)
                {
                    return entry.value().get<std::vector<std::string>>();
                }
            }
            return std::nullopt;
        }

        std::string describeOp(const wolvrix::lib::grh::Graph &graph,
                               wolvrix::lib::grh::OperationId opId)
        {
            if (!opId.valid())
            {
                return "<none>";
            }
            const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
            std::string out = std::string(op.symbolText());
            if (out.empty())
            {
                out = std::string("op_") + std::to_string(opId.index);
            }
            out.append(" (");
            out.append(wolvrix::lib::grh::toString(op.kind()));
            out.push_back(')');
            return out;
        }

        std::string joinPath(std::string_view prefix, std::string_view suffix)
        {
            if (prefix.empty())
            {
                return std::string(suffix);
            }
            if (suffix.empty())
            {
                return std::string(prefix);
            }
            std::string out(prefix);
            out.push_back('$');
            out.append(suffix);
            return out;
        }

    } // namespace

    MultiDrivenGuardPass::MultiDrivenGuardPass()
        : Pass("multidriven-guard", "multidriven-guard",
               "Detect values that are already driven before hierarchy flattening")
    {
    }

    PassResult MultiDrivenGuardPass::run()
    {
        PassResult result;
        const std::size_t graphCount = design().graphs().size();
        logDebug("begin graphs=" + std::to_string(graphCount));

        std::size_t conflictCount = 0;
        std::unordered_map<const wolvrix::lib::grh::Graph *, DriveInfo> driveCache;

        auto computeDriveInfo = [&](auto &&self, wolvrix::lib::grh::Graph *graph) -> DriveInfo & {
            DriveInfo &info = driveCache[graph];
            if (info.valid)
            {
                return info;
            }
            if (info.inProgress)
            {
                return info;
            }
            info.inProgress = true;

            auto markDriven = [&](wolvrix::lib::grh::ValueId value,
                                   const wolvrix::lib::grh::Graph *graph,
                                   wolvrix::lib::grh::OperationId op,
                                   std::string path) {
                if (!value.valid())
                {
                    return;
                }
                auto &slot = info.driven[value];
                if (slot.graph == nullptr)
                {
                    slot.graph = graph;
                    slot.op = op;
                    slot.path = std::move(path);
                }
            };

            const auto &graphView = *graph;
            for (const auto &port : graphView.inoutPorts())
            {
                const auto outDef = graphView.valueDef(port.out);
                const auto oeDef = graphView.valueDef(port.oe);
                if (outDef.valid())
                {
                    markDriven(port.out, graph, outDef, std::string());
                }
                if (oeDef.valid())
                {
                    markDriven(port.oe, graph, oeDef, std::string());
                }
            }

            for (const auto opId : graphView.operations())
            {
                if (!opId.valid())
                {
                    continue;
                }
                if (graphView.opKind(opId) != wolvrix::lib::grh::OperationKind::kInstance)
                {
                    continue;
                }
                const auto attrs = graphView.opAttrRange(opId);
                const std::string_view moduleName = getAttrString(attrs, "moduleName").value_or(std::string_view());
                wolvrix::lib::grh::Graph *child = moduleName.empty()
                                                     ? nullptr
                                                     : design().findGraph(moduleName);
                if (!child)
                {
                    continue;
                }

                const auto inputNames = getAttrStrings(attrs, "inputPortName");
                const auto outputNames = getAttrStrings(attrs, "outputPortName");
                const auto inoutNames = getAttrStrings(attrs, "inoutPortName");
                const std::size_t inputCount = inputNames
                                                   ? inputNames->size()
                                                   : child->inputPorts().size();
                const std::size_t outputCount = outputNames
                                                    ? outputNames->size()
                                                    : child->outputPorts().size();
                const std::size_t inoutCount = inoutNames
                                                   ? inoutNames->size()
                                                   : child->inoutPorts().size();
                if (inoutCount == 0)
                {
                    continue;
                }
                const auto operands = graphView.opOperands(opId);
                const auto results = graphView.opResults(opId);
                const std::size_t expectedOperands = inputCount + inoutCount;
                const std::size_t expectedResults = outputCount + inoutCount * 2;
                if (operands.size() < expectedOperands || results.size() < expectedResults)
                {
                    continue;
                }

                DriveInfo &childInfo = self(self, child);
                if (childInfo.inProgress && !childInfo.valid)
                {
                    continue;
                }

                const auto childInouts = child->inoutPorts();
                if (!inoutNames && childInouts.size() < inoutCount)
                {
                    continue;
                }
                std::unordered_map<std::string_view, std::size_t> childInoutByName;
                if (inoutNames)
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
                    if (inoutNames)
                    {
                        const std::string_view portName = (*inoutNames)[i];
                        auto it = childInoutByName.find(portName);
                        if (it == childInoutByName.end())
                        {
                            continue;
                        }
                        childIndex = it->second;
                    }
                    const std::size_t outIndex = outputCount + i;
                    const std::size_t oeIndex = outputCount + inoutCount + i;
                    if (outIndex >= results.size() || oeIndex >= results.size())
                    {
                        continue;
                    }
                    const auto &childInout = childInouts[childIndex];
                    const auto outIt = childInfo.driven.find(childInout.out);
                    if (outIt != childInfo.driven.end())
                    {
                        std::string inst = std::string(getAttrString(attrs, "instanceName").value_or(moduleName));
                        std::string path = joinPath(inst, outIt->second.path);
                        markDriven(results[outIndex], outIt->second.graph, outIt->second.op, std::move(path));
                    }
                    const auto oeIt = childInfo.driven.find(childInout.oe);
                    if (oeIt != childInfo.driven.end())
                    {
                        std::string inst = std::string(getAttrString(attrs, "instanceName").value_or(moduleName));
                        std::string path = joinPath(inst, oeIt->second.path);
                        markDriven(results[oeIndex], oeIt->second.graph, oeIt->second.op, std::move(path));
                    }
                }
            }

            info.inProgress = false;
            info.valid = true;
            return info;
        };

        for (const auto &entry : design().graphs())
        {
            if (!entry.second)
            {
                continue;
            }
            wolvrix::lib::grh::Graph &graph = *entry.second;
            const auto &graphView = graph;

            for (const auto opId : graphView.operations())
            {
                if (!opId.valid())
                {
                    continue;
                }
                if (graphView.opKind(opId) != wolvrix::lib::grh::OperationKind::kInstance)
                {
                    continue;
                }

                const auto attrs = graphView.opAttrRange(opId);
                const std::string_view moduleName = getAttrString(attrs, "moduleName").value_or(std::string_view());
                const std::optional<std::string_view> instanceName = getAttrString(attrs, "instanceName");
                wolvrix::lib::grh::Graph *child = moduleName.empty()
                                                     ? nullptr
                                                     : design().findGraph(moduleName);
                const auto inputNames = getAttrStrings(attrs, "inputPortName");
                const auto outputNames = getAttrStrings(attrs, "outputPortName");
                const auto inoutNames = getAttrStrings(attrs, "inoutPortName");

                const std::size_t inputCount = inputNames
                                                   ? inputNames->size()
                                                   : (child ? child->inputPorts().size() : 0U);
                const std::size_t outputCount = outputNames
                                                    ? outputNames->size()
                                                    : (child ? child->outputPorts().size() : 0U);
                const std::size_t inoutCount = inoutNames
                                                   ? inoutNames->size()
                                                   : (child ? child->inoutPorts().size() : 0U);

                if (inoutCount == 0)
                {
                    continue;
                }

                const auto operands = graphView.opOperands(opId);
                const auto results = graphView.opResults(opId);
                const std::size_t expectedOperands = inputCount + inoutCount;
                const std::size_t expectedResults = outputCount + inoutCount * 2;
                if (operands.size() < expectedOperands || results.size() < expectedResults)
                {
                    warning(graph, graph.getOperation(opId),
                            "multidriven-guard: instance port counts do not match inout ports");
                    continue;
                }

                if (!child)
                {
                    warning(graph, graph.getOperation(opId),
                            "multidriven-guard: instance module graph not found; skipping inout checks");
                    continue;
                }

                DriveInfo &childInfo = computeDriveInfo(computeDriveInfo, child);
                if (childInfo.inProgress && !childInfo.valid)
                {
                    warning(graph, graph.getOperation(opId),
                            "multidriven-guard: recursive instance detected; skipping inout checks");
                    continue;
                }
                const auto childInouts = child->inoutPorts();
                if (!inoutNames && childInouts.size() < inoutCount)
                {
                    warning(graph, graph.getOperation(opId),
                            "multidriven-guard: child inout port count mismatch");
                    continue;
                }
                std::unordered_map<std::string_view, std::size_t> childInoutByName;
                if (inoutNames)
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
                    std::string_view portName;
                    if (inoutNames)
                    {
                        portName = (*inoutNames)[i];
                        auto it = childInoutByName.find(portName);
                        if (it == childInoutByName.end())
                        {
                            warning(graph, graph.getOperation(opId),
                                    "multidriven-guard: missing inout port in child graph: " + std::string(portName));
                            continue;
                        }
                        childIndex = it->second;
                    }
                    else
                    {
                        portName = childInouts[childIndex].name;
                    }

                    const std::size_t outIndex = outputCount + i;
                    const std::size_t oeIndex = outputCount + inoutCount + i;
                    if (outIndex >= results.size() || oeIndex >= results.size())
                    {
                        warning(graph, graph.getOperation(opId),
                                "multidriven-guard: inout result index out of range");
                        continue;
                    }

                    const auto &childInout = childInouts[childIndex];
                    const auto outIt = childInfo.driven.find(childInout.out);
                    const auto oeIt = childInfo.driven.find(childInout.oe);
                    const bool childOutDriven = outIt != childInfo.driven.end();
                    const bool childOeDriven = oeIt != childInfo.driven.end();

                    auto checkDriven = [&](wolvrix::lib::grh::ValueId parentValue,
                                            std::string_view role,
                                            bool childDriven,
                                            const DriveSource *childSource) {
                        if (!childDriven || !parentValue.valid())
                        {
                            return;
                        }
                        const wolvrix::lib::grh::OperationId defOpId = graphView.valueDef(parentValue);
                        if (!defOpId.valid() || defOpId == opId)
                        {
                            return;
                        }
                        const std::string parentDef = describeOp(graph, defOpId);
                        std::string childDefText = "<none>";
                        std::string childDefGraph;
                        std::string childDefPath;
                        if (childSource)
                        {
                            if (childSource->graph)
                            {
                                childDefText = describeOp(*childSource->graph, childSource->op);
                                childDefGraph = childSource->graph->symbol();
                            }
                            childDefPath = childSource->path;
                        }
                        std::string message = "multidriven-guard: inout ";
                        message.append(role);
                        message.append(" already driven before hier-flatten");
                        message.append(" port=");
                        message.append(portName);
                        message.append(" parent_value=");
                        message.append(graph.symbolText(graph.valueSymbol(parentValue)));
                        message.append(" parent_def=");
                        message.append(parentDef);
                        message.append(" child_def=");
                        message.append(childDefText);
                        if (!childDefGraph.empty())
                        {
                            message.append(" child_def_graph=");
                            message.append(childDefGraph);
                        }
                        if (!childDefPath.empty())
                        {
                            message.append(" child_def_path=");
                            message.append(childDefPath);
                        }
                        if (!moduleName.empty())
                        {
                            message.append(" child_module=");
                            message.append(moduleName);
                        }
                        if (instanceName && !instanceName->empty())
                        {
                            message.append(" instance=");
                            message.append(*instanceName);
                        }
                        message.append(" graph=");
                        message.append(graph.symbol());
                        error(graph, graph.getOperation(opId), std::move(message));
                        conflictCount += 1;
                    };

                    checkDriven(results[outIndex], "out", childOutDriven,
                                childOutDriven ? &outIt->second : nullptr);
                    checkDriven(results[oeIndex], "oe", childOeDriven,
                                childOeDriven ? &oeIt->second : nullptr);
                }
            }
        }

        if (conflictCount > 0)
        {
            result.failed = true;
        }
        return result;
    }

} // namespace wolvrix::lib::transform
