#include "grhsim_oracle.hpp"
#include "../../lib/transform/grhsim/rewrite.hpp"

#include "core/store.hpp"
#include "transform/grhsim/grh-to-grhsim.hpp"
#include "transform/grhsim/simplify.hpp"
#include "transform/grhsim/pack-bit-registers.hpp"
#include "transform/grhsim/recover-masked-or.hpp"
#include "transform/verify.hpp"

#include <filesystem>
#include <iostream>
#include <random>

namespace grh = wolvrix::lib::grh;
namespace transform = wolvrix::lib::transform;
namespace sim = transform::grhsim;
using grh::OperationKind;

namespace {
void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }

struct Builder {
    grh::Design design;
    grh::Graph& graph = design.createGraph("top");
    Builder() { design.markAsTop("top"); }
    grh::LogicalType type(int32_t width, std::vector<int64_t> shape = {}, bool sign = false) {
        return {grh::ValueType::Logic, width, sign, grh::LogicDomain::FourState, std::move(shape)};
    }
    grh::ValueId input(std::string name, int32_t width = 1) {
        const auto value = graph.createValue(graph.internSymbol(name), type(width)); graph.bindInputPort(name, value); return value;
    }
    grh::ValueId compute(OperationKind kind, grh::LogicalType type, std::initializer_list<grh::ValueId> args, std::string name = {}) {
        const auto op = name.empty() ? graph.createOperation(kind) : graph.createOperation(kind, graph.internSymbol(name));
        const auto value = graph.createValue(type);
        for (const auto arg : args) graph.addOperand(op, arg);
        graph.addResult(op, value); return value;
    }
    grh::ValueId constant(int32_t width, std::string literal, std::vector<int64_t> shape = {}) {
        const auto value = compute(OperationKind::kConstant, type(width, std::move(shape)), {});
        graph.setAttr(graph.valueDef(value), "constValue", std::move(literal)); return value;
    }
    grh::ValueId slice(grh::ValueId source, int32_t lo, int32_t width) {
        const auto value = compute(OperationKind::kSliceStatic, type(width), {source});
        graph.setAttr(graph.valueDef(value), "sliceStart", static_cast<int64_t>(lo));
        graph.setAttr(graph.valueDef(value), "sliceEnd", static_cast<int64_t>(lo + width - 1)); return value;
    }
    grh::OperationId state(std::string name, grh::LogicalType type, std::string initial, bool anchor = true, bool latch = false) {
        const auto op = graph.createOperation(latch ? OperationKind::kLatch : OperationKind::kRegister, graph.internSymbol(name));
        grh::setStateLogicalType(graph, op, type);
        if (type.shape.empty()) graph.setAttr(op, "initValue", initial);
        else {
            graph.setAttr(op, "initKind", std::vector<std::string>{"literal"});
            graph.setAttr(op, "initValue", std::vector<std::string>{initial});
        }
        if (anchor) graph.addDeclaredSymbol(graph.operationSymbol(op));
        graph.setAttr(op, "originModules", std::vector<std::string>{"top$" + name}); return op;
    }
    grh::ValueId read(grh::OperationId state, std::initializer_list<grh::ValueId> indices = {}) {
        const auto stateType = grh::stateLogicalType(graph, state);
        auto resultType = stateType;
        for (std::size_t i = 0; i < indices.size(); ++i) resultType.width /= resultType.shape[i];
        resultType.shape.erase(resultType.shape.begin(), resultType.shape.begin() + indices.size());
        const auto kind = graph.opKind(state) == OperationKind::kLatch ? OperationKind::kLatchReadPort :
            indices.size() ? OperationKind::kRegisterReadArrayPort : OperationKind::kRegisterReadPort;
        const auto value = compute(kind, resultType, indices);
        graph.setAttr(graph.valueDef(value), kind == OperationKind::kLatchReadPort ? "latchSymbol" : "regSymbol", std::string(graph.getOperation(state).symbolText()));
        return value;
    }
    grh::OperationId write(OperationKind kind, grh::OperationId target, std::initializer_list<grh::ValueId> args,
                           std::vector<std::string> edges = {"posedge"}) {
        const auto op = graph.createOperation(kind);
        for (const auto arg : args) graph.addOperand(op, arg);
        graph.setAttr(op, kind == OperationKind::kLatchWritePort ? "latchSymbol" : "regSymbol", std::string(graph.getOperation(target).symbolText()));
        if (kind != OperationKind::kLatchWritePort) graph.setAttr(op, "eventEdge", std::move(edges));
        return op;
    }
    void output(std::string name, grh::ValueId value) { graph.bindOutputPort(name, value); }
};

