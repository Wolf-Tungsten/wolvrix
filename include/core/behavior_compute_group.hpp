#ifndef WOLVRIX_CORE_BEHAVIOR_COMPUTE_GROUP_HPP
#define WOLVRIX_CORE_BEHAVIOR_COMPUTE_GROUP_HPP

#include "core/grh.hpp"
#include <stdexcept>
#include <string>
#include <vector>

namespace wolvrix::lib::grh {

template<class T> inline T behaviorComputeAttr(const Operation &op, std::string_view key) {
    auto attr = op.attr(key);
    if (!attr || !std::holds_alternative<T>(*attr))
        throw std::runtime_error("compute group missing/invalid attribute " + std::string(key));
    return std::get<T>(*attr);
}

struct BehaviorComputeContract {
    std::string model;
    std::string boundary = "none";
    std::vector<std::string> operations;
    std::string bindingHash;
    std::vector<int64_t> inputWidths;
    std::vector<int64_t> outputWidths;
};

inline BehaviorComputeContract behaviorComputeContract(const Graph &graph, const Operation &op) {
    if (op.kind() != OperationKind::kBehaviorComputeGroup ||
        behaviorComputeAttr<int64_t>(op, "behavior.version") != 1 ||
        behaviorComputeAttr<bool>(op, "behavior.observerOnly") ||
        behaviorComputeAttr<bool>(op, "behavior.stateful"))
        throw std::runtime_error("invalid behavior compute group contract/version");
    BehaviorComputeContract c;
    c.model = behaviorComputeAttr<std::string>(op, "behavior.model");
    c.bindingHash = behaviorComputeAttr<std::string>(op, "behavior.bindingHash");
    c.inputWidths = behaviorComputeAttr<std::vector<int64_t>>(op, "behavior.inputWidths");
    c.outputWidths = behaviorComputeAttr<std::vector<int64_t>>(op, "behavior.outputWidths");
    if (c.model != "int-mul" && c.model != "fp-mul" &&
        c.model != "vec-int-mul" && c.model != "vec-fp-mul" &&
        c.model != "int-alu" && c.model != "int-bku" &&
        c.model != "vec-int-alu-lane" && c.model != "vec-int-alu-whole" &&
        c.model != "vec-int-pu-lane" && c.model != "vec-int-pu-whole" &&
        c.model != "vec-int-mac-lane" && c.model != "vec-int-mac-whole")
        throw std::runtime_error("unknown behavior compute model: " + c.model);
    if (auto attr = op.attr("behavior.boundary"); attr) {
        if (!std::holds_alternative<std::string>(*attr))
            throw std::runtime_error("invalid behavior compute boundary");
        c.boundary = std::get<std::string>(*attr);
    }
    if (auto attr = op.attr("behavior.operations"); attr) {
        if (!std::holds_alternative<std::vector<std::string>>(*attr))
            throw std::runtime_error("invalid behavior compute operation list");
        c.operations = std::get<std::vector<std::string>>(*attr);
    }
    if (c.boundary != "none" && c.boundary != "lane-core" && c.boundary != "whole-unit")
        throw std::runtime_error("unknown behavior compute boundary: " + c.boundary);
    if (c.model.ends_with("-lane") && c.boundary != "lane-core")
        throw std::runtime_error("lane behavior model requires lane-core boundary");
    if (c.model.ends_with("-whole") && c.boundary != "whole-unit")
        throw std::runtime_error("whole behavior model requires whole-unit boundary");
    if (c.inputWidths.size() != op.operands().size() ||
        c.outputWidths.size() != op.results().size() || c.inputWidths.empty() || c.outputWidths.empty())
        throw std::runtime_error("behavior compute port metadata/arity mismatch");
    for (size_t i = 0; i < c.inputWidths.size(); ++i) {
        if (c.inputWidths[i] != graph.valueWidth(op.operands()[i]) ||
            graph.valueType(op.operands()[i]) != ValueType::Logic || c.inputWidths[i] <= 0)
            throw std::runtime_error("behavior compute input width/type mismatch");
    }
    for (size_t i = 0; i < c.outputWidths.size(); ++i) {
        if (c.outputWidths[i] != graph.valueWidth(op.results()[i]) ||
            graph.valueType(op.results()[i]) != ValueType::Logic || c.outputWidths[i] <= 0)
            throw std::runtime_error("behavior compute output width/type mismatch");
    }
    if (c.bindingHash.size() != 64)
        throw std::runtime_error("behavior compute binding hash must be SHA-256");
    return c;
}

} // namespace wolvrix::lib::grh
#endif
