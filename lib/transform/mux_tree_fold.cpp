#include "transform/mux_tree_fold.hpp"

#include "core/grh.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
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
        using wolvrix::lib::grh::SymbolId;
        using wolvrix::lib::grh::Value;
        using wolvrix::lib::grh::ValueId;
        using wolvrix::lib::grh::ValueIdHash;

        // ------------------------------------------------------------------
        // constant literal helpers
        // ------------------------------------------------------------------

        std::string normalizeConstLiteral(std::string_view literal)
        {
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
            return cleaned;
        }

        // Parse a non-negative integer literal of at most 64 bits. On success
        // returns the value and stores the declared width (or -1 when the
        // literal carries no explicit size).
        std::optional<uint64_t> parseConstLiteralUint(std::string_view literal, int64_t &width)
        {
            const std::string cleaned = normalizeConstLiteral(literal);
            width = -1;
            if (cleaned.empty() || cleaned.front() == '"' || cleaned.front() == '$')
            {
                return std::nullopt;
            }
            int base = 10;
            std::string digits;
            const std::size_t tick = cleaned.find('\'');
            if (tick != std::string::npos)
            {
                const std::string widthText = cleaned.substr(0, tick);
                if (!widthText.empty())
                {
                    int64_t parsedWidth = 0;
                    for (char ch : widthText)
                    {
                        if (!std::isdigit(static_cast<unsigned char>(ch)))
                        {
                            return std::nullopt;
                        }
                        parsedWidth = parsedWidth * 10 + (ch - '0');
                        if (parsedWidth > (1 << 24))
                        {
                            return std::nullopt;
                        }
                    }
                    width = parsedWidth;
                }
                std::size_t pos = tick + 1;
                if (pos < cleaned.size() && cleaned[pos] == 's')
                {
                    return std::nullopt; // signed literal: not an index candidate
                }
                if (pos < cleaned.size())
                {
                    switch (cleaned[pos])
                    {
                    case 'b': base = 2; ++pos; break;
                    case 'o': base = 8; ++pos; break;
                    case 'd': base = 10; ++pos; break;
                    case 'h': base = 16; ++pos; break;
                    default: break; // unsized fill form: '0 / '1
                    }
                }
                digits = cleaned.substr(pos);
            }
            else
            {
                digits = cleaned;
                if (digits.rfind("0x", 0) == 0) { base = 16; digits = digits.substr(2); }
                else if (digits.rfind("0b", 0) == 0) { base = 2; digits = digits.substr(2); }
                else if (digits.rfind("0o", 0) == 0) { base = 8; digits = digits.substr(2); }
            }
            if (digits.empty())
            {
                return std::nullopt;
            }
            uint64_t value = 0;
            for (char ch : digits)
            {
                int digit = -1;
                if (ch >= '0' && ch <= '9') { digit = ch - '0'; }
                else if (ch >= 'a' && ch <= 'f') { digit = ch - 'a' + 10; }
                else if (ch == 'x' || ch == 'z' || ch == '?') { return std::nullopt; }
                else { return std::nullopt; }
                if (digit >= base)
                {
                    return std::nullopt;
                }
                if (value > (UINT64_MAX - static_cast<uint64_t>(digit)) / static_cast<uint64_t>(base))
                {
                    return std::nullopt; // beyond 64 bits
                }
                value = value * static_cast<uint64_t>(base) + static_cast<uint64_t>(digit);
            }
            return value;
        }

        struct RegisterInfo
        {
            OperationId op;
            int64_t width = 0;
            std::vector<int64_t> shape;
            int64_t elemCount = 0;
            int64_t elemWidth = 0;
        };

        struct EqConstMatch
        {
            ValueId ptr;
            uint64_t constant = 0;
            bool isEq = true;
        };

        struct ElemReadMatch
        {
            std::string regSym;
            int64_t index = 0;
        };

        bool isSweepableKind(OperationKind kind)
        {
            switch (kind)
            {
            case OperationKind::kAssign:
            case OperationKind::kMux:
            case OperationKind::kEq:
            case OperationKind::kNe:
            case OperationKind::kLt:
            case OperationKind::kLe:
            case OperationKind::kGt:
            case OperationKind::kGe:
            case OperationKind::kAnd:
            case OperationKind::kOr:
            case OperationKind::kXor:
            case OperationKind::kXnor:
            case OperationKind::kNot:
            case OperationKind::kLogicAnd:
            case OperationKind::kLogicOr:
            case OperationKind::kLogicNot:
            case OperationKind::kReduceAnd:
            case OperationKind::kReduceOr:
            case OperationKind::kReduceXor:
            case OperationKind::kReduceNor:
            case OperationKind::kReduceNand:
            case OperationKind::kReduceXnor:
            case OperationKind::kAdd:
            case OperationKind::kSub:
            case OperationKind::kMul:
            case OperationKind::kDiv:
            case OperationKind::kMod:
            case OperationKind::kShl:
            case OperationKind::kLShr:
            case OperationKind::kAShr:
            case OperationKind::kConcat:
            case OperationKind::kReplicate:
            case OperationKind::kSliceStatic:
            case OperationKind::kSliceDynamic:
            case OperationKind::kSliceArray:
            case OperationKind::kConstant:
            case OperationKind::kRegisterReadPort:
            case OperationKind::kRegisterReadArrayPort:
                return true;
            default:
                return false;
            }
        }

        // ------------------------------------------------------------------
        // per-graph folder
        // ------------------------------------------------------------------

        class Folder
        {
        public:
            explicit Folder(Graph &graph) : graph_(graph) {}

            bool run()
            {
                foldReadTrees();
                foldWriteGroups();
                foldPadReads();
                sweepDead();
                return readTreesFolded_ + writeGroupsFolded_ + padReadsFolded_ > 0;
            }

            int64_t readTreesFolded() const { return readTreesFolded_; }
            int64_t writeGroupsFolded() const { return writeGroupsFolded_; }
            int64_t padReadsFolded() const { return padReadsFolded_; }
            int64_t sweptOps() const { return sweptOps_; }

        private:
            // ---- basic helpers ----

            OperationId defOf(ValueId value) const
            {
                if (!value.valid())
                {
                    return OperationId::invalid();
                }
                const OperationId def = graph_.valueDef(value);
                if (!def.valid() || erased_.count(def) != 0)
                {
                    return OperationId::invalid();
                }
                return def;
            }

            OperationKind defKind(ValueId value) const
            {
                const OperationId def = defOf(value);
                if (!def.valid())
                {
                    return OperationKind::kConstant; // sentinel: never matched
                }
                return graph_.opKind(def);
            }

            // Follow transparent aliases (kAssign, single-input kConcat).
            ValueId strip(ValueId value) const
            {
                for (int depth = 0; depth < 128 && value.valid(); ++depth)
                {
                    const OperationId def = defOf(value);
                    if (!def.valid())
                    {
                        return value;
                    }
                    const Operation op = graph_.getOperation(def);
                    if ((op.kind() == OperationKind::kAssign || op.kind() == OperationKind::kConcat) &&
                        op.operands().size() == 1)
                    {
                        value = op.operands()[0];
                        continue;
                    }
                    return value;
                }
                return value;
            }

            std::optional<std::string> asConstText(ValueId value) const
            {
                value = strip(value);
                const OperationId def = defOf(value);
                if (!def.valid() || graph_.opKind(def) != OperationKind::kConstant)
                {
                    return std::nullopt;
                }
                const Operation op = graph_.getOperation(def);
                const auto attr = op.attr("constValue");
                if (!attr)
                {
                    return std::nullopt;
                }
                if (const auto *text = std::get_if<std::string>(&*attr))
                {
                    return normalizeConstLiteral(*text);
                }
                return std::nullopt;
            }

            std::optional<uint64_t> asConstUint(ValueId value, int64_t *widthOut = nullptr) const
            {
                if (auto text = asConstText(value))
                {
                    int64_t width = -1;
                    if (auto parsed = parseConstLiteralUint(*text, width))
                    {
                        if (widthOut)
                        {
                            *widthOut = width;
                        }
                        return parsed;
                    }
                }
                return std::nullopt;
            }

            std::optional<EqConstMatch> asEqConst(ValueId value) const
            {
                value = strip(value);
                const OperationId def = defOf(value);
                if (!def.valid())
                {
                    return std::nullopt;
                }
                const OperationKind kind = graph_.opKind(def);
                if (kind != OperationKind::kEq && kind != OperationKind::kNe)
                {
                    return std::nullopt;
                }
                const Operation op = graph_.getOperation(def);
                if (op.operands().size() != 2)
                {
                    return std::nullopt;
                }
                const ValueId lhs = strip(op.operands()[0]);
                const ValueId rhs = strip(op.operands()[1]);
                const bool isEq = kind == OperationKind::kEq;
                if (auto rc = asConstUint(rhs))
                {
                    return EqConstMatch{lhs, *rc, isEq};
                }
                if (auto lc = asConstUint(lhs))
                {
                    return EqConstMatch{rhs, *lc, isEq};
                }
                return std::nullopt;
            }

            const RegisterInfo *findRegister(std::string_view regSym)
            {
                const auto it = regs_.find(std::string(regSym));
                if (it != regs_.end())
                {
                    return it->second.op.valid() ? &it->second : nullptr;
                }
                RegisterInfo info;
                const OperationId op = graph_.findOperation(regSym);
                if (op.valid() && erased_.count(op) == 0 &&
                    graph_.opKind(op) == OperationKind::kRegister)
                {
                    info.op = op;
                    const Operation regOp = graph_.getOperation(op);
                    if (const auto attr = regOp.attr("width"))
                    {
                        if (const auto *w = std::get_if<int64_t>(&*attr))
                        {
                            info.width = *w;
                        }
                    }
                    if (const auto attr = regOp.attr("shape"))
                    {
                        if (const auto *dims = std::get_if<std::vector<int64_t>>(&*attr))
                        {
                            info.shape = *dims;
                        }
                    }
                }
                // Only rank-1 array registers qualify.
                if (!info.op.valid() || info.width <= 0 || info.shape.size() != 1 ||
                    info.shape[0] <= 0 || info.width % info.shape[0] != 0 ||
                    info.shape[0] > (1 << 20))
                {
                    info = RegisterInfo{};
                }
                else
                {
                    info.elemCount = info.shape[0];
                    info.elemWidth = info.width / info.shape[0];
                }
                auto [slot, inserted] = regs_.emplace(std::string(regSym), std::move(info));
                (void)inserted;
                return slot->second.op.valid() ? &slot->second : nullptr;
            }

            // Element read of a rank-1 array register with a constant index:
            // kRegisterReadArrayPort(reg, c) | kSliceStatic(kRegisterReadPort reg)
            // | kSliceArray(kRegisterReadPort reg, c). Aliases already stripped.
            std::optional<ElemReadMatch> asElemRead(ValueId value, int64_t expectedWidth)
            {
                value = strip(value);
                const OperationId def = defOf(value);
                if (!def.valid())
                {
                    return std::nullopt;
                }
                const Operation op = graph_.getOperation(def);
                const OperationKind kind = op.kind();
                if (kind == OperationKind::kRegisterReadArrayPort)
                {
                    const auto indexCount = wolvrix::lib::grh::arrayPortIndexCount(op);
                    if (!indexCount || *indexCount != 1 || op.operands().size() != 1)
                    {
                        return std::nullopt;
                    }
                    const auto attr = op.attr("regSymbol");
                    if (!attr)
                    {
                        return std::nullopt;
                    }
                    const auto *sym = std::get_if<std::string>(&*attr);
                    if (!sym)
                    {
                        return std::nullopt;
                    }
                    const RegisterInfo *info = findRegister(*sym);
                    if (!info || (expectedWidth > 0 && info->elemWidth != expectedWidth))
                    {
                        return std::nullopt;
                    }
                    if (graph_.getValue(value).width() != info->elemWidth)
                    {
                        return std::nullopt;
                    }
                    const auto index = asConstUint(op.operands()[0]);
                    if (!index || *index >= static_cast<uint64_t>(info->elemCount))
                    {
                        return std::nullopt;
                    }
                    return ElemReadMatch{*sym, static_cast<int64_t>(*index)};
                }
                if (kind == OperationKind::kSliceStatic && op.operands().size() == 1)
                {
                    const auto startAttr = op.attr("sliceStart");
                    const auto endAttr = op.attr("sliceEnd");
                    if (!startAttr || !endAttr)
                    {
                        return std::nullopt;
                    }
                    const auto *startPtr = std::get_if<int64_t>(&*startAttr);
                    const auto *endPtr = std::get_if<int64_t>(&*endAttr);
                    if (!startPtr || !endPtr || *startPtr < 0 || *endPtr < *startPtr)
                    {
                        return std::nullopt;
                    }
                    const ValueId base = strip(op.operands()[0]);
                    const OperationId baseDef = defOf(base);
                    if (!baseDef.valid() || graph_.opKind(baseDef) != OperationKind::kRegisterReadPort)
                    {
                        return std::nullopt;
                    }
                    const Operation baseOp = graph_.getOperation(baseDef);
                    const auto attr = baseOp.attr("regSymbol");
                    if (!attr)
                    {
                        return std::nullopt;
                    }
                    const auto *sym = std::get_if<std::string>(&*attr);
                    if (!sym)
                    {
                        return std::nullopt;
                    }
                    const RegisterInfo *info = findRegister(*sym);
                    if (!info)
                    {
                        return std::nullopt;
                    }
                    const int64_t width = *endPtr - *startPtr + 1;
                    if (width != info->elemWidth || *startPtr % info->elemWidth != 0)
                    {
                        return std::nullopt;
                    }
                    const int64_t index = *startPtr / info->elemWidth;
                    if (index >= info->elemCount)
                    {
                        return std::nullopt;
                    }
                    return ElemReadMatch{*sym, index};
                }
                if (kind == OperationKind::kSliceArray && op.operands().size() == 2)
                {
                    const ValueId base = strip(op.operands()[0]);
                    const OperationId baseDef = defOf(base);
                    if (!baseDef.valid() || graph_.opKind(baseDef) != OperationKind::kRegisterReadPort)
                    {
                        return std::nullopt;
                    }
                    const Operation baseOp = graph_.getOperation(baseDef);
                    const auto attr = baseOp.attr("regSymbol");
                    if (!attr)
                    {
                        return std::nullopt;
                    }
                    const auto *sym = std::get_if<std::string>(&*attr);
                    if (!sym)
                    {
                        return std::nullopt;
                    }
                    const RegisterInfo *info = findRegister(*sym);
                    if (!info || (expectedWidth > 0 && info->elemWidth != expectedWidth))
                    {
                        return std::nullopt;
                    }
                    if (graph_.getValue(value).width() != info->elemWidth)
                    {
                        return std::nullopt;
                    }
                    const auto index = asConstUint(op.operands()[1]);
                    if (!index || *index >= static_cast<uint64_t>(info->elemCount))
                    {
                        return std::nullopt;
                    }
                    return ElemReadMatch{*sym, static_cast<int64_t>(*index)};
                }
                return std::nullopt;
            }

            // Structural value equivalence with bounded depth: two separately
            // instantiated but identical expression trees compare equal.
            bool sameValue(ValueId lhs, ValueId rhs, int depth) const
            {
                lhs = strip(lhs);
                rhs = strip(rhs);
                if (lhs == rhs)
                {
                    return true;
                }
                if (depth <= 0)
                {
                    return false;
                }
                const OperationId lhsDef = defOf(lhs);
                const OperationId rhsDef = defOf(rhs);
                if (!lhsDef.valid() || !rhsDef.valid())
                {
                    return false;
                }
                if (graph_.opKind(lhsDef) != graph_.opKind(rhsDef))
                {
                    return false;
                }
                const auto lhsAttrs = graph_.opAttrs(lhsDef);
                const auto rhsAttrs = graph_.opAttrs(rhsDef);
                if (lhsAttrs.size() != rhsAttrs.size())
                {
                    return false;
                }
                for (std::size_t i = 0; i < lhsAttrs.size(); ++i)
                {
                    if (lhsAttrs[i].key != rhsAttrs[i].key || !(lhsAttrs[i].value == rhsAttrs[i].value))
                    {
                        return false;
                    }
                }
                const Operation lhsOp = graph_.getOperation(lhsDef);
                const Operation rhsOp = graph_.getOperation(rhsDef);
                if (lhsOp.operands().size() != rhsOp.operands().size())
                {
                    return false;
                }
                for (std::size_t i = 0; i < lhsOp.operands().size(); ++i)
                {
                    if (!sameValue(lhsOp.operands()[i], rhsOp.operands()[i], depth - 1))
                    {
                        return false;
                    }
                }
                return true;
            }

            // ---- creators ----

            ValueId makeConst(uint64_t value, int64_t width)
            {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%lld'h%llx",
                              static_cast<long long>(width),
                              static_cast<unsigned long long>(value));
                const ValueId result = graph_.createValue(
                    graph_.makeInternalValSym(), static_cast<int32_t>(width), false);
                const OperationId op = graph_.createOperation(OperationKind::kConstant,
                                                              graph_.makeInternalOpSym());
                graph_.addResult(op, result);
                graph_.setAttr(op, "constValue", std::string(buf));
                return result;
            }

            ValueId makeArrayRead(const std::string &regSym, ValueId index, int64_t elemWidth)
            {
                const ValueId result = graph_.createValue(
                    graph_.makeInternalValSym(), static_cast<int32_t>(elemWidth), false);
                const OperationId op = graph_.createOperation(
                    OperationKind::kRegisterReadArrayPort, graph_.makeInternalOpSym());
                graph_.addOperand(op, index);
                graph_.addResult(op, result);
                graph_.setAttr(op, "regSymbol", regSym);
                return result;
            }

            // idx < limit ? idx : fallback (index-space clamp, all width pw)
            ValueId makeIndexClamp(ValueId idx, int64_t limit, int64_t fallback, int64_t pw)
            {
                const ValueId limitConst = makeConst(static_cast<uint64_t>(limit), pw);
                const ValueId ltResult = graph_.createValue(graph_.makeInternalValSym(), 1, false);
                const OperationId lt = graph_.createOperation(OperationKind::kLt,
                                                              graph_.makeInternalOpSym());
                graph_.addOperand(lt, idx);
                graph_.addOperand(lt, limitConst);
                graph_.addResult(lt, ltResult);
                const ValueId fallbackConst = makeConst(static_cast<uint64_t>(fallback), pw);
                const ValueId muxResult = graph_.createValue(
                    graph_.makeInternalValSym(), static_cast<int32_t>(pw), false);
                const OperationId mux = graph_.createOperation(OperationKind::kMux,
                                                               graph_.makeInternalOpSym());
                graph_.addOperand(mux, ltResult);
                graph_.addOperand(mux, idx);
                graph_.addOperand(mux, fallbackConst);
                graph_.addResult(mux, muxResult);
                return muxResult;
            }

            // ---- pattern R: constant-index read mux tree -> dynamic read ----

            void foldReadTrees()
            {
                std::vector<OperationId> muxOps;
                for (const OperationId id : graph_.operations())
                {
                    if (id.valid() && erased_.count(id) == 0 &&
                        graph_.opKind(id) == OperationKind::kMux)
                    {
                        muxOps.push_back(id);
                    }
                }
                for (const OperationId root : muxOps)
                {
                    if (erased_.count(root) != 0)
                    {
                        continue;
                    }
                    const Operation rootOp = graph_.getOperation(root);
                    if (rootOp.results().size() != 1 || rootOp.operands().size() != 3)
                    {
                        continue;
                    }
                    const ValueId rootResult = rootOp.results()[0];
                    if (foldedTreeResults_.count(rootResult) != 0)
                    {
                        continue; // inner node of an already folded tree
                    }
                    if (graph_.valueIsOutput(rootResult) || graph_.valueIsInout(rootResult))
                    {
                        continue;
                    }
                    if (!graph_.valueShape(rootResult).empty())
                    {
                        continue;
                    }
                    const int64_t elemWidth = graph_.getValue(rootResult).width();
                    if (elemWidth <= 0)
                    {
                        continue;
                    }

                    std::vector<uint64_t> conds;
                    std::vector<ValueId> treeResults;
                    ValueId ptr;
                    std::string regSym;
                    const RegisterInfo *info = nullptr;
                    int64_t tailIndex = -1;
                    bool ok = true;
                    OperationId cur = root;
                    while (true)
                    {
                        const Operation curOp = graph_.getOperation(cur);
                        if (curOp.operands().size() != 3)
                        {
                            ok = false;
                            break;
                        }
                        const auto eqc = asEqConst(curOp.operands()[0]);
                        if (!eqc)
                        {
                            ok = false;
                            break;
                        }
                        if (conds.empty())
                        {
                            ptr = strip(eqc->ptr);
                            if (graph_.valueSigned(ptr) || !graph_.valueShape(ptr).empty())
                            {
                                ok = false;
                                break;
                            }
                        }
                        else if (!sameValue(eqc->ptr, ptr, 4))
                        {
                            ok = false;
                            break;
                        }
                        const ValueId leafValue = eqc->isEq ? curOp.operands()[1] : curOp.operands()[2];
                        const ValueId contValue = eqc->isEq ? curOp.operands()[2] : curOp.operands()[1];
                        const auto leaf = asElemRead(leafValue, elemWidth);
                        if (!leaf || static_cast<uint64_t>(leaf->index) != eqc->constant)
                        {
                            ok = false;
                            break;
                        }
                        if (regSym.empty())
                        {
                            regSym = leaf->regSym;
                            info = findRegister(regSym);
                            if (!info || info->elemWidth != elemWidth)
                            {
                                ok = false;
                                break;
                            }
                        }
                        else if (leaf->regSym != regSym)
                        {
                            ok = false;
                            break;
                        }
                        conds.push_back(eqc->constant);
                        treeResults.push_back(curOp.results()[0]);
                        if (conds.size() > (1 << 16))
                        {
                            ok = false;
                            break;
                        }
                        const ValueId contStripped = strip(contValue);
                        const OperationId next = defOf(contStripped);
                        if (next.valid() && graph_.opKind(next) == OperationKind::kMux)
                        {
                            cur = next;
                            continue;
                        }
                        const auto tail = asElemRead(contStripped, elemWidth);
                        if (!tail || tail->regSym != regSym)
                        {
                            ok = false;
                            break;
                        }
                        tailIndex = tail->index;
                        break;
                    }
                    if (!ok || !info || conds.empty())
                    {
                        continue;
                    }
                    const int64_t elemCount = info->elemCount;
                    if (static_cast<int64_t>(conds.size()) + 1 != elemCount)
                    {
                        continue; // full coverage required
                    }
                    std::vector<uint8_t> seen(static_cast<std::size_t>(elemCount), 0);
                    bool covered = true;
                    for (const uint64_t c : conds)
                    {
                        if (c >= static_cast<uint64_t>(elemCount) || seen[c] != 0)
                        {
                            covered = false;
                            break;
                        }
                        seen[c] = 1;
                    }
                    if (!covered || tailIndex < 0 || tailIndex >= elemCount || seen[tailIndex] != 0)
                    {
                        continue;
                    }
                    const int64_t pw = graph_.getValue(ptr).width();
                    if (pw <= 0 || pw > 62)
                    {
                        continue;
                    }
                    ValueId index = ptr;
                    if ((uint64_t{1} << pw) > static_cast<uint64_t>(elemCount))
                    {
                        index = makeIndexClamp(ptr, elemCount, tailIndex, pw);
                    }
                    const ValueId read = makeArrayRead(regSym, index, elemWidth);
                    graph_.replaceAllUses(rootResult, read);
                    for (const ValueId v : treeResults)
                    {
                        foldedTreeResults_.insert(v);
                    }
                    sweepSeeds_.push_back(root);
                    ++readTreesFolded_;
                }
            }

            // ---- pattern W: per-element write decoder -> dynamic write ports ----

            struct WriteLevel
            {
                ValueId enableTerm; // invalid when the condition is a bare equality
                ValueId ptr;
                ValueId data;
            };

            struct ChainWrite
            {
                OperationId op;
                ValueId enable;
                std::vector<WriteLevel> levels;
                bool tailHold = false;
                ValueId tailConstVal;
                std::string tailConstText;
            };

            struct ConstWrite
            {
                OperationId op;
                ValueId enable;
                ValueId dataVal;
                std::string text;
            };

            void foldWriteGroups()
            {
                std::map<std::string, std::vector<OperationId>> groups;
                for (const OperationId id : graph_.operations())
                {
                    if (!id.valid() || erased_.count(id) != 0)
                    {
                        continue;
                    }
                    if (!wolvrix::lib::grh::isRegisterWritePortKind(graph_.opKind(id)))
                    {
                        continue;
                    }
                    const Operation op = graph_.getOperation(id);
                    const auto attr = op.attr("regSymbol");
                    if (!attr)
                    {
                        continue;
                    }
                    if (const auto *sym = std::get_if<std::string>(&*attr))
                    {
                        groups[*sym].push_back(id);
                    }
                }
                for (auto &[sym, ops] : groups)
                {
                    foldOneWriteGroup(sym, ops);
                }
                if (std::getenv("WOLVRIX_MUXFOLD_DEBUG") != nullptr)
                {
                    for (auto &[sym, ops] : groups)
                    {
                        if (ops.size() >= 32)
                        {
                            debugAnalyzeGroup(sym, ops);
                        }
                    }
                }
            }

            // Debug-only (WOLVRIX_MUXFOLD_DEBUG): report why a large write
            // group did not fold — entry coverage, chain/const split, and the
            // first condition/tail form that is not a plain decoder.
            void debugAnalyzeGroup(const std::string &regSym, const std::vector<OperationId> &ops)
            {
                const RegisterInfo *info = findRegister(regSym);
                const int64_t elemCount = info ? info->elemCount : -1;
                std::unordered_set<int64_t> entries;
                std::string firstFail;
                int64_t chains = 0;
                int64_t consts = 0;
                int64_t cleanChains = 0;
                bool hasFoldedMarker = false;
                for (const OperationId id : ops)
                {
                    if (erased_.count(id) != 0)
                    {
                        continue;
                    }
                    const Operation op = graph_.getOperation(id);
                    if (op.kind() != OperationKind::kRegisterWriteArrayPort)
                    {
                        firstFail = std::string("non-array-write:") +
                                    std::string(wolvrix::lib::grh::toString(op.kind()));
                        break;
                    }
                    if (op.operands().size() < 4)
                    {
                        firstFail = "arity";
                        break;
                    }
                    const auto idxC = asConstUint(op.operands()[1]);
                    if (!idxC)
                    {
                        hasFoldedMarker = true; // already-dynamic write port
                        continue;
                    }
                    entries.insert(static_cast<int64_t>(*idxC));
                    const ValueId data = strip(op.operands()[2]);
                    if (asConstText(data))
                    {
                        ++consts;
                        continue;
                    }
                    ++chains;
                    std::string thisFail;
                    ValueId cur = data;
                    while (true)
                    {
                        const OperationId curDef = defOf(cur);
                        if (!curDef.valid() || graph_.opKind(curDef) != OperationKind::kMux)
                        {
                            break;
                        }
                        const Operation muxOp = graph_.getOperation(curDef);
                        if (muxOp.operands().size() != 3)
                        {
                            thisFail = "mux-arity";
                            break;
                        }
                        const ValueId cond = strip(muxOp.operands()[0]);
                        if (asEqConst(cond))
                        {
                            cur = strip(muxOp.operands()[2]);
                            continue;
                        }
                        const OperationId condDef = defOf(cond);
                        bool andWithEq = false;
                        if (condDef.valid() && graph_.opKind(condDef) == OperationKind::kAnd)
                        {
                            const Operation andOp = graph_.getOperation(condDef);
                            for (const ValueId a : andOp.operands())
                            {
                                if (asEqConst(a))
                                {
                                    andWithEq = true;
                                    break;
                                }
                            }
                        }
                        if (andWithEq)
                        {
                            cur = strip(muxOp.operands()[2]);
                            continue;
                        }
                        thisFail = std::string("cond:") +
                                   (condDef.valid()
                                        ? std::string(wolvrix::lib::grh::toString(graph_.opKind(condDef)))
                                        : std::string("leaf"));
                        break;
                    }
                    if (thisFail.empty())
                    {
                        if (asElemRead(cur, info ? info->elemWidth : 0))
                        {
                            ++cleanChains;
                        }
                        else if (asConstText(cur))
                        {
                            ++cleanChains;
                        }
                        else
                        {
                            const OperationId tailDef = defOf(cur);
                            thisFail = std::string("tail:") +
                                       (tailDef.valid()
                                            ? std::string(wolvrix::lib::grh::toString(graph_.opKind(tailDef)))
                                            : std::string("leaf"));
                        }
                    }
                    if (firstFail.empty() && !thisFail.empty())
                    {
                        firstFail = thisFail;
                    }
                }
                if (firstFail.empty() && chains > 0 && cleanChains == chains)
                {
                    firstFail = "uniformity-or-coverage";
                }
                debugLines.push_back(
                    "mux-tree-fold group [" + regSym + "] writes=" + std::to_string(ops.size()) +
                    " entries=" + std::to_string(entries.size()) + "/" + std::to_string(elemCount) +
                    " consts=" + std::to_string(consts) + " chains=" + std::to_string(chains) +
                    (hasFoldedMarker ? " [dyn-present]" : "") + " first=" + firstFail);
            }

            void foldOneWriteGroup(const std::string &regSym, const std::vector<OperationId> &ops)
            {
                const RegisterInfo *info = findRegister(regSym);
                if (!info)
                {
                    return;
                }
                const int64_t elemCount = info->elemCount;
                const int64_t elemWidth = info->elemWidth;
                if (elemCount < 2 || elemCount > (1 << 16) ||
                    static_cast<int64_t>(ops.size()) < elemCount)
                {
                    return;
                }

                std::vector<std::optional<ChainWrite>> chainByEntry(static_cast<std::size_t>(elemCount));
                std::vector<std::optional<ConstWrite>> constByEntry(static_cast<std::size_t>(elemCount));
                std::vector<ValueId> events;
                std::optional<std::vector<std::string>> eventEdges;
                bool signatureKnown = false;

                for (const OperationId id : ops)
                {
                    const Operation op = graph_.getOperation(id);
                    if (op.kind() != OperationKind::kRegisterWriteArrayPort)
                    {
                        return; // non-array write in the group: leave the register alone
                    }
                    const auto indexCount = wolvrix::lib::grh::arrayPortIndexCount(op);
                    if (!indexCount || *indexCount != 1)
                    {
                        return;
                    }
                    const auto eventStart = wolvrix::lib::grh::firstEventOperandIndex(op);
                    if (!eventStart || *eventStart != 3)
                    {
                        return; // [updateCond, i0, data, events...] layout required
                    }
                    const auto edgeAttr = op.attr("eventEdge");
                    if (!edgeAttr)
                    {
                        return;
                    }
                    const auto *edges = std::get_if<std::vector<std::string>>(&*edgeAttr);
                    if (!edges)
                    {
                        return;
                    }
                    std::vector<ValueId> opEvents(op.operands().begin() + *eventStart,
                                                  op.operands().end());
                    if (opEvents.size() != edges->size())
                    {
                        return;
                    }
                    if (!signatureKnown)
                    {
                        events = opEvents;
                        eventEdges = *edges;
                        signatureKnown = true;
                    }
                    else
                    {
                        if (opEvents.size() != events.size() || *edges != *eventEdges)
                        {
                            return;
                        }
                        bool sameEvents = true;
                        for (std::size_t i = 0; i < events.size(); ++i)
                        {
                            if (!sameValue(opEvents[i], events[i], 2))
                            {
                                sameEvents = false;
                                break;
                            }
                        }
                        if (!sameEvents)
                        {
                            return;
                        }
                    }
                    const auto indexConst = asConstUint(op.operands()[1]);
                    if (!indexConst || *indexConst >= static_cast<uint64_t>(elemCount))
                    {
                        return;
                    }
                    const int64_t entry = static_cast<int64_t>(*indexConst);
                    const ValueId enable = strip(op.operands()[0]);
                    const ValueId data = strip(op.operands()[2]);

                    if (const auto constText = asConstText(data))
                    {
                        if (graph_.getValue(data).width() != elemWidth || constByEntry[entry])
                        {
                            return;
                        }
                        constByEntry[entry] = ConstWrite{id, enable, data, *constText};
                        continue;
                    }

                    ChainWrite chain;
                    chain.op = id;
                    chain.enable = enable;
                    ValueId cur = data;
                    while (true)
                    {
                        const OperationId curDef = defOf(cur);
                        if (!curDef.valid() || graph_.opKind(curDef) != OperationKind::kMux)
                        {
                            break;
                        }
                        const Operation muxOp = graph_.getOperation(curDef);
                        if (muxOp.operands().size() != 3)
                        {
                            return;
                        }
                        const ValueId condValue = strip(muxOp.operands()[0]);
                        auto eqc = asEqConst(condValue);
                        ValueId enableTerm;
                        if (!eqc)
                        {
                            const OperationId condDef = defOf(condValue);
                            if (!condDef.valid() || graph_.opKind(condDef) != OperationKind::kAnd)
                            {
                                return;
                            }
                            const Operation andOp = graph_.getOperation(condDef);
                            if (andOp.operands().size() != 2)
                            {
                                return;
                            }
                            eqc = asEqConst(andOp.operands()[0]);
                            if (eqc)
                            {
                                enableTerm = strip(andOp.operands()[1]);
                            }
                            else
                            {
                                eqc = asEqConst(andOp.operands()[1]);
                                if (eqc)
                                {
                                    enableTerm = strip(andOp.operands()[0]);
                                }
                            }
                            if (!eqc)
                            {
                                return;
                            }
                        }
                        if (!eqc->isEq || static_cast<int64_t>(eqc->constant) != entry)
                        {
                            return; // condition must decode this entry's address
                        }
                        if (graph_.valueSigned(strip(eqc->ptr)) ||
                            !graph_.valueShape(strip(eqc->ptr)).empty())
                        {
                            return;
                        }
                        chain.levels.push_back(WriteLevel{enableTerm, strip(eqc->ptr),
                                                          strip(muxOp.operands()[1])});
                        if (chain.levels.size() > 64)
                        {
                            return;
                        }
                        cur = strip(muxOp.operands()[2]);
                    }
                    if (chain.levels.empty() || chainByEntry[entry])
                    {
                        return;
                    }
                    if (const auto hold = asElemRead(cur, elemWidth))
                    {
                        if (hold->regSym != regSym || hold->index != entry)
                        {
                            return; // hold branch must read back the same element
                        }
                        chain.tailHold = true;
                    }
                    else if (const auto tailText = asConstText(cur))
                    {
                        if (graph_.getValue(cur).width() != elemWidth)
                        {
                            return;
                        }
                        chain.tailHold = false;
                        chain.tailConstVal = cur;
                        chain.tailConstText = *tailText;
                    }
                    else
                    {
                        return;
                    }
                    chainByEntry[entry] = std::move(chain);
                }

                if (!signatureKnown)
                {
                    return;
                }
                std::size_t chainCount = 0;
                std::size_t constCount = 0;
                for (const auto &slot : chainByEntry)
                {
                    if (slot)
                    {
                        ++chainCount;
                    }
                }
                for (const auto &slot : constByEntry)
                {
                    if (slot)
                    {
                        ++constCount;
                    }
                }
                if (chainCount != static_cast<std::size_t>(elemCount) ||
                    (constCount != 0 && constCount != static_cast<std::size_t>(elemCount)))
                {
                    return;
                }

                // Uniformity across entries: the reference chain (entry 0) sets
                // the port list; every other entry must match structurally.
                const ChainWrite &ref = *chainByEntry[0];
                const std::size_t portCount = ref.levels.size();
                for (const auto &slot : chainByEntry)
                {
                    const ChainWrite &chain = *slot;
                    if (!sameValue(chain.enable, ref.enable, 4) ||
                        chain.levels.size() != portCount)
                    {
                        return;
                    }
                    for (std::size_t i = 0; i < portCount; ++i)
                    {
                        const WriteLevel &level = chain.levels[i];
                        const WriteLevel &refLevel = ref.levels[i];
                        if (level.enableTerm.valid() != refLevel.enableTerm.valid())
                        {
                            return;
                        }
                        if (level.enableTerm.valid() &&
                            !sameValue(level.enableTerm, refLevel.enableTerm, 6))
                        {
                            return;
                        }
                        if (!sameValue(level.ptr, refLevel.ptr, 6) ||
                            !sameValue(level.data, refLevel.data, 6))
                        {
                            return;
                        }
                    }
                    if (chain.tailHold != ref.tailHold)
                    {
                        return;
                    }
                    if (!chain.tailHold && chain.tailConstText != ref.tailConstText)
                    {
                        return;
                    }
                }
                ValueId resetEnable;
                ValueId resetData;
                if (constCount != 0)
                {
                    resetEnable = constByEntry[0]->enable;
                    resetData = constByEntry[0]->dataVal;
                    const std::string &resetText = constByEntry[0]->text;
                    for (const auto &slot : constByEntry)
                    {
                        if (!sameValue(slot->enable, resetEnable, 4) || slot->text != resetText)
                        {
                            return;
                        }
                    }
                }

                // ---- rewrite ----
                for (const auto &slot : chainByEntry)
                {
                    const Operation op = graph_.getOperation(slot->op);
                    for (const ValueId operand : op.operands())
                    {
                        const OperationId producer = defOf(operand);
                        if (producer.valid())
                        {
                            sweepSeeds_.push_back(producer);
                        }
                    }
                    erased_.insert(slot->op);
                    graph_.eraseOp(slot->op);
                }
                for (const auto &slot : constByEntry)
                {
                    if (!slot)
                    {
                        continue;
                    }
                    const Operation op = graph_.getOperation(slot->op);
                    for (const ValueId operand : op.operands())
                    {
                        const OperationId producer = defOf(operand);
                        if (producer.valid())
                        {
                            sweepSeeds_.push_back(producer);
                        }
                    }
                    erased_.insert(slot->op);
                    graph_.eraseOp(slot->op);
                }

                bool enableIsOne = false;
                if (const auto c = asConstUint(ref.enable))
                {
                    int64_t enableWidth = graph_.getValue(ref.enable).width();
                    enableIsOne = *c == 1 && enableWidth == 1;
                }

                auto makeEvents = [this, &events, &eventEdges](OperationId op)
                {
                    for (const ValueId event : events)
                    {
                        graph_.addOperand(op, event);
                    }
                    graph_.setAttr(op, "eventEdge", *eventEdges);
                };

                // Lowest priority first: const-default fill (zero-default form).
                if (!ref.tailHold)
                {
                    const OperationId fill = graph_.createOperation(
                        OperationKind::kRegisterFillPort, graph_.makeInternalOpSym());
                    graph_.addOperand(fill, ref.enable);
                    graph_.addOperand(fill, ref.tailConstVal);
                    makeEvents(fill);
                    graph_.setAttr(fill, "regSymbol", regSym);
                }
                // Dynamic ports: innermost chain level first (declaration order
                // = priority; the outermost mux level wins collisions).
                for (std::size_t i = portCount; i-- > 0;)
                {
                    const WriteLevel &level = ref.levels[i];
                    ValueId enable = ref.enable;
                    if (level.enableTerm.valid())
                    {
                        if (enableIsOne)
                        {
                            enable = level.enableTerm;
                        }
                        else
                        {
                            enable = graph_.createValue(graph_.makeInternalValSym(), 1, false);
                            const OperationId andOp = graph_.createOperation(
                                OperationKind::kAnd, graph_.makeInternalOpSym());
                            graph_.addOperand(andOp, ref.enable);
                            graph_.addOperand(andOp, level.enableTerm);
                            graph_.addResult(andOp, enable);
                        }
                    }
                    const OperationId write = graph_.createOperation(
                        OperationKind::kRegisterWriteArrayPort, graph_.makeInternalOpSym());
                    graph_.addOperand(write, enable);
                    graph_.addOperand(write, level.ptr);
                    graph_.addOperand(write, level.data);
                    makeEvents(write);
                    graph_.setAttr(write, "regSymbol", regSym);
                }
                // Highest priority: uniform const (reset) fill.
                if (constCount != 0)
                {
                    const OperationId fill = graph_.createOperation(
                        OperationKind::kRegisterFillPort, graph_.makeInternalOpSym());
                    graph_.addOperand(fill, resetEnable);
                    graph_.addOperand(fill, resetData);
                    makeEvents(fill);
                    graph_.setAttr(fill, "regSymbol", regSym);
                }
                ++writeGroupsFolded_;
            }

            // ---- pattern P: padded-concat dynamic read -> clamped array read ----

            // Map a concat input (an element-width slice) to its source register
            // element; returns the element index, or -1 when unrecognized.
            // Constant inputs return -2 with constOut set (zero-pad form).
            int64_t traceElementSource(ValueId input, int64_t elemWidth,
                                       std::string &regSym, const RegisterInfo *&info,
                                       ValueId &constOut)
            {
                const ValueId stripped = strip(input);
                if (const auto constText = asConstText(stripped))
                {
                    if (graph_.getValue(stripped).width() == elemWidth)
                    {
                        constOut = stripped;
                        return -2;
                    }
                    return -1;
                }
                if (const auto elem = asElemRead(stripped, elemWidth))
                {
                    if (regSym.empty())
                    {
                        regSym = elem->regSym;
                        info = findRegister(regSym);
                        if (!info)
                        {
                            return -1;
                        }
                    }
                    else if (elem->regSym != regSym)
                    {
                        return -1;
                    }
                    return elem->index;
                }
                // slice of a replicated assembled element: {K{entry}}[c].field
                const OperationId def = defOf(stripped);
                if (!def.valid() || graph_.opKind(def) != OperationKind::kSliceStatic)
                {
                    return -1;
                }
                const Operation sliceOp = graph_.getOperation(def);
                if (sliceOp.operands().size() != 1)
                {
                    return -1;
                }
                const auto startAttr = sliceOp.attr("sliceStart");
                const auto endAttr = sliceOp.attr("sliceEnd");
                if (!startAttr || !endAttr)
                {
                    return -1;
                }
                const auto *startPtr = std::get_if<int64_t>(&*startAttr);
                const auto *endPtr = std::get_if<int64_t>(&*endAttr);
                if (!startPtr || !endPtr || *startPtr < 0 || *endPtr - *startPtr + 1 != elemWidth)
                {
                    return -1;
                }
                const ValueId repValue = strip(sliceOp.operands()[0]);
                const OperationId repDef = defOf(repValue);
                if (!repDef.valid() || graph_.opKind(repDef) != OperationKind::kReplicate)
                {
                    return -1;
                }
                const Operation repOp = graph_.getOperation(repDef);
                if (repOp.operands().size() != 1)
                {
                    return -1;
                }
                const ValueId assembled = strip(repOp.operands()[0]);
                const int64_t assembledWidth = graph_.getValue(assembled).width();
                if (assembledWidth <= 0 || elemWidth > assembledWidth)
                {
                    return -1;
                }
                const int64_t fieldOffset = *startPtr % assembledWidth;
                const ValueId leaf = findLeafAt(assembled, fieldOffset, fieldOffset + elemWidth);
                if (!leaf.valid())
                {
                    return -1;
                }
                if (const auto constText = asConstText(leaf))
                {
                    (void)constText;
                    constOut = leaf;
                    return -2;
                }
                if (const auto elem = asElemRead(leaf, elemWidth))
                {
                    if (regSym.empty())
                    {
                        regSym = elem->regSym;
                        info = findRegister(regSym);
                        if (!info)
                        {
                            return -1;
                        }
                    }
                    else if (elem->regSym != regSym)
                    {
                        return -1;
                    }
                    return elem->index;
                }
                return -1;
            }

            // Find the concat leaf covering [lo, hi) exactly within value's flat
            // layout; invalid when the range does not align to a single leaf.
            ValueId findLeafAt(ValueId value, int64_t lo, int64_t hi)
            {
                value = strip(value);
                const int64_t width = graph_.getValue(value).width();
                if (width < hi || lo < 0)
                {
                    return ValueId::invalid();
                }
                const OperationId def = defOf(value);
                if (def.valid() && graph_.opKind(def) == OperationKind::kConcat)
                {
                    const Operation op = graph_.getOperation(def);
                    int64_t offset = width;
                    for (const ValueId child : op.operands())
                    {
                        const int64_t childWidth = graph_.getValue(strip(child)).width();
                        offset -= childWidth;
                        if (lo >= offset && hi <= offset + childWidth)
                        {
                            return findLeafAt(child, lo - offset, hi - offset);
                        }
                    }
                    return ValueId::invalid();
                }
                if (lo == 0 && hi == width)
                {
                    return value;
                }
                return ValueId::invalid();
            }

            void foldPadReads()
            {
                std::vector<OperationId> candidates;
                for (const OperationId id : graph_.operations())
                {
                    if (id.valid() && erased_.count(id) == 0 &&
                        graph_.opKind(id) == OperationKind::kSliceArray)
                    {
                        candidates.push_back(id);
                    }
                }
                for (const OperationId id : candidates)
                {
                    if (erased_.count(id) != 0)
                    {
                        continue;
                    }
                    const Operation op = graph_.getOperation(id);
                    if (op.operands().size() != 2 || op.results().size() != 1)
                    {
                        continue;
                    }
                    const ValueId result = op.results()[0];
                    if (graph_.valueIsOutput(result) || graph_.valueIsInout(result) ||
                        !graph_.valueShape(result).empty())
                    {
                        continue;
                    }
                    const int64_t elemWidth = graph_.getValue(result).width();
                    if (elemWidth <= 0)
                    {
                        continue;
                    }
                    ValueId index = strip(op.operands()[1]);
                    if (graph_.valueSigned(index) || !graph_.valueShape(index).empty())
                    {
                        continue;
                    }
                    const ValueId base = strip(op.operands()[0]);
                    const OperationId baseDef = defOf(base);
                    if (!baseDef.valid())
                    {
                        continue;
                    }
                    if (graph_.opKind(baseDef) == OperationKind::kRegisterReadPort)
                    {
                        // dynamic read directly on a whole-register read
                        const Operation baseOp = graph_.getOperation(baseDef);
                        const auto attr = baseOp.attr("regSymbol");
                        if (!attr)
                        {
                            continue;
                        }
                        const auto *sym = std::get_if<std::string>(&*attr);
                        if (!sym)
                        {
                            continue;
                        }
                        const RegisterInfo *info = findRegister(*sym);
                        if (!info || info->elemWidth != elemWidth)
                        {
                            continue;
                        }
                        const ValueId read = makeArrayRead(*sym, index, elemWidth);
                        graph_.replaceAllUses(result, read);
                        sweepSeeds_.push_back(id);
                        ++padReadsFolded_;
                        continue;
                    }
                    if (graph_.opKind(baseDef) != OperationKind::kConcat)
                    {
                        continue;
                    }
                    const Operation concat = graph_.getOperation(baseDef);
                    const int64_t concatInputs = static_cast<int64_t>(concat.operands().size());
                    if (concatInputs < 2)
                    {
                        continue;
                    }
                    // element map: concat input 0 is the highest element
                    std::vector<int64_t> elementMap(static_cast<std::size_t>(concatInputs), -1);
                    std::vector<ValueId> constMap(static_cast<std::size_t>(concatInputs));
                    std::string regSym;
                    const RegisterInfo *info = nullptr;
                    bool ok = true;
                    for (int64_t j = 0; j < concatInputs; ++j)
                    {
                        const int64_t element = concatInputs - 1 - j;
                        ValueId constVal;
                        const int64_t source = traceElementSource(
                            concat.operands()[static_cast<std::size_t>(j)], elemWidth,
                            regSym, info, constVal);
                        if (source == -1)
                        {
                            ok = false;
                            break;
                        }
                        elementMap[static_cast<std::size_t>(element)] = source;
                        constMap[static_cast<std::size_t>(element)] = constVal;
                    }
                    if (!ok || !info)
                    {
                        continue;
                    }
                    const int64_t elemCount = info->elemCount;
                    auto samePad = [&](int64_t from) -> std::optional<int64_t>
                    {
                        // all entries [from, concatInputs) identical?
                        const int64_t firstElem = elementMap[static_cast<std::size_t>(from)];
                        const ValueId firstConst = constMap[static_cast<std::size_t>(from)];
                        for (int64_t k = from + 1; k < concatInputs; ++k)
                        {
                            const int64_t elem = elementMap[static_cast<std::size_t>(k)];
                            const ValueId cst = constMap[static_cast<std::size_t>(k)];
                            if (firstElem == -2)
                            {
                                if (elem != -2 || !sameValue(cst, firstConst, 2))
                                {
                                    return std::nullopt;
                                }
                            }
                            else if (elem != firstElem)
                            {
                                return std::nullopt;
                            }
                        }
                        return firstElem;
                    };
                    // identity map: concat == whole array reassembled
                    if (concatInputs == elemCount)
                    {
                        bool identity = true;
                        for (int64_t k = 0; k < concatInputs; ++k)
                        {
                            if (elementMap[static_cast<std::size_t>(k)] != k)
                            {
                                identity = false;
                                break;
                            }
                        }
                        if (!identity)
                        {
                            continue;
                        }
                        const ValueId read = makeArrayRead(regSym, index, elemWidth);
                        graph_.replaceAllUses(result, read);
                        sweepSeeds_.push_back(id);
                        sweepSeeds_.push_back(baseDef);
                        ++padReadsFolded_;
                        continue;
                    }
                    if (concatInputs <= elemCount)
                    {
                        continue;
                    }
                    // padded: identity on [0, N), uniform pad on [N, M)
                    bool padOk = true;
                    for (int64_t k = 0; k < elemCount; ++k)
                    {
                        if (elementMap[static_cast<std::size_t>(k)] != k)
                        {
                            padOk = false;
                            break;
                        }
                    }
                    if (!padOk)
                    {
                        continue;
                    }
                    const auto padSource = samePad(elemCount);
                    if (!padSource)
                    {
                        continue;
                    }
                    const int64_t pw = graph_.getValue(index).width();
                    if (pw <= 0 || pw > 62)
                    {
                        continue;
                    }
                    ValueId folded;
                    if (*padSource == -2)
                    {
                        // zero/const pad: data-side mux  idx < N ? arr[idx] : padConst
                        const ValueId directRead = makeArrayRead(regSym, index, elemWidth);
                        const ValueId limitConst = makeConst(static_cast<uint64_t>(elemCount), pw);
                        const ValueId ltResult = graph_.createValue(
                            graph_.makeInternalValSym(), 1, false);
                        const OperationId lt = graph_.createOperation(OperationKind::kLt,
                                                                      graph_.makeInternalOpSym());
                        graph_.addOperand(lt, index);
                        graph_.addOperand(lt, limitConst);
                        graph_.addResult(lt, ltResult);
                        folded = graph_.createValue(graph_.makeInternalValSym(),
                                                    static_cast<int32_t>(elemWidth), false);
                        const OperationId mux = graph_.createOperation(OperationKind::kMux,
                                                                       graph_.makeInternalOpSym());
                        graph_.addOperand(mux, ltResult);
                        graph_.addOperand(mux, directRead);
                        graph_.addOperand(mux, constMap[static_cast<std::size_t>(elemCount)]);
                        graph_.addResult(mux, folded);
                    }
                    else
                    {
                        // element pad: index clamp  arr[idx < N ? idx : padElem]
                        const ValueId clamped = makeIndexClamp(index, elemCount, *padSource, pw);
                        folded = makeArrayRead(regSym, clamped, elemWidth);
                    }
                    graph_.replaceAllUses(result, folded);
                    sweepSeeds_.push_back(id);
                    sweepSeeds_.push_back(baseDef);
                    ++padReadsFolded_;
                }
            }

            // ---- dead cone sweep ----

            void sweepDead()
            {
                // The fold rewires/orphans whole cones (e.g. a padded concat
                // built per field where only one field is read); one full scan
                // seeds every already-dead pure-comb op so the worklist sweep
                // collects them all. Runs after xmr-resolve, so no cross-module
                // reference can point into these wires. No visited set: an op
                // must be recheckable — its results may lose their last user
                // only after a later erasure in this same sweep.
                std::vector<OperationId> worklist = sweepSeeds_;
                for (const OperationId id : graph_.operations())
                {
                    if (id.valid() && erased_.count(id) == 0 &&
                        isSweepableKind(graph_.opKind(id)))
                    {
                        worklist.push_back(id);
                    }
                }
                while (!worklist.empty())
                {
                    const OperationId id = worklist.back();
                    worklist.pop_back();
                    if (!id.valid() || erased_.count(id) != 0)
                    {
                        continue;
                    }
                    const OperationKind kind = graph_.opKind(id);
                    if (!isSweepableKind(kind))
                    {
                        continue;
                    }
                    const auto resultSpan = graph_.opResults(id);
                    if (resultSpan.empty())
                    {
                        continue;
                    }
                    bool dead = true;
                    for (const ValueId result : resultSpan)
                    {
                        const Value resultValue = graph_.getValue(result);
                        if (!resultValue.users().empty() ||
                            graph_.valueIsInput(result) || graph_.valueIsOutput(result) ||
                            graph_.valueIsInout(result))
                        {
                            dead = false;
                            break;
                        }
                    }
                    if (!dead)
                    {
                        continue;
                    }
                    std::vector<ValueId> results(resultSpan.begin(), resultSpan.end());
                    std::vector<ValueId> operands;
                    {
                        const auto operandSpan = graph_.opOperands(id);
                        operands.assign(operandSpan.begin(), operandSpan.end());
                    }
                    for (const ValueId operand : operands)
                    {
                        const OperationId producer = defOf(operand);
                        if (producer.valid())
                        {
                            worklist.push_back(producer);
                        }
                    }
                    erased_.insert(id);
                    graph_.eraseOp(id);
                    for (const ValueId result : results)
                    {
                        const SymbolId valueSym = graph_.valueSymbol(result);
                        if (valueSym.valid() && graph_.isDeclaredSymbol(valueSym))
                        {
                            graph_.removeDeclaredSymbol(valueSym);
                        }
                        graph_.eraseValue(result);
                        ++sweptOps_;
                    }
                }
            }

            Graph &graph_;
            std::unordered_map<std::string, RegisterInfo> regs_;
            std::unordered_set<OperationId, OperationIdHash> erased_;
            std::unordered_set<ValueId, ValueIdHash> foldedTreeResults_;
            std::vector<OperationId> sweepSeeds_;
            int64_t readTreesFolded_ = 0;
            int64_t writeGroupsFolded_ = 0;
            int64_t padReadsFolded_ = 0;
            int64_t sweptOps_ = 0;

        public:
            std::vector<std::string> debugLines;
        };

    } // namespace

    MuxTreeFoldPass::MuxTreeFoldPass()
        : Pass("mux-tree-fold", "mux-tree-fold",
               "Fold constant-index mux trees / per-element write decoders / padded-concat "
               "reads back into dynamic array accesses")
    {
    }

    PassResult MuxTreeFoldPass::run()
    {
        PassResult result;
        for (const auto &entry : design().graphs())
        {
            Graph &graph = *entry.second;
            Folder folder(graph);
            const bool folded = folder.run();
            if (folded)
            {
                result.changed = true;
                logInfo("mux-tree-fold [" + graph.symbol() + "] readTrees=" +
                        std::to_string(folder.readTreesFolded()) +
                        " writeGroups=" + std::to_string(folder.writeGroupsFolded()) +
                        " padReads=" + std::to_string(folder.padReadsFolded()) +
                        " swept=" + std::to_string(folder.sweptOps()));
            }
            for (const std::string &line : folder.debugLines)
            {
                logInfo(line);
            }
        }
        return result;
    }

} // namespace wolvrix::lib::transform
