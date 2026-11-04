#include "core/allocator.hpp"
#include "core/grh.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace wolvrix::lib::grh;
using Statistics = wolvrix::lib::allocator::Statistics;

constexpr double nearZeroThreshold = 0.01;
constexpr std::size_t nearZeroMinimumOperations = 1048576;
constexpr std::array<int64_t, 2> payloadShape{2, 2};
constexpr std::array<std::string_view, 3> workloadNames{
    "topology", "shared-payload", "long-names"};
constexpr std::array<std::string_view, 4> caseNames{
    "construct", "clone-mutable", "clone-compacted", "import-cross-design"};

struct Options {
    std::vector<std::size_t> sizes{4096, 65536, 1048576};
    std::vector<std::string_view> workloads{workloadNames.begin(), workloadNames.end()};
    std::vector<std::string_view> cases{caseNames.begin(), caseNames.end()};
    std::filesystem::path output;
    bool reserveHints = false;
    bool help = false;
};

struct Measurement {
    Statistics before;
    Statistics after;
    uint64_t allocations = 0;
};

struct Result {
    std::string_view workload;
    std::string_view operation;
    std::string_view sourceState;
    std::size_t operations = 0;
    std::size_t values = 0;
    Measurement measurement;
    bool verified = false;
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Statistics snapshot() {
    const Statistics value = wolvrix::lib::allocator::statistics();
    require(value.enabled, "bundled mimalloc MI_STAT=1 statistics are required");
    require(value.liveThreads == 0, "allocator snapshot has a live worker thread");
    require(value.allocations == value.normalAllocations + value.hugeAllocations,
            "allocator count does not equal normal plus huge allocations");
    return value;
}

Measurement finish(const Statistics& before) {
    const Statistics after = snapshot();
    require(after.allocations >= before.allocations, "allocator counters decreased");
    require(after.normalAllocations >= before.normalAllocations &&
                after.hugeAllocations >= before.hugeAllocations,
            "allocator component counters decreased");
    return {before, after, after.allocations - before.allocations};
}

void* volatile controlAllocation = nullptr;

std::array<Measurement, 2> controls() {
    // Initialize allocator statistics before testing whether snapshots allocate.
    (void)snapshot();
    const Statistics emptyBefore = snapshot();
    const Measurement empty = finish(emptyBefore);
    require(empty.allocations == 0, "empty snapshot control allocated");
    const Statistics positiveBefore = snapshot();
    controlAllocation = ::operator new(64);
    const Measurement positive = finish(positiveBefore);
    ::operator delete(controlAllocation);
    controlAllocation = nullptr;
    require(positive.allocations >= 1, "operator new did not reach the measured allocator");
    return {empty, positive};
}

template <typename Function>
void split(std::string_view text, Function function) {
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto part = text.substr(0, comma);
        require(!part.empty(), "empty comma-separated option");
        function(part);
        if (comma == std::string_view::npos) return;
        text.remove_prefix(comma + 1);
        require(!text.empty(), "empty comma-separated option");
    }
}

Options options(int argc, char** argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help") {
            result.help = true;
            continue;
        }
        if (arg == "--reserve") {
            result.reserveHints = true;
            continue;
        }
        require(i + 1 < argc, "option requires a value");
        const std::string_view value = argv[++i];
        if (arg == "--output") result.output = value;
        else if (arg == "--sizes") {
            result.sizes.clear();
            split(value, [&](std::string_view part) {
                std::size_t count = 0;
                const auto parsed = std::from_chars(part.data(), part.data() + part.size(), count);
                require(parsed.ec == std::errc{} && parsed.ptr == part.data() + part.size() &&
                            count > 0 && count <= std::numeric_limits<uint32_t>::max() / 2,
                        "invalid operation count");
                result.sizes.push_back(count);
            });
        } else if (arg == "--workloads" || arg == "--cases") {
            auto& destination = arg == "--workloads" ? result.workloads : result.cases;
            destination.clear();
            split(value, [&](std::string_view part) {
                const bool known = arg == "--workloads"
                    ? std::find(workloadNames.begin(), workloadNames.end(), part) != workloadNames.end()
                    : std::find(caseNames.begin(), caseNames.end(), part) != caseNames.end();
                require(known, "unknown workload or case");
                destination.push_back(part);
            });
        } else throw std::invalid_argument("unknown allocation benchmark option");
    }
    require(!result.sizes.empty() && !result.workloads.empty() && !result.cases.empty(),
            "benchmark selections must not be empty");
    return result;
}

