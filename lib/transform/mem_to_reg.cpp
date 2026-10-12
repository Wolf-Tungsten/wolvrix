#include "transform/mem_to_reg.hpp"

#include "core/grh.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wolvrix::lib::transform
{

    namespace
    {
        template <typename T>
        std::optional<T> getAttr(const wolvrix::lib::grh::Operation &op, std::string_view key)
        {
            auto attr = op.attr(key);
            if (!attr)
            {
                return std::nullopt;
            }
            if (const auto *value = std::get_if<T>(&*attr))
            {
                return *value;
            }
            return std::nullopt;
        }

        bool hasAnyInitAttrs(const wolvrix::lib::grh::Operation &op)
        {
            return op.attr("initKind").has_value() ||
                   op.attr("initFile").has_value() ||
                   op.attr("initValue").has_value() ||
                   op.attr("initStart").has_value() ||
                   op.attr("initLen").has_value();
        }

        std::optional<int64_t> parseConstIntLiteral(std::string_view literal)
        {
            if (literal.empty())
            {
                return std::nullopt;
            }
            if (literal.front() == '"' || literal.front() == '$')
            {
                return std::nullopt;
            }
            std::string cleaned;
            cleaned.reserve(literal.size());
            for (char ch : literal)
            {
                if (ch == '_' || std::isspace(static_cast<unsigned char>(ch)))
                {
                    continue;
                }
                cleaned.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            }
            if (cleaned.empty())
            {
                return std::nullopt;
            }

            int base = 10;
            std::string digits;
            const std::size_t tick = cleaned.find('\'');
            if (tick != std::string::npos)
            {
                if (tick + 1 >= cleaned.size())
                {
                    return std::nullopt;
                }
                const char baseChar = cleaned[tick + 1];
                switch (baseChar)
                {
                case 'b':
                    base = 2;
                    break;
                case 'o':
                    base = 8;
                    break;
                case 'd':
                    base = 10;
                    break;
                case 'h':
                    base = 16;
                    break;
                default:
                    return std::nullopt;
                }
                if (tick + 2 >= cleaned.size())
                {
                    return std::nullopt;
                }
                digits = cleaned.substr(tick + 2);
            }
            else
            {
                digits = cleaned;
                if (digits.rfind("0x", 0) == 0)
                {
                    base = 16;
                    digits = digits.substr(2);
                }
                else if (digits.rfind("0b", 0) == 0)
                {
                    base = 2;
                    digits = digits.substr(2);
                }
                else if (digits.rfind("0o", 0) == 0)
                {
                    base = 8;
                    digits = digits.substr(2);
                }
            }
            if (digits.empty())
            {
                return std::nullopt;
            }
            for (char ch : digits)
            {
                if (ch == 'x' || ch == 'z' || ch == '?')
                {
                    return std::nullopt;
                }
            }
            try
            {
                return std::stoll(digits, nullptr, base);
            }
            catch (const std::exception &)
            {
                return std::nullopt;
            }
        }

        std::string makeIntLiteral(int32_t width, int64_t value)
        {
            const int32_t normalizedWidth = width > 0 ? width : 1;
            return std::to_string(normalizedWidth) + "'d" + std::to_string(value);
        }

        wolvrix::lib::grh::ValueId createConstantValue(wolvrix::lib::grh::Graph &graph,
                                                       int32_t width,
                                                       bool isSigned,
                                                       std::string literal,
                                                       std::string_view note)
        {
            const wolvrix::lib::grh::SymbolId valueSym = graph.makeInternalValSym();
            const wolvrix::lib::grh::SymbolId opSym = graph.makeInternalOpSym();
            const wolvrix::lib::grh::ValueId valueId = graph.createValue(
                valueSym,
                width > 0 ? width : 1,
                isSigned,
                wolvrix::lib::grh::ValueType::Logic);
            const wolvrix::lib::grh::OperationId opId = graph.createOperation(
                wolvrix::lib::grh::OperationKind::kConstant,
                opSym);
            graph.addResult(opId, valueId);
            graph.setAttr(opId, "constValue", std::move(literal));
            const auto srcLoc = makeTransformSrcLoc("mem-to-reg", note);
            graph.setValueSrcLoc(valueId, srcLoc);
            graph.setOpSrcLoc(opId, srcLoc);
            return valueId;
        }

        wolvrix::lib::grh::ValueId createBinaryOp(wolvrix::lib::grh::Graph &graph,
                                                  wolvrix::lib::grh::OperationKind kind,
                                                  wolvrix::lib::grh::ValueId lhs,
                                                  wolvrix::lib::grh::ValueId rhs,
                                                  int32_t outWidth,
                                                  bool outSigned,
                                                  std::string_view note)
        {
            const wolvrix::lib::grh::SymbolId valueSym = graph.makeInternalValSym();
            const wolvrix::lib::grh::SymbolId opSym = graph.makeInternalOpSym();
            const wolvrix::lib::grh::ValueId out = graph.createValue(
                valueSym,
                outWidth > 0 ? outWidth : 1,
                outSigned,
                wolvrix::lib::grh::ValueType::Logic);
            const wolvrix::lib::grh::OperationId op = graph.createOperation(kind, opSym);
            graph.addOperand(op, lhs);
            graph.addOperand(op, rhs);
            graph.addResult(op, out);
            const auto srcLoc = makeTransformSrcLoc("mem-to-reg", note);
            graph.setValueSrcLoc(out, srcLoc);
            graph.setOpSrcLoc(op, srcLoc);
            return out;
        }

        wolvrix::lib::grh::ValueId createMux(wolvrix::lib::grh::Graph &graph,
                                             wolvrix::lib::grh::ValueId sel,
                                             wolvrix::lib::grh::ValueId whenTrue,
                                             wolvrix::lib::grh::ValueId whenFalse,
                                             int32_t outWidth,
                                             bool outSigned,
                                             wolvrix::lib::grh::ValueType outType,
                                             std::string_view note)
        {
            const wolvrix::lib::grh::SymbolId valueSym = graph.makeInternalValSym();
            const wolvrix::lib::grh::SymbolId opSym = graph.makeInternalOpSym();
            const wolvrix::lib::grh::ValueId out = graph.createValue(
                valueSym,
                outWidth > 0 ? outWidth : 1,
                outSigned,
                outType);
            const wolvrix::lib::grh::OperationId op = graph.createOperation(
                wolvrix::lib::grh::OperationKind::kMux,
                opSym);
            graph.addOperand(op, sel);
            graph.addOperand(op, whenTrue);
            graph.addOperand(op, whenFalse);
            graph.addResult(op, out);
            const auto srcLoc = makeTransformSrcLoc("mem-to-reg", note);
            graph.setValueSrcLoc(out, srcLoc);
            graph.setOpSrcLoc(op, srcLoc);
            return out;
        }

        std::string makeUniqueRowRegName(const wolvrix::lib::grh::Graph &graph,
                                         std::string_view memSymbol,
                                         int64_t row)
        {
            const std::string base = std::string(memSymbol) + "$row$" + std::to_string(row);
            std::string candidate = base;
            int64_t suffix = 0;
            while (graph.findOperation(candidate).valid() || graph.findValue(candidate).valid())
            {
                ++suffix;
                candidate = base + "$m2r$" + std::to_string(suffix);
            }
            return candidate;
        }

        bool isConstValue(const wolvrix::lib::grh::Graph &graph,
                          wolvrix::lib::grh::ValueId valueId,
                          std::string *literalOut = nullptr)
        {
            if (!valueId.valid())
            {
                return false;
            }
            const auto def = graph.getValue(valueId).definingOp();
            if (!def.valid())
            {
                return false;
            }
            const auto defOp = graph.getOperation(def);
            if (defOp.kind() != wolvrix::lib::grh::OperationKind::kConstant)
            {
                return false;
            }
            auto literal = getAttr<std::string>(defOp, "constValue");
            if (!literal)
            {
                return false;
            }
            if (literalOut)
            {
                *literalOut = *literal;
            }
            return true;
        }

        struct MemoryRefs
        {
            std::vector<wolvrix::lib::grh::OperationId> readPorts;
            std::vector<wolvrix::lib::grh::OperationId> writePorts;
        };

        MemoryRefs collectMemoryRefs(const wolvrix::lib::grh::Graph &graph,
                                     std::string_view memSymbol)
        {
            MemoryRefs refs;
            for (const auto opId : graph.operations())
            {
                const auto op = graph.getOperation(opId);
                if (op.kind() != wolvrix::lib::grh::OperationKind::kMemoryReadPort &&
                    op.kind() != wolvrix::lib::grh::OperationKind::kMemoryWritePort)
                {
                    continue;
                }
                auto sym = getAttr<std::string>(op, "memSymbol");
                if (!sym || *sym != memSymbol)
                {
                    continue;
                }
                if (op.kind() == wolvrix::lib::grh::OperationKind::kMemoryReadPort)
                {
                    refs.readPorts.push_back(opId);
                }
                else
                {
                    refs.writePorts.push_back(opId);
                }
            }
            return refs;
        }

        bool buildInitMap(const wolvrix::lib::grh::Operation &memoryOp,
                          int64_t rowCount,
                          bool strictInit,
                          std::vector<std::optional<std::string>> &initMap,
                          std::string &reason)
        {
            initMap.assign(static_cast<std::size_t>(rowCount), std::nullopt);

            const bool hasInit = hasAnyInitAttrs(memoryOp);
            if (!hasInit)
            {
                if (strictInit)
                {
                    reason = "strict-init requires literal init";
                    return false;
                }
                return true;
            }

            auto kindsOpt = getAttr<std::vector<std::string>>(memoryOp, "initKind");
            auto filesOpt = getAttr<std::vector<std::string>>(memoryOp, "initFile");
            auto startsOpt = getAttr<std::vector<int64_t>>(memoryOp, "initStart");
            auto lensOpt = getAttr<std::vector<int64_t>>(memoryOp, "initLen");
            auto values = getAttr<std::vector<std::string>>(memoryOp, "initValue").value_or(std::vector<std::string>{});

            if (!kindsOpt || !filesOpt || !startsOpt || !lensOpt)
            {
                reason = "incomplete init attrs";
                return false;
            }

            const auto &kinds = *kindsOpt;
            const auto &files = *filesOpt;
            const auto &starts = *startsOpt;
            const auto &lens = *lensOpt;

            if (kinds.empty() ||
                kinds.size() != files.size() ||
                kinds.size() != starts.size() ||
                kinds.size() != lens.size())
            {
                reason = "init attr size mismatch";
                return false;
            }

            for (std::size_t i = 0; i < kinds.size(); ++i)
            {
                if (kinds[i] != "literal")
                {
                    reason = "non-literal init kind";
                    return false;
                }

                std::string literal = (i < values.size() && !values[i].empty()) ? values[i] : "0";
                if (!parseConstIntLiteral(literal).has_value())
                {
                    reason = "init literal is not statically evaluable";
                    return false;
                }

                const int64_t start = starts[i];
                const int64_t len = lens[i];
                if (start < 0)
                {
                    for (int64_t row = 0; row < rowCount; ++row)
                    {
                        initMap[static_cast<std::size_t>(row)] = literal;
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
                    initMap[static_cast<std::size_t>(row)] = literal;
                }
            }

            return true;
        }

        struct ConversionStats
        {
            int64_t memoryTotal = 0;
            int64_t memoryConverted = 0;
            int64_t skippedRowLimit = 0;
            int64_t skippedNonConstMask = 0;
            int64_t skippedInit = 0;
            int64_t skippedMalformed = 0;
            int64_t skippedUnselected = 0;
            int64_t registerCreated = 0;
            int64_t readPortRewritten = 0;
            int64_t writePortRewritten = 0;
        };
    } // namespace

    MemToRegPass::MemToRegPass()
        : MemToRegPass(MemToRegOptions{})
    {
    }

    MemToRegPass::MemToRegPass(MemToRegOptions options)
        : Pass("mem-to-reg", "mem-to-reg", "Lower small kMemory to row kRegister set"),
          options_(std::move(options))
    {
    }

    PassResult MemToRegPass::run()
    {
        PassResult result;
        ConversionStats totalStats;
        const std::unordered_set<std::string> selectedSymbols(
            options_.memorySymbols.begin(), options_.memorySymbols.end());
        const bool selectAll = selectedSymbols.empty();

        for (const auto &entry : design().graphs())
        {
            auto &graph = *entry.second;
            std::vector<wolvrix::lib::grh::OperationId> memories;
            for (const auto opId : graph.operations())
            {
                const auto op = graph.getOperation(opId);
                if (op.kind() == wolvrix::lib::grh::OperationKind::kMemory)
                {
                    memories.push_back(opId);
                }
            }

            for (const auto memId : memories)
            {
                if (std::find(graph.operations().begin(), graph.operations().end(), memId) == graph.operations().end())
                {
                    continue;
                }

                const auto memOp = graph.getOperation(memId);
                ++totalStats.memoryTotal;

                // Filter before collecting any port references. This keeps a targeted
                // experiment from paying the whole-graph reference scan for unrelated RAMs.
                const std::string memSymbol = std::string(memOp.symbolText());
                if (!selectAll && selectedSymbols.find(memSymbol) == selectedSymbols.end())
                {
                    ++totalStats.skippedUnselected;
                    continue;
                }

                const auto widthOpt = getAttr<int64_t>(memOp, "width");
                const auto rowOpt = getAttr<int64_t>(memOp, "row");
                const bool isSigned = getAttr<bool>(memOp, "isSigned").value_or(false);
                if (!widthOpt || !rowOpt || *widthOpt <= 0 || *rowOpt <= 0)
                {
                    ++totalStats.skippedMalformed;
                    continue;
                }

                const int64_t width = *widthOpt;
                const int64_t row = *rowOpt;
                if (row > options_.rowLimit)
                {
                    ++totalStats.skippedRowLimit;
                    continue;
                }

                MemoryRefs refs = collectMemoryRefs(graph, memSymbol);

                bool eligible = true;
                bool nonConstMask = false;
                for (const auto writeId : refs.writePorts)
                {
                    const auto writeOp = graph.getOperation(writeId);
                    if (writeOp.operands().size() < 4)
                    {
                        eligible = false;
                        break;
                    }
                    if (!isConstValue(graph, writeOp.operands()[3]))
                    {
                        nonConstMask = true;
                        eligible = false;
                        break;
                    }
                    auto edges = getAttr<std::vector<std::string>>(writeOp, "eventEdge");
                    const std::size_t eventCount = writeOp.operands().size() - 4;
                    if (!edges || edges->size() != eventCount)
                    {
                        eligible = false;
                        break;
                    }
                }
                if (eligible)
                {
                    for (const auto readId : refs.readPorts)
                    {
                        const auto readOp = graph.getOperation(readId);
                        if (readOp.operands().size() != 1 || readOp.results().size() != 1)
                        {
                            eligible = false;
                            break;
                        }
                    }
                }
                if (!eligible)
                {
                    if (nonConstMask)
                    {
                        ++totalStats.skippedNonConstMask;
                    }
                    else
                    {
                        ++totalStats.skippedMalformed;
                    }
                    continue;
                }

                std::vector<std::optional<std::string>> initMap;
                std::string initSkipReason;
                if (!buildInitMap(memOp, row, options_.strictInit, initMap, initSkipReason))
                {
                    (void)initSkipReason;
                    ++totalStats.skippedInit;
                    continue;
                }

                std::vector<std::string> rowRegSymbols;
                rowRegSymbols.reserve(static_cast<std::size_t>(row));

                for (int64_t i = 0; i < row; ++i)
                {
                    const std::string regName = makeUniqueRowRegName(graph, memSymbol, i);
                    rowRegSymbols.push_back(regName);
                    const auto regOp = graph.createOperation(
                        wolvrix::lib::grh::OperationKind::kRegister,
                        graph.internSymbol(regName));
                    graph.setAttr(regOp, "width", width);
                    graph.setAttr(regOp, "isSigned", isSigned);
                    if (initMap[static_cast<std::size_t>(i)].has_value())
                    {
                        graph.setAttr(regOp, "initValue", *initMap[static_cast<std::size_t>(i)]);
                    }
                    graph.setOpSrcLoc(regOp, makeTransformSrcLoc("mem-to-reg", "row_register"));
                    ++totalStats.registerCreated;
                }

                for (const auto readId : refs.readPorts)
                {
                    if (std::find(graph.operations().begin(), graph.operations().end(), readId) == graph.operations().end())
                    {
                        continue;
                    }
                    const auto readOp = graph.getOperation(readId);
                    if (readOp.operands().size() != 1 || readOp.results().size() != 1)
                    {
                        ++totalStats.skippedMalformed;
                        continue;
                    }

                    const auto addr = readOp.operands()[0];
                    const auto outValueId = readOp.results()[0];
                    const auto outValue = graph.getValue(outValueId);
                    const auto addrValue = graph.getValue(addr);

                    std::vector<wolvrix::lib::grh::ValueId> rowData;
                    std::vector<wolvrix::lib::grh::ValueId> hits;
                    rowData.reserve(static_cast<std::size_t>(row));
                    hits.reserve(static_cast<std::size_t>(row));

                    for (int64_t i = 0; i < row; ++i)
                    {
                        const auto readPort = graph.createOperation(
                            wolvrix::lib::grh::OperationKind::kRegisterReadPort,
                            graph.makeInternalOpSym());
                        graph.setAttr(readPort, "regSymbol", rowRegSymbols[static_cast<std::size_t>(i)]);

                        const auto rowDataValue = graph.createValue(
                            graph.makeInternalValSym(),
                            outValue.width(),
                            outValue.isSigned(),
                            outValue.type());
                        graph.addResult(readPort, rowDataValue);
                        graph.setOpSrcLoc(readPort, makeTransformSrcLoc("mem-to-reg", "lower_read_port"));
                        graph.setValueSrcLoc(rowDataValue, makeTransformSrcLoc("mem-to-reg", "lower_read_port"));
                        rowData.push_back(rowDataValue);

                        const auto rowConst = createConstantValue(
                            graph,
                            addrValue.width(),
                            addrValue.isSigned(),
                            makeIntLiteral(addrValue.width(), i),
                            "addr_row_const");
                        const auto hit = createBinaryOp(
                            graph,
                            wolvrix::lib::grh::OperationKind::kEq,
                            addr,
                            rowConst,
                            1,
                            false,
                            "addr_row_eq");
                        hits.push_back(hit);
                    }

                    auto defaultValue = createConstantValue(
                        graph,
                        outValue.width(),
                        outValue.isSigned(),
                        makeIntLiteral(outValue.width(), 0),
                        "read_default_zero");

                    for (int64_t i = row - 1; i >= 0; --i)
                    {
                        defaultValue = createMux(
                            graph,
                            hits[static_cast<std::size_t>(i)],
                            rowData[static_cast<std::size_t>(i)],
                            defaultValue,
                            outValue.width(),
                            outValue.isSigned(),
                            outValue.type(),
                            "read_mux");
                    }

                    if (!graph.eraseOp(readId, std::array<wolvrix::lib::grh::ValueId, 1>{defaultValue}))
                    {
                        error(graph, readOp, "failed to erase lowered kMemoryReadPort");
                        result.failed = true;
                        continue;
                    }
                    ++totalStats.readPortRewritten;
                }

                for (const auto writeId : refs.writePorts)
                {
                    if (std::find(graph.operations().begin(), graph.operations().end(), writeId) == graph.operations().end())
                    {
                        continue;
                    }
                    const auto writeOp = graph.getOperation(writeId);
                    if (writeOp.operands().size() < 4)
                    {
                        ++totalStats.skippedMalformed;
                        continue;
                    }

                    const auto updateCond = writeOp.operands()[0];
                    const auto addr = writeOp.operands()[1];
                    const auto data = writeOp.operands()[2];
                    const auto mask = writeOp.operands()[3];
                    const std::vector<wolvrix::lib::grh::ValueId> events(
                        writeOp.operands().begin() + 4,
                        writeOp.operands().end());
                    auto eventEdge = getAttr<std::vector<std::string>>(writeOp, "eventEdge").value_or(std::vector<std::string>{});

                    const auto addrValue = graph.getValue(addr);
                    for (int64_t i = 0; i < row; ++i)
                    {
                        const auto rowConst = createConstantValue(
                            graph,
                            addrValue.width(),
                            addrValue.isSigned(),
                            makeIntLiteral(addrValue.width(), i),
                            "addr_row_const");
                        const auto hit = createBinaryOp(
                            graph,
                            wolvrix::lib::grh::OperationKind::kEq,
                            addr,
                            rowConst,
                            1,
                            false,
                            "addr_row_eq");
                        const auto rowEnable = createBinaryOp(
                            graph,
                            wolvrix::lib::grh::OperationKind::kAnd,
                            updateCond,
                            hit,
                            1,
                            false,
                            "row_write_enable");

                        const auto regWrite = graph.createOperation(
                            wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                            graph.makeInternalOpSym());
                        graph.setAttr(regWrite, "regSymbol", rowRegSymbols[static_cast<std::size_t>(i)]);
                        graph.setAttr(regWrite, "eventEdge", eventEdge);
                        graph.addOperand(regWrite, rowEnable);
                        graph.addOperand(regWrite, data);
                        graph.addOperand(regWrite, mask);
                        for (const auto event : events)
                        {
                            graph.addOperand(regWrite, event);
                        }
                        graph.setOpSrcLoc(regWrite, makeTransformSrcLoc("mem-to-reg", "lower_write_port"));
                    }

                    if (!graph.eraseOp(writeId))
                    {
                        error(graph, writeOp, "failed to erase lowered kMemoryWritePort");
                        result.failed = true;
                        continue;
                    }
                    ++totalStats.writePortRewritten;
                }

                if (!graph.eraseOp(memId))
                {
                    error(graph, memOp, "failed to erase lowered kMemory");
                    result.failed = true;
                    continue;
                }

                ++totalStats.memoryConverted;
                result.changed = true;
            }
        }

        logInfo("mem-to-reg: memory_total=" + std::to_string(totalStats.memoryTotal) +
                " converted=" + std::to_string(totalStats.memoryConverted) +
                " skipped_row_limit=" + std::to_string(totalStats.skippedRowLimit) +
                " skipped_nonconst_mask=" + std::to_string(totalStats.skippedNonConstMask) +
                " skipped_init=" + std::to_string(totalStats.skippedInit) +
                " skipped_malformed=" + std::to_string(totalStats.skippedMalformed) +
                " skipped_unselected=" + std::to_string(totalStats.skippedUnselected) +
                " regs_created=" + std::to_string(totalStats.registerCreated) +
                " read_rewritten=" + std::to_string(totalStats.readPortRewritten) +
                " write_rewritten=" + std::to_string(totalStats.writePortRewritten));

        return result;
    }

} // namespace wolvrix::lib::transform
