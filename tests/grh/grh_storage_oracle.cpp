#include "core/grh.hpp"
#include "core/store.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>

// This exact translation unit is compiled against both archived and current
// public headers. Keep backend-specific APIs and conditional code out of it.
using namespace wolvrix::lib::grh;

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void text(std::ostream& out, std::string_view value) {
    static constexpr char digits[] = "0123456789abcdef";
    out << value.size() << ':';
    for (const unsigned char ch : value)
        out << digits[ch >> 4] << digits[ch & 15];
}

void id(std::ostream& out, GraphId value) {
    out << value.index << ':' << value.generation;
}

template<typename Id>
void id(std::ostream& out, Id value) {
    out << value.index << ':' << value.generation << '@';
    id(out, value.graph);
}

void number(std::ostream& out, double value) {
    // Preserve signed zero and every finite floating-point bit.
    out << std::hex << std::setw(16) << std::setfill('0')
        << std::bit_cast<uint64_t>(value) << std::dec << std::setfill(' ');
}

void attribute(std::ostream& out, const AttributeValue& value) {
    out << "variant=" << value.index() << ' ';
    std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::string>) text(out, item);
        else if constexpr (std::is_same_v<T, double>) number(out, item);
        else if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, int64_t>) out << item;
        else {
            out << "count=" << item.size();
            for (const auto element : item) {
                out << ' ';
                if constexpr (std::is_same_v<T, std::vector<std::string>>) text(out, element);
                else if constexpr (std::is_same_v<T, std::vector<double>>) number(out, element);
                else out << element;
            }
        }
    }, value);
}

void location(std::ostream& out, const std::optional<SrcLoc>& value) {
    out << "present=" << value.has_value();
    if (!value) return;
    out << " file="; text(out, value->file);
    out << " line=" << value->line << " column=" << value->column
        << " endLine=" << value->endLine << " endColumn=" << value->endColumn;
    out << " origin="; text(out, value->origin);
    out << " pass="; text(out, value->pass);
    out << " note="; text(out, value->note);
}

std::string locationText(const std::optional<SrcLoc>& value) {
    std::ostringstream out;
    location(out, value);
    return out.str();
}

template<typename RangeA, typename RangeB>
bool sameIds(const RangeA& lhs, const RangeB& rhs) {
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

bool sameUsers(std::span<const ValueUser> lhs, std::span<const ValueUser> rhs) {
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin(),
        [](const ValueUser& a, const ValueUser& b) {
            return a.operation == b.operation && a.operandIndex == b.operandIndex;
        });
}

