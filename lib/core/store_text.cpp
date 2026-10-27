#include "core/store.hpp"

#include "core/op_descriptor.hpp"
#include "core/state.hpp"
#include "transform/grhsim/ops.hpp"
#include "transform/grhsim/sink-schedule.hpp"

#include <algorithm>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <type_traits>

namespace wolvrix::lib::store {
namespace {
namespace sim = transform::grhsim;
using Key = sim::PartitionKey;

void quoted(std::ostream& out, std::string_view value) {
    out << '"';
    for (const unsigned char ch : value) {
        if (ch == '\\' || ch == '"') out << '\\' << static_cast<char>(ch);
        else if (ch == '\n') out << "\\n";
        else if (ch == '\r') out << "\\r";
        else if (ch == '\t') out << "\\t";
        else if (ch < 0x20 || ch == 0x7f) {
            const auto flags = out.flags();
            const auto fill = out.fill();
            out << "\\x" << std::hex << std::setw(2) << std::setfill('0') << unsigned(ch);
            out.flags(flags); out.fill(fill);
        } else out << static_cast<char>(ch);
    }
    out << '"';
}

template <typename T>
void attribute(std::ostream& out, const T& value) {
    if constexpr (std::is_same_v<T, std::string>) quoted(out, value);
    else if constexpr (std::is_same_v<T, bool>) out << (value ? "true" : "false");
    else if constexpr (std::is_arithmetic_v<T>) out << value;
    else {
        out << '[';
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (i) out << ", ";
            attribute(out, typename T::value_type(value[i]));
        }
        out << ']';
    }
}

void attribute(std::ostream& out, const grh::AttributeValue& value) {
    std::visit([&](const auto& item) { attribute(out, item); }, value);
}

void key(std::ostream& out, const Key& path) { attribute(out, path); }

Key owner(const grh::Graph& graph, grh::OperationId op) {
    return sim::attribute<Key>(graph, op, sim::kPartition).value_or(Key{});
}

Key executionOwner(Key path) {
    if (path.size() == 4 && path[0] == "general" && path[1] == "nonsink") path.resize(3);
    return path;
}

void opRef(std::ostream& out, const grh::Graph& graph, grh::OperationId op) {
    if (!op) { out << "none"; return; }
    out << "op" << op.index << '.' << op.generation << ' ';
    quoted(out, graph.symbolText(graph.operationSymbol(op)));
}

void valueRef(std::ostream& out, const grh::Graph& graph, grh::ValueId value) {
    if (!value) { out << "none"; return; }
    out << 'v' << value.index << '.' << value.generation << ' ';
    quoted(out, graph.symbolText(graph.valueSymbol(value)));
}

void type(std::ostream& out, const grh::LogicalType& value) {
    out << grh::toString(value.kind);
    if (value.kind == grh::ValueType::Logic)
        out << '<' << value.width << ", " << (value.isSigned ? "signed" : "unsigned")
            << ", " << grh::toString(value.domain) << '>';
    if (!value.shape.empty()) { out << " shape="; attribute(out, value.shape); out << " leaf-width=" << value.leafWidth(); }
}

void source(std::ostream& out, const std::optional<grh::SrcLoc>& location, std::string_view indent) {
    out << indent << "source: ";
    if (!location) { out << "unavailable\n"; return; }
    const auto& loc = *location;
    if (!loc.file.empty()) { quoted(out, loc.file); out << ':' << loc.line << ':' << loc.column; }
    else out << "unavailable";
    if (loc.endLine || loc.endColumn) out << " end=" << loc.endLine << ':' << loc.endColumn;
    if (!loc.origin.empty()) { out << " origin="; quoted(out, loc.origin); }
    if (!loc.pass.empty()) { out << " pass="; quoted(out, loc.pass); }
    if (!loc.note.empty()) { out << " note="; quoted(out, loc.note); }
    out << '\n';
}

std::string_view effectName(grh::OpEffect effect) {
    switch (effect) {
    case grh::OpEffect::Pure: return "pure";
    case grh::OpEffect::Declaration: return "declaration";
    case grh::OpEffect::ReadState: return "read-state";
    case grh::OpEffect::WriteState: return "write-state";
    case grh::OpEffect::History: return "history";
    case grh::OpEffect::External: return "external";
    }
    return "unknown";
}

std::string operandRole(const grh::Graph& graph, grh::OperationId op, std::size_t index) {
    const auto kind = graph.opKind(op);
    const auto size = graph.opOperands(op).size();
    if (sim::isWriter(kind)) {
        const bool latch = kind == sim::kLatchWrite;
        const bool masked = latch || kind == sim::kWriteMask || kind == sim::kWriteArrayMask;
        if (!latch && index == 0) return "fire";
        if (index == (latch ? 0u : 1u)) return "enable";
        if (masked && index + 1 == size) return "mask";
        if (index + (masked ? 2u : 1u) == size) return "data";
        return "index[" + std::to_string(index - 2) + "]";
    }
    if (kind == sim::kEdgeDetect) return "event-value";
    if (kind == sim::kHistoryWrite) return "sampled-value";
    if (kind == sim::kSystemTask || kind == sim::kDpiCall)
        return index == 0 ? "fire" : index == 1 ? "call-condition" : "argument[" + std::to_string(index - 2) + "]";
    if (kind == grh::OperationKind::kRegisterReadArrayPort) return "index[" + std::to_string(index) + "]";
    if (kind == grh::OperationKind::kMux) return index == 0 ? "condition" : index == 1 ? "true-value" : "false-value";
    return "arg[" + std::to_string(index) + "]";
}

void partitions(std::ostream& out, const grh::Graph& graph) {
    struct Node { std::size_t direct = 0, total = 0; };
    std::map<Key, Node> tree;
    std::map<Key, std::vector<grh::OperationId>> execution;
    std::size_t unowned = 0;
    for (const auto op : graph.operations()) {
        const auto path = owner(graph, op);
        if (path.empty()) { ++unowned; continue; }
        execution[executionOwner(path)].push_back(op);
        ++tree[path].direct;
        for (std::size_t depth = 1; depth <= path.size(); ++depth)
            ++tree[Key(path.begin(), path.begin() + depth)].total;
    }
    out << "  logical-partition-tree:\n";
    out << "    # Full leaf paths constrain rewrites; parent containers do not own executable ops.\n";
    for (const auto& [path, counts] : tree) {
        out << std::string(2 + path.size() * 2, ' '); key(out, path);
        out << " direct-ops=" << counts.direct << " subtree-ops=" << counts.total << '\n';
    }
    out << "    unowned-ops=" << unowned << " (declarations or not partitioned yet; inspect effect/stage)\n";
    out << "  execution-owners:\n";
    out << "    # CPU nonsink execution collapses [general,nonsink,supernode,leaf] to the first 3 components.\n";
    out << "    # Owner keys below are identities, not emitted sn_N ordinals or C++ chunk boundaries.\n";
    std::map<Key, std::set<Key>> dependencies;
    for (const auto& [path, operations] : execution) {
        for (const auto op : operations) for (const auto value : graph.opOperands(op)) {
            const auto producer = graph.valueDef(value);
            if (!producer) continue;
            const auto from = executionOwner(owner(graph, producer));
            if (!from.empty() && from != path && from.front() == path.front()) dependencies[path].insert(from);
        }
    }
    std::map<int64_t, Key> effects;
    for (const auto op : graph.operations())
        if (const auto order = sim::attribute<int64_t>(graph, op, "grhsim.effectOrder"))
            effects.emplace(*order, executionOwner(owner(graph, op)));
    Key previous;
    for (const auto& [order, path] : effects) {
        if (!previous.empty() && path != previous && !path.empty() && path.front() == previous.front())
            dependencies[path].insert(previous);
        previous = path;
    }
    for (const auto& [path, operations] : execution) {
        out << "    owner "; key(out, path); out << " ops=" << operations.size() << '\n';
        for (const auto& prerequisite : dependencies[path]) {
            out << "      requires-owner "; key(out, prerequisite); out << '\n';
        }
        out << "      members:";
        for (const auto op : operations) out << " op" << op.index << '.' << op.generation;
        out << '\n';
    }
    out << "    # Cross-phase pulses and held samples are protocol boundaries, not same-round DAG edges.\n";
}

void schedule(std::ostream& out, const grh::Graph& graph, const transform::SessionStore* session) {
    out << "  sink-schedule:\n";
    const sim::SinkSchedule* plan = nullptr;
    try { plan = &sim::requireSinkSchedule(session, graph); }
    catch (const std::exception& error) {
        out << "    status: unavailable ("; quoted(out, error.what()); out << ")\n";
        out << "    # Rebuild grhsim-partition-sink in the current session; JSON does not carry resolved schedule IDs.\n";
        return;
    }
    out << "    status: validated complete=" << (plan->complete ? "true" : "false")
        << " config-hash=" << plan->partitionConfigHash << '\n';
    out << "    stamp: instance=" << plan->stamp.instance << " revision=" << plan->stamp.revision
        << " graph=" << plan->stamp.top.index << '.' << plan->stamp.top.generation << '\n';
    std::unordered_map<grh::OperationId, std::size_t, grh::OperationIdHash> targetRanks, nextRank;
    for (const auto& run : plan->generalRuns) for (const auto writer : run.orderedWriters)
        targetRanks.emplace(writer, nextRank[sim::writerTarget(graph, writer)]++);
    for (std::size_t i = 0; i < plan->sinkGroups.size(); ++i) {
        const auto& group = plan->sinkGroups[i];
        out << "    group[" << i << "] owner="; key(out, group.partitionKey); out << " fire=";
        if (group.fire) valueRef(out, graph, group.fire); else out << "level-sensitive";
        out << '\n';
        for (const auto guard : group.guards) { out << "      hoisted-enable "; valueRef(out, graph, guard); out << '\n'; }
    }
    const auto runs = [&](std::string_view name, const auto& sequence) {
        out << "    " << name << ":\n";
        for (std::size_t i = 0; i < sequence.size(); ++i) {
            const auto& run = sequence[i];
            out << "      run[" << i << "] owner="; key(out, run.partitionKey); out << '\n';
            for (const auto& prerequisite : run.requiredNonsinkKeys) {
                out << "        requires-logical-leaf "; key(out, prerequisite);
                out << " execution-owner="; key(out, executionOwner(prerequisite)); out << '\n';
            }
            for (std::size_t j = 0; j < run.orderedWriters.size(); ++j) {
                const auto writer = run.orderedWriters[j];
                out << "        writer[" << j << "] "; opRef(out, graph, writer);
                out << " writerOrder=";
                if (const auto order = sim::attribute<int64_t>(graph, writer, "writerOrder")) out << *order;
                else out << "unavailable";
                out << " target-write-rank=" << targetRanks.at(writer);
                out << '\n';
                const auto args = graph.opOperands(writer);
                const bool latch = graph.opKind(writer) == sim::kLatchWrite;
                out << "          fire=";
                if (latch) out << "level-sensitive";
                else if (!args.empty()) valueRef(out, graph, args[0]);
                else out << "unavailable";
                out << " enable=";
                if (args.size() > (latch ? 0u : 1u)) valueRef(out, graph, args[latch ? 0 : 1]);
                else out << "unavailable";
                out << " target="; opRef(out, graph, sim::writerTarget(graph, writer)); out << '\n';
            }
        }
    };
    runs("general-runs (sample small next / large payload)", plan->generalRuns);
    runs("large-commit-runs (commit previously sampled payload)", plan->largeCommitRuns);
    out << "    # writerOrder is the explicit priority; target-write-rank counts that state's writers in schedule order.\n";
    out << "    # Runs preserve writer order; a group may occur in several runs. Equal writerOrder retains declaration order.\n";
}

void graphText(std::ostream& out, const grh::Graph& graph, const transform::SessionStore* session) {
    out << "graph "; quoted(out, graph.symbol()); out << " id=" << graph.id().index << '.' << graph.id().generation << " {\n";
    out << "  metadata:\n";
    for (const auto& item : graph.metadata()) { out << "    " << item.key << " = "; attribute(out, item.value); out << '\n'; }
    out << "  declaration-anchors:\n";
    for (const auto symbol : graph.declaredSymbols()) {
        out << "    "; quoted(out, graph.symbolText(symbol)); out << '\n';
    }
    out << "  generate-groups:\n";
    for (const auto& group : graph.generateGroups()) {
        out << "    scope="; quoted(out, graph.symbolText(group.scope));
        out << " name="; quoted(out, graph.symbolText(group.name)); out << '\n';
        for (std::size_t i = 0; i < group.symbols.size(); ++i) {
            out << "      iteration[" << i << "] "; quoted(out, graph.symbolText(group.symbols[i])); out << '\n';
        }
    }
    out << "  ports:\n";
    const auto ports = [&](std::string_view direction, const auto& ports) {
        for (const auto& port : ports) { out << "    " << direction << ' '; quoted(out, port.name); out << " -> "; valueRef(out, graph, port.value); out << '\n'; }
    };
    ports("input", graph.inputPorts()); ports("output", graph.outputPorts());
    for (const auto& port : graph.inoutPorts()) {
        out << "    inout "; quoted(out, port.name); out << " in="; valueRef(out, graph, port.in);
        out << " out="; valueRef(out, graph, port.out); out << " oe="; valueRef(out, graph, port.oe); out << '\n';
    }
    std::map<std::vector<std::string>, std::size_t> origins;
    for (const auto op : graph.operations())
        if (const auto paths = transform::getOriginModules(graph, op); paths && !paths->empty()) origins.try_emplace(*paths, origins.size());
    out << "  origin-sets:\n";
    out << "    # Defining module instance paths; shared logic can have several origins. Lists are pooled without truncation.\n";
    for (const auto& [paths, id] : origins) { out << "    origin-set[" << id << "] = "; attribute(out, paths); out << '\n'; }
    partitions(out, graph);
    if (graph.metadata(sim::kStage)) schedule(out, graph, session);
    out << "  values:\n";
    for (const auto value : graph.values()) {
        out << "    "; valueRef(out, graph, value); out << " : "; type(out, graph.valueLogicalType(value));
        out << " def="; opRef(out, graph, graph.valueDef(value)); out << '\n';
        source(out, graph.valueSrcLoc(value), "      ");
    }
    out << "  operations:\n";
    for (const auto op : graph.operations()) {
        out << "    "; opRef(out, graph, op);
        out << " kind=" << grh::toString(graph.opKind(op)) << " effect=" << effectName(grh::operationEffect(graph, op)) << '\n';
        out << "      logical-owner="; key(out, owner(graph, op)); out << " execution-owner="; key(out, executionOwner(owner(graph, op))); out << '\n';
        if (const auto paths = transform::getOriginModules(graph, op); paths && !paths->empty()) out << "      origin-set[" << origins.at(*paths) << "]\n";
        else out << "      origin: unavailable\n";
        source(out, graph.opSrcLoc(op), "      ");
        if (sim::isWriter(graph.opKind(op))) {
            out << "      target-state: "; opRef(out, graph, sim::writerTarget(graph, op)); out << '\n';
        }
        if (graph.opKind(op) == grh::OperationKind::kRegister || graph.opKind(op) == grh::OperationKind::kLatch) {
            out << "      state-type: "; type(out, grh::stateLogicalType(graph, op)); out << '\n';
        }
        const auto operands = graph.opOperands(op);
        for (std::size_t i = 0; i < operands.size(); ++i) {
            out << "      operand[" << i << "] " << operandRole(graph, op, i) << " = ";
            valueRef(out, graph, operands[i]); out << " : "; type(out, graph.valueLogicalType(operands[i])); out << '\n';
        }
        const auto results = graph.opResults(op);
        for (std::size_t i = 0; i < results.size(); ++i) {
            out << "      result[" << i << "] = "; valueRef(out, graph, results[i]);
            out << " : "; type(out, graph.valueLogicalType(results[i])); out << '\n';
        }
        for (const auto& item : graph.opAttrs(op)) {
            if (item.key == transform::kOriginModulesKey && std::holds_alternative<std::vector<std::string>>(item.value)) continue;
            out << "      attr " << item.key << " = "; attribute(out, item.value); out << '\n';
        }
    }
    out << "}\n\n";
}

void designText(std::ostream& out, const grh::Design& design, std::span<const grh::Graph* const> tops,
                const transform::SessionStore* session) {
    out << "# Wolvrix readable GRH / GrhSIM inspection v1\n";
    out << "# Entity IDs (op<index>.<generation>, v<index>.<generation>) identify this graph snapshot.\n";
    out << "# This is an inspection view; use GRH JSON for round-trip persistence.\n";
    out << "tops:"; for (const auto* top : tops) { out << ' '; quoted(out, top->symbol()); } out << "\n\n";
    out << "six-phase-model:\n";
    out << "  Input: snapshot public inputs once per eval; activate changed-input consumers.\n";
    out << "  repeat until Publish reports convergence (a round is not a hardware cycle):\n";
    out << "    Event: evaluate event cones; update edge history unconditionally; produce round fire pulses.\n";
    out << "    General: run active nonsink supernodes, then ordered writer runs; update small next and sample large payload.\n";
    out << "    LargeSink: commit the sampled large-state writer runs in their established order.\n";
    out << "    Publish: publish small next/current and large dirty/touched activity; choose the next round.\n";
    out << "  Output: observe converged state once per eval; run timeslot tasks and update history last.\n\n";
    std::vector<const grh::Graph*> graphs;
    for (const auto& [symbol, graph] : design.graphs()) graphs.push_back(graph.get());
    std::sort(graphs.begin(), graphs.end(), [](const auto* left, const auto* right) { return left->symbol() < right->symbol(); });
    for (const auto* graph : graphs) graphText(out, *graph, session);
}
}

