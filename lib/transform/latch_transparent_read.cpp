#include "transform/latch_transparent_read.hpp"

#include "core/grh.hpp"

#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace wolvrix::lib::transform
{

    namespace
    {
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

        struct LatchWritePortInfo
        {
            wolvrix::lib::grh::OperationId opId;
            wolvrix::lib::grh::ValueId updateCond;  // oper[0]
            wolvrix::lib::grh::ValueId nextValue;   // oper[1]
            wolvrix::lib::grh::ValueId mask;        // oper[2]
        };

        struct LatchInfo
        {
            wolvrix::lib::grh::OperationId latchOpId;
            int32_t width = 0;
            bool isSigned = false;
            std::vector<LatchWritePortInfo> writePorts;
            std::vector<wolvrix::lib::grh::OperationId> readPortOps;
        };
    } // namespace

    LatchTransparentReadPass::LatchTransparentReadPass()
        : Pass("latch-transparent-read", "latch-transparent-read",
               "Model latch transparent read behavior by inserting mux on read ports")
    {
    }

    PassResult LatchTransparentReadPass::run()
    {
        PassResult result;
        const std::size_t graphCount = design().graphs().size();
        logDebug("begin graphs=" + std::to_string(graphCount));

        for (auto* selected : graphs())
        {
            wolvrix::lib::grh::Graph &graph = *selected;
            
            // Collect latch information
            std::unordered_map<std::string, LatchInfo> latchBySymbol;

            for (const auto opId : graph.operations())
            {
                if (!opId.valid())
                {
                    continue;
                }
                const auto kind = graph.opKind(opId);
                const auto attrs = graph.opAttrRange(opId);
                
                if (kind == wolvrix::lib::grh::OperationKind::kLatch)
                {
                    int32_t width = 1;
                    bool isSigned = false;
                    for (const auto &attr : attrs)
                    {
                        if (attr.key() == "width")
                        {
                            if (const auto w = attr.value().get<int64_t>())
                            {
                                width = static_cast<int32_t>(*w);
                            }
                        }
                        else if (attr.key() == "isSigned")
                        {
                            if (const auto s = attr.value().get<bool>())
                            {
                                isSigned = *s;
                            }
                        }
                    }
                    std::string symbol = std::string(graph.symbolText(graph.operationSymbol(opId)));
                    LatchInfo &info = latchBySymbol[symbol];
                    info.latchOpId = opId;
                    info.width = width;
                    info.isSigned = isSigned;
                }
                else if (kind == wolvrix::lib::grh::OperationKind::kLatchWritePort)
                {
                    auto latchSymOpt = getAttrString(attrs, "latchSymbol");
                    if (!latchSymOpt)
                    {
                        warning(graph, graph.getOperation(opId), "kLatchWritePort missing latchSymbol attribute");
                        continue;
                    }
                    
                    const auto operands = graph.opOperands(opId);
                    if (operands.size() < 2)
                    {
                        error(graph, graph.getOperation(opId),
                              "kLatchWritePort must have at least 2 operands (updateCond, nextValue)");
                        result.failed = true;
                        continue;
                    }
                    
                    LatchWritePortInfo writeInfo;
                    writeInfo.opId = opId;
                    writeInfo.updateCond = operands[0];
                    writeInfo.nextValue = operands[1];
                    writeInfo.mask = operands.size() >= 3 ? operands[2] : wolvrix::lib::grh::ValueId::invalid();
                    
                    latchBySymbol[std::string(*latchSymOpt)].writePorts.push_back(writeInfo);
                }
                else if (kind == wolvrix::lib::grh::OperationKind::kLatchReadPort)
                {
                    auto latchSymOpt = getAttrString(attrs, "latchSymbol");
                    if (!latchSymOpt)
                    {
                        warning(graph, graph.getOperation(opId), "kLatchReadPort missing latchSymbol attribute");
                        continue;
                    }
                    latchBySymbol[std::string(*latchSymOpt)].readPortOps.push_back(opId);
                }
            }

            // Check and transform
            for (auto &[latchSym, info] : latchBySymbol)
            {
                if (!info.latchOpId.valid())
                {
                    error(graph, "Latch '" + latchSym + "' has ports but no declaration");
                    result.failed = true;
                    continue;
                }

                // Check: latch can only have one write port (no multi-driven)
                if (info.writePorts.size() > 1)
                {
                    error(graph, "Latch '" + latchSym + "' has multiple write ports (" + 
                          std::to_string(info.writePorts.size()) + "), which causes multi-driven conflict");
                    result.failed = true;
                    continue;
                }

                // If no write port, skip (read-only latch is invalid but we let other passes handle it)
                if (info.writePorts.empty())
                {
                    warning(graph, "Latch '" + latchSym + "' has no write port");
                    continue;
                }

                // If no read port, nothing to transform
                if (info.readPortOps.empty())
                {
                    continue;
                }

                const auto &writePort = info.writePorts[0];
                const wolvrix::lib::grh::ValueId updateCond = writePort.updateCond;
                const wolvrix::lib::grh::ValueId nextValue = writePort.nextValue;
                const wolvrix::lib::grh::ValueId mask = writePort.mask;
                const bool hasMask = mask.valid();

                // Process each read port
                for (const auto readPortOpId : info.readPortOps)
                {
                    const auto results = graph.opResults(readPortOpId);
                    if (results.empty())
                    {
                        continue;
                    }
                    const wolvrix::lib::grh::ValueId readResult = results[0];
                    const auto users = graph.valueUsers(readResult);
                    const std::vector<wolvrix::lib::grh::ValueUser> originalUsers(
                        users.begin(), users.end());
                    
                    // Create the transparent read logic:
                    // When updateCond is 1, return new value (with mask consideration)
                    // When updateCond is 0, return current latch value
                    
                    // First, compute the masked value if needed
                    // masked_value = hasMask ? ((nextValue & mask) | (readValue & ~mask)) : nextValue
                    // But since readValue is actually the latch value in this context, we use:
                    // effective_value = (nextValue & effective_mask) | (readValue & ~effective_mask)
                    // where effective_mask = updateCond ? mask : 0
                    
                    // However, we model this as:
                    // result = updateCond ? masked_new_value : latch_value
                    // where masked_new_value = hasMask ? ((nextValue & mask) | (latch_value & ~mask)) : nextValue
                    
                    wolvrix::lib::grh::ValueId muxTrueValue = nextValue;
                    
                    if (hasMask)
                    {
                        // Need to create: (nextValue & mask) | (readValue & ~mask)
                        // This represents partial update: only masked bits get new value
                        
                        // Create ~mask
                        wolvrix::lib::grh::SymbolId notMaskSym = graph.makeInternalOpSym();
                        wolvrix::lib::grh::OperationId notMaskOp = graph.createOperation(
                            wolvrix::lib::grh::OperationKind::kNot, notMaskSym);
                        graph.addOperand(notMaskOp, mask);
                        wolvrix::lib::grh::ValueId notMaskResult = graph.createValue(info.width, info.isSigned);
                        graph.addResult(notMaskOp, notMaskResult);
                        
                        // Create nextValue & mask
                        wolvrix::lib::grh::SymbolId and1Sym = graph.makeInternalOpSym();
                        wolvrix::lib::grh::OperationId and1Op = graph.createOperation(
                            wolvrix::lib::grh::OperationKind::kAnd, and1Sym);
                        graph.addOperand(and1Op, nextValue);
                        graph.addOperand(and1Op, mask);
                        wolvrix::lib::grh::ValueId and1Result = graph.createValue(info.width, info.isSigned);
                        graph.addResult(and1Op, and1Result);
                        
                        // Create readValue & ~mask
                        wolvrix::lib::grh::SymbolId and2Sym = graph.makeInternalOpSym();
                        wolvrix::lib::grh::OperationId and2Op = graph.createOperation(
                            wolvrix::lib::grh::OperationKind::kAnd, and2Sym);
                        graph.addOperand(and2Op, readResult);
                        graph.addOperand(and2Op, notMaskResult);
                        wolvrix::lib::grh::ValueId and2Result = graph.createValue(info.width, info.isSigned);
                        graph.addResult(and2Op, and2Result);
                        
                        // Create (nextValue & mask) | (readValue & ~mask)
                        wolvrix::lib::grh::SymbolId orSym = graph.makeInternalOpSym();
                        wolvrix::lib::grh::OperationId orOp = graph.createOperation(
                            wolvrix::lib::grh::OperationKind::kOr, orSym);
                        graph.addOperand(orOp, and1Result);
                        graph.addOperand(orOp, and2Result);
                        wolvrix::lib::grh::ValueId orResult = graph.createValue(info.width, info.isSigned);
                        graph.addResult(orOp, orResult);
                        
                        muxTrueValue = orResult;
                    }
                    
                    // Create the Mux: result = updateCond ? muxTrueValue : readResult
                    wolvrix::lib::grh::SymbolId muxSym = graph.makeInternalOpSym();
                    wolvrix::lib::grh::OperationId muxOp = graph.createOperation(
                        wolvrix::lib::grh::OperationKind::kMux, muxSym);
                    graph.addOperand(muxOp, updateCond);      // condition
                    graph.addOperand(muxOp, muxTrueValue);    // true value
                    graph.addOperand(muxOp, readResult);      // false value (current latch value)
                    
                    // Create result value for mux
                    wolvrix::lib::grh::ValueId muxResult = graph.createValue(info.width, info.isSigned);
                    graph.addResult(muxOp, muxResult);
                    
                    // Replace only the pre-existing users of readResult so the
                    // mux false branch and mask logic keep the raw latch value.
                    for (const auto &user : originalUsers)
                    {
                        if (!user.operation.valid())
                        {
                            continue;
                        }
                        graph.replaceOperand(user.operation, user.operandIndex, muxResult);
                    }
                    
                    result.changed = true;
                    debug(graph, "Inserted transparent-read mux for latch '" + latchSym + "' read port");
                }
            }
        }

        return result;
    }

} // namespace wolvrix::lib::transform
