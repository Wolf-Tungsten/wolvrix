#include "transform/memory_read_retime.hpp"
#include "ingest/node_attributes.hpp"

#include "core/grh.hpp"

#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wolvrix::lib::transform
{

    namespace
    {
        using wolvrix::lib::grh::OperationId;
        using wolvrix::lib::grh::OperationKind;
        using wolvrix::lib::grh::SrcLoc;
        using wolvrix::lib::grh::SymbolId;
        using wolvrix::lib::grh::ValueId;
        using wolvrix::lib::grh::ValueType;

        std::optional<wolvrix::lib::grh::AttrValueView> findAttr(
            const wolvrix::lib::grh::Graph &graph, OperationId op, std::string_view key)
        {
            for (const auto &attr : graph.opAttrRange(op))
                if (attr.key() == key) return attr.value();
            return std::nullopt;
        }

        template <typename T>
        std::optional<wolvrix::lib::grh::AttrValueView::Borrowed<T>> getAttr(const wolvrix::lib::grh::Graph &graph, OperationId op, std::string_view key)
        {
            const auto attr = findAttr(graph, op, key);
            return attr ? attr->get<T>() : std::nullopt;
        }

        std::optional<slang::SVInt> parseConstLiteral(std::string_view literal)
        {
            std::string compact;
            compact.reserve(literal.size());
            for (char ch : literal)
            {
                if (ch == '_' || std::isspace(static_cast<unsigned char>(ch)))
                {
                    continue;
                }
                compact.push_back(ch);
            }
            if (compact.empty())
            {
                return std::nullopt;
            }

            bool negative = false;
            if (compact.front() == '-' || compact.front() == '+')
            {
                negative = compact.front() == '-';
                compact.erase(compact.begin());
            }
            if (compact.empty())
            {
                return std::nullopt;
            }

            try
            {
                slang::SVInt parsed = slang::SVInt::fromString(compact);
                if (negative)
                {
                    parsed = -parsed;
                }
                return parsed;
            }
            catch (const std::exception &)
            {
                return std::nullopt;
            }
        }

        std::optional<slang::SVInt> getConstValue(const wolvrix::lib::grh::Graph &graph,
                                                  ValueId valueId)
        {
            if (!valueId.valid())
            {
                return std::nullopt;
            }
            const OperationId def = graph.valueDef(valueId);
            if (!def.valid())
            {
                return std::nullopt;
            }
            if (graph.opKind(def) != OperationKind::kConstant)
            {
                return std::nullopt;
            }
            const auto literal = getAttr<std::string>(graph, def, "constValue");
            if (!literal)
            {
                return std::nullopt;
            }
            return parseConstLiteral(*literal);
        }

        std::string formatLiteral(const slang::SVInt &value)
        {
            return value.toString(slang::SVInt::MAX_BITS);
        }

        std::string makeZeroLiteral(int32_t width)
        {
            const int32_t normalizedWidth = width > 0 ? width : 1;
            return std::to_string(normalizedWidth) + "'d0";
        }

        std::string makeIntLiteral(int32_t width, int64_t value)
        {
            const int32_t normalizedWidth = width > 0 ? width : 1;
            return std::to_string(normalizedWidth) + "'d" + std::to_string(value);
        }

        ValueId createConstantValue(wolvrix::lib::grh::Graph &graph,
                                    int32_t width,
                                    bool isSigned,
                                    ValueType type,
                                    std::string literal,
                                    std::string_view note)
        {
            const SymbolId valueSym = graph.makeInternalValSym();
            const SymbolId opSym = graph.makeInternalOpSym();
            const ValueId value = graph.createValue(valueSym, width > 0 ? width : 1, isSigned, type);
            const OperationId op = graph.createOperation(OperationKind::kConstant, opSym);
            graph.addResult(op, value);
            graph.setAttr(op, "constValue", std::move(literal));
            const SrcLoc srcLoc = makeTransformSrcLoc("memory-read-retime", note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(value, srcLoc);
            return value;
        }

        ValueId createBinaryOp(wolvrix::lib::grh::Graph &graph,
                               OperationKind kind,
                               ValueId lhs,
                               ValueId rhs,
                               int32_t outWidth,
                               bool outSigned,
                               ValueType outType,
                               std::string_view note)
        {
            const SymbolId valueSym = graph.makeInternalValSym();
            const SymbolId opSym = graph.makeInternalOpSym();
            const ValueId out = graph.createValue(valueSym,
                                                  outWidth > 0 ? outWidth : 1,
                                                  outSigned,
                                                  outType);
            const OperationId op = graph.createOperation(kind, opSym);
            graph.addOperand(op, lhs);
            graph.addOperand(op, rhs);
            graph.addResult(op, out);
            const SrcLoc srcLoc = makeTransformSrcLoc("memory-read-retime", note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        ValueId createMux(wolvrix::lib::grh::Graph &graph,
                          ValueId sel,
                          ValueId whenTrue,
                          ValueId whenFalse,
                          int32_t outWidth,
                          bool outSigned,
                          ValueType outType,
                          std::string_view note)
        {
            const SymbolId valueSym = graph.makeInternalValSym();
            const SymbolId opSym = graph.makeInternalOpSym();
            const ValueId out = graph.createValue(valueSym,
                                                  outWidth > 0 ? outWidth : 1,
                                                  outSigned,
                                                  outType);
            const OperationId op = graph.createOperation(OperationKind::kMux, opSym);
            graph.addOperand(op, sel);
            graph.addOperand(op, whenTrue);
            graph.addOperand(op, whenFalse);
            graph.addResult(op, out);
            const SrcLoc srcLoc = makeTransformSrcLoc("memory-read-retime", note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        ValueId createRegisterReadPort(wolvrix::lib::grh::Graph &graph,
                                       std::string_view regSymbol,
                                       int32_t width,
                                       bool isSigned,
                                       ValueType type,
                                       std::string_view note)
        {
            const SymbolId valueSym = graph.makeInternalValSym();
            const SymbolId opSym = graph.makeInternalOpSym();
            const ValueId out = graph.createValue(valueSym, width > 0 ? width : 1, isSigned, type);
            const OperationId op = graph.createOperation(OperationKind::kRegisterReadPort, opSym);
            graph.setAttr(op, "regSymbol", std::string(regSymbol));
            graph.addResult(op, out);
            const SrcLoc srcLoc = makeTransformSrcLoc("memory-read-retime", note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        ValueId createMemoryReadPort(wolvrix::lib::grh::Graph &graph,
                                     std::string_view memSymbol,
                                     ValueId addr,
                                     int32_t width,
                                     bool isSigned,
                                     ValueType type,
                                     std::string_view note)
        {
            const SymbolId valueSym = graph.makeInternalValSym();
            const SymbolId opSym = graph.makeInternalOpSym();
            const ValueId out = graph.createValue(valueSym, width > 0 ? width : 1, isSigned, type);
            const OperationId op = graph.createOperation(OperationKind::kRegisterReadArrayPort, opSym);
            graph.addOperand(op, addr);
            graph.setAttr(op, "regSymbol", std::string(memSymbol));
            graph.addResult(op, out);
            const SrcLoc srcLoc = makeTransformSrcLoc("memory-read-retime", note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        OperationId createRegisterWritePort(wolvrix::lib::grh::Graph &graph,
                                            std::string_view regSymbol,
                                            ValueId updateCond,
                                            ValueId nextValue,
                                            std::span<const ValueId> events,
                                            const std::vector<std::string> &eventEdges,
                                            std::string_view note)
        {
            const SymbolId opSym = graph.makeInternalOpSym();
            const OperationId op = graph.createOperation(OperationKind::kRegisterWritePort, opSym);
            graph.setAttr(op, "regSymbol", std::string(regSymbol));
            graph.setAttr(op, "eventEdge", eventEdges);
            graph.assignWriterOrder(op, regSymbol);
            graph.addOperand(op, updateCond);
            graph.addOperand(op, nextValue);
            for (const ValueId event : events)
            {
                graph.addOperand(op, event);
            }
            graph.setOpSrcLoc(op, makeTransformSrcLoc("memory-read-retime", note));
            return op;
        }

        std::string makeUniqueSymbol(const wolvrix::lib::grh::Graph &graph,
                                     std::string_view base)
        {
            std::string candidate(base);
            int64_t suffix = 0;
            while (graph.findOperation(candidate).valid() || graph.findValue(candidate).valid())
            {
                ++suffix;
                candidate = std::string(base) + "$" + std::to_string(suffix);
            }
            return candidate;
        }

        bool valueEq(std::span<const ValueId> lhs, std::span<const ValueId> rhs)
        {
            if (lhs.size() != rhs.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < lhs.size(); ++i)
            {
                if (lhs[i] != rhs[i])
                {
                    return false;
                }
            }
            return true;
        }

        bool isFullMaskConst(const wolvrix::lib::grh::Graph &graph,
                             ValueId valueId,
                             int32_t width)
        {
            if (width <= 0)
            {
                return false;
            }
            auto value = getConstValue(graph, valueId);
            if (!value)
            {
                return false;
            }
            slang::SVInt resized = value->resize(static_cast<slang::bitwidth_t>(width));
            for (int32_t bit = 0; bit < width; ++bit)
            {
                if (!resized[bit])
                {
                    return false;
                }
            }
            return true;
        }

        std::optional<int64_t> extractConstIndex(const wolvrix::lib::grh::Graph &graph,
                                                 ValueId valueId,
                                                 int32_t width)
        {
            auto parsed = getConstValue(graph, valueId);
            if (!parsed)
            {
                return std::nullopt;
            }
            slang::SVInt resized = parsed->resize(static_cast<slang::bitwidth_t>(width > 0 ? width : 1));
            return resized.as<int64_t>();
        }

        bool anyInitAttrs(const wolvrix::lib::grh::Graph &graph, OperationId op)
        {
            return findAttr(graph, op, "initKind") ||
                   findAttr(graph, op, "initFile") ||
                   findAttr(graph, op, "initValue") ||
                   findAttr(graph, op, "initStart") ||
                   findAttr(graph, op, "initLen");
        }

        bool buildLiteralInitTable(const wolvrix::lib::grh::Graph &graph, OperationId memoryOp,
                                   int64_t rowCount,
                                   int32_t width,
                                   std::vector<std::optional<std::string>> &table,
                                   std::string &reason)
        {
            table.assign(static_cast<std::size_t>(rowCount), std::nullopt);
            if (!anyInitAttrs(graph, memoryOp))
            {
                reason = "memory init attrs missing";
                return false;
            }

            const auto kindsOpt = getAttr<std::vector<std::string>>(graph, memoryOp, "initKind");
            const auto filesOpt = getAttr<std::vector<std::string>>(graph, memoryOp, "initFile");
            const auto startsOpt = getAttr<std::vector<int64_t>>(graph, memoryOp, "initStart");
            const auto lensOpt = getAttr<std::vector<int64_t>>(graph, memoryOp, "initLen");
            const auto values = getAttr<std::vector<std::string>>(graph, memoryOp, "initValue");
            if (!kindsOpt || !filesOpt || !startsOpt || !lensOpt)
            {
                reason = "memory init attrs incomplete";
                return false;
            }

            const auto &kinds = *kindsOpt;
            const auto &files = *filesOpt;
            const auto &starts = *startsOpt;
            const auto &lens = *lensOpt;
            if (kinds.size() != files.size() ||
                kinds.size() != starts.size() ||
                kinds.size() != lens.size())
            {
                reason = "memory init attrs size mismatch";
                return false;
            }

            for (std::size_t i = 0; i < kinds.size(); ++i)
            {
                if (kinds[i] != "literal")
                {
                    reason = "memory init kind is not literal";
                    return false;
                }
                const std::string literalText =
                    (values && i < values->size() && !(*values)[i].empty()) ? std::string((*values)[i]) : makeZeroLiteral(width);
                auto parsed = parseConstLiteral(literalText);
                if (!parsed)
                {
                    reason = "memory init literal is not statically parseable";
                    return false;
                }
                std::string normalized = formatLiteral(parsed->resize(static_cast<slang::bitwidth_t>(width > 0 ? width : 1)));

                const int64_t start = starts[i];
                const int64_t len = lens[i];
                if (start < 0)
                {
                    for (int64_t row = 0; row < rowCount; ++row)
                    {
                        table[static_cast<std::size_t>(row)] = normalized;
                    }
                    continue;
                }
                if (len <= 0)
                {
                    reason = "literal init requires positive len";
                    return false;
                }
                const int64_t begin = std::max<int64_t>(0, start);
                const int64_t end = std::min<int64_t>(rowCount, start + len);
                for (int64_t row = begin; row < end; ++row)
                {
                    table[static_cast<std::size_t>(row)] = normalized;
                }
            }

            for (const auto &entry : table)
            {
                if (!entry.has_value())
                {
                    reason = "memory init table is incomplete";
                    return false;
                }
            }
            return true;
        }

        // Candidate matching finishes before rewrites; keys borrow the graph's
        // symbol and attribute arenas throughout that read-only phase.
        struct GraphIndex
        {
            std::unordered_map<std::string_view, OperationId> registerDecls;
            std::unordered_map<std::string_view, OperationId> memoryDecls;
            std::unordered_map<std::string_view, std::vector<OperationId>> registerReadPorts;
            std::unordered_map<std::string_view, std::vector<OperationId>> registerWritePorts;
            std::unordered_map<std::string_view, std::vector<OperationId>> memoryWritePorts;
        };

        GraphIndex buildGraphIndex(const wolvrix::lib::grh::Graph &graph)
        {
            GraphIndex index;
            for (const OperationId opId : graph.operations())
            {
                switch (graph.opKind(opId))
                {
                case OperationKind::kRegister:
                    // Array registers (shape attr) play the retired kMemory
                    // declaration role for this pass.
                    if (findAttr(graph, opId, "shape"))
                    {
                        index.memoryDecls.emplace(graph.symbolText(graph.operationSymbol(opId)), opId);
                    }
                    else
                    {
                        index.registerDecls.emplace(graph.symbolText(graph.operationSymbol(opId)), opId);
                    }
                    break;
                case OperationKind::kRegisterReadPort:
                {
                    const auto regSymbol = getAttr<std::string>(graph, opId, "regSymbol");
                    if (regSymbol)
                    {
                        index.registerReadPorts[*regSymbol].push_back(opId);
                    }
                    break;
                }
                case OperationKind::kRegisterWritePort:
                case OperationKind::kRegisterWriteMaskPort:
                {
                    const auto regSymbol = getAttr<std::string>(graph, opId, "regSymbol");
                    if (regSymbol)
                    {
                        index.registerWritePorts[*regSymbol].push_back(opId);
                    }
                    break;
                }
                case OperationKind::kRegisterWriteArrayPort:
                case OperationKind::kRegisterWriteArrayMaskPort:
                {
                    const auto regSymbol = getAttr<std::string>(graph, opId, "regSymbol");
                    if (regSymbol)
                    {
                        index.memoryWritePorts[*regSymbol].push_back(opId);
                    }
                    break;
                }
                default:
                    break;
                }
            }
            return index;
        }

        struct Candidate
        {
            enum class Mode
            {
                Rom,
                SimpleRam
            };

            Mode mode = Mode::Rom;
            OperationId memoryReadOp;
            OperationId memoryOp;
            OperationId addressReadOp;
            OperationId addressRegisterOp;
            OperationId addressWriteOp;
            OperationId memoryWriteOp;
            std::string memorySymbol;
            std::string addressRegisterSymbol;
            std::vector<std::string> eventEdges;
            std::vector<ValueId> events;
            ValueId readResult;
            ValueId addressValue;
            ValueId nextAddress;
            ValueId addressEnable;
            ValueId memoryWriteEnable;
            ValueId memoryWriteAddress;
            ValueId memoryWriteData;
            int32_t addrWidth = 0;
            int32_t dataWidth = 0;
            bool dataSigned = false;
            ValueType dataType = ValueType::Logic;
            std::string dataInitValue;
        };

        struct Stats
        {
            int64_t totalReadPorts = 0;
            int64_t candidates = 0;
            int64_t retimed = 0;
            int64_t retimedRom = 0;
            int64_t retimedSimpleRam = 0;
            int64_t skipNonRegisterAddr = 0;
            int64_t skipMultiwriteAddrReg = 0;
            int64_t skipPartialMask = 0;
            int64_t skipAddrFanout = 0;
            int64_t skipMultiwriteMemory = 0;
            int64_t skipWriteportPartialMask = 0;
            int64_t skipMismatchedEventDomain = 0;
            int64_t skipDeclaredSymbol = 0;
            int64_t skipMalformed = 0;
        };

        bool opStillAlive(const wolvrix::lib::grh::Graph &graph, OperationId opId)
        {
            const auto ops = graph.operations();
            return std::find(ops.begin(), ops.end(), opId) != ops.end();
        }

        void rebindOutputPorts(wolvrix::lib::grh::Graph &graph, ValueId from, ValueId to)
        {
            std::vector<std::string> names;
            for (const auto &port : graph.outputPorts())
            {
                if (port.value == from)
                {
                    names.push_back(port.name);
                }
            }
            for (const auto &name : names)
            {
                graph.bindOutputPort(name, to);
            }
        }

        OperationId createRegisterDecl(wolvrix::lib::grh::Graph &graph,
                                       std::string_view symbol,
                                       int32_t width,
                                       bool isSigned,
                                       std::string_view initValue,
                                       std::string_view note)
        {
            const OperationId op = graph.createOperation(OperationKind::kRegister, graph.internSymbol(symbol));
            graph.setAttr(op, "width", static_cast<int64_t>(width));
            graph.setAttr(op, "isSigned", isSigned);
            if (!initValue.empty())
            {
                graph.setAttr(op, "initValue", std::string(initValue));
            }
            graph.setOpSrcLoc(op, makeTransformSrcLoc("memory-read-retime", note));
            return op;
        }

        std::optional<int64_t> extractIndexFromLiteral(std::string_view literal, int32_t width)
        {
            auto parsed = parseConstLiteral(literal);
            if (!parsed)
            {
                return std::nullopt;
            }
            slang::SVInt resized = parsed->resize(static_cast<slang::bitwidth_t>(width > 0 ? width : 1));
            return resized.as<int64_t>();
        }

        std::optional<Candidate> matchCandidate(const wolvrix::lib::grh::Graph &graph,
                                                const GraphIndex &index,
                                                OperationId readOpId,
                                                bool keepDeclaredSymbols,
                                                Stats &stats)
        {
            const auto readOperands = graph.opOperands(readOpId);
            const auto readResults = graph.opResults(readOpId);
            if (graph.opKind(readOpId) != OperationKind::kRegisterReadArrayPort ||
                readOperands.size() != 1 || readResults.size() != 1)
            {
                ++stats.skipMalformed;
                return std::nullopt;
            }

            const ValueId addressValue = readOperands[0];
            const ValueId readResult = readResults[0];
            const OperationId addressReadId = graph.valueDef(addressValue);
            const bool outputSigned = graph.valueSigned(readResult);
            const ValueType outputType = graph.valueType(readResult);
            if (!addressReadId.valid())
            {
                ++stats.skipNonRegisterAddr;
                return std::nullopt;
            }
            if (graph.opKind(addressReadId) != OperationKind::kRegisterReadPort)
            {
                ++stats.skipNonRegisterAddr;
                return std::nullopt;
            }

            const auto addressRegSymbol = getAttr<std::string>(graph, addressReadId, "regSymbol");
            if (!addressRegSymbol)
            {
                ++stats.skipMalformed;
                return std::nullopt;
            }
            auto regDeclIt = index.registerDecls.find(*addressRegSymbol);
            if (regDeclIt == index.registerDecls.end())
            {
                ++stats.skipMalformed;
                return std::nullopt;
            }
            const OperationId addressRegOp = regDeclIt->second;
            const auto widthAttr = getAttr<int64_t>(graph, addressRegOp, "width");
            const int32_t addrWidth = static_cast<int32_t>(widthAttr ? *widthAttr : graph.valueWidth(addressValue));
            if (addrWidth <= 0)
            {
                ++stats.skipMalformed;
                return std::nullopt;
            }

            const auto regReadIt = index.registerReadPorts.find(*addressRegSymbol);
            if (regReadIt == index.registerReadPorts.end() ||
                regReadIt->second.size() != 1 ||
                regReadIt->second.front() != addressReadId ||
                graph.valueUsers(addressValue).size() != 1 ||
                graph.valueUsers(addressValue).front().operation != readOpId)
            {
                ++stats.skipAddrFanout;
                return std::nullopt;
            }
            for (const auto &port : graph.outputPorts())
            {
                if (port.value == addressValue)
                {
                    ++stats.skipAddrFanout;
                    return std::nullopt;
                }
            }

            const auto regWriteIt = index.registerWritePorts.find(*addressRegSymbol);
            if (regWriteIt == index.registerWritePorts.end() || regWriteIt->second.size() != 1)
            {
                ++stats.skipMultiwriteAddrReg;
                return std::nullopt;
            }
            const OperationId addressWriteId = regWriteIt->second.front();
            const auto addressWriteOperands = graph.opOperands(addressWriteId);
            // Only whole writes (no mask operand) keep the retime equivalence.
            if (graph.opKind(addressWriteId) != OperationKind::kRegisterWritePort ||
                addressWriteOperands.size() < 2)
            {
                ++stats.skipPartialMask;
                return std::nullopt;
            }

            const auto memorySymbol = getAttr<std::string>(graph, readOpId, "regSymbol");
            if (!memorySymbol)
            {
                ++stats.skipMalformed;
                return std::nullopt;
            }
            auto memoryDeclIt = index.memoryDecls.find(*memorySymbol);
            if (memoryDeclIt == index.memoryDecls.end())
            {
                ++stats.skipMalformed;
                return std::nullopt;
            }
            const OperationId memoryOp = memoryDeclIt->second;
            const auto memoryShape = getAttr<std::vector<int64_t>>(graph, memoryOp, "shape");
            const auto memoryWidth = getAttr<int64_t>(graph, memoryOp, "width");
            const int64_t memoryTotalWidth = memoryWidth ? *memoryWidth : 0;
            if (!memoryShape || memoryShape->size() != 1 || memoryShape->front() <= 0 || memoryTotalWidth <= 0 ||
                memoryTotalWidth % memoryShape->front() != 0)
            {
                ++stats.skipMalformed;
                return std::nullopt;
            }
            const int64_t rowCount = memoryShape->front();
            const int32_t dataWidth = static_cast<int32_t>(memoryTotalWidth / rowCount);
            const auto signedAttr = getAttr<bool>(graph, memoryOp, "isSigned");
            const bool dataSigned = signedAttr ? *signedAttr : outputSigned;
            if (dataWidth <= 0)
            {
                ++stats.skipMalformed;
                return std::nullopt;
            }

            Candidate candidate;
            candidate.memoryReadOp = readOpId;
            candidate.memoryOp = memoryDeclIt->second;
            candidate.addressReadOp = addressReadId;
            candidate.addressRegisterOp = regDeclIt->second;
            candidate.addressWriteOp = addressWriteId;
            candidate.memorySymbol = *memorySymbol;
            candidate.addressRegisterSymbol = *addressRegSymbol;
            candidate.readResult = readResult;
            candidate.addressValue = addressValue;
            candidate.nextAddress = addressWriteOperands[1];
            candidate.addressEnable = addressWriteOperands[0];
            candidate.addrWidth = addrWidth;
            candidate.dataWidth = dataWidth;
            candidate.dataSigned = dataSigned;
            candidate.dataType = outputType;

            // If both address init and memory init are statically known, preserve data reg init.
            // Otherwise leave data_reg uninitialized and do not guarantee time 0 equivalence.
            if (const auto addrInit = getAttr<std::string>(graph, addressRegOp, "initValue"))
            {
                if (auto addrIndex = extractIndexFromLiteral(*addrInit, addrWidth))
                {
                    std::vector<std::optional<std::string>> initTable;
                    std::string initReason;
                    if (*addrIndex >= 0 &&
                        *addrIndex < rowCount &&
                        buildLiteralInitTable(graph, memoryOp, rowCount, dataWidth, initTable, initReason) &&
                        initTable[static_cast<std::size_t>(*addrIndex)].has_value())
                    {
                        candidate.dataInitValue = *initTable[static_cast<std::size_t>(*addrIndex)];
                    }
                }
            }

            const auto addrEdges = getAttr<std::vector<std::string>>(graph, addressWriteId, "eventEdge");
            if (!addrEdges || addrEdges->size() != addressWriteOperands.size() - 2)
            {
                ++stats.skipMalformed;
                return std::nullopt;
            }
            candidate.eventEdges.assign(addrEdges->begin(), addrEdges->end());
            candidate.events.assign(addressWriteOperands.begin() + 2, addressWriteOperands.end());

            const auto memWriteIt = index.memoryWritePorts.find(*memorySymbol);
            const std::size_t memWriteCount = memWriteIt == index.memoryWritePorts.end() ? 0 : memWriteIt->second.size();
            if (memWriteCount == 0)
            {
                if (keepDeclaredSymbols && graph.isDeclaredSymbol(graph.operationSymbol(addressRegOp)))
                {
                    ++stats.skipDeclaredSymbol;
                    return std::nullopt;
                }
                candidate.mode = Candidate::Mode::Rom;
                return candidate;
            }
            if (memWriteCount > 1)
            {
                ++stats.skipMultiwriteMemory;
                return std::nullopt;
            }

            const OperationId memoryWriteId = memWriteIt->second.front();
            const auto memoryWriteOperands = graph.opOperands(memoryWriteId);
            const std::optional<std::size_t> memEventStart =
                wolvrix::lib::grh::firstEventOperandIndex(graph, memoryWriteId);
            // One address index is required: layouts are
            //   WriteArray:     [cond, addr, data, events...]
            //   WriteArrayMask: [cond, addr, data, mask, events...]
            if (!memEventStart)
            {
                ++stats.skipMalformed;
                return std::nullopt;
            }
            const bool memWriteHasMask =
                graph.opKind(memoryWriteId) == OperationKind::kRegisterWriteArrayMaskPort;
            if (*memEventStart != (memWriteHasMask ? 4 : 3))
            {
                ++stats.skipMalformed;
                return std::nullopt;
            }
            const auto memEdges = getAttr<std::vector<std::string>>(graph, memoryWriteId, "eventEdge");
            if (!memEdges || memEdges->size() != memoryWriteOperands.size() - *memEventStart)
            {
                ++stats.skipMalformed;
                return std::nullopt;
            }
            if (!valueEq(candidate.events,
                         memoryWriteOperands.subspan(*memEventStart)) ||
                !std::equal(memEdges->begin(), memEdges->end(), candidate.eventEdges.begin(), candidate.eventEdges.end()))
            {
                ++stats.skipMismatchedEventDomain;
                return std::nullopt;
            }
            if (memWriteHasMask &&
                !isFullMaskConst(graph, memoryWriteOperands[*memEventStart - 1], dataWidth))
            {
                ++stats.skipWriteportPartialMask;
                return std::nullopt;
            }
            if (graph.valueWidth(memoryWriteOperands[1]) != addrWidth ||
                graph.valueWidth(memoryWriteOperands[*memEventStart - (memWriteHasMask ? 2 : 1)]) != dataWidth)
            {
                ++stats.skipMalformed;
                return std::nullopt;
            }

            candidate.mode = Candidate::Mode::SimpleRam;
            candidate.memoryWriteOp = memoryWriteId;
            candidate.memoryWriteEnable = memoryWriteOperands[0];
            candidate.memoryWriteAddress = memoryWriteOperands[1];
            candidate.memoryWriteData = memoryWriteOperands[*memEventStart - (memWriteHasMask ? 2 : 1)];
            return candidate;
        }

        bool rewriteRomCandidate(wolvrix::lib::grh::Graph &graph, const Candidate &candidate)
        {
            const std::string dataRegSymbol = makeUniqueSymbol(graph,
                                                               candidate.memorySymbol + "$retime_data");
            createRegisterDecl(graph,
                               dataRegSymbol,
                               candidate.dataWidth,
                               candidate.dataSigned,
                               candidate.dataInitValue,
                               "rom_data_reg");

            const ValueId memData = createMemoryReadPort(graph,
                                                         candidate.memorySymbol,
                                                         candidate.nextAddress,
                                                         candidate.dataWidth,
                                                         candidate.dataSigned,
                                                         candidate.dataType,
                                                         "rom_mem_read");
            createRegisterWritePort(graph,
                                    dataRegSymbol,
                                    candidate.addressEnable,
                                    memData,
                                    candidate.events,
                                    candidate.eventEdges,
                                    "rom_data_write");
            const ValueId dataOut = createRegisterReadPort(graph,
                                                           dataRegSymbol,
                                                           candidate.dataWidth,
                                                           candidate.dataSigned,
                                                           candidate.dataType,
                                                           "rom_data_read");

            const auto replacement = ingest::node::redirectRoots(graph, candidate.readResult, dataOut);
            const auto view = graph.replaceAllUsesWithView(candidate.readResult, replacement);
            rebindOutputPorts(graph, candidate.readResult, view);

            if (!graph.eraseOp(candidate.memoryReadOp))
            {
                return false;
            }
            if (!graph.eraseOp(candidate.addressReadOp))
            {
                return false;
            }
            if (!graph.eraseOp(candidate.addressWriteOp))
            {
                return false;
            }
            if (!graph.eraseOp(candidate.addressRegisterOp))
            {
                return false;
            }
            return true;
        }

        bool rewriteSimpleRamCandidate(wolvrix::lib::grh::Graph &graph, const Candidate &candidate)
        {
            const std::string dataRegSymbol = makeUniqueSymbol(graph,
                                                               candidate.memorySymbol + "$retime_data");
            createRegisterDecl(graph,
                               dataRegSymbol,
                               candidate.dataWidth,
                               candidate.dataSigned,
                               candidate.dataInitValue,
                               "ram_data_reg");

            const ValueId selectedAddr = createMux(graph,
                                                   candidate.addressEnable,
                                                   candidate.nextAddress,
                                                   candidate.addressValue,
                                                   candidate.addrWidth,
                                                   false,
                                                   ValueType::Logic,
                                                   "sel_addr_next");
            const ValueId memData = createMemoryReadPort(graph,
                                                         candidate.memorySymbol,
                                                         selectedAddr,
                                                         candidate.dataWidth,
                                                         candidate.dataSigned,
                                                         candidate.dataType,
                                                         "ram_mem_read");
            const ValueId writeAddrEq = createBinaryOp(graph,
                                                       OperationKind::kEq,
                                                       candidate.memoryWriteAddress,
                                                       selectedAddr,
                                                       1,
                                                       false,
                                                       ValueType::Logic,
                                                       "write_hit_eq");
            const ValueId writeHit = createBinaryOp(graph,
                                                    OperationKind::kLogicAnd,
                                                    candidate.memoryWriteEnable,
                                                    writeAddrEq,
                                                    1,
                                                    false,
                                                    ValueType::Logic,
                                                    "write_hit");
            const ValueId dataEnable = createBinaryOp(graph,
                                                      OperationKind::kLogicOr,
                                                      candidate.addressEnable,
                                                      writeHit,
                                                      1,
                                                      false,
                                                      ValueType::Logic,
                                                      "data_enable");
            const ValueId nextData = createMux(graph,
                                               writeHit,
                                               candidate.memoryWriteData,
                                               memData,
                                               candidate.dataWidth,
                                               candidate.dataSigned,
                                               candidate.dataType,
                                               "data_refresh");
            createRegisterWritePort(graph,
                                    dataRegSymbol,
                                    dataEnable,
                                    nextData,
                                    candidate.events,
                                    candidate.eventEdges,
                                    "ram_data_write");
            const ValueId dataOut = createRegisterReadPort(graph,
                                                           dataRegSymbol,
                                                           candidate.dataWidth,
                                                           candidate.dataSigned,
                                                           candidate.dataType,
                                                           "ram_data_read");

            const auto replacement = ingest::node::redirectRoots(graph, candidate.readResult, dataOut);
            const auto view = graph.replaceAllUsesWithView(candidate.readResult, replacement);
            rebindOutputPorts(graph, candidate.readResult, view);
            if (!graph.eraseOp(candidate.memoryReadOp))
            {
                return false;
            }
            return true;
        }
    } // namespace

    MemoryReadRetimePass::MemoryReadRetimePass()
        : Pass("memory-read-retime",
               "memory-read-retime",
               "Retime dedicated memory address registers to data side when behavior is preserved")
    {
    }

    PassResult MemoryReadRetimePass::run()
    {
        PassResult result;
        Stats stats;
        for (wolvrix::lib::grh::Graph* selected : graphs())
        {
            auto &graph = *selected;
            const GraphIndex index = buildGraphIndex(graph);
            std::vector<Candidate> candidates;
            for (const OperationId opId : graph.operations())
            {
                if (graph.opKind(opId) != OperationKind::kRegisterReadArrayPort)
                {
                    continue;
                }
                ++stats.totalReadPorts;
                auto candidate = matchCandidate(graph, index, opId, keepDeclaredSymbols(), stats);
                if (!candidate)
                {
                    continue;
                }
                ++stats.candidates;
                candidates.push_back(std::move(*candidate));
            }

            for (const auto &candidate : candidates)
            {
                if (!opStillAlive(graph, candidate.memoryReadOp))
                {
                    continue;
                }
                const bool changed = candidate.mode == Candidate::Mode::Rom
                                         ? rewriteRomCandidate(graph, candidate)
                                         : rewriteSimpleRamCandidate(graph, candidate);
                if (!changed)
                {
                    error(graph, graph.getOperation(candidate.memoryReadOp), "failed to rewrite candidate");
                    result.failed = true;
                    continue;
                }
                result.changed = true;
                ++stats.retimed;
                if (candidate.mode == Candidate::Mode::Rom)
                {
                    ++stats.retimedRom;
                }
                else
                {
                    ++stats.retimedSimpleRam;
                }
            }
        }

        result.graphData = stats;
        if (!graphKernel()) finishGraphKernels(std::span<const PassResult>(&result, 1));
        return result;
    }

    void MemoryReadRetimePass::finishGraphKernels(std::span<const PassResult> results)
    {
        Stats stats;
        for (const auto& result : results) {
            const auto* item = std::any_cast<Stats>(&result.graphData);
            if (!item) continue;
            stats.totalReadPorts += item->totalReadPorts;
            stats.candidates += item->candidates;
            stats.retimed += item->retimed;
            stats.retimedRom += item->retimedRom;
            stats.retimedSimpleRam += item->retimedSimpleRam;
            stats.skipNonRegisterAddr += item->skipNonRegisterAddr;
            stats.skipMultiwriteAddrReg += item->skipMultiwriteAddrReg;
            stats.skipPartialMask += item->skipPartialMask;
            stats.skipAddrFanout += item->skipAddrFanout;
            stats.skipMultiwriteMemory += item->skipMultiwriteMemory;
            stats.skipWriteportPartialMask += item->skipWriteportPartialMask;
            stats.skipMismatchedEventDomain += item->skipMismatchedEventDomain;
            stats.skipDeclaredSymbol += item->skipDeclaredSymbol;
            stats.skipMalformed += item->skipMalformed;
        }
        logInfo("memory-read-retime: readport_total=" + std::to_string(stats.totalReadPorts) +
                " candidate=" + std::to_string(stats.candidates) +
                " retimed=" + std::to_string(stats.retimed) +
                " retimed_rom=" + std::to_string(stats.retimedRom) +
                " retimed_simple_ram=" + std::to_string(stats.retimedSimpleRam) +
                " skip_non_register_addr=" + std::to_string(stats.skipNonRegisterAddr) +
                " skip_multiwrite_addr_reg=" + std::to_string(stats.skipMultiwriteAddrReg) +
                " skip_partial_mask=" + std::to_string(stats.skipPartialMask) +
                " skip_addr_fanout=" + std::to_string(stats.skipAddrFanout) +
                " skip_multiwrite_memory=" + std::to_string(stats.skipMultiwriteMemory) +
                " skip_writeport_partial_mask=" + std::to_string(stats.skipWriteportPartialMask) +
                " skip_mismatched_event_domain=" + std::to_string(stats.skipMismatchedEventDomain) +
                " skip_declared_symbol=" + std::to_string(stats.skipDeclaredSymbol) +
                " skip_malformed=" + std::to_string(stats.skipMalformed));
    }

} // namespace wolvrix::lib::transform
