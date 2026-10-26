#ifndef WOLVRIX_OP_DESCRIPTOR_HPP
#define WOLVRIX_OP_DESCRIPTOR_HPP

#include "core/grh.hpp"

#include <limits>
#include <map>

namespace wolvrix::lib::grh {

enum class OpEffect { Pure, Declaration, ReadState, WriteState, History, External };

struct AttrSchema {
    std::string_view name;
    std::size_t variantIndex;
    bool required = false;
};

struct OpDescriptor {
    OperationKind kind;
    std::string_view name;
    std::string_view dialect;
    uint32_t version = 1;
    OpEffect effect = OpEffect::External;
    bool canCse = false;
    bool canClone = false;
    std::size_t minOperands = 0;
    std::size_t maxOperands = std::numeric_limits<std::size_t>::max();
    std::size_t minResults = 0;
    std::size_t maxResults = std::numeric_limits<std::size_t>::max();
    std::span<const AttrSchema> attributes;
    std::string (*verify)(const Graph&, OperationId) = nullptr;
    OpEffect (*effectFor)(const Graph&, OperationId) = nullptr;
};

// Called once by statically linked dialect modules before graph processing.
// Descriptors and their schemas must have static lifetime.
void registerOpDescriptors(std::span<const OpDescriptor> descriptors);
const OpDescriptor* findOpDescriptor(OperationKind kind) noexcept;
std::optional<OperationKind> findDialectOperation(std::string_view name) noexcept;
std::optional<uint32_t> dialectVersion(std::string_view dialect) noexcept;
std::map<std::string, uint32_t> requiredDialects(const Graph& graph);
std::string verifyOpDescriptor(const Graph& graph, OperationId operation);
OpEffect operationEffect(const Graph& graph, OperationId operation);
bool canCseOperation(const Graph& graph, OperationId operation);
bool canCloneOperation(const Graph& graph, OperationId operation);
bool hasDialectOperations(const Graph& graph);

} // namespace wolvrix::lib::grh

#endif