bool run(grh::Design& design, std::unique_ptr<transform::Pass> pass, bool* changed = nullptr) {
    transform::PassManager manager; manager.addPass(std::move(pass)); transform::PassDiagnostics diagnostics;
    const auto result = manager.run(design, diagnostics);
    if (changed) *changed = result.changed;
    if (!result.success) for (const auto& message : diagnostics.messages()) std::cerr << message.message << '\n';
    return result.success;
}
std::size_t count(const grh::Graph& graph, OperationKind kind) {
    return std::count_if(graph.operations().begin(), graph.operations().end(), [&](auto op) { return graph.opKind(op) == kind; });
}
grh::Design roundTrip(const grh::Design& design) {
    wolvrix::lib::store::StoreJson store;
    const auto json = store.storeToString(design); require(json.has_value(), "checkpoint store failed");
    auto loaded = grh::Design::fromJsonString(*json);
    require(run(loaded, std::make_unique<transform::GrhVerifyPass>()), "checkpoint verify failed"); return loaded;
}
void convert(grh::Design& design) { require(run(design, std::make_unique<sim::GrhToGrhsimPass>()), "conversion failed"); }
void simplify(grh::Design& design) { require(run(design, std::make_unique<sim::SimplifyPass>()), "simplify failed"); }

void testOperandReplacement() {
    Builder b;
    const auto a = b.input("a", 8), c = b.input("c", 8), d = b.input("d", 8);
    const auto value = b.compute(OperationKind::kConcat, b.type(32), {a, c, a, d});
    const auto op = b.graph.valueDef(value);
    const auto check = [&](std::initializer_list<grh::ValueId> expected) {
        require(std::equal(expected.begin(), expected.end(), b.graph.opOperands(op).begin(),
                           b.graph.opOperands(op).end()), "replacement operands differ");
        for (const auto input : {a, c, d}) {
            std::vector<uint32_t> indices;
            const auto view = b.graph.getValue(input);
            for (const auto& user : view.users()) {
                require(user.operation == op, "replacement introduced a foreign use");
                indices.push_back(user.operandIndex);
            }
            std::sort(indices.begin(), indices.end());
            std::vector<uint32_t> wanted;
            uint32_t index = 0;
            for (const auto arg : expected) { if (arg == input) wanted.push_back(index); ++index; }
            require(indices == wanted, "replacement use-list differs");
        }
    };
    sim::detail::replaceOperands(b.graph, op, b.graph.opOperands(op));
    check({a, c, a, d});
    sim::detail::replaceOperands(b.graph, op, b.graph.opOperands(op).subspan(1, 2));
    check({c, a});
    const std::array grown{d, a, a, c, d};
    sim::detail::replaceOperands(b.graph, op, grown);
    check({d, a, a, c, d});
    sim::detail::replaceOperands(b.graph, op, {});
    check({});

    // Repeated arguments and non-creation-order erasure exercise the moved
    // use's backlink, including insert/erase index shifts and bulk aliases.
    std::vector<grh::OperationId> consumers;
    for (unsigned i = 0; i < 1024; ++i)
        consumers.push_back(b.graph.valueDef(b.compute(OperationKind::kConcat, b.type(24), {a, a, c})));
    for (std::size_t i = 0; i < consumers.size(); i += 2) {
        b.graph.insertOperand(consumers[i], 1, d);
        b.graph.eraseOperand(consumers[i], 0);
        b.graph.replaceOperand(consumers[i], 2, a);
    }
    b.graph.replaceAllUses(a, c);
    for (std::size_t i = consumers.size(); i-- > 0;)
        require(b.graph.eraseOp(consumers[(i * 511) % consumers.size()]), "high fanout erase failed");
    for (const auto input : {a, c, d}) {
        const auto view = b.graph.getValue(input);
        require(view.users().empty(), "high fanout erasure left stale uses");
    }
    b.graph.freeze();
    const auto appended = b.compute(OperationKind::kConcat, b.type(24), {a, c, c});
    const auto appendedOp = b.graph.valueDef(appended);
    b.graph.replaceOperand(appendedOp, 0, c);
    b.graph.freeze();
    b.graph.replaceAllUses(c, d);
    require(b.graph.eraseOp(b.graph.valueDef(appended)), "reloaded use backlinks differ");
}

