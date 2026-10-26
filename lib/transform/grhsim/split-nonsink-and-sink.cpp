#include "transform/grhsim/split-nonsink-and-sink.hpp"

#include "partition-internal.hpp"

namespace wolvrix::lib::transform::grhsim {
namespace {
using namespace grh;
using namespace detail;
}

SplitNonsinkAndSinkPass::SplitNonsinkAndSinkPass(SplitNonsinkAndSinkOptions options)
    : Pass("grhsim-split-nonsink-and-sink", "grhsim-split-nonsink-and-sink", "Split General into nonsink and state sinks"),
      options_(std::move(options)) {}

bool SplitNonsinkAndSinkPass::supportsDialect(std::string_view dialect) const noexcept {
    return supports(dialect);
}

PassResult SplitNonsinkAndSinkPass::run() {
    PassResult result;
    Graph* graph = nullptr;
    try {
        if (options_.memMinBytes > uint64_t(INT64_MAX)) throw std::invalid_argument("mem-min-bytes exceeds checkpoint range");
        graph = &partitionGraph(design(), options_.top);
        if (stage(*graph) == "nonsink-sink-split") {
            if (graph->metadata("grhsim.memMinBytes") != std::optional<AttributeValue>(int64_t(options_.memMinBytes)))
                throw std::invalid_argument("Changed mem-min-bytes requires the event-output-split checkpoint");
            return result;
        }
        if (stage(*graph) != "event-output-split") throw std::invalid_argument("General split requires event-output-split stage");
        const auto topo = topological(*graph, [&](OperationId op) { return owner(*graph, op) == Owner{"general"}; });
        std::unordered_map<OperationId, std::string, OperationIdHash> policy;
        for (const auto op : graph->operations()) {
            if (graph->opKind(op) != OperationKind::kRegister && graph->opKind(op) != OperationKind::kLatch) continue;
            const auto type = stateLogicalType(*graph, op);
            policy[op] = type.kind == ValueType::Logic && !graph->getOperation(op).attr("grhsim.history") &&
                         (uint64_t(type.width) + 7) / 8 >= options_.memMinBytes ? "large-sink" : "small-sink";
        }
        // A pure cone can move to a sink only when every use already belongs
        // to that same sink. Cross-region reads and effectful samples stay put.
        std::unordered_map<OperationId, Owner, OperationIdHash> destinations;
        std::unordered_map<OperationId, OperationId, OperationIdHash> privateWriter;
        for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
            const auto op = *it;
            Owner destination{"general", "nonsink"};
            if (isWriter(graph->opKind(op))) {
                destination = {"general", policy.at(writerTarget(*graph, op))};
                privateWriter[op] = op;
            }
            else if (canCloneOperation(*graph, op)) {
                std::optional<Owner> only;
                OperationId writer;
                bool shared = false;
                for (const auto value : graph->opResults(op)) {
                    const auto view = graph->getValue(value);
                    if (view.isOutput() || view.isInout()) shared = true;
                    for (const auto& use : view.users()) {
                        const auto found = destinations.find(use.operation);
                        if (found == destinations.end() || found->second.size() != 2 || found->second[1] == "nonsink") {
                            shared = true; continue;
                        }
                        if (only && *only != found->second) shared = true;
                        only = found->second;
                        const auto userWriter = privateWriter.at(use.operation);
                        if (writer && writer != userWriter) shared = true;
                        writer = userWriter;
                    }
                }
                if (only && !shared) { destination = *only; privateWriter[op] = writer; }
            }
            destinations.emplace(op, std::move(destination));
        }
        result.changed = true;
        for (const auto& [op, value] : policy) graph->setAttr(op, kStatePolicy, value);
        for (const auto& [op, value] : destinations) graph->setAttr(op, kPartition, value);
        graph->setMetadata("grhsim.memMinBytes", int64_t(options_.memMinBytes));
        graph->setMetadata(kStage, std::string("nonsink-sink-split"));
        verifyPartitionResult(*graph);
    } catch (const std::exception& exception) {
        if (graph && result.changed) graph->setMetadata(kStage, std::string("nonsink-sink-split-failed"));
        error(exception.what(), graph ? graph->symbol() : std::string{});
        result.failed = true;
    }
    return result;
}

}
