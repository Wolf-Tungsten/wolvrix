#ifndef WOLVRIX_TRANSFORM_CONSTANT_INDEX_HPP
#define WOLVRIX_TRANSFORM_CONSTANT_INDEX_HPP

#include "core/grh.hpp"
#include "slang/numeric/SVInt.h"

#include <cstdint>
#include <exception>
#include <optional>

namespace wolvrix::lib::transform::detail {

// Recover an index only when its producer bits and equal-width assign views
// establish a known, nonnegative integer. Uncertain indices remain dynamic.
inline std::optional<uint64_t> constantIndex(const grh::Graph &graph, grh::ValueId value) {
    if (!value.valid() || graph.valueType(value) != grh::ValueType::Logic) return std::nullopt;
    const auto width = graph.valueWidth(value);
    const bool isSigned = graph.valueSigned(value);
    if (width <= 0) return std::nullopt;
    for (unsigned depth = 0; depth < 16; ++depth) {
        const auto def = graph.valueDef(value);
        if (!def.valid()) return std::nullopt;
        if (graph.opKind(def) == grh::OperationKind::kAssign) {
            const auto args = graph.opOperands(def);
            if (args.size() != 1 || !args[0].valid() || graph.valueWidth(args[0]) != width ||
                graph.valueType(args[0]) != grh::ValueType::Logic)
                return std::nullopt;
            value = args[0];
            continue;
        }
        if (graph.opKind(def) != grh::OperationKind::kConstant) return std::nullopt;
        const auto attr = graph.opAttrRange(def).find("constValue");
        if (!attr) return std::nullopt;
        const auto literal = attr->value().get<std::string>();
        if (!literal) return std::nullopt;
        try {
            const bool sourceSigned = graph.valueSigned(value);
            auto bits = *literal == "'1" ? ~slang::SVInt(width, 0, sourceSigned) :
                *literal == "'0" ? slang::SVInt(width, 0, sourceSigned) :
                slang::SVInt::fromString(*literal);
            // Size the producer before reinterpreting the final index view.
            // Assign changes signedness without changing the equal-width bits.
            bits.setSigned(sourceSigned);
            bits = bits.resize(width);
            bits.setSigned(isSigned);
            if (bits.hasUnknown() || (bits.isSigned() && bits.isNegative())) return std::nullopt;
            return bits.as<uint64_t>();
        }
        catch (const std::exception &) { return std::nullopt; }
    }
    return std::nullopt;
}

} // namespace wolvrix::lib::transform::detail

#endif