void compare(const grh::Design& reference, const grh::Design& candidate, const std::vector<std::string>& probes,
             uint32_t samples, std::string_view stem) {
    grhsim_test::Oracle old(*reference.findGraph("top"), probes), current(*candidate.findGraph("top"), probes);
    std::mt19937_64 random(417);
    const auto path = std::filesystem::path(WOLVRIX_GRHSIM_TRANSFORM_ARTIFACT_DIR) / (std::string(stem) + ".observations.txt");
    std::filesystem::create_directories(path.parent_path()); std::ofstream log(path);
    for (unsigned epoch = 0; epoch < 3; ++epoch) {
        old.init(); current.init();
        for (uint32_t step = 0; step < samples; ++step) {
            std::map<std::string, uint64_t> inputs;
            for (const auto& port : reference.findGraph("top")->inputPorts()) inputs.emplace(port.name, random());
            if (inputs.contains("clk")) inputs["clk"] = step % 2;
            if (inputs.contains("reset")) inputs["reset"] = step % 11 != 0;
            const auto expected = old.eval(inputs), actual = current.eval(inputs);
            if (expected != actual) {
                log << "mismatch epoch=" << epoch << " step=" << step << '\n';
                for (const auto& [key, value] : expected.outputs) log << "old output " << key << '=' << value << '\n';
                for (const auto& [key, value] : actual.outputs) log << "new output " << key << '=' << value << '\n';
                for (const auto& effect : expected.effects) log << "old effect " << effect << '\n';
                for (const auto& effect : actual.effects) log << "new effect " << effect << '\n';
                throw std::runtime_error(std::string(stem) + " execution differs at " + std::to_string(step));
            }
            log << epoch << ':' << step << " rounds=" << actual.rounds.size() << '\n';
            for (const auto& [key, value] : actual.states) log << "state " << key << '=' << value << '\n';
            for (const auto& [key, value] : actual.outputs) log << "output " << key << '=' << value << '\n';
            for (const auto& effect : actual.effects) log << "effect " << effect << '\n';
        }
    }
}

void testGenerateGroupMembershipIndex() {
    Builder b;
    const auto value = b.constant(8, "8'h2a");
    const auto symbol = b.graph.valueSymbol(value);
    const auto scope = b.graph.internSymbol("generateScope");
    const auto name = b.graph.internSymbol("groupName");
    for (int i = 0; i < 128; ++i) {
        const auto group = b.graph.addGenerateGroup(scope, name);
        b.graph.addGenerateGroupSymbol(group, symbol);
        b.graph.addGenerateGroupSymbol(group, symbol);
    }
    auto loaded = roundTrip(b.design);
    loaded.findGraph("top")->freeze();
    auto cloned = loaded.clone();
    for (auto* graph : {&b.graph, loaded.findGraph("top"), cloned.findGraph("top")}) {
        const auto current = graph->findValue(graph->symbolText(graph->valueSymbol(graph->values()[0])));
        const auto member = graph->valueSymbol(current);
        require(graph->eraseOp(graph->valueDef(current)) && graph->eraseValue(current), "indexed group member erase failed");
        for (const auto& group : graph->generateGroups()) require(group.symbols.empty(), "duplicate/multiple membership survived erase");
        const auto replacement = graph->createValue(member, 8, false);
        graph->addGenerateGroupSymbol(0, member);
        require(graph->eraseValue(replacement) && graph->generateGroups()[0].symbols.empty(), "re-added membership index is stale");
        const auto ungrouped = graph->createValue(8, false);
        require(graph->eraseValue(ungrouped) && graph->validateDeclaredSymbols().empty(), "ungrouped erase changed memberships");
    }
}