struct Inputs {
    Design design;
    Graph& graph;
    ValueId value;
    OperationId operation;
    SrcLoc location;

    Inputs() : graph(design.createGraph("payload-input")) {
        value = graph.createValue(32, false, ValueType::Logic, payloadShape);
        operation = graph.createOperation(OperationKind::kConstant);
        graph.addResult(operation, value);
        graph.setAttr(operation, "bool", true);
        graph.setAttr(operation, "int", int64_t{7});
        graph.setAttr(operation, "double", 1.5);
        graph.setAttr(operation, "string", std::string("repeated payload text longer than a short string"));
        graph.setAttr(operation, "bits", std::vector<bool>{true, false, true, true, false});
        graph.setAttr(operation, "ints", std::vector<int64_t>{1, 2, 3});
        graph.setAttr(operation, "doubles", std::vector<double>{1.5, 2.5});
        graph.setAttr(operation, "strings", std::vector<std::string>{
            "first repeated list text longer than a short string", "second repeated list text"});
        location.file = "/allocation-bench/repeated/long/source/location/fixture.fir";
        location.origin = "repeated origin longer than a short string";
        location.pass = "repeated-pass-longer-than-SSO";
        location.note = "repeated note longer than a short string";
        location.line = 37;
        location.column = 11;
        location.endLine = 38;
        location.endColumn = 19;
        graph.setValueSrcLoc(value, location);
        graph.setOpSrcLoc(operation, location);
    }
};

std::string_view longName(std::array<char, 128>& buffer, bool operation, std::size_t index) {
    const std::string_view prefix = operation
        ? "high_cardinality_operation_symbol_longer_than_SSO_"
        : "high_cardinality_value_symbol_longer_than_SSO_";
    std::copy(prefix.begin(), prefix.end(), buffer.begin());
    const auto converted = std::to_chars(buffer.data() + prefix.size(),
                                         buffer.data() + buffer.size(), index);
    require(converted.ec == std::errc{}, "symbol formatting overflow");
    return {buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())};
}

ValueId createValue(Graph& graph, bool named, bool payload, std::size_t index) {
    const std::span<const int64_t> shape = payload
        ? std::span<const int64_t>(payloadShape) : std::span<const int64_t>{};
    if (!named) return graph.createValue(32, false, ValueType::Logic, shape);
    std::array<char, 128> buffer;
    return graph.createValue(graph.internSymbol(longName(buffer, false, index)),
                             32, false, ValueType::Logic, shape);
}

OperationId createOperation(Graph& graph, bool named, std::size_t index) {
    if (!named) return graph.createOperation(OperationKind::kAssign);
    std::array<char, 128> buffer;
    return graph.createOperation(OperationKind::kAssign,
                                 graph.internSymbol(longName(buffer, true, index)));
}

Graph& populate(Design& design, std::size_t count, std::string_view workload,
                bool reserveHints, const Inputs& inputs) {
    Graph& graph = design.createGraph("source");
    if (reserveHints) {
        graph.reserveSymbolCapacity(2 * count + 1);
        graph.reserveValueCapacity(count + 1);
        graph.reserveOperationCapacity(count);
    }
    const bool named = workload == "long-names";
    const bool payload = workload != "topology";
    ValueId previous = createValue(graph, named, payload, 0);
    graph.bindInputPort("in", previous);
    if (payload) graph.copyValueSrcLoc(previous, inputs.graph, inputs.value);
    for (std::size_t index = 0; index < count; ++index) {
        const ValueId value = createValue(graph, named, payload, index + 1);
        const OperationId op = createOperation(graph, named, index);
        if (reserveHints) {
            graph.reserveOpOperandCapacity(op, 1);
            graph.reserveOpResultCapacity(op, 1);
            if (payload) graph.reserveOpAttrCapacity(op, 8);
        }
        graph.addOperand(op, previous);
        graph.addResult(op, value);
        if (payload) {
            for (const AttrRef attr : inputs.graph.opAttrRange(inputs.operation))
                graph.setAttrView(op, attr.key(), attr.value());
            graph.copyValueSrcLoc(value, inputs.graph, inputs.value);
            graph.copyOpSrcLoc(op, inputs.graph, inputs.operation);
        }
        previous = value;
    }
    graph.bindOutputPort("out", previous);
    return graph;
}