StoreText::StoreText(StoreDiagnostics* diagnostics, const transform::SessionStore* session)
    : Store(diagnostics), session_(session) {}

std::optional<std::string> StoreText::storeToString(const grh::Design& design, const StoreOptions& options) {
    const auto tops = resolveTopGraphs(design, options);
    if (!validateTopGraphs(tops)) return std::nullopt;
    try {
        std::ostringstream out;
        designText(out, design, tops, session_);
        return out.str();
    } catch (const std::exception& error) {
        reportError("Failed to serialize readable design: " + std::string(error.what()));
        return std::nullopt;
    }
}

StoreResult StoreText::storeImpl(const grh::Design& design, std::span<const grh::Graph* const> topGraphs,
                                const StoreOptions& options) {
    StoreResult result;
    const auto path = resolveOutputDir(options) / options.outputFilename.value_or("design.grh.txt");
    auto out = openOutputFile(path);
    if (!out) { result.success = false; return result; }
    try {
        designText(*out, design, topGraphs, session_);
        out->flush();
        if (!*out) throw std::runtime_error("Failed to write readable design file");
        result.artifacts.push_back(path.string());
    } catch (const std::exception& error) {
        reportError("Failed to serialize readable design: " + std::string(error.what()), path.string());
        result.success = false;
    }
    return result;
}
} // namespace wolvrix::lib::store