void testEventsCallsAndPacking() {
    Builder b;
    const auto clk = b.input("clk"), reset = b.input("reset"), en = b.input("enable"), gate = b.input("gate");
    const auto data = b.input("data", 8), mask = b.input("mask", 8), io = b.input("io", 32);
    const auto one = b.constant(1, "1'h1");
    const auto gated = b.compute(OperationKind::kAnd, b.type(1), {clk, gate});
    std::vector<std::string> probes;
    for (int32_t lane = 0; lane < 8; ++lane) {
        const auto name = "flag" + std::to_string(lane); probes.push_back(name);
        const auto state = b.state(name, b.type(1), lane % 2 ? "1'h1" : "1'h0");
        const auto group = b.graph.addGenerateGroup(b.graph.internSymbol("flags"), b.graph.internSymbol(name + "_decl"));
        b.graph.addGenerateGroupSymbol(group, b.graph.operationSymbol(state));
        b.write(OperationKind::kRegisterWritePort, state, {en, b.slice(data, lane, 1), gated});
        b.output(name, b.read(state));
    }
    const auto state = b.state("q", b.type(8), "8'h01"); probes.push_back("q");
    const auto q = b.read(state); b.output("q", q);
    b.write(OperationKind::kRegisterWriteMaskPort, state, {en, data, mask, clk}, {"negedge"});
    b.write(OperationKind::kRegisterWritePort, state, {en, data, clk, reset}, {"posedge", "negedge"});
    for (const auto& [name, function] : std::vector<std::pair<std::string, std::string>>{{"random0", "random"}, {"random1", "urandom"}, {"unused_random", "random"}}) {
        const auto value = b.compute(OperationKind::kSystemFunction, b.type(32), {}, name);
        b.graph.setAttr(b.graph.valueDef(value), "name", function);
        if (name != "unused_random") b.output(name, value);
    }
    for (const auto& [name, taskName] : std::vector<std::pair<std::string, std::string>>{{"display", "display"}, {"monitor", "monitor"}, {"strobe", "strobe"}}) {
        const auto op = b.graph.createOperation(OperationKind::kSystemTask, b.graph.internSymbol(name));
        for (const auto value : {en, q, clk}) b.graph.addOperand(op, value);
        b.graph.setAttr(op, "name", taskName); b.graph.setAttr(op, "eventEdge", std::vector<std::string>{"posedge"});
    }
    const auto imported = b.graph.createOperation(OperationKind::kDpicImport, b.graph.internSymbol("dpi"));
    b.graph.setAttr(imported, "argsDirection", std::vector<std::string>{"inout", "input", "output"});
    b.graph.setAttr(imported, "argsWidth", std::vector<int64_t>{32, 8, 16});
    b.graph.setAttr(imported, "argsSigned", std::vector<bool>{false, false, false});
    b.graph.setAttr(imported, "argsName", std::vector<std::string>{"io", "in", "out"});
    b.graph.setAttr(imported, "hasReturn", true); b.graph.setAttr(imported, "returnWidth", int64_t{8});
    for (unsigned i = 0; i < 2; ++i) {
        const auto name = "dpi_call" + std::to_string(i);
        const auto call = b.graph.createOperation(OperationKind::kDpicCall, b.graph.internSymbol(name));
        for (const auto arg : {en, data, io, clk}) b.graph.addOperand(call, arg);
        for (const auto width : {8, 16, 32}) { const auto result = b.graph.createValue(b.type(width)); b.graph.addResult(call, result); b.output(name + "_" + std::to_string(width), result); }
        b.graph.setAttr(call, "targetImportSymbol", std::string("dpi"));
        b.graph.setAttr(call, "eventEdge", std::vector<std::string>{"posedge"});
    }
    const auto reference = b.design.clone();
    convert(b.design);
    const auto converted = roundTrip(b.design);
    compare(reference, converted, probes, 128, "r2-calls-events");
    const auto before = count(b.graph, sim::kEdgeDetect);
    simplify(b.design);
    require(count(b.graph, sim::kEdgeDetect) < before, "equivalent detectors did not share");
    require(count(b.graph, sim::kSystemFunction) == 3 && count(b.graph, sim::kDpiCall) == 2, "effectful calls removed or merged");
    require(run(b.design, std::make_unique<sim::PackBitRegistersPass>()), "packing failed");
    require(count(b.graph, OperationKind::kRegister) == 2, "eight flags did not pack into one word");
    std::vector<std::string> packedOrigins;
    for (unsigned lane = 0; lane < 8; ++lane) {
        const auto name = "flag" + std::to_string(lane);
        packedOrigins.push_back("top$" + name);
        const auto view = b.graph.findValue(name);
        require(view && transform::getOriginModules(b.graph, b.graph.valueDef(view)) ==
                std::vector<std::string>{"top$" + name}, "packed declaration view inherited unrelated lane origins");
    }
    for (const auto op : b.graph.operations())
        if (b.graph.opKind(op) == OperationKind::kRegister && op != b.graph.findOperation("q"))
            require(transform::getOriginModules(b.graph, op) == packedOrigins, "packed state lost member origins");
    for (const auto& probe : probes) require(b.graph.findOperation(probe) || b.graph.findValue(probe), "state name projection disappeared");
    require(b.graph.validateDeclaredSymbols().empty() && b.graph.generateGroups().size() == 8, "packing lost declaration provenance");
    simplify(b.design);
    bool changed = true;
    require(run(b.design, std::make_unique<sim::SimplifyPass>(), &changed) && !changed, "simplify is not idempotent");
    auto loaded = roundTrip(b.design); loaded.findGraph("top")->freeze();
    compare(reference, loaded, probes, 256, "r3-calls-packed");
    for (const auto op : b.graph.operations()) require(!b.graph.getOperation(op).attr(sim::kPartition), "optimization created partitions");
}

