#include "transform/blackbox_guard.hpp"

#include "core/grh.hpp"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <unordered_set>

namespace wolvrix::lib::transform
{
    namespace
    {
        struct PortSpec
        {
            std::string name;
            int32_t width = 1;
            bool isSigned = false;
            wolvrix::lib::grh::ValueType type = wolvrix::lib::grh::ValueType::Logic;
        };

        struct InoutSpec
        {
            std::string name;
            PortSpec in;
            PortSpec out;
            PortSpec oe;
        };

        struct StubSpec
        {
            std::string moduleName;
            std::vector<PortSpec> inputs;
            std::vector<PortSpec> outputs;
            std::vector<InoutSpec> inouts;
        };

        std::optional<std::string_view> getAttrString(std::span<const wolvrix::lib::grh::AttrKV> attrs,
                                                      std::string_view key)
        {
            for (const auto &entry : attrs)
            {
                if (entry.key == key)
                {
                    if (const auto *value = std::get_if<std::string>(&entry.value))
                    {
                        return *value;
                    }
                    return std::nullopt;
                }
            }
            return std::nullopt;
        }

        const std::vector<std::string> *getAttrStrings(std::span<const wolvrix::lib::grh::AttrKV> attrs,
                                                       std::string_view key)
        {
            for (const auto &entry : attrs)
            {
                if (entry.key == key)
                {
                    return std::get_if<std::vector<std::string>>(&entry.value);
                }
            }
            return nullptr;
        }

        std::string opContext(const wolvrix::lib::grh::Graph &graph,
                              wolvrix::lib::grh::OperationId opId)
        {
            std::string ctx = graph.symbol();
            if (!ctx.empty())
            {
                ctx.append("::");
            }
            const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
            std::string sym = std::string(op.symbolText());
            if (sym.empty())
            {
                sym = std::string("op_") + std::to_string(opId.index);
            }
            ctx.append(sym);
            return ctx;
        }

        PortSpec makePortSpec(const wolvrix::lib::grh::Graph &graph,
                              std::string name,
                              wolvrix::lib::grh::ValueId valueId,
                              bool fallback)
        {
            PortSpec spec;
            spec.name = std::move(name);
            if (!valueId.valid() || fallback)
            {
                return spec;
            }
            spec.width = std::max<int32_t>(1, graph.valueWidth(valueId));
            spec.isSigned = graph.valueSigned(valueId);
            spec.type = graph.valueType(valueId);
            return spec;
        }

        wolvrix::lib::grh::ValueId createNamedValue(wolvrix::lib::grh::Graph &graph,
                                                    const std::string &base,
                                                    int32_t width,
                                                    bool isSigned,
                                                    wolvrix::lib::grh::ValueType type)
        {
            for (int i = 0; i < 16; ++i)
            {
                std::string candidate = base;
                if (i > 0)
                {
                    candidate.push_back('_');
                    candidate.append(std::to_string(i));
                }
                wolvrix::lib::grh::SymbolId sym = graph.internSymbol(candidate);
                if (sym.valid())
                {
                    return graph.createValue(sym, width, isSigned, type);
                }
            }
            return graph.createValue(width, isSigned, type);
        }