void graphTrace(const Graph& graph) {
    auto& out = std::cout;
    out << "graph "; text(out, graph.symbol()); out << " id="; id(out, graph.id()); out << '\n';
    // frozen() is a storage lifecycle marker: metadata writes deliberately no
    // longer thaw the new backend, so it is not a semantic equality field.
    out << "metadata count=" << graph.metadata().size() << '\n';
    for (const auto& entry : graph.metadata()) {
        text(out, entry.key); out << ' '; attribute(out, entry.value); out << '\n';
        require(graph.metadata(entry.key) == entry.value, "metadata lookup disagrees with iteration");
    }
    out << "declared count=" << graph.declaredSymbols().size() << '\n';
    for (const auto symbol : graph.declaredSymbols()) {
        out << symbol.value << ' '; text(out, graph.symbolText(symbol)); out << '\n';
        require(graph.isDeclaredSymbol(symbol), "declared symbol lookup failed");
    }
    out << "generate count=" << graph.generateGroups().size() << '\n';
    for (const auto& group : graph.generateGroups()) {
        out << "scope=" << group.scope.value << ' '; text(out, graph.symbolText(group.scope));
        out << " name=" << group.name.value << ' '; text(out, graph.symbolText(group.name));
        out << " count=" << group.symbols.size();
        for (const auto symbol : group.symbols) {
            out << ' ' << symbol.value << '='; text(out, graph.symbolText(symbol));
        }
        out << '\n';
    }
    const auto warnings = graph.validateDeclaredSymbols();
    out << "declaration-warnings count=" << warnings.size() << '\n';
    for (const auto& warning : warnings) { text(out, warning); out << '\n'; }
    out << "values count=" << graph.values().size() << '\n';
    for (const auto valueId : graph.values()) {
        const auto value = graph.getValue(valueId);
        require(value.id() == valueId && value.symbol() == graph.valueSymbol(valueId) &&
            value.width() == graph.valueWidth(valueId) && value.isSigned() == graph.valueSigned(valueId) &&
            value.type() == graph.valueType(valueId) && value.domain() == graph.valueDomain(valueId) &&
            value.logicalType() == graph.valueLogicalType(valueId) &&
            sameIds(value.shape(), graph.valueShape(valueId)) &&
            value.isInput() == graph.valueIsInput(valueId) && value.isOutput() == graph.valueIsOutput(valueId) &&
            value.isInout() == graph.valueIsInout(valueId) && value.definingOp() == graph.valueDef(valueId) &&
            locationText(value.srcLoc()) == locationText(graph.valueSrcLoc(valueId)) &&
            sameUsers(value.users(), graph.valueUsers(valueId)), "value snapshot disagrees with scalar API");
        out << "value "; id(out, valueId);
        out << " symbol=" << value.symbol().value << ' '; text(out, value.symbolText());
        if (value.symbol().valid()) {
            require(value.symbolText() == graph.symbolText(value.symbol()) &&
                graph.lookupSymbol(value.symbolText()) == value.symbol() &&
                graph.findValue(value.symbol()) == valueId && graph.findValue(value.symbolText()) == valueId,
                "value symbol lookup disagrees with snapshot");
        }
        out << " width=" << value.width() << " signed=" << value.isSigned()
            << " type=" << static_cast<unsigned>(value.type())
            << " domain=" << static_cast<unsigned>(value.domain()) << " shape=" << value.shape().size();
        for (const auto dimension : value.shape()) out << ' ' << dimension;
        out << " input=" << value.isInput() << " output=" << value.isOutput() << " inout=" << value.isInout();
        out << " def="; id(out, value.definingOp()); out << " srcloc="; location(out, value.srcLoc()); out << '\n';
        // Preserve the returned order. Never sort away a backend difference.
        out << "users count=" << value.users().size();
        for (const auto& user : value.users()) {
            out << ' '; id(out, user.operation); out << '/' << user.operandIndex;
        }
        out << '\n';
    }
    out << "operations count=" << graph.operations().size() << '\n';
    for (const auto opId : graph.operations()) {
        const auto op = graph.getOperation(opId);
        require(op.id() == opId && op.kind() == graph.opKind(opId) &&
            op.symbol() == graph.operationSymbol(opId) && sameIds(op.operands(), graph.opOperands(opId)) &&
            sameIds(op.results(), graph.opResults(opId)) &&
            locationText(op.srcLoc()) == locationText(graph.opSrcLoc(opId)),
            "operation snapshot disagrees with scalar API");
        out << "operation "; id(out, opId); out << " kind="; text(out, toString(op.kind()));
        out << " symbol=" << op.symbol().value << ' '; text(out, op.symbolText());
        if (op.symbol().valid()) {
            require(op.symbolText() == graph.symbolText(op.symbol()) &&
                graph.lookupSymbol(op.symbolText()) == op.symbol() &&
                graph.findOperation(op.symbol()) == opId && graph.findOperation(op.symbolText()) == opId,
                "operation symbol lookup disagrees with snapshot");
        }
        out << " srcloc="; location(out, op.srcLoc()); out << '\n';
        out << "operands count=" << op.operands().size();
        for (const auto operand : op.operands()) { out << ' '; id(out, operand); }
        out << "\nresults count=" << op.results().size();
        for (const auto result : op.results()) { out << ' '; id(out, result); }
        out << "\nattrs count=" << op.attrs().size() << '\n';
        for (const auto& attr : op.attrs()) {
            text(out, attr.key); out << ' '; attribute(out, attr.value); out << '\n';
            require(op.attr(attr.key) == attr.value, "attribute lookup disagrees with iteration");
        }
    }
    const auto ports = [&](std::string_view label, const auto& list) {
        out << label << " count=" << list.size() << '\n';
        for (const auto& port : list) {
            text(out, port.name); out << ' '; id(out, port.value); out << '\n';
            require((label == "input-ports" ? graph.inputPortValue(port.name) : graph.outputPortValue(port.name)) ==
                port.value, "port lookup disagrees with iteration");
        }
    };
    ports("input-ports", graph.inputPorts());
    ports("output-ports", graph.outputPorts());
    out << "inout-ports count=" << graph.inoutPorts().size() << '\n';
    for (const auto& port : graph.inoutPorts()) {
        text(out, port.name); out << " in="; id(out, port.in);
        out << " out="; id(out, port.out); out << " oe="; id(out, port.oe); out << '\n';
    }
}