void testConstantAliasConvergence() {
    Builder b;
    const auto first = b.constant(8, "8'h2a");
    const auto duplicate = b.constant(8, "8'h2a");
    const auto declared = b.constant(8, "8'h2a");
    b.graph.addDeclaredSymbol(b.graph.valueSymbol(declared));
    b.output("first", first); b.output("duplicate", duplicate); b.output("declared", declared);
    // Retained declarations must share the source without growing an alias
    // chain one link per simplify round.
    for (int i = 0; i < 32; ++i) {
        const auto value = b.constant(8, "8'h2a");
        b.graph.addDeclaredSymbol(b.graph.valueSymbol(value));
        b.output("alias" + std::to_string(i), value);
    }
    const auto reference = b.design.clone(); convert(b.design);
    transform::PassManager manager;
    sim::SimplifyOptions options; options.maxRounds = 4;
    manager.addPass(std::make_unique<sim::SimplifyPass>(options));
    transform::PassDiagnostics diagnostics;
    require(manager.run(b.design, diagnostics).success, "constant alias simplify failed");
    for (const auto& message : diagnostics.messages())
        require(message.message.find("before convergence") == std::string::npos, "constant aliases did not converge");
    require(count(b.graph, OperationKind::kConstant) == 1, "duplicate exported constants did not share");
    bool changed = true;
    require(run(b.design, std::make_unique<sim::SimplifyPass>(), &changed) && !changed,
            "retained constant aliases are not idempotent");
    compare(reference, roundTrip(b.design), {}, 4, "r3-constant-aliases");
}

void testNamedAliasConsumers() {
    Builder b;
    const auto input = b.input("input", 8), rhs = b.input("rhs", 8);
    auto previous = input;
    std::vector<grh::ValueId> aliases;
    for (int i = 0; i < 96; ++i) {
        const auto value = b.compute(OperationKind::kAssign, b.type(8), {previous}, "named" + std::to_string(i));
        b.graph.addDeclaredSymbol(b.graph.operationSymbol(b.graph.valueDef(value)));
        b.graph.addDeclaredSymbol(b.graph.valueSymbol(value));
        aliases.push_back(value); previous = value;
    }
    b.output("direct", b.compute(OperationKind::kAnd, b.type(8), {input, rhs}));
    b.output("throughAliases", b.compute(OperationKind::kAnd, b.type(8), {previous, rhs}));
    const auto reference = b.design.clone(); convert(b.design);
    transform::PassManager manager;
    sim::SimplifyOptions options; options.maxRounds = 2;
    manager.addPass(std::make_unique<sim::SimplifyPass>(options));
    transform::PassDiagnostics diagnostics;
    require(manager.run(b.design, diagnostics).success, "named alias simplify failed");
    for (const auto& message : diagnostics.messages())
        require(message.message.find("before convergence") == std::string::npos, "named alias consumers did not converge");
    require(count(b.graph, OperationKind::kAnd) == 1, "named aliases blocked same-round CSE");
    for (const auto value : aliases) {
        const auto op = b.graph.valueDef(value);
        require(op && b.graph.opKind(op) == OperationKind::kAssign && b.graph.opOperands(op)[0] == input,
                "named alias view disappeared or retained a dependency chain");
    }
    require(b.graph.validateDeclaredSymbols().empty(), "alias propagation lost declarations");
    compare(reference, roundTrip(b.design), {}, 128, "r3-named-alias-consumers");
}

void testArraysAndWriters() {
    Builder b;
    const auto clk = b.input("clk"), en = b.input("enable"), en2 = b.input("enable2"), fill = b.input("fill");
    const auto i = b.input("index0", 2), j = b.input("index1", 2), data = b.input("data", 8), mask = b.input("mask", 8);
    const auto flat = b.input("flat", 32), one = b.constant(1, "1'h1");
    const auto memory = b.state("memory", b.type(32, {2, 2}), "8'h12");
    const auto row = b.compute(OperationKind::kArrayCreate, b.type(16, {2}), {data, mask});
    const auto all = b.read(memory);
    b.output("packed", b.compute(OperationKind::kAssign, b.type(32), {all}));
    b.output("element", b.read(memory, {i, j}));
    b.output("row", b.read(memory, {i}));
    b.write(OperationKind::kRegisterWritePort, memory, {en, flat, clk});
    b.write(OperationKind::kRegisterWriteArrayPort, memory, {en2, i, row, clk});
    b.write(OperationKind::kRegisterWriteArrayMaskPort, memory, {en, i, j, data, mask, clk});
    b.write(OperationKind::kRegisterFillPort, memory, {fill, data, clk});
    const auto latch = b.state("latch", b.type(8), "8'h00", true, true);
    b.write(OperationKind::kLatchWritePort, latch, {en, data, mask}, {}); b.output("latch", b.read(latch));
    const auto c0 = b.constant(2, "2'h0"), c1 = b.constant(2, "2'h1"), c3 = b.constant(2, "2'h3");
    const auto table = b.compute(OperationKind::kArrayCreate, b.type(32, {2, 2}), {row, row});
    b.output("sliceArrayValid", b.compute(OperationKind::kSliceArray, b.type(8), {table, c0, c1}));
    b.output("sliceArrayDynamic", b.compute(OperationKind::kSliceArray, b.type(8), {table, c0, j}));
    b.output("sliceArrayOob", b.compute(OperationKind::kSliceArray, b.type(8), {table, c0, c3}));
    b.output("readArrayOob", b.read(memory, {c0, c3}));
    const auto known = b.compute(OperationKind::kArrayCreate, b.type(16, {2}), {b.constant(8, "8'h23"), b.constant(8, "8'h45")});
    b.output("constArray", known);
    const auto reference = b.design.clone(); convert(b.design); simplify(b.design);
    require(b.graph.valueLogicalType(known).shape == std::vector<int64_t>{2}, "folding erased array type");
    require(count(b.graph, sim::kWriteArray) == 1 && count(b.graph, sim::kWriteArrayMask) == 1 && count(b.graph, sim::kFill) == 1 && count(b.graph, sim::kLatchWrite) == 1, "array/latch writers lost");
    auto loaded = roundTrip(b.design); compare(reference, loaded, {"memory", "latch"}, 256, "r2-r3-arrays");
}

