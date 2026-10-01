// grhsim.select-state-stores (M5d-4, pipeline stage A7): the single semantic
// store classification decision point. Every state is assigned
// StateStoreClass::RegLatch (small/scalar states committed by the P_publish
// whole-block next->current copy) or StateStoreClass::Mem (large contiguous
// arrays committed in place by P_mem from sampled write parameters). The
// decision uses the optimized size, contiguity and update cost of each state;
// downstream passes (B5 split-phases, C3 layout-named-stores, emit) consume
// the annotation and must not re-derive the classification from the state's
// type. This pass performs no semantic rewrite: it only annotates states.

#include "grhsim/pass/select_state_stores.hpp"

#include "grhsim/ir/model.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace wolvrix::lib::grhsim
{
    namespace
    {
        struct SelectStateStoresOptions
        {
            uint64_t memMinBytes = 64;
            bool reclassify = false;
            std::filesystem::path report;
        };

        // Linear semantic bit size of a type: arrays flatten row-major
        // (element stride = element size), non-logic scalars carry no bits.
        // Returns false on an invalid type reference or a 64-bit overflow.
        bool linearBitSize(const GrhSimModel &model, TypeId typeId, uint64_t &out)
        {
            if (!typeId.valid() || typeId.generation != 0 || typeId.index > model.types().size())
                return false;
            const Type &type = model.types()[typeId.index - 1];
            switch (type.kind)
            {
            case TypeKind::Logic:
                out = type.width;
                return true;
            case TypeKind::Real:
            case TypeKind::String:
                out = 0;
                return true;
            case TypeKind::Array:
            {
                uint64_t element = 0;
                if (!linearBitSize(model, type.elementType, element)) return false;
                if (type.count != 0 &&
                    element > std::numeric_limits<uint64_t>::max() / type.count)
                    return false;
                out = type.count * element;
                return true;
            }
            }
            return false;
        }

        // Array dimensions outermost-first, joined by 'x'; empty for scalars.
        std::string shapeOf(const GrhSimModel &model, TypeId typeId)
        {
            std::string shape;
            while (typeId.valid() && typeId.generation == 0 && typeId.index <= model.types().size())
            {
                const Type &type = model.types()[typeId.index - 1];
                if (type.kind != TypeKind::Array) break;
                if (!shape.empty()) shape += 'x';
                shape += std::to_string(type.count);
                typeId = type.elementType;
            }
            return shape;
        }

        StateStoreClass classify(const GrhSimModel &model, const StateObject &state,
                                 uint64_t memMinBytes, uint64_t &bits, uint64_t &bytes, bool &ok)
        {
            ok = linearBitSize(model, state.type, bits);
            if (!ok) return StateStoreClass::None;
            bytes = bits / 8 + (bits % 8 != 0 ? 1 : 0);
            const TypeKind kind = model.types()[state.type.index - 1].kind;
            if (kind == TypeKind::Array && bytes >= memMinBytes) return StateStoreClass::Mem;
            return StateStoreClass::RegLatch;
        }

        class SelectStateStoresPass final : public Pass
        {
        public:
            explicit SelectStateStoresPass(SelectStateStoresOptions options = {})
                : Pass("grhsim.select-state-stores", PassKind::MetadataTransform),
                  options_(std::move(options)) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                // Read/write reference counts per state, for the report.
                std::vector<uint64_t> writes(model.states().size() + 1, 0);
                std::vector<uint64_t> reads(model.states().size() + 1, 0);
                for (const auto &op : model.operations())
                {
                    if (!model.strings().valid(op.opType)) continue;
                    const std::string_view name = model.text(op.opType);
                    const auto refs = model.objectRefs(op);
                    if (refs.empty() || refs[0].kind != ObjectKind::State ||
                        refs[0].index > model.states().size())
                        continue;
                    if (name == "core.state.regWrite" || name == "core.state.latchWrite" ||
                        name == "core.state.memWrite" || name == "core.state.memFill" ||
                        name == "core.state.memAssign" || name == "core.state.memWriteSeq")
                        ++writes[refs[0].index];
                    else if (name == "core.state.read" || name == "core.state.memRead")
                        ++reads[refs[0].index];
                }

                struct Row
                {
                    uint32_t index;
                    StateStoreClass storeClass;
                    uint64_t bits;
                    uint64_t bytes;
                };
                std::vector<Row> rows;
                uint64_t kept = 0, regLatch = 0, mem = 0;
                uint64_t regLatchBytes = 0, memBytes = 0;
                bool changed = false;
                for (const auto &state : model.states())
                {
                    uint64_t bits = 0, bytes = 0;
                    bool ok = false;
                    const StateStoreClass target =
                        classify(model, state, options_.memMinBytes, bits, bytes, ok);
                    if (!ok)
                    {
                        diagnostics.error("state type is invalid or overflows the 64-bit "
                                          "linear size",
                                          "states[" + std::to_string(state.id.index - 1) + "]");
                        return {false, false, {}};
                    }
                    const bool skip = state.storeClass != StateStoreClass::None &&
                                      !options_.reclassify;
                    const StateStoreClass finalClass = skip ? state.storeClass : target;
                    if (skip) ++kept;
                    else if (target == StateStoreClass::Mem) ++mem;
                    else ++regLatch;
                    if (!skip && state.storeClass != target)
                    {
                        model.setStateStoreClass(state.id, target);
                        changed = true;
                    }
                    if (finalClass == StateStoreClass::Mem) memBytes += bytes;
                    else regLatchBytes += bytes;
                    rows.push_back({state.id.index, finalClass, bits, bytes});
                }

                if (!options_.report.empty())
                {
                    std::ofstream out(options_.report);
                    if (!out)
                    {
                        diagnostics.error("cannot open select-state-stores report", name());
                        return {false, false, {}};
                    }
                    out << "state\tname\tclass\tkind\tshape\tbits\tbytes\twrites\treads\n";
                    for (const auto &row : rows)
                    {
                        const StateObject &state = model.states()[row.index - 1];
                        const Type &type = model.types()[state.type.index - 1];
                        const std::string shape = shapeOf(model, state.type);
                        out << row.index << '\t' << model.text(state.name) << '\t'
                            << toString(row.storeClass) << '\t' << toString(type.kind) << '\t'
                            << (shape.empty() ? "-" : shape) << '\t' << row.bits << '\t'
                            << row.bytes << '\t' << writes[row.index] << '\t' << reads[row.index]
                            << '\n';
                    }
                    if (!out.good())
                    {
                        diagnostics.error("cannot write select-state-stores report", name());
                        return {false, false, {}};
                    }
                }

                diagnostics.info(
                    "select-state-stores: mem_min_bytes=" + std::to_string(options_.memMinBytes) +
                        " state_stores_reg_latch=" + std::to_string(regLatch) +
                        " state_stores_mem=" + std::to_string(mem) +
                        " state_stores_kept=" + std::to_string(kept) +
                        " reg_latch_bytes=" + std::to_string(regLatchBytes) +
                        " mem_bytes=" + std::to_string(memBytes) +
                        " (bytes totals cover all states)",
                    name());
                return {true, changed, {}};
            }

        private:
            SelectStateStoresOptions options_;
        };

        bool parseUint64(std::string_view text, uint64_t &out)
        {
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
            return ec == std::errc{} && end == text.data() + text.size();
        }

        std::unique_ptr<Pass> createSelectStateStoresPass(std::span<const std::string_view> args,
                                                          std::string &error)
        {
            SelectStateStoresOptions options;
            for (std::size_t i = 0; i < args.size(); i += 2)
            {
                if (i + 1 == args.size())
                {
                    error = "grhsim.select-state-stores option requires a value";
                    return {};
                }
                const auto name = args[i], value = args[i + 1];
                if (name == "--mem-min-bytes")
                {
                    if (!parseUint64(value, options.memMinBytes))
                    {
                        error = "invalid mem-min-bytes (unsigned integer expected)";
                        return {};
                    }
                }
                else if (name == "--reclassify")
                {
                    if (value != "true" && value != "false")
                    {
                        error = "invalid reclassify boolean: " + std::string(value);
                        return {};
                    }
                    options.reclassify = value == "true";
                }
                else if (name == "--report")
                {
                    options.report = std::filesystem::path(value);
                }
                else
                {
                    error = "unknown grhsim.select-state-stores option: " + std::string(name);
                    return {};
                }
            }
            return std::make_unique<SelectStateStoresPass>(std::move(options));
        }
    } // namespace

    void registerSelectStateStoresPass(PassRegistry &registry)
    {
        std::string error;
        registry.registerPass("grhsim.select-state-stores", PassKind::MetadataTransform,
                              createSelectStateStoresPass, error);
    }
} // namespace wolvrix::lib::grhsim
