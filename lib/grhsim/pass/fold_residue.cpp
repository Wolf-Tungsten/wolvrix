#include "grhsim/pass/fold_residue.hpp"
#include "grhsim/backend/cpu.hpp"
#include "grhsim/ir/model.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace wolvrix::lib::grhsim {
    namespace {
        // Post-schedule residue folding (NO00016). Mirrors probe_identity4.py:
        // pure compute ops whose result is a local, unpinned, non-event-gate
        // value consumed only by same-unit pure compute ops are folded when they
        // are identities or constant residues:
        //   assign_strict      assign(x), type(x) == type(r)
        //   assign_width       assign(x), equal width, consumers all
        //                      signedness-agnostic kinds
        //   slice_full_strict  sliceStatic(x, 0, w-1) full range, same type
        //   slice_full_width   full range, width equal, agnostic consumers
        //   const_slice        sliceStatic(const c, start, end); rewires to an
        //                      EXISTING constant of the result type with value
        //                      (c >> start) & mask (CSE; no constant is created).
        //                      The hit constant must be ordered before the
        //                      folded op in the verifier's compute walk
        //                      (preorder partition DFS), so rewired consumers
        //                      keep defined-before-use ordering.
        //   not_not            not(not(x)), inner not single-use,
        //                      type(x) == type(r), two-state result
        //   self_{eq,ne,lt,gt,le,ge}  cmp(x, x), two-state, rewires to an
        //                      existing 0/1 constant of the result type
        //                      (same ordering rule as const_slice)
        //   not_not            not(not(x)), inner not single-use,
        //                      type(x) == type(r), two-state result
        //   self_{eq,ne,lt,gt,le,ge}  cmp(x, x), two-state, rewires to an
        //                      existing 0/1 constant of the result type
        //   dce_cascade        pure compute local op left with zero uses after
        //                      rewiring (constants excluded), to fixpoint
        // Unselected consumers are rewired to the fold source (chains resolved
        // to the final unfolded value). Selected ops stay in the model and
        // partition tables; the CPU emitter skips them (schedule.foldResidueOps).
        // The selection is computed once over the pre-pass model (single
        // primaries pass + DCE fixpoint), so verifyCpuSchedule's rebuild replays
        // the plan deterministically.

        bool pureCompute(std::string_view name) {
            return name.starts_with("core.compute.") &&
                   name != "core.compute.constant" && name != "core.compute.expr";
        }

        bool agnosticKind(std::string_view name) {
            static const std::string_view kinds[]{
                "core.compute.assign", "core.compute.and", "core.compute.or",
                "core.compute.xor", "core.compute.not", "core.compute.mux",
                "core.compute.concat", "core.compute.sliceStatic", "core.compute.bitSelect",
                "core.compute.eq", "core.compute.ne", "core.compute.logicAnd",
                "core.compute.logicOr", "core.compute.logicNot", "core.compute.reduceAnd",
                "core.compute.reduceOr", "core.compute.reduceXor", "core.compute.replicate"};
            for (const auto kind : kinds)
                if (kind == name) return true;
            return false;
        }

        // Self-comparison fold values: eq/le/ge -> 1, ne/lt/gt -> 0.
        std::optional<uint64_t> selfCompareValue(std::string_view name) {
            if (name == "core.compute.eq" || name == "core.compute.le" || name == "core.compute.ge") return 1;
            if (name == "core.compute.ne" || name == "core.compute.lt" || name == "core.compute.gt") return 0;
            return std::nullopt;
        }

        uint64_t maskFor(uint32_t width) { return width >= 64 ? ~uint64_t(0) : ((uint64_t(1) << width) - 1); }

        // Constant value mirror of the probe's parse_sv_literal: low 64 bits
        // (two's complement) plus a fits flag (exact value in [0, 2^64)).
        // Slices needing bits at or above bit 64 of a non-fitting constant are
        // rejected by the caller (production model has none).
        struct ConstValue {
            uint64_t low = 0;
            bool fits = true;
        };

        int digitValue(char c, unsigned base, bool &unknown) {
            unknown = false;
            if (c == 'x' || c == 'X' || c == 'z' || c == 'Z' || c == '?') { unknown = true; return 0; }
            unsigned v = 0;
            if (c >= '0' && c <= '9') v = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v = static_cast<unsigned>(c - 'a') + 10;
            else if (c >= 'A' && c <= 'F') v = static_cast<unsigned>(c - 'A') + 10;
            else return -1;
            return v < base ? static_cast<int>(v) : -1;
        }

        ConstValue accumulateDecimal(const std::string &digits) {
            ConstValue out;
            for (const char c : digits) {
                const unsigned d = static_cast<unsigned>(c - '0');
                if (out.fits && out.low > (~uint64_t(0) - d) / 10) out.fits = false;
                out.low = out.low * 10 + d;
            }
            return out;
        }

        std::optional<ConstValue> parseSvLiteral(std::string_view text) {
            const auto space = [](char c) {
                return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
            };
            while (!text.empty() && space(text.front())) text.remove_prefix(1);
            while (!text.empty() && space(text.back())) text.remove_suffix(1);
            std::string clean;
            clean.reserve(text.size());
            for (const char c : text)
                if (c != '_') clean.push_back(c);
            if (clean.empty()) return std::nullopt;
            const auto quote = clean.find('\'');
            if (quote == std::string::npos) {
                if (!std::all_of(clean.begin(), clean.end(),
                                 [](char c) { return c >= '0' && c <= '9'; }))
                    return std::nullopt;
                return accumulateDecimal(clean);
            }
            // Size prefix: (\d+)\s* or empty.
            std::size_t pos = 0;
            bool sawDigit = false;
            while (pos < quote && clean[pos] >= '0' && clean[pos] <= '9') { ++pos; sawDigit = true; }
            if (!sawDigit && pos != quote) return std::nullopt;
            std::size_t pad = pos;
            while (pad < quote && space(clean[pad])) ++pad;
            if (pad != quote) return std::nullopt;
            pos = quote + 1;
            if (pos < clean.size() && (clean[pos] == 's' || clean[pos] == 'S')) ++pos;
            if (pos >= clean.size()) return std::nullopt;
            const char baseChar = static_cast<char>(std::tolower(static_cast<unsigned char>(clean[pos++])));
            unsigned base = 0, shift = 0;
            switch (baseChar) {
            case 'b': base = 2; shift = 1; break;
            case 'o': base = 8; shift = 3; break;
            case 'd': base = 10; break;
            case 'h': base = 16; shift = 4; break;
            default: return std::nullopt;
            }
            if (pos >= clean.size()) return std::nullopt;
            const std::string digits = clean.substr(pos);
            if (base == 10) {
                for (const char c : digits) {
                    bool unknown = false;
                    if (digitValue(c, 10, unknown) < 0 || unknown) return std::nullopt;
                }
                return accumulateDecimal(digits);
            }
            ConstValue out;
            for (const char c : digits) {
                bool unknown = false;
                const int d = digitValue(c, base, unknown);
                if (d < 0) return std::nullopt;
                if (out.fits && (out.low >> (64 - shift))) out.fits = false;
                out.low = (out.low << shift) | static_cast<unsigned>(d);
            }
            return out;
        }

        enum class FoldClass : uint8_t {
            AssignStrict, AssignWidth, SliceFullStrict, SliceFullWidth, ConstSlice,
            NotNot, SelfEq, SelfNe, SelfLt, SelfGt, SelfLe, SelfGe, DceCascade
        };

        std::string_view foldClassName(FoldClass cls) {
            switch (cls) {
            case FoldClass::AssignStrict: return "assign_strict";
            case FoldClass::AssignWidth: return "assign_width";
            case FoldClass::SliceFullStrict: return "slice_full_strict";
            case FoldClass::SliceFullWidth: return "slice_full_width";
            case FoldClass::ConstSlice: return "const_slice";
            case FoldClass::NotNot: return "not_not";
            case FoldClass::SelfEq: return "self_eq";
            case FoldClass::SelfNe: return "self_ne";
            case FoldClass::SelfLt: return "self_lt";
            case FoldClass::SelfGt: return "self_gt";
            case FoldClass::SelfLe: return "self_le";
            case FoldClass::SelfGe: return "self_ge";
            case FoldClass::DceCascade: return "dce_cascade";
            }
            return "unknown";
        }

        struct CseKey {
            uint32_t type = 0;
            uint64_t value = 0;
            friend bool operator==(const CseKey &, const CseKey &) = default;
        };

        struct CseKeyHash {
            std::size_t operator()(CseKey key) const noexcept {
                return (static_cast<std::size_t>(key.type) << 32) ^ static_cast<std::size_t>(key.value);
            }
        };

        class FoldResiduePass final : public Pass {
        public:
            FoldResiduePass() : Pass("grhsim.fold-residue", PassKind::BackendMapping) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override {
                const auto *previous = model.cpuMapping();
                if (!previous || previous->stage != CpuMappingStage::Schedule ||
                    !previous->dataLayout || !previous->schedule)
                    throw std::runtime_error("grhsim.fold-residue requires a complete CPU schedule mapping");
                if (previous->schedule->foldResidue) {
                    diagnostics.info("fold_residue already applied", name());
                    return {true, false, {}};
                }

                const auto &operations = model.operations();
                const auto &values = model.values();
                const auto &types = model.types();
                const auto &layout = *previous->dataLayout;
                const auto &schedule = *previous->schedule;
                const auto &tree = previous->partitionTree;
                const std::size_t valueCount = values.size();

                const auto typeOf = [&](ValueId value) -> const Type & {
                    return types[values[value.index - 1].type.index - 1];
                };
                const auto widthOf = [&](TypeId type) -> uint32_t {
                    const auto &row = types[type.index - 1];
                    return row.kind == TypeKind::Logic ? row.width : 0;
                };
                const auto twoState = [&](TypeId type) {
                    const auto &row = types[type.index - 1];
                    return row.kind == TypeKind::Logic && row.domain == LogicDomain::TwoState;
                };

                std::vector<uint32_t> producer(valueCount + 1, 0);
                std::vector<std::vector<OpId>> consumers(valueCount + 1);
                for (const auto &op : operations)
                    for (const auto result : model.results(op)) producer[result.index] = op.id.index;
                for (const auto &op : operations)
                    for (const auto operand : model.operands(op)) consumers[operand.index].push_back(op.id);

                std::vector<uint8_t> blocked(valueCount + 1, 0);
                for (std::size_t i = 0; i < layout.values.size(); ++i)
                    if (layout.values[i].kind == CpuStorageKind::Boundary) blocked[i + 1] = 1;
                for (const auto &row : schedule.inputFanout) blocked[row.source.index] = 1;
                for (const auto &shadow : schedule.inputShadows) blocked[shadow.value.index] = 1;
                for (const auto &slot : layout.runtime)
                    if (slot.value) blocked[slot.value.index] = 1;
                for (const auto &partition : tree.partitions)
                    if (partition.attrs.eventGate)
                        for (const auto &event : partition.attrs.eventGate->events)
                            blocked[event.value.index] = 1;

                // Compute unit per op: probe walks each supernode partition's
                // subtree (itself included) and tags every op found.
                std::vector<uint32_t> unit(operations.size() + 1, 0);
                for (const auto &partition : tree.partitions) {
                    if (partition.attrs.kind != CpuPartitionKind::Supernode) continue;
                    std::vector<PartitionId> stack{partition.id};
                    while (!stack.empty()) {
                        const auto node = stack.back();
                        stack.pop_back();
                        const auto &row = tree.partitions[node.index - 1];
                        for (const auto child : row.children) stack.push_back(child);
                        for (const auto op : row.ops) unit[op.index] = partition.id.index;
                    }
                }

                // Verifier compute-walk positions: preorder partition DFS with
                // children in listed order; ops visited in list order. A CSE
                // hit must sit earlier in this walk than the folded op so
                // rewired consumers keep defined-before-use ordering.
                std::vector<uint32_t> walkPos(operations.size() + 1, 0);
                uint32_t walkOrder = 0;
                std::vector<PartitionId> walk{tree.root};
                while (!walk.empty()) {
                    const auto pid = walk.back();
                    walk.pop_back();
                    const auto &row = tree.partitions[pid.index - 1];
                    for (const auto op : row.ops) walkPos[op.index] = ++walkOrder;
                    for (auto it = row.children.rbegin(); it != row.children.rend(); ++it)
                        walk.push_back(*it);
                }

                // Constant table (model order) and CSE candidates over existing
                // constants: (result type, value masked to width) -> values in
                // model order.
                std::unordered_map<uint32_t, ConstValue> constValues;
                std::unordered_map<CseKey, std::vector<uint32_t>, CseKeyHash> cse;
                for (const auto &op : operations) {
                    if (model.text(op.opType) != "core.compute.constant") continue;
                    const auto results = model.results(op);
                    if (results.empty()) continue;
                    std::optional<ConstValue> parsed;
                    for (const auto &param : model.parameters(op)) {
                        const auto pname = model.text(param.name);
                        if (pname != "constValue" && pname != "value") continue;
                        if (const auto *iv = std::get_if<int64_t>(&param.value))
                            parsed = ConstValue{static_cast<uint64_t>(*iv), *iv >= 0};
                        else if (const auto *bv = std::get_if<bool>(&param.value))
                            parsed = ConstValue{*bv ? uint64_t(1) : uint64_t(0), true};
                        else if (const auto *sv = std::get_if<std::string>(&param.value))
                            parsed = parseSvLiteral(*sv);
                        break;
                    }
                    if (!parsed) continue;
                    const auto result = results[0];
                    constValues.emplace(result.index, *parsed);
                    const auto &type = typeOf(result);
                    if (type.kind == TypeKind::Logic && type.width >= 1 && type.width <= 64)
                        cse[CseKey{type.id.index, parsed->low & maskFor(type.width)}].push_back(result.index);
                }
                // First CSE candidate (model order) positioned before the fold.
                const auto cseHit = [&](CseKey key, uint32_t beforePos) -> std::optional<uint32_t> {
                    const auto it = cse.find(key);
                    if (it == cse.end()) return std::nullopt;
                    for (const auto value : it->second) {
                        const uint32_t prod = producer[value];
                        if (prod && walkPos[prod] && walkPos[prod] < beforePos) return value;
                    }
                    return std::nullopt;
                };

                const auto eligibleResult = [&](ValueId value) {
                    if (blocked[value.index]) return false;
                    const uint32_t prod = producer[value.index];
                    if (!prod) return false;
                    const uint32_t owner = unit[prod];
                    if (!owner) return false;
                    for (const auto consumer : consumers[value.index]) {
                        if (!pureCompute(model.text(operations[consumer.index - 1].opType))) return false;
                        if (unit[consumer.index] != owner) return false;
                    }
                    return true;
                };
                const auto agnosticConsumers = [&](ValueId value) {
                    for (const auto consumer : consumers[value.index])
                        if (!agnosticKind(model.text(operations[consumer.index - 1].opType))) return false;
                    return true;
                };

                std::vector<uint8_t> selected(operations.size() + 1, 0);
                std::vector<std::pair<uint32_t, uint32_t>> rewireList;
                std::unordered_map<uint32_t, uint32_t> rewireMap;
                uint64_t classCounts[13] = {};
                uint64_t assignTypeRejects = 0, constSliceCseMiss = 0, constSliceRangeRejects = 0;
                uint64_t selfCmpCseMiss = 0, twoStateGuard = 0;

                for (const auto &op : operations) {
                    const auto opName = model.text(op.opType);
                    if (!pureCompute(opName)) continue;
                    const auto operands = model.operands(op);
                    const auto results = model.results(op);
                    if (operands.empty() || results.size() != 1) continue;
                    const auto result = results[0];
                    if (!eligibleResult(result)) continue;
                    const auto resultType = values[result.index - 1].type;
                    const uint32_t resultWidth = widthOf(resultType);
                    FoldClass cls = FoldClass::DceCascade;
                    uint32_t source = 0;
                    bool matched = false;
                    if (opName == "core.compute.assign" && operands.size() == 1) {
                        const auto x = operands[0];
                        if (values[x.index - 1].type == resultType) {
                            matched = true; cls = FoldClass::AssignStrict; source = x.index;
                        } else if (widthOf(values[x.index - 1].type) == resultWidth && agnosticConsumers(result)) {
                            matched = true; cls = FoldClass::AssignWidth; source = x.index;
                        } else ++assignTypeRejects;
                    } else if (opName == "core.compute.sliceStatic" && operands.size() == 1) {
                        std::optional<int64_t> start, end;
                        for (const auto &param : model.parameters(op)) {
                            const auto pname = model.text(param.name);
                            const auto *iv = std::get_if<int64_t>(&param.value);
                            if (!iv) continue;
                            if (pname == "sliceStart") start = *iv;
                            else if (pname == "sliceEnd") end = *iv;
                        }
                        const auto x = operands[0];
                        if (start && end && *start >= 0 && *end >= *start) {
                            const uint64_t sliceWidth = static_cast<uint64_t>(*end - *start) + 1;
                            if (*start == 0 && static_cast<uint64_t>(*end) + 1 == widthOf(values[x.index - 1].type) &&
                                resultWidth == sliceWidth) {
                                if (values[x.index - 1].type == resultType) {
                                    matched = true; cls = FoldClass::SliceFullStrict; source = x.index;
                                } else if (agnosticConsumers(result)) {
                                    matched = true; cls = FoldClass::SliceFullWidth; source = x.index;
                                }
                            } else if (const auto it = constValues.find(x.index); it != constValues.end()) {
                                if (sliceWidth > 64) ++constSliceCseMiss;
                                else if (!it->second.fits && static_cast<uint64_t>(*start) + sliceWidth > 64)
                                    ++constSliceRangeRejects;
                                else {
                                    const uint64_t want = (it->second.low >> *start) & maskFor(static_cast<uint32_t>(sliceWidth));
                                    if (const auto hit = cseHit(CseKey{resultType.index, want}, walkPos[op.id.index])) {
                                        matched = true; cls = FoldClass::ConstSlice; source = *hit;
                                    } else ++constSliceCseMiss;
                                }
                            }
                        }
                    } else if (opName == "core.compute.not" && operands.size() == 1) {
                        const uint32_t prod = producer[operands[0].index];
                        if (prod && model.text(operations[prod - 1].opType) == "core.compute.not") {
                            const auto innerOperands = model.operands(operations[prod - 1]);
                            if (!innerOperands.empty() && values[innerOperands[0].index - 1].type == resultType &&
                                consumers[operands[0].index].size() == 1) {
                                if (twoState(resultType)) {
                                    matched = true; cls = FoldClass::NotNot; source = innerOperands[0].index;
                                } else ++twoStateGuard;
                            }
                        }
                    } else if (operands.size() == 2 && operands[0] == operands[1]) {
                        if (const auto cmpValue = selfCompareValue(opName)) {
                            if (!twoState(resultType) || !twoState(values[operands[0].index - 1].type)) {
                                ++twoStateGuard;
                            } else {
                                std::optional<uint32_t> hit;
                                if (resultWidth >= 1 && resultWidth <= 64)
                                    hit = cseHit(CseKey{resultType.index, *cmpValue & maskFor(resultWidth)},
                                                 walkPos[op.id.index]);
                                if (hit) {
                                    matched = true; source = *hit;
                                    if (opName == "core.compute.eq") cls = FoldClass::SelfEq;
                                    else if (opName == "core.compute.ne") cls = FoldClass::SelfNe;
                                    else if (opName == "core.compute.lt") cls = FoldClass::SelfLt;
                                    else if (opName == "core.compute.gt") cls = FoldClass::SelfGt;
                                    else if (opName == "core.compute.le") cls = FoldClass::SelfLe;
                                    else cls = FoldClass::SelfGe;
                                } else ++selfCmpCseMiss;
                            }
                        }
                    }
                    if (!matched) continue;
                    selected[op.id.index] = 1;
                    ++classCounts[static_cast<std::size_t>(cls)];
                    rewireList.push_back({result.index, source});
                    rewireMap.emplace(result.index, source);
                }

                // Cascade DCE fixpoint over pure compute local ops (constants
                // excluded); uses skip selected ops' operand reads, and rewiring
                // transfers every use of a folded result to its source.
                std::vector<uint64_t> uses(valueCount + 1, 0);
                for (const auto &op : operations) {
                    if (selected[op.id.index]) continue;
                    for (const auto operand : model.operands(op)) ++uses[operand.index];
                }
                for (const auto [result, source] : rewireList) {
                    uses[source] += uses[result];
                    uses[result] = 0;
                }
                bool fixing = true;
                while (fixing) {
                    fixing = false;
                    for (const auto &op : operations) {
                        if (selected[op.id.index]) continue;
                        if (!pureCompute(model.text(op.opType))) continue;
                        const auto results = model.results(op);
                        if (results.size() != 1) continue;
                        const auto result = results[0];
                        if (uses[result.index] != 0) continue;
                        if (!eligibleResult(result)) continue;
                        selected[op.id.index] = 1;
                        ++classCounts[static_cast<std::size_t>(FoldClass::DceCascade)];
                        fixing = true;
                        for (const auto operand : model.operands(op)) --uses[operand.index];
                    }
                }

                std::vector<OpId> foldOps;
                for (const auto &op : operations)
                    if (selected[op.id.index]) foldOps.push_back(op.id);
                if (foldOps.empty()) {
                    diagnostics.info("fold_residue no candidates", name());
                    return {true, false, {}};
                }

                // Rewire unselected consumers to the resolved fold source.
                const auto resolve = [&](uint32_t value) {
                    std::vector<uint32_t> chain;
                    for (;;) {
                        const auto it = rewireMap.find(value);
                        if (it == rewireMap.end()) return value;
                        if (std::find(chain.begin(), chain.end(), value) != chain.end())
                            throw std::runtime_error("grhsim.fold-residue rewire cycle");
                        chain.push_back(value);
                        value = it->second;
                    }
                };
                for (const auto &op : operations) {
                    if (selected[op.id.index]) continue;
                    const auto operandSpan = model.operands(op);
                    std::vector<ValueId> operands(operandSpan.begin(), operandSpan.end());
                    bool changed = false;
                    for (auto &operand : operands)
                        if (rewireMap.contains(operand.index)) {
                            const uint32_t root = resolve(operand.index);
                            if (root != operand.index) { operand.index = root; changed = true; }
                        }
                    if (!changed) continue;
                    const auto resultSpan = model.results(op);
                    const auto refSpan = model.objectRefs(op);
                    const auto paramSpan = model.parameters(op);
                    const std::vector<ValueId> results(resultSpan.begin(), resultSpan.end());
                    const std::vector<ObjectRef> refs(refSpan.begin(), refSpan.end());
                    const std::vector<Parameter> params(paramSpan.begin(), paramSpan.end());
                    model.replaceOperation(op.id, model.text(op.opType), operands, results, refs, params);
                }

                CpuBackendMapping mapping = *previous;
                mapping.schedule->foldResidue = true;
                mapping.schedule->foldResidueOps = std::move(foldOps);
                const uint64_t total = mapping.schedule->foldResidueOps.size();
                model.setCpuMapping(std::move(mapping));
                // Rewiring changed operand structure: helper read caches and
                // boundary densification are operand-derived, and the quiescence
                // projection may shrink when a folded chain strands a state read.
                if (!refreshCpuDataLayout(model, diagnostics)) return {false, true, {}};
                if (!refreshCpuSchedule(model, diagnostics)) return {false, true, {}};

                std::string report = "fold_residue=" + std::to_string(total);
                for (std::size_t i = 0; i < 13; ++i)
                    if (classCounts[i])
                        report += " " + std::string(foldClassName(static_cast<FoldClass>(i))) +
                                  "=" + std::to_string(classCounts[i]);
                if (assignTypeRejects) report += " reject_assign_type=" + std::to_string(assignTypeRejects);
                if (constSliceCseMiss) report += " reject_const_slice_cse_miss=" + std::to_string(constSliceCseMiss);
                if (constSliceRangeRejects) report += " reject_const_slice_range=" + std::to_string(constSliceRangeRejects);
                if (selfCmpCseMiss) report += " reject_self_cmp_cse_miss=" + std::to_string(selfCmpCseMiss);
                if (twoStateGuard) report += " reject_twostate=" + std::to_string(twoStateGuard);
                diagnostics.info(report, name());
                return {true, true, {}};
            }
        };
    }

    void registerFoldResiduePass(PassRegistry &registry) {
        std::string error;
        if (!registry.registerPass("grhsim.fold-residue", PassKind::BackendMapping,
            [](std::span<const std::string_view> args, std::string &factoryError) -> std::unique_ptr<Pass> {
                if (!args.empty()) {
                    factoryError = "grhsim.fold-residue does not accept arguments";
                    return {};
                }
                return std::make_unique<FoldResiduePass>();
            }, error))
            throw std::logic_error(error);
    }
}