bool sameAttribute(AttrValueView lhs, AttrValueView rhs) {
    if (lhs.kind() != rhs.kind()) return false;
    const auto equal = [](const auto& a, const auto& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
    };
    switch (lhs.kind()) {
    case AttrValueKind::Bool: return lhs.asBool() == rhs.asBool();
    case AttrValueKind::Int: return lhs.asInt() == rhs.asInt();
    case AttrValueKind::Double: return lhs.asDouble() == rhs.asDouble();
    case AttrValueKind::String: return lhs.asString() == rhs.asString();
    case AttrValueKind::BoolList: return equal(lhs.asBoolList(), rhs.asBoolList());
    case AttrValueKind::IntList: return equal(lhs.asIntList(), rhs.asIntList());
    case AttrValueKind::DoubleList: return equal(lhs.asDoubleList(), rhs.asDoubleList());
    case AttrValueKind::StringList: return equal(lhs.asStringList(), rhs.asStringList());
    }
    return false;
}

bool sameLocation(const std::optional<SrcLoc>& value, const SrcLoc& expected) {
    return value && value->file == expected.file && value->origin == expected.origin &&
        value->pass == expected.pass && value->note == expected.note &&
        value->line == expected.line && value->column == expected.column &&
        value->endLine == expected.endLine && value->endColumn == expected.endColumn;
}

void verify(const Graph& graph, std::size_t count, std::string_view workload,
            const Inputs& inputs) {
    // Reload spans/IDs from the actual result, including after source compact.
    const auto values = graph.values();
    const auto operations = graph.operations();
    require(values.size() == count + 1 && operations.size() == count,
            "result entity count mismatch");
    require(graph.inputPortValue("in") == values.front() &&
                graph.outputPortValue("out") == values.back(), "result port mismatch");
    const bool payload = workload != "topology";
    const std::span<const int64_t> expectedShape = payload
        ? std::span<const int64_t>(payloadShape) : std::span<const int64_t>{};
    for (const ValueId value : values) {
        const auto shape = graph.valueShape(value);
        require(graph.valueWidth(value) == 32 && shape.size() == expectedShape.size() &&
                    std::equal(shape.begin(), shape.end(), expectedShape.begin()),
                "result value width or shape mismatch");
    }
    for (std::size_t index = 0; index < count; ++index) {
        const OperationId op = operations[index];
        const auto operands = graph.opOperands(op);
        const auto results = graph.opResults(op);
        require(graph.opKind(op) == OperationKind::kAssign && operands.size() == 1 &&
                    operands[0] == values[index] && results.size() == 1 &&
                    results[0] == values[index + 1], "result chain mismatch");
        require(graph.valueDef(values[index + 1]) == op &&
                    graph.valueWidth(values[index + 1]) == 32,
                "result value definition or width mismatch");
        const auto users = graph.valueUsers(values[index]);
        require(users.size() == 1 && users[0].operation == op && users[0].operandIndex == 0,
                "result use-list mismatch");
        const auto attrs = graph.opAttrRange(op);
        require(attrs.size() == (payload ? 8 : 0), "result attribute count mismatch");
        if (payload) {
            for (const AttrRef sourceAttr : inputs.graph.opAttrRange(inputs.operation)) {
                const auto attr = attrs.find(sourceAttr.key());
                require(attr && sameAttribute(attr->value(), sourceAttr.value()),
                        "result attribute payload mismatch");
            }
        }
        if (workload == "long-names") {
            std::array<char, 128> opBuffer;
            std::array<char, 128> valueBuffer;
            require(graph.symbolText(graph.operationSymbol(op)) == longName(opBuffer, true, index) &&
                        graph.symbolText(graph.valueSymbol(values[index + 1])) ==
                            longName(valueBuffer, false, index + 1), "result long symbol mismatch");
        }
    }
    require(graph.valueUsers(values.back()).empty(), "last result has unexpected users");
    // Owning SrcLoc reads are deliberately outside measured intervals. Sample
    // the beginning, middle and end; complete clone semantics have core tests.
    for (const std::size_t index : {std::size_t{0}, count / 2, count - 1}) {
        if (payload) {
            require(sameLocation(graph.opSrcLoc(operations[index]), inputs.location) &&
                        sameLocation(graph.valueSrcLoc(values[index + 1]), inputs.location),
                    "result source location mismatch");
        } else require(!graph.opSrcLoc(operations[index]) &&
                           !graph.valueSrcLoc(values[index + 1]),
                       "topology-only result has source locations");
    }
}

