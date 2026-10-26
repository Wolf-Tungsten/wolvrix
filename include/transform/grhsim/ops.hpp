#ifndef WOLVRIX_TRANSFORM_GRHSIM_OPS_HPP
#define WOLVRIX_TRANSFORM_GRHSIM_OPS_HPP

#include "core/op_descriptor.hpp"
#include "core/state.hpp"

#include <stdexcept>

namespace wolvrix::lib::transform::grhsim {

// Extension kinds are owned here; the base GRH enum contains no simulation ops.
inline constexpr grh::OperationKind kEdgeDetect = static_cast<grh::OperationKind>(0x10000);
inline constexpr grh::OperationKind kWrite = static_cast<grh::OperationKind>(0x10001);
inline constexpr grh::OperationKind kWriteMask = static_cast<grh::OperationKind>(0x10002);
inline constexpr grh::OperationKind kWriteArray = static_cast<grh::OperationKind>(0x10003);
inline constexpr grh::OperationKind kWriteArrayMask = static_cast<grh::OperationKind>(0x10004);
inline constexpr grh::OperationKind kFill = static_cast<grh::OperationKind>(0x10005);
inline constexpr grh::OperationKind kLatchWrite = static_cast<grh::OperationKind>(0x10006);
inline constexpr grh::OperationKind kSystemTask = static_cast<grh::OperationKind>(0x10007);
inline constexpr grh::OperationKind kSystemFunction = static_cast<grh::OperationKind>(0x10008);
inline constexpr grh::OperationKind kDpiCall = static_cast<grh::OperationKind>(0x10009);
inline constexpr grh::OperationKind kBitSelect = static_cast<grh::OperationKind>(0x1000a);
inline constexpr grh::OperationKind kPrioritySelect = static_cast<grh::OperationKind>(0x1000b);
inline constexpr grh::OperationKind kHistoryWrite = static_cast<grh::OperationKind>(0x1000c);
inline constexpr grh::OperationKind kMaskedOr = static_cast<grh::OperationKind>(0x20000);

inline constexpr std::string_view kStage = "grhsim.stage";
inline constexpr std::string_view kPartition = "grhsim.partition";

template <typename T>
std::optional<T> attribute(const grh::Graph& graph, grh::OperationId operation, std::string_view key)
{
    for (const grh::AttrKV& entry : graph.opAttrs(operation))
    {
        if (entry.key != key) continue;
        if (const auto* typed = std::get_if<T>(&entry.value)) return *typed;
        throw std::invalid_argument("Wrong attribute type: " + std::string(key));
    }
    return std::nullopt;
}

bool isWriter(grh::OperationKind kind) noexcept;
bool isPureSystemFunction(std::string_view name) noexcept;
bool isSampledSystemFunction(std::string_view name) noexcept;
std::string_view systemFunctionEffect(const grh::Graph& graph, grh::OperationId operation);
bool isFire(const grh::Graph& graph, grh::ValueId value, bool allowAlways = false);
grh::OperationId writerTarget(const grh::Graph& graph, grh::OperationId writer);
std::optional<grh::ValueType> parseDpiValueType(std::string_view name) noexcept;
// Base GRH calls can require an explicit equal-width signedness view during
// conversion. Dialect calls always use the strict default signature.
std::string verifyDpiSignature(const grh::Graph& graph, grh::OperationId call,
                               std::size_t firstArgument, std::size_t trailingEvents = 0,
                               bool permitInputSignViews = false);
std::string verifyGraph(const grh::Graph& graph);

} // namespace wolvrix::lib::transform::grhsim

#endif
