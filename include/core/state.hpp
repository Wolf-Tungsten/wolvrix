#ifndef WOLVRIX_STATE_HPP
#define WOLVRIX_STATE_HPP

#include "core/grh.hpp"

namespace wolvrix::lib::grh {

enum class InitKind { Constant, Fill, Readmem, Random };

struct InitStep {
    InitKind kind = InitKind::Constant;
    std::string value;
    std::string file;
    std::string format;
    std::optional<int64_t> start;
    std::optional<int64_t> count;
    std::optional<int64_t> seed;
    bool random = false;
    friend bool operator==(const InitStep&, const InitStep&) = default;
};

// Typed access to the original declaration, not an independent state model.
using InitSpec = std::vector<InitStep>;
LogicalType stateLogicalType(const Graph& graph, OperationId declaration);
void setStateLogicalType(Graph& graph, OperationId declaration, const LogicalType& type);
InitSpec stateInitSpec(const Graph& graph, OperationId declaration);
void setStateInitSpec(Graph& graph, OperationId declaration, const InitSpec& steps);
void validateInitSpec(const LogicalType& type, const InitSpec& steps);

} // namespace wolvrix::lib::grh

#endif