Result run(std::size_t count, std::string_view workload, std::string_view operation,
           bool reserveHints, const Inputs& inputs) {
    Result result{workload, operation, "none", count, count + 1, {}, false};
    if (operation == "construct") {
        std::optional<Design> destination;
        const Statistics before = snapshot();
        destination.emplace();
        Graph& graph = populate(*destination, count, workload, reserveHints, inputs);
        result.measurement = finish(before);
        verify(graph, count, workload, inputs);
    } else {
        Design sourceOwner;
        Graph& source = populate(sourceOwner, count, workload, reserveHints, inputs);
        if (operation == "clone-compacted") source.compact();
        const bool sourceCompacted = source.frozen();
        result.sourceState = sourceCompacted ? "compacted" : "mutable";
        verify(source, count, workload, inputs);
        std::optional<Design> destination;
        const Statistics before = snapshot();
        Graph* graph;
        if (operation == "import-cross-design") {
            destination.emplace();
            graph = &destination->importGraph(source, "clone");
        } else graph = &sourceOwner.cloneGraph("source", "clone");
        result.measurement = finish(before);
        verify(*graph, count, workload, inputs);
        require(source.frozen() == sourceCompacted, "clone changed source compact state");
        verify(source, count, workload, inputs);
    }
    // Graph destruction and all verification happen after the closing snapshot.
    result.verified = true;
    return result;
}

void string(std::ostream& output, std::string_view text) {
    output << '"';
    for (const unsigned char ch : text) {
        if (ch == '"' || ch == '\\') output << '\\' << static_cast<char>(ch);
        else if (ch < 32) {
            const char* digits = "0123456789abcdef";
            output << "\\u00" << digits[ch >> 4] << digits[ch & 15];
        } else output << static_cast<char>(ch);
    }
    output << '"';
}

void statistics(std::ostream& output, const Statistics& value) {
    output << "{\"enabled\":" << (value.enabled ? "true" : "false")
           << ",\"allocations\":" << value.allocations
           << ",\"normal_allocations\":" << value.normalAllocations
           << ",\"huge_allocations\":" << value.hugeAllocations
           << ",\"live_allocator_threads\":" << value.liveThreads
           << ",\"all_threads_merged\":" << (value.liveThreads == 0 ? "true" : "false") << '}';
}

void measurement(std::ostream& output, const Measurement& value) {
    output << "{\"allocations\":" << value.allocations
           << ",\"normal_allocations\":" << value.after.normalAllocations - value.before.normalAllocations
           << ",\"huge_allocations\":" << value.after.hugeAllocations - value.before.hugeAllocations
           << ",\"before\":";
    statistics(output, value.before);
    output << ",\"after\":";
    statistics(output, value.after);
    output << '}';
}

