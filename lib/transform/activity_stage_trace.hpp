#pragma once
// Research-only, read-only exports. IDs are local to a snapshot, not match keys.
#include "core/grh.hpp"
#include <array>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace wolvrix::lib::transform::stage_trace {
inline std::filesystem::path directory() {
    const char* value = std::getenv("WOLVRIX_ACTIVITY_STAGE_TRACE");
    return value && *value ? std::filesystem::path(value) : std::filesystem::path();
}
inline std::ofstream open(const std::string& name) {
    const auto root = directory();
    if (root.empty()) return {};
    std::filesystem::create_directories(root);
    std::ofstream out(root / name, std::ios::binary);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    return out;
}
inline void word(std::ostream& out, uint32_t value) {
    const std::array<unsigned char, 4> bytes{static_cast<unsigned char>(value),
        static_cast<unsigned char>(value >> 8), static_cast<unsigned char>(value >> 16),
        static_cast<unsigned char>(value >> 24)};
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
template<class Range> void ids(std::ostream& out, const Range& range) {
    word(out, static_cast<uint32_t>(range.size()));
    for (const auto id : range) {
        if constexpr (std::is_integral_v<decltype(id)>) word(out, id);
        else word(out, id.index);
    }
}
inline void quoted(std::ostream& out, std::string_view text) {
    out << '"';
    for (char c : text) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c == '\n') out << "\\n";
        else if (c == '\r') out << "\\r";
        else if (c == '\t') out << "\\t";
        else out << c;
    }
    out << '"';
}
template<class T> void attr(std::ostream& out, const T& value) {
    if constexpr (std::is_same_v<T, std::string>) quoted(out, value);
    else if constexpr (std::is_arithmetic_v<T>) out << std::setprecision(17) << value;
    else {
        out << '[';
        bool first = true;
        for (const auto& item : value) {
            if (!first) out << ',';
            first = false;
            attr(out, item);
        }
        out << ']';
    }
}
inline std::string sourceKey(const grh::Graph& graph, grh::OperationId id) {
    const auto op = graph.getOperation(id);
    // The pre-schedule operation symbol is generated before the constant-table
    // rewrite and is unique in both frozen source sets. Width/operand details
    // are checked by the source-set inventory before this replay is allowed.
    return std::string(op.symbolText());
}
// Reject unknown, duplicate, added, or removed sources; never silently fall back.
inline void sourceOrder(const grh::Graph& graph, std::vector<grh::OperationId>& sources) {
    const char* replay = std::getenv("WOLVRIX_RESEARCH_SOURCE_ORDER");
    if (replay && *replay) {
        std::ifstream input(replay);
        if (!input) throw std::runtime_error("source-order replay cannot open reference");
        std::unordered_map<std::string, size_t> rank;
        std::string key;
        while (std::getline(input, key)) {
            const auto n = rank.size();
            if (!rank.emplace(key, n).second) throw std::runtime_error("source-order replay has ambiguous reference keys");
        }
        if (rank.size() != sources.size()) throw std::runtime_error("source-order replay source-set size differs");
        std::vector<grh::OperationId> ordered(sources.size(), grh::OperationId::invalid());
        for (const auto id : sources) {
            const auto it = rank.find(sourceKey(graph, id));
            if (it == rank.end()) {
                throw std::runtime_error("source-order replay missing semantic source key=" + sourceKey(graph, id) +
                                         " rank_size=" + std::to_string(rank.size()) +
                                         " source_size=" + std::to_string(sources.size()));
            }
            if (ordered[it->second].valid()) throw std::runtime_error("source-order replay source key is ambiguous");
            ordered[it->second] = id;
        }
        sources = std::move(ordered);
    }
    if (!directory().empty()) {
        auto out = open("source-order.keys.txt");
        for (const auto id : sources) out << sourceKey(graph, id) << '\n';
        if (replay && *replay) {
            auto gate = open("source-order-replay.txt");
            gate << "exact semantic source set verified; replayed " << sources.size() << " entries\n";
        }
    }
}
template<class Data, class Canonical>
void graph(const grh::Graph& graph, const Data& data, const Canonical& canonical, const std::string& stage) {
    if (directory().empty()) return;
    auto ops = open(stage + ".ops.tsv");
    auto values = open(stage + ".values.tsv");
    auto topology = open(stage + ".topo.bin");
    ids(topology, data.topoOps);
    ops << "id\tkind\tsymbol\tattrs\tfile\tline\tcol\tinputs\tresults\n";
    for (auto id : graph.operations()) {
        const auto op = graph.getOperation(id);
        ops << id.index << '\t' << grh::toString(op.kind()) << '\t';
        quoted(ops, op.symbolText()); ops << '\t';
        // Type tags and length-delimited JSON preserve attribute distinctions.
        ops << '[';
        bool first = true;
        for (const auto& a : op.attrs()) {
            if (!first) ops << ',';
            first = false;
            ops << '['; quoted(ops, a.key);
            ops << ',' << a.value.index() << ',';
            std::visit([&](const auto& v) { attr(ops, v); }, a.value);
            ops << ']';
        }
        ops << "]\t";
        const auto loc = graph.opSrcLoc(id);
        quoted(ops, loc ? loc->file : "");
        ops << '\t' << (loc ? loc->line : 0) << '\t' << (loc ? loc->column : 0) << '\t';
        first = true;
        for (auto v : op.operands()) { if (!first) ops << ','; first = false; ops << v.index; }
        ops << '\t'; first = true;
        for (auto v : op.results()) { if (!first) ops << ','; first = false; ops << v.index; }
        ops << '\n';
    }
    values << "id\tcanonical\twidth\tsigned\tdef\tname\n";
    for (auto id : graph.values()) {
        const auto v = graph.getValue(id);
        const auto it = canonical.find(id);
        values << id.index << '\t' << (it == canonical.end() ? id.index : it->second.index)
               << '\t' << v.width() << '\t' << v.isSigned() << '\t' << v.definingOp().index << '\t';
        quoted(values, v.symbolText()); values << '\n';
    }
}
template<class Nodes> void nodes(const Nodes& nodes, const std::string& stage) {
    if (directory().empty()) return;
    auto out = open(stage + ".nodes.bin");
    word(out, static_cast<uint32_t>(nodes.size()));
    for (const auto& node : nodes) {
        word(out, static_cast<uint32_t>(node.commonExpr) | (static_cast<uint32_t>(node.indivisible) << 1));
        ids(out, node.ops); ids(out, node.boundaryInputs);
    }
}
template<class Groups> void groups(const Groups& groups, const std::string& stage) {
    if (directory().empty()) return;
    auto out = open(stage + ".groups.bin");
    word(out, static_cast<uint32_t>(groups.size()));
    for (const auto& group : groups) ids(out, group);
}
template<class Range> void order(const Range& range, const std::string& stage) {
    if (directory().empty()) return;
    auto out = open(stage + ".order.bin");
    ids(out, range);
}
} // namespace wolvrix::lib::transform::stage_trace
