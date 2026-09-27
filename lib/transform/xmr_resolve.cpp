#include "transform/xmr_resolve.hpp"

#include "core/grh.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace wolvrix::lib::transform
{

    namespace
    {
        using PortNameCache = std::unordered_map<std::string, std::unordered_map<std::string, std::string>>;

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

        std::string sanitizePath(std::string_view path)
        {
            std::string out;
            out.reserve(path.size());
            for (char ch : path)
            {
                if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                    (ch >= '0' && ch <= '9') || ch == '_')
                {
                    out.push_back(ch);
                }
                else
                {
                    out.push_back('_');
                }
            }
            if (out.empty())
            {
                out = "xmr";
            }
            return out;
        }

        std::string hashPath(std::string_view path)
        {
            const std::size_t hash = std::hash<std::string_view>{}(path);
            std::ostringstream oss;
            oss << std::hex << hash;
            return oss.str();
        }

        std::string makeUniqueSymbol(wolvrix::lib::grh::Graph &graph, std::string base)
        {
            std::string candidate = base;
            int suffix = 0;
            while (graph.symbols().contains(candidate))
            {
                candidate = base + "_" + std::to_string(++suffix);
            }
            return candidate;
        }

        std::string getPortName(wolvrix::lib::grh::Graph &graph, PortNameCache &cache,
                                std::string_view path, std::string_view prefix)
        {
            auto &graphMap = cache[graph.symbol()];
            auto it = graphMap.find(std::string(path));
            if (it != graphMap.end())
            {
                return it->second;
            }
            std::string base = "__";
            base.append(prefix);
            base.push_back('_');
            std::string sanitized = sanitizePath(path);
            if (sanitized.size() > 64)
            {
                sanitized = hashPath(path);
            }
            base.append(sanitized);
            std::string unique = makeUniqueSymbol(graph, base);
            graphMap.emplace(std::string(path), unique);
            return unique;
        }

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

        std::optional<int64_t> getAttrInt(const wolvrix::lib::grh::Operation &op,
                                          std::string_view key)
        {
            auto attr = op.attr(key);
            if (!attr)
            {
                return std::nullopt;
            }
            if (auto value = std::get_if<int64_t>(&*attr))
            {
                return *value;
            }
            return std::nullopt;
        }

        std::optional<bool> getAttrBool(const wolvrix::lib::grh::Operation &op,
                                        std::string_view key)
        {
            auto attr = op.attr(key);
            if (!attr)
            {
                return std::nullopt;
            }
            if (auto value = std::get_if<bool>(&*attr))
            {
                return *value;
            }
            return std::nullopt;
        }

        std::vector<std::string> getAttrStrings(const wolvrix::lib::grh::Operation &op,
                                                std::string_view key)
        {
            auto attr = op.attr(key);
            if (!attr)
            {
                return {};
            }
            if (auto values = std::get_if<std::vector<std::string>>(&*attr))
            {
                return *values;
            }
            return {};
        }

        std::optional<std::vector<std::string>> getAttrStringsOptional(const wolvrix::lib::grh::Operation &op,
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

        wolvrix::lib::grh::OperationId findInstanceOp(const wolvrix::lib::grh::Graph &graph,
                                            std::string_view instanceName)
        {
            for (const auto opId : graph.operations())
            {
                const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
                if (op.kind() != wolvrix::lib::grh::OperationKind::kInstance &&
                    op.kind() != wolvrix::lib::grh::OperationKind::kBlackbox)
                {
                    continue;
                }
                auto name = getAttrString(op, "instanceName");
                if (name && *name == instanceName)
                {
                    return opId;
                }
            }
            return wolvrix::lib::grh::OperationId::invalid();
        }

        int32_t normalizeWidth(int32_t width)
        {
            return width > 0 ? width : 1;
        }

    } // namespace

    XmrResolvePass::XmrResolvePass()
        : Pass("xmr-resolve", "xmr-resolve",
               "Resolve hierarchical references by adding ports and instance connections")
    {
    }

    PassResult XmrResolvePass::run()
    {
        PassResult result;
        const std::size_t graphCount = design().graphs().size();
        logDebug("begin graphs=" + std::to_string(graphCount));
        PortNameCache readPortNames;
        PortNameCache writePortNames;
        std::size_t xmrOpCount = 0;
        std::size_t xmrReadCount = 0;
        std::size_t xmrWriteCount = 0;
        struct PendingPort
        {
            std::string moduleName;
            std::string portName;
            int32_t width = 0;
            bool isSigned = false;
        };
        std::vector<PendingPort> pendingOutputPorts;
        std::vector<PendingPort> pendingInputPorts;
        std::unordered_map<std::string, std::unordered_map<std::string, wolvrix::lib::grh::ValueId>> inputPadCache;
        auto makeInternalValueSymbol = [&](wolvrix::lib::grh::Graph &graph)
        {
            return graph.makeInternalValSym();
        };

        auto makeInternalOpSymbol = [&](wolvrix::lib::grh::Graph &graph)
        {
            return graph.makeInternalOpSym();
        };

        auto makeLoc = [&](std::string_view note) {
            return makeTransformSrcLoc("xmr-resolve", note);
        };

        auto createValue = [&](wolvrix::lib::grh::Graph &graph,
                               wolvrix::lib::grh::SymbolId sym,
                               int32_t width,
                               bool isSigned,
                               wolvrix::lib::grh::ValueType type,
                               std::string_view note) -> wolvrix::lib::grh::ValueId {
            wolvrix::lib::grh::ValueId value = graph.createValue(sym, width, isSigned, type);
            graph.setValueSrcLoc(value, makeLoc(note));
            return value;
        };

        auto createOp = [&](wolvrix::lib::grh::Graph &graph,
                            wolvrix::lib::grh::OperationKind kind,
                            wolvrix::lib::grh::SymbolId sym,
                            std::string_view note) -> wolvrix::lib::grh::OperationId {
            wolvrix::lib::grh::OperationId op = graph.createOperation(kind, sym);
            graph.setOpSrcLoc(op, makeLoc(note));
            return op;
        };

        enum class StorageKind
        {
            Register,
            Latch,
            Memory
        };

        struct StorageInfo
        {
            StorageKind kind{};
            wolvrix::lib::grh::OperationId opId = wolvrix::lib::grh::OperationId::invalid();
            int32_t width = 0;
            bool isSigned = false;
        };

        auto getPadInput = [&](wolvrix::lib::grh::Graph &graph, int32_t width, bool isSigned) -> wolvrix::lib::grh::ValueId {
            const int32_t normalized = normalizeWidth(width);
            const std::string key = std::to_string(normalized) + (isSigned ? "s" : "u");
            auto &graphCache = inputPadCache[graph.symbol()];
            auto it = graphCache.find(key);
            if (it != graphCache.end())
            {
                return it->second;
            }
            wolvrix::lib::grh::SymbolId sym = makeInternalValueSymbol(graph);
            wolvrix::lib::grh::ValueId value =
                createValue(graph, sym, normalized, isSigned, wolvrix::lib::grh::ValueType::Logic,
                            "pad_in");
            wolvrix::lib::grh::SymbolId opSym = makeInternalOpSymbol(graph);
            wolvrix::lib::grh::OperationId op =
                createOp(graph, wolvrix::lib::grh::OperationKind::kConstant, opSym, "pad_in_const");
            graph.addResult(op, value);
            graph.setAttr(op, "constValue", std::to_string(normalized) + "'b0");
            graphCache.emplace(key, value);
            result.changed = true;
            return value;
        };

        auto findStorageInfo = [&](const wolvrix::lib::grh::Graph &contextGraph,
                                   wolvrix::lib::grh::Graph &graph,
                                   std::string_view symbol,
                                   const wolvrix::lib::grh::Operation &contextOp) -> std::optional<StorageInfo> {
            const wolvrix::lib::grh::OperationId opId = graph.findOperation(symbol);
            if (!opId.valid())
            {
                return std::nullopt;
            }
            const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
            StorageInfo info;
            if (op.kind() == wolvrix::lib::grh::OperationKind::kRegister)
            {
                info.kind = StorageKind::Register;
            }
            else if (op.kind() == wolvrix::lib::grh::OperationKind::kLatch)
            {
                info.kind = StorageKind::Latch;
            }
            else if (op.kind() == wolvrix::lib::grh::OperationKind::kMemory)
            {
                info.kind = StorageKind::Memory;
            }
            else
            {
                return std::nullopt;
            }
            auto widthAttr = getAttrInt(op, "width");
            auto signedAttr = getAttrBool(op, "isSigned");
            if (!widthAttr || !signedAttr)
            {
                error(contextGraph, contextOp, "XMR target storage missing width/isSigned");
                return std::nullopt;
            }
            info.opId = opId;
            info.width = normalizeWidth(static_cast<int32_t>(*widthAttr));
            info.isSigned = *signedAttr;
            return info;
        };

        // Path segments that match no instance op may name generate scopes.
        // Ingest names generate-scope signals "<block>$<round>$...$<name>"
        // inside the enclosing graph, so the remaining segments compose the
        // leaf symbol text directly ("gen_loop[3]" -> "gen_loop$3").
        auto composeGenerateLeafName = [](const std::vector<std::string> &segments,
                                          std::size_t first) -> std::string
        {
            std::string out;
            for (std::size_t k = first; k < segments.size(); ++k)
            {
                if (!out.empty())
                {
                    out.push_back('$');
                }
                const std::string_view seg = segments[k];
                const std::size_t open = seg.find('[');
                if (open != std::string_view::npos && !seg.empty() && seg.back() == ']')
                {
                    out.append(seg.substr(0, open));
                    out.push_back('$');
                    out.append(seg.substr(open + 1, seg.size() - open - 2));
                    continue;
                }
                out.append(seg);
            }
            return out;
        };

        auto findGenerateLeaf = [&](wolvrix::lib::grh::Graph &graph,
                                    const std::vector<std::string> &segments,
                                    std::size_t first) -> std::optional<std::string>
        {
            if (first >= segments.size())
            {
                return std::nullopt;
            }
            std::string candidate = composeGenerateLeafName(segments, first);
            if (candidate.empty())
            {
                return std::nullopt;
            }
            if (graph.findValue(candidate).valid() || graph.findOperation(candidate).valid())
            {
                return candidate;
            }
            return std::nullopt;
        };

        auto ensureOutputPort = [&](wolvrix::lib::grh::Graph &graph,
                                    const std::string &portName,
                                    wolvrix::lib::grh::ValueId value,
                                    bool *added) -> wolvrix::lib::grh::ValueId {
            if (added)
            {
                *added = false;
            }
            wolvrix::lib::grh::ValueId existing = graph.outputPortValue(portName);
            if (existing.valid() && existing != value)
            {
                warning(graph, "XMR output port already bound; keeping existing binding");
                return existing;
            }
            if (!existing.valid())
            {
                graph.bindOutputPort(portName, value);
                result.changed = true;
                if (added)
                {
                    *added = true;
                }
            }
            return value;
        };

        auto ensureInputPort = [&](wolvrix::lib::grh::Graph &graph,
                                   const std::string &portName,
                                   int32_t width,
                                   bool isSigned,
                                   bool *added) -> wolvrix::lib::grh::ValueId {
            if (added)
            {
                *added = false;
            }
            wolvrix::lib::grh::ValueId existing = graph.inputPortValue(portName);
            if (existing.valid())
            {
                return existing;
            }
            wolvrix::lib::grh::ValueId value = graph.findValue(portName);
            if (!value.valid())
            {
                wolvrix::lib::grh::SymbolId sym = graph.internSymbol(portName);
                if (!sym.valid())
                {
                    warning(graph, "XMR input port name already bound; using internal value symbol");
                    sym = makeInternalValueSymbol(graph);
                }
                value = createValue(graph, sym, normalizeWidth(width), isSigned,
                                    wolvrix::lib::grh::ValueType::Logic, "input_port");
            }
            graph.bindInputPort(portName, value);
            result.changed = true;
            if (added)
            {
                *added = true;
            }
            return value;
        };

        auto createStorageReadPort = [&](wolvrix::lib::grh::Graph &graph,
                                         const StorageInfo &storage,
                                         std::string_view storageName) -> wolvrix::lib::grh::ValueId {
            const wolvrix::lib::grh::OperationKind kind =
                storage.kind == StorageKind::Register
                    ? wolvrix::lib::grh::OperationKind::kRegisterReadPort
                    : wolvrix::lib::grh::OperationKind::kLatchReadPort;
            wolvrix::lib::grh::SymbolId valueSym = makeInternalValueSymbol(graph);
            wolvrix::lib::grh::ValueId value =
                createValue(graph, valueSym, storage.width, storage.isSigned,
                            wolvrix::lib::grh::ValueType::Logic, "storage_read");
            wolvrix::lib::grh::SymbolId opSym = makeInternalOpSymbol(graph);
            wolvrix::lib::grh::OperationId op =
                createOp(graph, kind, opSym, "storage_read");
            graph.addResult(op, value);
            if (storage.kind == StorageKind::Register)
            {
                graph.setAttr(op, "regSymbol", std::string(storageName));
            }
            else
            {
                graph.setAttr(op, "latchSymbol", std::string(storageName));
            }
            result.changed = true;
            return value;
        };

        auto ensureInstanceOutput = [&](wolvrix::lib::grh::Graph &graph,
                                        wolvrix::lib::grh::OperationId opId,
                                        const std::string &portName,
                                        int32_t width,
                                        bool isSigned) -> wolvrix::lib::grh::ValueId {
            const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
            auto names = getAttrStrings(op, "outputPortName");
            const auto inoutNames = getAttrStrings(op, "inoutPortName");
            const auto resultsSpan = op.results();
            const std::size_t limit = std::min(names.size(), resultsSpan.size());
            for (std::size_t i = 0; i < limit; ++i)
            {
                if (names[i] == portName)
                {
                    return resultsSpan[i];
                }
            }
            wolvrix::lib::grh::SymbolId sym = makeInternalValueSymbol(graph);
            wolvrix::lib::grh::ValueId value =
                createValue(graph, sym, normalizeWidth(width), isSigned,
                            wolvrix::lib::grh::ValueType::Logic, "instance_out");
            const std::size_t inoutCount = inoutNames.size();
            const std::size_t resultCount = resultsSpan.size();
            const std::size_t outputLimit =
                resultCount > inoutCount * 2 ? resultCount - inoutCount * 2 : 0U;
            const std::size_t insertIndex = std::min(names.size(), outputLimit);
            graph.insertResult(opId, insertIndex, value);
            names.push_back(portName);
            graph.setAttr(opId, "outputPortName", names);
            result.changed = true;
            return value;
        };

        auto ensureInstanceInput = [&](wolvrix::lib::grh::Graph &graph,
                                       wolvrix::lib::grh::OperationId opId,
                                       const std::string &portName,
                                       wolvrix::lib::grh::ValueId value) -> wolvrix::lib::grh::ValueId {
            const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
            auto names = getAttrStrings(op, "inputPortName");
            const auto inoutNames = getAttrStrings(op, "inoutPortName");
            const auto operandsSpan = op.operands();
            const std::size_t limit = std::min(names.size(), operandsSpan.size());
            for (std::size_t i = 0; i < limit; ++i)
            {
                if (names[i] == portName)
                {
                    if (operandsSpan[i] != value)
                    {
                        warning(graph, op, "XMR input port already connected; keeping existing operand");
                    }
                    return operandsSpan[i];
                }
            }
            const std::size_t inoutCount = inoutNames.size();
            const std::size_t operandCount = operandsSpan.size();
            const std::size_t inputLimit =
                operandCount > inoutCount ? operandCount - inoutCount : 0U;
            const std::size_t insertIndex = std::min(names.size(), inputLimit);
            graph.insertOperand(opId, insertIndex, value);
            names.push_back(portName);
            graph.setAttr(opId, "inputPortName", names);
            result.changed = true;
            return value;
        };

        auto forceSingleDriver = [&](wolvrix::lib::grh::Graph &graph,
                                     wolvrix::lib::grh::ValueId target,
                                     wolvrix::lib::grh::OperationId contextOp,
                                     std::string_view path) -> std::optional<wolvrix::lib::grh::ValueId> {
            if (!target.valid())
            {
                return std::nullopt;
            }
            const wolvrix::lib::grh::Value value = graph.getValue(target);
            if (value.isInout())
            {
                warning(graph, graph.getOperation(contextOp),
                        "XMR write to inout is not supported: " + std::string(path));
                return std::nullopt;
            }
            const std::string symbolText(value.symbolText());
            if (symbolText.empty())
            {
                warning(graph, graph.getOperation(contextOp),
                        "XMR write target missing symbol: " + std::string(path));
                return std::nullopt;
            }
            if (value.definingOp().valid())
            {
                warning(graph, graph.getOperation(contextOp),
                        "XMR write replaces existing driver for " + symbolText);
            }
            if (value.isInput())
            {
                warning(graph, graph.getOperation(contextOp),
                        "XMR write overrides input; leaving input port unconnected for " +
                            symbolText);
                wolvrix::lib::grh::SymbolId sym = makeInternalValueSymbol(graph);
                wolvrix::lib::grh::ValueId replacement =
                    createValue(graph, sym, normalizeWidth(value.width()), value.isSigned(),
                                value.type(), "override_input");
                graph.replaceAllUses(target, replacement);
                result.changed = true;
                return replacement;
            }
            std::vector<std::string> outputNames;
            if (value.isOutput())
            {
                for (const auto &port : graph.outputPorts())
                {
                    if (port.value == target)
                    {
                        outputNames.push_back(port.name);
                    }
                }
            }
            const wolvrix::lib::grh::SymbolId originalSym = value.symbol();
            std::string rebindPurpose = "rebind_";
            rebindPurpose.append(symbolText);
            wolvrix::lib::grh::SymbolId tempSym = makeInternalValueSymbol(graph);
            graph.setValueSymbol(target, tempSym);
            wolvrix::lib::grh::SymbolId sym = originalSym;
            wolvrix::lib::grh::ValueId replacement =
                createValue(graph, sym, normalizeWidth(value.width()), value.isSigned(),
                            value.type(), "rebind_output");
            graph.replaceAllUses(target, replacement);
            for (const auto &name : outputNames)
            {
                graph.bindOutputPort(name, replacement);
            }
            result.changed = true;
            return replacement;
        };

        auto resolveRead = [&](wolvrix::lib::grh::Graph &root,
                               wolvrix::lib::grh::OperationId opId,
                               const std::string &path) -> std::optional<wolvrix::lib::grh::ValueId> {
            const wolvrix::lib::grh::Operation contextOp = root.getOperation(opId);
            auto segments = splitPath(path);
            if (segments.empty())
            {
                warning(root, root.getOperation(opId), "XMR read has empty path");
                return std::nullopt;
            }
            if (!segments.empty() && segments.front() == root.symbol())
            {
                segments.erase(segments.begin());
                if (segments.empty())
                {
                    warning(root, root.getOperation(opId), "XMR read has empty path after trimming root");
                    return std::nullopt;
                }
            }
            if (segments.size() == 1)
            {
                const std::string &leafName = segments.front();
                if (auto storage = findStorageInfo(root, root, leafName, contextOp))
                {
                    if (storage->kind == StorageKind::Memory)
                    {
                        error(root, contextOp, "XMR read to memory requires explicit address");
                        return std::nullopt;
                    }
                    return createStorageReadPort(root, *storage, leafName);
                }

                wolvrix::lib::grh::ValueId local = root.findValue(leafName);
                if (!local.valid())
                {
                    warning(root, root.getOperation(opId), "XMR read target not found in graph");
                    return std::nullopt;
                }
                return local;
            }

            struct Hop
            {
                wolvrix::lib::grh::Graph *parent = nullptr;
                wolvrix::lib::grh::OperationId instOp = wolvrix::lib::grh::OperationId::invalid();
                wolvrix::lib::grh::Graph *child = nullptr;
            };
            std::vector<Hop> hops;
            wolvrix::lib::grh::Graph *current = &root;
            std::optional<std::string> generateLeafName;
            for (std::size_t i = 0; i + 1 < segments.size(); ++i)
            {
                const std::string &instName = segments[i];
                wolvrix::lib::grh::OperationId instOp = findInstanceOp(*current, instName);
                if (!instOp.valid())
                {
                    generateLeafName = findGenerateLeaf(*current, segments, i);
                    if (generateLeafName)
                    {
                        break;
                    }
                    warning(root, root.getOperation(opId),
                            "XMR read instance not found: " + instName);
                    return std::nullopt;
                }
                const wolvrix::lib::grh::Operation op = current->getOperation(instOp);
                auto moduleName = getAttrString(op, "moduleName");
                if (!moduleName || moduleName->empty())
                {
                    warning(root, root.getOperation(opId),
                            "XMR read instance missing moduleName");
                    return std::nullopt;
                }
                wolvrix::lib::grh::Graph *childGraph = design().findGraph(*moduleName);
                if (!childGraph)
                {
                    warning(root, root.getOperation(opId),
                            "XMR read module not found: " + *moduleName);
                    return std::nullopt;
                }
                hops.push_back(Hop{current, instOp, childGraph});
                current = childGraph;
            }

            wolvrix::lib::grh::Graph *leafGraph = current;
            const std::string leafName =
                generateLeafName ? *generateLeafName : segments.back();
            wolvrix::lib::grh::ValueId propagated = wolvrix::lib::grh::ValueId::invalid();
            if (auto storage = findStorageInfo(root, *leafGraph, leafName, contextOp))
            {
                if (storage->kind == StorageKind::Memory)
                {
                    error(root, contextOp, "XMR read to memory requires explicit address");
                    return std::nullopt;
                }
                const std::string portName = getPortName(*leafGraph, readPortNames, path, "xmr_r");
                propagated = leafGraph->outputPortValue(portName);
                if (!propagated.valid())
                {
                    propagated = createStorageReadPort(*leafGraph, *storage, leafName);
                }
            }
            else
            {
                wolvrix::lib::grh::ValueId leafValue = leafGraph->findValue(leafName);
                if (!leafValue.valid())
                {
                    warning(root, root.getOperation(opId),
                            "XMR read target not found: " + leafName);
                    return std::nullopt;
                }
                propagated = leafValue;
            }
            for (std::size_t i = hops.size(); i-- > 0;)
            {
                wolvrix::lib::grh::Graph *childGraph = hops[i].child;
                wolvrix::lib::grh::Graph *parentGraph = hops[i].parent;
                const wolvrix::lib::grh::OperationId instOp = hops[i].instOp;
                const std::string portName = getPortName(*childGraph, readPortNames, path, "xmr_r");

                bool newPort = false;
                propagated = ensureOutputPort(*childGraph, portName, propagated, &newPort);

                const wolvrix::lib::grh::Value childValue = childGraph->getValue(propagated);
                if (newPort)
                {
                    pendingOutputPorts.push_back(
                        PendingPort{childGraph->symbol(), portName,
                                    childValue.width(), childValue.isSigned()});
                }
                wolvrix::lib::grh::ValueId parentValue =
                    ensureInstanceOutput(*parentGraph, instOp, portName,
                                         childValue.width(), childValue.isSigned());
                propagated = parentValue;
            }

            return propagated;
        };

        auto resolveWrite = [&](wolvrix::lib::grh::Graph &root,
                                wolvrix::lib::grh::OperationId opId,
                                const std::string &path,
                                const std::vector<wolvrix::lib::grh::ValueId> &operands,
                                const std::optional<std::vector<std::string>> &eventEdgesOpt) -> bool {
            const wolvrix::lib::grh::Operation contextOp = root.getOperation(opId);
            if (operands.empty())
            {
                error(root, contextOp, "XMR write missing operands");
                return false;
            }

            auto segments = splitPath(path);
            if (segments.empty())
            {
                warning(root, root.getOperation(opId), "XMR write has empty path");
                return false;
            }
            if (!segments.empty() && segments.front() == root.symbol())
            {
                segments.erase(segments.begin());
                if (segments.empty())
                {
                    warning(root, root.getOperation(opId), "XMR write has empty path after trimming root");
                    return false;
                }
            }

            struct Hop
            {
                wolvrix::lib::grh::Graph *parent = nullptr;
                wolvrix::lib::grh::OperationId instOp = wolvrix::lib::grh::OperationId::invalid();
                wolvrix::lib::grh::Graph *child = nullptr;
            };
            std::vector<Hop> hops;
            wolvrix::lib::grh::Graph *current = &root;
            std::optional<std::string> generateLeafName;
            for (std::size_t i = 0; i + 1 < segments.size(); ++i)
            {
                const std::string &instName = segments[i];
                wolvrix::lib::grh::OperationId instOp = findInstanceOp(*current, instName);
                if (!instOp.valid())
                {
                    generateLeafName = findGenerateLeaf(*current, segments, i);
                    if (generateLeafName)
                    {
                        break;
                    }
                    warning(root, root.getOperation(opId),
                            "XMR write instance not found: " + instName);
                    return false;
                }
                const wolvrix::lib::grh::Operation op = current->getOperation(instOp);
                auto moduleName = getAttrString(op, "moduleName");
                if (!moduleName || moduleName->empty())
                {
                    warning(root, root.getOperation(opId),
                            "XMR write instance missing moduleName");
                    return false;
                }
                wolvrix::lib::grh::Graph *childGraph = design().findGraph(*moduleName);
                if (!childGraph)
                {
                    warning(root, root.getOperation(opId),
                            "XMR write module not found: " + *moduleName);
                    return false;
                }
                hops.push_back(Hop{current, instOp, childGraph});
                current = childGraph;
            }

            wolvrix::lib::grh::Graph *leafGraph = current;
            const std::string leafName =
                generateLeafName ? *generateLeafName : segments.back();
            const std::optional<StorageInfo> storage =
                findStorageInfo(root, *leafGraph, leafName, contextOp);

            std::vector<std::string> labels;
            std::vector<wolvrix::lib::grh::ValueId> drivers;
            if (storage)
            {
                if (storage->kind == StorageKind::Register)
                {
                    if (operands.size() < 3)
                    {
                        error(root, contextOp, "XMR write to register missing operands");
                        return false;
                    }
                    const std::size_t eventCount = operands.size() - 3;
                    if (!eventEdgesOpt || eventEdgesOpt->size() != eventCount || eventCount == 0)
                    {
                        error(root, contextOp, "XMR write to register missing eventEdge operands");
                        return false;
                    }
                    labels = {"cond", "data", "mask"};
                    for (std::size_t i = 0; i < eventCount; ++i)
                    {
                        labels.push_back("evt" + std::to_string(i));
                    }
                }
                else if (storage->kind == StorageKind::Memory)
                {
                    if (operands.size() < 4)
                    {
                        error(root, contextOp, "XMR write to memory missing operands");
                        return false;
                    }
                    const std::size_t eventCount = operands.size() - 4;
                    if (!eventEdgesOpt || eventEdgesOpt->size() != eventCount || eventCount == 0)
                    {
                        error(root, contextOp, "XMR write to memory missing eventEdge operands");
                        return false;
                    }
                    labels = {"cond", "addr", "data", "mask"};
                    for (std::size_t i = 0; i < eventCount; ++i)
                    {
                        labels.push_back("evt" + std::to_string(i));
                    }
                }
                else
                {
                    if (operands.size() != 3)
                    {
                        error(root, contextOp, "XMR write to latch expects 3 operands");
                        return false;
                    }
                    if (eventEdgesOpt && !eventEdgesOpt->empty())
                    {
                        error(root, contextOp, "XMR write to latch must not include eventEdge");
                        return false;
                    }
                    labels = {"cond", "data", "mask"};
                }
                drivers = operands;
            }
            else
            {
                if (operands.size() > 1)
                {
                    warning(root, contextOp, "XMR write has extra operands; using first");
                }
                labels.push_back("");
                drivers.push_back(operands.front());
            }

            if (labels.size() != drivers.size())
            {
                error(root, contextOp, "XMR write operand/label size mismatch");
                return false;
            }

            current = &root;
            for (const auto &hop : hops)
            {
                for (std::size_t idx = 0; idx < drivers.size(); ++idx)
                {
                    if (!drivers[idx].valid())
                    {
                        error(root, contextOp, "XMR write operand is invalid");
                        return false;
                    }
                    const wolvrix::lib::grh::Value driverValue = current->getValue(drivers[idx]);
                    const std::string &label = labels[idx];
                    const std::string pathKey =
                        label.empty() ? path : (path + ":" + label);
                    const std::string portName =
                        getPortName(*hop.child, writePortNames, pathKey, "xmr_w");

                    bool newPort = false;
                    wolvrix::lib::grh::ValueId childPort =
                        ensureInputPort(*hop.child, portName, driverValue.width(),
                                        driverValue.isSigned(), &newPort);
                    if (newPort)
                    {
                        pendingInputPorts.push_back(
                            PendingPort{hop.child->symbol(), portName,
                                        driverValue.width(), driverValue.isSigned()});
                    }
                    ensureInstanceInput(*hop.parent, hop.instOp, portName, drivers[idx]);
                    drivers[idx] = childPort;
                }
                current = hop.child;
            }

            if (storage)
            {
                if (storage->kind == StorageKind::Register)
                {
                    wolvrix::lib::grh::SymbolId opSym = makeInternalOpSymbol(*leafGraph);
                    wolvrix::lib::grh::OperationId writeOp =
                        createOp(*leafGraph, wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                 opSym, "reg_write_port");
                    for (const auto value : drivers)
                    {
                        leafGraph->addOperand(writeOp, value);
                    }
                    leafGraph->setAttr(writeOp, "regSymbol", leafName);
                    leafGraph->setAttr(writeOp, "eventEdge", *eventEdgesOpt);
                    result.changed = true;
                    return true;
                }
                if (storage->kind == StorageKind::Latch)
                {
                    wolvrix::lib::grh::SymbolId opSym = makeInternalOpSymbol(*leafGraph);
                    wolvrix::lib::grh::OperationId writeOp =
                        createOp(*leafGraph, wolvrix::lib::grh::OperationKind::kLatchWritePort,
                                 opSym, "latch_write_port");
                    for (const auto value : drivers)
                    {
                        leafGraph->addOperand(writeOp, value);
                    }
                    leafGraph->setAttr(writeOp, "latchSymbol", leafName);
                    result.changed = true;
                    return true;
                }
                wolvrix::lib::grh::SymbolId opSym = makeInternalOpSymbol(*leafGraph);
                wolvrix::lib::grh::OperationId writeOp =
                    createOp(*leafGraph, wolvrix::lib::grh::OperationKind::kMemoryWritePort,
                             opSym, "mem_write_port");
                for (const auto value : drivers)
                {
                    leafGraph->addOperand(writeOp, value);
                }
                leafGraph->setAttr(writeOp, "memSymbol", leafName);
                leafGraph->setAttr(writeOp, "eventEdge", *eventEdgesOpt);
                result.changed = true;
                return true;
            }

            wolvrix::lib::grh::ValueId target = leafGraph->findValue(leafName);
            if (!target.valid())
            {
                warning(root, root.getOperation(opId),
                        "XMR write target not found: " + leafName);
                return false;
            }
            auto replacement = forceSingleDriver(*leafGraph, target, opId, path);
            if (!replacement || !replacement->valid())
            {
                return false;
            }
            wolvrix::lib::grh::SymbolId opSym = makeInternalOpSymbol(*leafGraph);
            wolvrix::lib::grh::OperationId assign =
                createOp(*leafGraph, wolvrix::lib::grh::OperationKind::kAssign,
                         opSym, "assign_write");
            leafGraph->addOperand(assign, drivers.front());
            leafGraph->addResult(assign, *replacement);
            result.changed = true;
            return true;
        };

        for (auto &graphEntry : design().graphs())
        {
            wolvrix::lib::grh::Graph &graph = *graphEntry.second;
            std::vector<wolvrix::lib::grh::OperationId> xmrOps;
            for (const auto opId : graph.operations())
            {
                const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
                if (op.kind() == wolvrix::lib::grh::OperationKind::kXMRRead ||
                    op.kind() == wolvrix::lib::grh::OperationKind::kXMRWrite)
                {
                    xmrOps.push_back(opId);
                    ++xmrOpCount;
                    if (op.kind() == wolvrix::lib::grh::OperationKind::kXMRRead)
                    {
                        ++xmrReadCount;
                    }
                    else
                    {
                        ++xmrWriteCount;
                    }
                }
            }

            for (const auto opId : xmrOps)
            {
                const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
                auto path = getAttrString(op, "xmrPath");
                {
                    std::string message = "xmr ";
                    message.append(op.kind() == wolvrix::lib::grh::OperationKind::kXMRRead ? "read" : "write");
                    message.append(" graph=");
                    message.append(graph.symbol());
                    message.append(" path=");
                    if (path)
                    {
                        message.append(*path);
                    }
                    else
                    {
                        message.append("<missing>");
                    }
                    log(LogLevel::Trace, std::move(message));
                }
                if (!path)
                {
                    warning(graph, op, "XMR op missing xmrPath attribute");
                    continue;
                }
                if (op.kind() == wolvrix::lib::grh::OperationKind::kXMRRead)
                {
                    const auto results = op.results();
                    if (results.empty())
                    {
                        warning(graph, op, "XMR read missing result");
                        graph.eraseOp(opId);
                        result.changed = true;
                        continue;
                    }
                    log(LogLevel::Trace, "xmr read resolve begin graph=" + graph.symbol());
                    auto replacement = resolveRead(graph, opId, *path);
                    log(LogLevel::Trace, "xmr read resolve end graph=" + graph.symbol());
                    if (!replacement || !replacement->valid())
                    {
                        continue;
                    }
                    graph.replaceAllUses(results.front(), *replacement);
                    graph.eraseOp(opId);
                    result.changed = true;
                    continue;
                }

                if (op.kind() == wolvrix::lib::grh::OperationKind::kXMRWrite)
                {
                    const auto operandsSpan = op.operands();
                    std::vector<wolvrix::lib::grh::ValueId> operands(operandsSpan.begin(), operandsSpan.end());
                    const auto eventEdgesOpt = getAttrStringsOptional(op, "eventEdge");
                    logDebug("xmr write resolve begin graph=" + graph.symbol());
                    const bool resolved = resolveWrite(graph, opId, *path, operands, eventEdgesOpt);
                    logDebug("xmr write resolve end graph=" + graph.symbol());
                    if (resolved)
                    {
                        graph.eraseOp(opId);
                        result.changed = true;
                    }
                }
            }
        }

        if (!pendingOutputPorts.empty() || !pendingInputPorts.empty())
        {
            auto instanceHasInput = [&](const wolvrix::lib::grh::Operation &op,
                                        std::string_view portName) -> bool {
                const auto names = getAttrStrings(op, "inputPortName");
                for (const auto &name : names)
                {
                    if (name == portName)
                    {
                        return true;
                    }
                }
                return false;
            };

            struct InstanceRef
            {
                wolvrix::lib::grh::Graph *graph = nullptr;
                wolvrix::lib::grh::OperationId opId = wolvrix::lib::grh::OperationId::invalid();
            };

            std::unordered_map<std::string, std::vector<InstanceRef>> instancesByModule;
            for (auto &graphEntry : design().graphs())
            {
                wolvrix::lib::grh::Graph &graph = *graphEntry.second;
                for (const auto opId : graph.operations())
                {
                    const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
                    if (op.kind() != wolvrix::lib::grh::OperationKind::kInstance &&
                        op.kind() != wolvrix::lib::grh::OperationKind::kBlackbox)
                    {
                        continue;
                    }
                    const auto moduleName = getAttrString(op, "moduleName");
                    if (!moduleName || moduleName->empty())
                    {
                        continue;
                    }
                    instancesByModule[*moduleName].push_back(InstanceRef{&graph, opId});
                }
            }

            std::unordered_map<std::string, std::vector<const PendingPort *>> pendingOutputsByModule;
            pendingOutputsByModule.reserve(pendingOutputPorts.size());
            for (const auto &pending : pendingOutputPorts)
            {
                pendingOutputsByModule[pending.moduleName].push_back(&pending);
            }

            std::unordered_map<std::string, std::vector<const PendingPort *>> pendingInputsByModule;
            pendingInputsByModule.reserve(pendingInputPorts.size());
            for (const auto &pending : pendingInputPorts)
            {
                pendingInputsByModule[pending.moduleName].push_back(&pending);
            }

            logDebug("xmr pending ports begin outputs=" + std::to_string(pendingOutputPorts.size()) +
                    " inputs=" + std::to_string(pendingInputPorts.size()) +
                    " outputModules=" + std::to_string(pendingOutputsByModule.size()) +
                    " inputModules=" + std::to_string(pendingInputsByModule.size()) +
                    " instanceModules=" + std::to_string(instancesByModule.size()));

            std::size_t moduleIndex = 0;
            for (const auto &entry : pendingOutputsByModule)
            {
                if (moduleIndex % 10 == 0)
                {
                    logDebug("xmr pending output module progress " + std::to_string(moduleIndex) +
                            "/" + std::to_string(pendingOutputsByModule.size()));
                }
                auto instIt = instancesByModule.find(entry.first);
                if (instIt == instancesByModule.end())
                {
                    ++moduleIndex;
                    continue;
                }
                const auto &instances = instIt->second;
                const auto &pendingPorts = entry.second;
                for (const auto &inst : instances)
                {
                    for (const PendingPort *pending : pendingPorts)
                    {
                        ensureInstanceOutput(*inst.graph, inst.opId, pending->portName,
                                             pending->width, pending->isSigned);
                    }
                }
                ++moduleIndex;
            }

            moduleIndex = 0;
            for (const auto &entry : pendingInputsByModule)
            {
                if (moduleIndex % 10 == 0)
                {
                    logDebug("xmr pending input module progress " + std::to_string(moduleIndex) +
                            "/" + std::to_string(pendingInputsByModule.size()));
                }
                auto instIt = instancesByModule.find(entry.first);
                if (instIt == instancesByModule.end())
                {
                    ++moduleIndex;
                    continue;
                }
                const auto &instances = instIt->second;
                const auto &pendingPorts = entry.second;
                for (const auto &inst : instances)
                {
                    for (const PendingPort *pending : pendingPorts)
                    {
                        const wolvrix::lib::grh::Operation op = inst.graph->getOperation(inst.opId);
                        if (instanceHasInput(op, pending->portName))
                        {
                            continue;
                        }
                        wolvrix::lib::grh::ValueId padValue =
                            getPadInput(*inst.graph, pending->width, pending->isSigned);
                        ensureInstanceInput(*inst.graph, inst.opId, pending->portName, padValue);
                    }
                }
                ++moduleIndex;
            }
            logDebug("xmr pending ports end");
        }

        std::string message = "xmr ops=" + std::to_string(xmrOpCount);
        message.append(", reads=");
        message.append(std::to_string(xmrReadCount));
        message.append(", writes=");
        message.append(std::to_string(xmrWriteCount));
        message.append(", newInputPorts=");
        message.append(std::to_string(pendingInputPorts.size()));
        message.append(", newOutputPorts=");
        message.append(std::to_string(pendingOutputPorts.size()));
        message.append(result.changed ? ", changed=true" : ", changed=false");
        logDebug(std::move(message));
        return result;
    }

} // namespace wolvrix::lib::transform