void report(std::ostream& output, const Options& configuration,
            const std::array<Measurement, 2>& control, const std::vector<Result>& results) {
    const auto allocator = wolvrix::lib::allocator::configuration();
    output << std::setprecision(12)
           << "{\n  \"schema\":\"wolvrix.grh-allocation-bench.v1\",\n"
           << "  \"scope\":\"native Graph construction/clone/import allocation events only; "
              "input preparation, verification, output and destruction excluded\",\n"
           << "  \"allocator_source\":\"bundled-mimalloc-MI_STAT=1\",\n"
           << "  \"allocator_configuration\":{\"mimalloc\":" << (allocator.mimalloc ? "true" : "false")
           << ",\"project_defaults\":" << (allocator.projectDefaults ? "true" : "false")
           << ",\"generic_collect\":" << allocator.genericCollect << "},\n"
           << "  \"reserve_hints_inside_measured_scope\":"
           << (configuration.reserveHints ? "true" : "false") << ",\n"
           << "  \"graph_profile_enabled\":false,\n"
           << "  \"near_zero_observation\":{\"threshold_allocations_per_op\":" << nearZeroThreshold
           << ",\"minimum_operations\":" << nearZeroMinimumOperations
           << ",\"affects_process_exit\":false,\"meaning\":\"planning observation; "
              "smaller cases show startup amortization and never gate acceptance\"},\n"
           << "  \"controls\":{\"empty\":";
    measurement(output, control[0]);
    output << ",\"positive_operator_new\":";
    measurement(output, control[1]);
    output << "},\n  \"results\":[\n";
    bool first = true;
    for (const Result& result : results) {
        if (!first) output << ",\n";
        first = false;
        output << "    {\"workload\":";
        string(output, result.workload);
        output << ",\"case\":";
        string(output, result.operation);
        output << ",\"source_state\":";
        string(output, result.sourceState);
        const double perOp = static_cast<double>(result.measurement.allocations) / result.operations;
        output << ",\"operations\":" << result.operations << ",\"values\":" << result.values
               << ",\"measurement\":";
        measurement(output, result.measurement);
        output << ",\"allocations_per_op\":" << perOp
               << ",\"below_planning_threshold\":" << (perOp <= nearZeroThreshold ? "true" : "false")
               << ",\"near_zero_observation_pass\":";
        if (result.operations < nearZeroMinimumOperations) output << "null";
        else output << (perOp <= nearZeroThreshold ? "true" : "false");
        output << ",\"verified\":" << (result.verified ? "true" : "false") << '}';
    }
    output << "\n  ],\n  \"integrity_pass\":true\n}\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options configuration = options(argc, argv);
        if (configuration.help) {
            std::cout << "grh-allocation-bench [--sizes 4096,65536,1048576] [--reserve] "
                         "[--workloads topology,shared-payload,long-names] "
                         "[--cases construct,clone-mutable,clone-compacted,import-cross-design] "
                         "[--output results.json]\n";
            return 0;
        }
        setGraphProfileEnabled(false);
        require(wolvrix::lib::allocator::configuration().mimalloc,
                "native mimalloc allocator is required");
        const auto control = controls();
        const Inputs inputs;
        std::vector<Result> results;
        results.reserve(configuration.sizes.size() * configuration.workloads.size() * configuration.cases.size());
        for (const std::size_t count : configuration.sizes) {
            for (const std::string_view workload : configuration.workloads) {
                for (const std::string_view operation : configuration.cases) {
                    std::cerr << "[grh-allocation-bench] " << workload << ' ' << operation
                              << " operations=" << count << '\n';
                    results.push_back(run(count, workload, operation, configuration.reserveHints, inputs));
                }
            }
        }
        if (configuration.output.empty()) report(std::cout, configuration, control, results);
        else {
            if (!configuration.output.parent_path().empty())
                std::filesystem::create_directories(configuration.output.parent_path());
            std::ofstream output(configuration.output);
            require(output.is_open(), "could not open allocation benchmark JSON output");
            report(output, configuration, control, results);
            require(static_cast<bool>(output), "could not write allocation benchmark JSON output");
            std::cerr << "[grh-allocation-bench] wrote " << configuration.output << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[grh-allocation-bench] " << error.what() << '\n';
        return 1;
    }
}