void testNarrowingAndStateSharing() {
    Builder b;
    const auto clk = b.input("clk"), en = b.input("enable"), data = b.input("data", 129);
    const auto mask = b.constant(129, "129'h1ffffffffffffffffffffffffffffffff");
    const auto wide = b.state("wide", b.type(129), "129'h123", false);
    const auto q = b.read(wide); b.output("low", b.slice(q, 0, 5));
    b.write(OperationKind::kRegisterWriteMaskPort, wide, {en, data, mask, clk});
    const auto dead = b.state("dead", b.type(1), "1'h0", false);
    const auto deadRead = b.read(dead), inverted = b.compute(OperationKind::kNot, b.type(1), {deadRead});
    b.write(OperationKind::kRegisterWritePort, dead, {en, inverted, clk});
    const auto deadSecond = b.state("dead_second_with_a_long_symbol", b.type(2), "2'h0", false);
    const auto secondRead = b.read(deadSecond);
    b.write(OperationKind::kRegisterWritePort, deadSecond,
            {en, b.compute(OperationKind::kNot, b.type(2), {secondRead}), clk});
    const auto byte = b.slice(data, 0, 8);
    for (const auto& name : {"same0", "same1"}) {
        const auto state = b.state(name, b.type(8), "8'h0");
        b.write(OperationKind::kRegisterWritePort, state, {en, byte, clk}); b.output(name, b.read(state));
    }
    const auto sum = b.compute(OperationKind::kAdd, b.type(129), {data, b.constant(129, "129'h7")});
    b.output("sumLow", b.slice(sum, 0, 9));
    const auto reference = b.design.clone(); convert(b.design); simplify(b.design);
    require(grh::stateLogicalType(b.graph, b.graph.findOperation("wide")).width == 5, "used-bits did not narrow shared state/read/write/init");
    require(!b.graph.findOperation("dead"), "unobserved feedback state survived");
    require(!b.graph.findOperation("dead_second_with_a_long_symbol"), "second unobserved feedback state survived");
    require(count(b.graph, OperationKind::kRegister) == 2, "equivalent scalar states did not merge");
    require(b.graph.validateDeclaredSymbols().empty(), "state sharing lost declared anchors");
    compare(reference, roundTrip(b.design), {"same0", "same1"}, 256, "r3-narrow-shared");
}

void testMaskedOr() {
    for (const bool dense : {true, false}) {
        Builder b; const auto mask = b.input("mask", 4), enable = b.input("enable");
        const auto data = b.input("data", 32), zero = b.constant(8, "8'h0");
        const auto array = b.compute(OperationKind::kAssign, b.type(32, {4}), {data});
        grh::ValueId reduction;
        for (int32_t lane = 0; lane < 4; ++lane) {
            const auto select = b.slice(mask, lane, 1);
            const auto index = b.constant(2, "2'd" + std::to_string(lane));
            const auto item = b.compute(OperationKind::kSliceArray, b.type(8), {array, index});
            grh::ValueId term;
            if (dense) term = b.compute(OperationKind::kMux, b.type(8), {select, item, zero});
            else {
                const auto broadcast = b.compute(OperationKind::kReplicate, b.type(8), {select});
                b.graph.setAttr(b.graph.valueDef(broadcast), "rep", int64_t{8});
                const auto gate = b.compute(OperationKind::kReplicate, b.type(8), {enable});
                b.graph.setAttr(b.graph.valueDef(gate), "rep", int64_t{8});
                term = b.compute(OperationKind::kAnd, b.type(8), {b.compute(OperationKind::kAnd, b.type(8), {item, gate}), broadcast});
            }
            reduction = reduction ? b.compute(OperationKind::kOr, b.type(8), {reduction, term}) : term;
        }
        b.output("result", reduction); const auto reference = b.design.clone(); convert(b.design);
        require(run(b.design, std::make_unique<sim::RecoverMaskedOrPass>(sim::RecoverMaskedOrOptions{"", 2, {}})), "maskedOr recovery failed");
        require(count(b.graph, sim::kMaskedOr) == 1, "maskedOr tree did not recover");
        simplify(b.design); compare(reference, roundTrip(b.design), {}, 256, dense ? "r3-masked-dense" : "r3-masked-views");
    }
}