class Oracle {
public:
    explicit Oracle(std::filesystem::path output) : output_(std::move(output)) {
        std::filesystem::create_directories(output_);
    }

    void checkpoint(std::string_view label, const Design& design) {
        std::ostringstream prefix;
        prefix << std::setw(3) << std::setfill('0') << sequence_++ << '-' << label;
        std::cout << "checkpoint " << prefix.str() << '\n';
        std::cout << "design-declarations count=" << design.declaredSymbols().size() << '\n';
        for (const auto symbol : design.declaredSymbols()) {
            std::cout << symbol.value << ' '; text(std::cout, design.symbolText(symbol)); std::cout << '\n';
            require(design.isDeclaredSymbol(symbol), "design declaration lookup failed");
        }
        std::cout << "tops count=" << design.topGraphs().size() << '\n';
        for (const auto& top : design.topGraphs()) { text(std::cout, top); std::cout << '\n'; }
        std::cout << "graphs count=" << design.graphOrder().size() << '\n';
        require(design.graphOrder().size() == design.graphs().size(), "graph catalog disagrees with order");
        for (const auto& name : design.graphOrder()) {
            const auto* graph = design.findGraph(name);
            require(graph != nullptr, "graph order contains absent graph");
            graphTrace(*graph);
            const auto aliases = design.aliasesForGraph(name);
            std::cout << "aliases count=" << aliases.size() << '\n';
            for (const auto& alias : aliases) { text(std::cout, alias); std::cout << '\n'; }
        }
        using namespace wolvrix::lib::store;
        StoreDiagnostics diagnostics;
        StoreJson store(&diagnostics);
        for (const auto mode : {JsonPrintMode::Compact, JsonPrintMode::PrettyCompact, JsonPrintMode::Pretty}) {
            StoreOptions options;
            options.jsonMode = mode;
            const auto json = store.storeToString(design, options);
            require(json.has_value(), "checkpoint JSON serialization failed");
            const auto path = output_ / (prefix.str() + '-' + std::to_string(static_cast<int>(mode)) + ".json");
            std::ofstream file(path, std::ios::binary);
            file.write(json->data(), static_cast<std::streamsize>(json->size()));
            require(file.good(), "checkpoint JSON write failed");
            std::cout << "json mode=" << static_cast<int>(mode) << " bytes=" << json->size() << '\n';
        }
    }

    template<typename Action>
    void reject(std::string_view label, const Design& design, Action&& action) {
        checkpoint(std::string(label) + "-before", design);
        bool rejected = false;
        try { action(); }
        catch (const std::exception& error) {
            rejected = true;
            std::cout << "rejected "; text(std::cout, label); std::cout << " message=";
            text(std::cout, error.what()); std::cout << '\n';
        }
        require(rejected, "invalid mutation was accepted");
        checkpoint(std::string(label) + "-after", design);
    }

private:
    std::filesystem::path output_;
    unsigned sequence_ = 0;
};