        bool specsMatch(const StubSpec &lhs, const StubSpec &rhs)
        {
            if (lhs.inputs.size() != rhs.inputs.size() ||
                lhs.outputs.size() != rhs.outputs.size() ||
                lhs.inouts.size() != rhs.inouts.size())
            {
                return false;
            }
            auto matchNames = [](const auto &a, const auto &b) {
                if (a.size() != b.size())
                {
                    return false;
                }
                for (std::size_t i = 0; i < a.size(); ++i)
                {
                    if (a[i].name != b[i].name)
                    {
                        return false;
                    }
                }
                return true;
            };
            if (!matchNames(lhs.inputs, rhs.inputs) || !matchNames(lhs.outputs, rhs.outputs))
            {
                return false;
            }
            for (std::size_t i = 0; i < lhs.inouts.size(); ++i)
            {
                if (lhs.inouts[i].name != rhs.inouts[i].name)
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    BlackboxGuardPass::BlackboxGuardPass()
        : Pass("blackbox-guard", "blackbox-guard",
               "Create stub graphs for unresolved blackbox modules")
    {
    }

    PassResult BlackboxGuardPass::run()
    {
        PassResult result;
        std::unordered_map<std::string, StubSpec> stubs;
        std::unordered_set<std::string> convertibleModules;

        for (const auto &entry : design().graphs())
        {
            if (!entry.second)
            {
                continue;
            }
            const wolvrix::lib::grh::Graph &graph = *entry.second;
            for (const auto opId : graph.operations())
            {
                if (graph.opKind(opId) != wolvrix::lib::grh::OperationKind::kBlackbox)
                {
                    continue;
                }
                const std::string context = opContext(graph, opId);
                const auto attrs = graph.opAttrs(opId);
                auto moduleName = getAttrString(attrs, "moduleName");
                if (!moduleName || moduleName->empty())
                {
                    warning("blackbox-guard: missing moduleName", context);
                    continue;
                }
                if (design().findGraph(*moduleName) != nullptr)
                {
                    convertibleModules.insert(std::string(*moduleName));
                    continue;
                }

                auto inputNames = getAttrStrings(attrs, "inputPortName");
                auto outputNames = getAttrStrings(attrs, "outputPortName");
                auto inoutNames = getAttrStrings(attrs, "inoutPortName");
                if (!inputNames || !outputNames)
                {
                    warning("blackbox-guard: missing port names", context);
                    continue;
                }
                const std::size_t inCount = inputNames->size();
                const std::size_t outCount = outputNames->size();
                const std::size_t inoutCount = inoutNames ? inoutNames->size() : 0;
                const auto operands = graph.opOperands(opId);
                const auto results = graph.opResults(opId);
                bool sizeMismatch = false;

                StubSpec spec;
                spec.moduleName = std::string(*moduleName);
                spec.inputs.reserve(inCount);
                spec.outputs.reserve(outCount);
                spec.inouts.reserve(inoutCount);

                for (std::size_t i = 0; i < inCount; ++i)
                {
                    bool fallback = false;
                    wolvrix::lib::grh::ValueId valueId =
                        i < operands.size() ? operands[i] : wolvrix::lib::grh::ValueId::invalid();
                    fallback = !valueId.valid();
                    if (fallback)
                    {
                        sizeMismatch = true;
                    }
                    spec.inputs.push_back(makePortSpec(graph, (*inputNames)[i], valueId, fallback));
                }
                for (std::size_t i = 0; i < outCount; ++i)
                {
                    bool fallback = false;
                    wolvrix::lib::grh::ValueId valueId =
                        i < results.size() ? results[i] : wolvrix::lib::grh::ValueId::invalid();
                    fallback = !valueId.valid();
                    if (fallback)
                    {
                        sizeMismatch = true;
                    }
                    spec.outputs.push_back(makePortSpec(graph, (*outputNames)[i], valueId, fallback));
                }
                if (inoutNames)
                {
                    for (std::size_t i = 0; i < inoutCount; ++i)
                    {
                        const std::size_t inIndex = inCount + i;
                        const std::size_t outIndex = outCount + i;
                        const std::size_t oeIndex = outCount + inoutCount + i;
                        wolvrix::lib::grh::ValueId inValue =
                            inIndex < operands.size() ? operands[inIndex] : wolvrix::lib::grh::ValueId::invalid();
                        wolvrix::lib::grh::ValueId outValue =
                            outIndex < results.size() ? results[outIndex] : wolvrix::lib::grh::ValueId::invalid();
                        wolvrix::lib::grh::ValueId oeValue =
                            oeIndex < results.size() ? results[oeIndex] : wolvrix::lib::grh::ValueId::invalid();
                        bool inFallback = !inValue.valid();
                        bool outFallback = !outValue.valid();
                        bool oeFallback = !oeValue.valid();
                        if (inFallback || outFallback || oeFallback)
                        {
                            sizeMismatch = true;
                        }
                        InoutSpec inout;
                        inout.name = (*inoutNames)[i];
                        inout.in = makePortSpec(graph, inout.name + "__in", inValue, inFallback);
                        inout.out = makePortSpec(graph, inout.name + "__out", outValue, outFallback);
                        inout.oe = makePortSpec(graph, inout.name + "__oe", oeValue, oeFallback);
                        spec.inouts.push_back(std::move(inout));
                    }
                }
                if (sizeMismatch)
                {
                    warning("blackbox-guard: port count mismatch; using fallback widths", context);
                }

                auto it = stubs.find(std::string(*moduleName));
                if (it == stubs.end())
                {
                    stubs.emplace(*moduleName, std::move(spec));
                }
                else if (!specsMatch(it->second, spec))
                {
                    warning("blackbox-guard: blackbox port signature mismatch; keeping first stub", context);
                }
                convertibleModules.insert(std::string(*moduleName));
            }
        }

        for (const auto &entry : stubs)
        {
            const StubSpec &spec = entry.second;
            if (design().findGraph(spec.moduleName) != nullptr)
            {
                continue;
            }
            wolvrix::lib::grh::Graph &stub = design().createGraph(spec.moduleName);
            for (const auto &port : spec.inputs)
            {
                wolvrix::lib::grh::ValueId value =
                    createNamedValue(stub, port.name, port.width, port.isSigned, port.type);
                stub.bindInputPort(port.name, value);
            }
            for (const auto &port : spec.outputs)
            {
                wolvrix::lib::grh::ValueId value =
                    createNamedValue(stub, port.name, port.width, port.isSigned, port.type);
                stub.bindOutputPort(port.name, value);
            }
            for (const auto &port : spec.inouts)
            {
                wolvrix::lib::grh::ValueId inValue =
                    createNamedValue(stub, port.in.name, port.in.width, port.in.isSigned, port.in.type);
                wolvrix::lib::grh::ValueId outValue =
                    createNamedValue(stub, port.out.name, port.out.width, port.out.isSigned, port.out.type);
                wolvrix::lib::grh::ValueId oeValue =
                    createNamedValue(stub, port.oe.name, port.oe.width, port.oe.isSigned, port.oe.type);
                stub.bindInoutPort(port.name, inValue, outValue, oeValue);
            }
            warning("blackbox-guard: created stub module for blackbox", spec.moduleName);
            result.changed = true;
        }

        if (!convertibleModules.empty())
        {
            for (const auto &entry : design().graphs())
            {
                if (!entry.second)
                {
                    continue;
                }
                wolvrix::lib::grh::Graph &graph = *entry.second;
                for (const auto opId : graph.operations())
                {
                    if (graph.opKind(opId) != wolvrix::lib::grh::OperationKind::kBlackbox)
                    {
                        continue;
                    }
                    const auto attrs = graph.opAttrs(opId);
                    auto moduleName = getAttrString(attrs, "moduleName");
                    if (!moduleName || moduleName->empty())
                    {
                        continue;
                    }
                    if (convertibleModules.find(std::string(*moduleName)) == convertibleModules.end())
                    {
                        continue;
                    }
                    graph.setOpKind(opId, wolvrix::lib::grh::OperationKind::kInstance);
                    result.changed = true;
                }
            }
        }

        return result;
    }
} // namespace wolvrix::lib::transform