void testPackingRejectionsAndOrigins() {
    Builder b;
    const auto clk = b.input("clk"), en = b.input("enable");
    const auto one = b.constant(1, "1'h1");
    std::vector<std::string> probes;
    for (int32_t lane = 0; lane < 5; ++lane) {
        const auto name = "kept" + std::to_string(lane);
        const auto data = b.input("data" + std::to_string(lane));
        const auto mask = lane < 2 ? b.input("mask" + std::to_string(lane)) : one;
        const auto state = b.state(name, b.type(1, lane == 3 ? std::vector<int64_t>{1} : std::vector<int64_t>{}, lane == 2), "1'h0");
        b.write(OperationKind::kRegisterWriteMaskPort, state, {en, data, mask, clk});
        if (lane == 4) b.write(OperationKind::kRegisterWritePort, state, {en, one, clk});
        b.output(name, b.read(state)); probes.push_back(name);
    }
    const auto reference = b.design.clone(); convert(b.design); simplify(b.design);
    bool changed = true;
    require(run(b.design, std::make_unique<sim::PackBitRegistersPass>(), &changed) && !changed,
            "packing merged different masks, signed/shaped states or a multi-writer state");
    compare(reference, roundTrip(b.design), probes, 128, "r3-pack-rejections");

    Builder origins;
    const auto a = origins.input("a", 8), c = origins.input("b", 8);
    for (const auto& name : {"u_core", "u_mmu"}) {
        const auto value = origins.compute(OperationKind::kAnd, origins.type(8), {a, c});
        origins.graph.setAttr(origins.graph.valueDef(value), "originModules", std::vector<std::string>{name});
        origins.output(name, value);
    }
    convert(origins.design); simplify(origins.design);
    require(count(origins.graph, OperationKind::kAnd) == 1, "origin annotations prevented CSE");
    for (const auto op : origins.graph.operations())
        if (origins.graph.opKind(op) == OperationKind::kAnd)
            require(sim::attribute<std::vector<std::string>>(origins.graph, op, "originModules") ==
                    std::vector<std::string>{"u_core", "u_mmu"}, "CSE lost the ordered origin union");
}

void testFourStateBoundaries() {
    Builder b;
    const auto input = b.input("input", 8);
    b.output("xor", b.compute(OperationKind::kXor, b.type(8), {input, input}));
    const auto x = b.constant(4, "4'hx"), z = b.constant(4, "4'hz");
    const auto array = b.compute(OperationKind::kArrayCreate, b.type(8, {2}), {x, z});
    b.output("array", array);
    const auto a = b.state("a", b.type(1), "1'hx"), c = b.state("c", b.type(1), "1'hz");
    b.output("a", b.read(a)); b.output("c", b.read(c));
    sim::GrhToGrhsimOptions options; options.logicDomain = grh::LogicDomain::FourState;
    require(run(b.design, std::make_unique<sim::GrhToGrhsimPass>(options)), "FourState conversion failed");
    simplify(b.design);
    require(count(b.graph, OperationKind::kXor) == 1, "TwoState xor identity was applied to FourState input");
    const auto folded = b.graph.valueDef(array);
    require(b.graph.opKind(folded) == OperationKind::kConstant &&
            b.graph.valueLogicalType(array).shape == std::vector<int64_t>{2} &&
            slang::SVInt::fromString(sim::attribute<std::string>(b.graph, folded, "constValue").value()).hasUnknown(),
            "FourState array folding lost shape or unknown bits");
    bool changed = true;
    require(run(b.design, std::make_unique<sim::PackBitRegistersPass>(), &changed) && !changed,
            "FourState states were bit packed");
    roundTrip(b.design);
}