void scenario(Oracle& oracle) {
    Design design;
    auto& graph = design.createGraph("storage_oracle");
    design.markAsTop(graph.symbol());
    design.addDeclaredSymbol(design.internSymbol(graph.symbol()));
    design.registerGraphAlias("oracle_alias", graph);
    const auto check = [&](std::string_view label) { oracle.checkpoint(label, design); };
    check("empty");
    const auto makeValue = [&](std::string_view name, int32_t width = 8) {
        return graph.createValue(graph.internSymbol(name), width, false);
    };
    auto a = makeValue("a");
    auto b = graph.createValue(graph.internSymbol("b"), 8, true, ValueType::Logic, {}, LogicDomain::TwoState);
    auto c = makeValue("c");
    auto oe = makeValue("oe", 1);
    auto r0 = makeValue("r0");
    auto r1 = makeValue("r1");
    auto spare = makeValue("spare");
    const std::array<int64_t, 2> dimensions{2, 3};
    auto array = graph.createValue(graph.internSymbol("array"), 48, false, ValueType::Logic, dimensions);
    graph.createValue(graph.internSymbol("real"), 64, true, ValueType::Real);
    graph.createValue(graph.internSymbol("string"), 8, false, ValueType::String);
    const std::string longSymbol = "long_" + std::string(1024, 's');
    graph.createValue(graph.internSymbol(longSymbol), 3, true);
    check("values");
    auto multi = graph.createOperation(OperationKind::kAdd, graph.internSymbol("multi"));
    graph.addResult(multi, r0);
    graph.addResult(multi, r1);
    auto other = graph.createOperation(OperationKind::kXor, graph.internSymbol("other"));
    graph.addOperand(other, r0);
    graph.addOperand(multi, a);
    graph.addOperand(other, a);
    graph.addOperand(multi, a);
    graph.addOperand(multi, c);
    check("non-tail-operands");
    for (int i = 0; i < 12; ++i) graph.addOperand(multi, i % 2 ? a : c);
    check("grown-operands");
    graph.insertOperand(multi, 1, c); check("insert-operand");
    graph.replaceOperand(multi, 2, c); check("replace-operand");
    require(graph.eraseOperand(multi, 3), "erase operand failed"); check("erase-operand");
    graph.insertResult(multi, 1, spare); check("insert-result");
    require(graph.eraseResult(multi, 1), "erase result failed"); check("erase-result");
    graph.replaceResult(multi, 1, spare); check("replace-result");
    graph.replaceAllUses(c, a); check("replace-all-uses");
    const SrcLoc loc{"source/" + std::string(1024, 'f') + ".sv", 11, 22, 33, 44,
        "origin\ntext", "oracle", std::string("note\0tail", 9)};
    graph.setValueSrcLoc(array, loc);
    graph.setOpSrcLoc(multi, loc);
    check("source-locations");
    const std::array<std::pair<std::string_view, AttributeValue>, 8> attrs{{
        {"z_bool", true}, {"y_int", int64_t{-9223372036854775807LL}}, {"x_double", -0.0},
        {"w_string", std::string("text\0\nend", 9)}, {"v_bools", std::vector<bool>{true, false, true}},
        {"u_ints", std::vector<int64_t>{-3, 0, 9223372036854775807LL}},
        {"t_doubles", std::vector<double>{-0.0, 0.125, -9.5}},
        {"s_strings", std::vector<std::string>{"", "quote\"slash\\", std::string(1024, 'p')}}}};
    for (const auto& [key, value] : attrs) {
        graph.setAttr(multi, key, value);
        check(std::string("attribute-") + std::string(key));
    }
    graph.setAttr(multi, "w_string", std::string("overwritten")); check("overwrite-attribute");
    require(graph.eraseAttr(multi, "y_int"), "erase attribute failed"); check("erase-attribute");
    graph.setAttr(multi, "y_int", int64_t{17}); check("reinsert-attribute");
    graph.setMetadata("z", std::vector<std::string>{"metadata", "payload"});
    graph.setMetadata("a", false);
    graph.setMetadata("z", std::vector<std::string>{"replaced"});
    require(graph.eraseMetadata("a"), "erase metadata failed"); check("metadata");
    graph.bindInputPort("in", a);
    graph.bindOutputPort("out", r0);
    graph.bindInoutPort("io", c, spare, oe); check("bind-ports");
    require(graph.removeInoutPort("io"), "remove old inout before rebinding failed");
    graph.bindInputPort("in", c);
    graph.bindOutputPort("out", spare);
    graph.bindInoutPort("io", a, r0, oe); check("rebind-ports");
    require(graph.removeInoutPort("io"), "remove inout failed"); check("remove-inout");
    require(graph.removeInputPort("in"), "remove input failed");
    require(graph.removeOutputPort("out"), "remove output failed"); check("remove-ports");
    graph.setValueShape(c, std::array<int64_t, 1>{2}); check("shape-backfill");
    graph.setValueLogicalType(b, LogicalType{ValueType::Logic, 8, false, LogicDomain::FourState, {}});
    check("logical-type");
    graph.setValueSymbol(r1, graph.internSymbol("renamed")); check("rename-value");
    oracle.reject("clear-value-symbol", design, [&] { graph.clearValueSymbol(r1); });
    graph.setValueSymbol(r1, graph.internSymbol("r1"));
    graph.setOpSymbol(other, graph.internSymbol("renamed_op")); check("rename-operation");
    oracle.reject("clear-operation-symbol", design, [&] { graph.clearOpSymbol(other); });
    graph.setOpSymbol(other, graph.internSymbol("other"));
    graph.addDeclaredSymbol(graph.valueSymbol(a));
    graph.addDeclaredSymbol(graph.operationSymbol(multi));
    const auto group = graph.addGenerateGroup(graph.internSymbol("scope"), graph.internSymbol("member"));
    graph.addGenerateGroupSymbol(group, graph.valueSymbol(a));
    graph.addGenerateGroupSymbol(group, graph.valueSymbol(r1)); check("declarations");
    auto erasedOp = graph.createOperation(OperationKind::kConstant, graph.internSymbol("dead_op"));
    auto erasedValue = makeValue("dead_value");
    graph.addResult(erasedOp, erasedValue);
    require(graph.eraseOp(erasedOp), "erase operation failed");
    require(graph.eraseValue(erasedValue), "erase value failed"); check("tombstones");
    oracle.reject("invalid-value", design, [&] { (void)graph.valueWidth(ValueId::invalid()); });
    oracle.reject("erased-value", design, [&] { (void)graph.valueWidth(erasedValue); });
    oracle.reject("invalid-operation", design, [&] { (void)graph.opKind(OperationId::invalid()); });
    oracle.reject("erased-operation", design, [&] { (void)graph.opKind(erasedOp); });
    oracle.reject("bad-shape", design, [&] { graph.setValueShape(b, std::array<int64_t, 1>{3}); });
    oracle.reject("duplicate-definition", design, [&] { graph.addResult(other, r0); });
    graph.freeze(); check("first-freeze");
    // freeze compacts IDs; always reacquire handles through the common API.
    multi = graph.findOperation("multi");
    other = graph.findOperation("other");
    a = graph.findValue("a");
    b = graph.findValue("b");
    c = graph.findValue("c");
    r0 = graph.findValue("r0");
    r1 = graph.findValue("r1");
    spare = graph.findValue("spare");
    graph.setAttr(multi, "post_freeze", std::vector<double>{1.0, -0.0}); check("frozen-attribute-write");
    graph.insertOperand(other, 0, b); check("frozen-operand-write");
    graph.addOperand(multi, b); check("post-freeze-grow");
    graph.bindInputPort("post_in", c);
    graph.bindOutputPort("post_out", r0); check("post-freeze-ports");
    graph.setOpKind(other, OperationKind::kOr); check("change-operation-kind");
    const auto replacement = std::array<ValueId, 2>{r1, r1};
    require(graph.eraseOp(multi, replacement), "erase with replacements failed"); check("erase-with-replacements");
    graph.freeze(); check("second-freeze");
    auto& foreign = design.createGraph("foreign");
    const auto foreignValue = foreign.createValue(8, false);
    const auto foreignOp = foreign.createOperation(OperationKind::kConstant);
    oracle.reject("foreign-value", design, [&] { (void)graph.valueWidth(foreignValue); });
    oracle.reject("foreign-operation", design, [&] { (void)graph.opKind(foreignOp); });
    design.cloneGraph(graph.symbol(), "local_clone"); check("clone-graph");
    Design imported;
    imported.importGraph(graph, "imported_graph");
    imported.markAsTop("imported_graph");
    oracle.checkpoint("import-graph", imported);
    Design cloned = design.clone();
    oracle.checkpoint("clone-design", cloned);
    graph.setMetadata("source_only", true);
    auto& cloneGraph = *cloned.findGraph("storage_oracle");
    cloneGraph.setMetadata("clone_only", true);
    check("source-after-clone");
    oracle.checkpoint("clone-independent-write", cloned);
    require(design.deleteGraph("storage_oracle"), "delete source graph failed");
    oracle.checkpoint("clone-source-destroyed", cloned);
    oracle.checkpoint("import-source-destroyed", imported);
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: grh-storage-oracle OUTPUT_DIRECTORY");
        // Provenance goes to a separate file, outside the byte-compared trace.
        std::filesystem::create_directories(argv[1]);
        std::ofstream runtime(std::filesystem::path(argv[1]) / "runtime-library.txt");
        Dl_info info{};
        require(dladdr(reinterpret_cast<void*>(&attributeValueIsJsonSerializable), &info) != 0 && info.dli_fname,
            "cannot resolve loaded GRH library");
        runtime << std::filesystem::canonical(info.dli_fname).string() << '\n';
        require(runtime.good(), "runtime provenance write failed");
        Oracle oracle(argv[1]);
        scenario(oracle);
        std::cout << "oracle-complete\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "grh-storage-oracle: " << error.what() << '\n';
        return 1;
    }
}