void testSelectRules() {
    Builder b;
    const auto c0 = b.input("c0"), c1 = b.input("c1"), c2 = b.input("c2");
    const auto bit0 = b.input("bit0"), bit1 = b.input("bit1");
    b.output("bitChoice", b.compute(OperationKind::kMux, b.type(1), {c0, bit0, bit1}));
    const auto a = b.input("a", 8), c = b.input("b", 8), d = b.input("d", 8), fallback = b.input("fallback", 8);
    const auto tail = b.compute(OperationKind::kMux, b.type(8), {c2, d, fallback});
    const auto middle = b.compute(OperationKind::kMux, b.type(8), {c1, c, tail});
    b.output("priority", b.compute(OperationKind::kMux, b.type(8), {c0, a, middle}));
    const auto logarithm = b.compute(OperationKind::kSystemFunction, b.type(32), {b.constant(32, "32'd17")});
    b.graph.setAttr(b.graph.valueDef(logarithm), "name", std::string("clog2")); b.output("logarithm", logarithm);
    const auto reference = b.design.clone(); convert(b.design); simplify(b.design);
    require(count(b.graph, sim::kBitSelect) == 1 && count(b.graph, sim::kPrioritySelect) == 1,
            "simulation select rewrites did not apply");
    require(count(b.graph, sim::kSystemFunction) == 0, "pure system function did not fold");
    compare(reference, roundTrip(b.design), {}, 256, "r3-select-rules");
}

void testOwnersAndDetectorKeys() {
    Builder b; const auto a = b.input("a", 8), rhs = b.input("b", 8);
    std::vector<grh::ValueId> roots;
    for (int i = 0; i < 3; ++i) { const auto result = b.compute(OperationKind::kAnd, b.type(8), {a, rhs}); roots.push_back(result); b.output("and" + std::to_string(i), result); }
    convert(b.design);
    for (int i = 0; i < 3; ++i) b.graph.setAttr(b.graph.valueDef(roots[i]), sim::kPartition, std::vector<std::string>{"general", "nonsink", i == 2 ? "sn1" : "sn0", "leaf"});
    const std::vector<grh::Port> ports(b.graph.outputPorts().begin(), b.graph.outputPorts().end());
    // A complete split graph owns its output calculation in Output. This
    // artificial owner-isolation fixture probes declared internal values.
    std::vector<std::string> internalProbes;
    for (const auto& port : ports) {
        b.graph.removeOutputPort(port.name);
        b.graph.addDeclaredSymbol(b.graph.valueSymbol(port.value));
        internalProbes.emplace_back(b.graph.symbolText(b.graph.valueSymbol(port.value)));
    }
    b.graph.setMetadata(sim::kStage, std::string("nonsink-partitioned"));
    const auto before = b.design.clone(); simplify(b.design);
    require(count(b.graph, OperationKind::kAnd) == 2, "same-owner CSE or cross-owner barrier failed");
    compare(before, roundTrip(b.design), internalProbes, 128, "r3-owner-isolation");
    b.graph.eraseAttr(b.graph.valueDef(roots[2]), sim::kPartition);
    const auto stamp = transform::DesignStamp::capture(b.graph);
    require(!run(b.design, std::make_unique<sim::SimplifyPass>()) && stamp.matches(b.graph), "partial owner attribution was optimized");

    Builder events; const auto clk = events.input("clk"), en = events.constant(1, "1'h1"), data = events.input("data", 8);
    const auto state = events.state("state", events.type(8), "8'h0"); events.output("q", events.read(state));
    for (int i = 0; i < 5; ++i) events.write(OperationKind::kRegisterWritePort, state, {en, data, clk});
    convert(events.design);
    std::vector<grh::OperationId> detectors;
    for (const auto op : events.graph.operations()) if (events.graph.opKind(op) == sim::kEdgeDetect) detectors.push_back(op);
    events.graph.setAttr(detectors[2], "edge", std::string("negedge"));
    events.graph.setAttr(detectors[3], "prevInit", std::string("1'h1"));
    events.graph.setAttr(detectors[4], "sampleScope", std::string("another-round"));
    const auto reference = events.design.clone(); simplify(events.design);
    require(count(events.graph, sim::kEdgeDetect) == 4, "different edge/init/scope detectors merged");
    compare(reference, roundTrip(events.design), {"state"}, 128, "r3-detector-keys");
    std::string error; const std::array<std::string_view, 1> role{"-role=event"};
    require(!transform::makePass("grhsim-simplify", role, error), "simplify still accepts a role");
}
}

int main() {
    try {
        testOperandReplacement();
        testGenerateGroupMembershipIndex();
        testEventsCallsAndPacking(); testConstantAliasConvergence(); testNamedAliasConsumers(); testArraysAndWriters(); testNarrowingAndStateSharing(); testMaskedOr();
        testPackingRejectionsAndOrigins(); testFourStateBoundaries(); testSelectRules(); testOwnersAndDetectorKeys();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "GRH-based GrhSIM conversion/optimization observations agree\n";
    return 0;
}
