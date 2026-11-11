#include "grhsim_oracle.hpp"
#include "../../lib/transform/grhsim/rewrite.hpp"

#include "core/store.hpp"
#include "transform/grhsim/grh-to-grhsim.hpp"
#include "transform/grhsim/expr-opt.hpp"
#include "transform/grhsim/used-bits.hpp"
#include "transform/alias_analysis.hpp"
#include "transform/constant_analysis.hpp"
#include "transform/common_expr.hpp"
#include "transform/pattern_detect.hpp"
#include "transform/split_nodes.hpp"
#include "transform/dead_code_elim.hpp"
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
        const std::string symbol(graph.getOperation(target).symbolText());
        graph.setAttr(op, kind == OperationKind::kLatchWritePort ? "latchSymbol" : "regSymbol", symbol);
        graph.assignWriterOrder(op, symbol);
        if (kind != OperationKind::kLatchWritePort) graph.setAttr(op, "eventEdge", std::move(edges));
        return op;
    }
    void output(std::string name, grh::ValueId value) { graph.bindOutputPort(name, value); }
};

bool run(grh::Design& design, std::unique_ptr<transform::Pass> pass, bool* changed = nullptr,
         std::optional<bool> keepDeclaredSymbols = std::nullopt) {
    transform::PassManager manager; manager.options().keepDeclaredSymbols = keepDeclaredSymbols;
    manager.addPass(std::move(pass)); transform::PassDiagnostics diagnostics;
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

void testF22Passes() {
    // ExprOpt must carry source identities while rewriting an internal
    // same-arm mux.  GSIM leaves dead rows for the explicit DCE stage, so the
    // operation remains as an assignment until that stage is requested.
    {
        Builder b;
        const auto input = b.input("input", 8);
        const auto base = b.compute(OperationKind::kAssign, b.type(8), {input});
        const auto mux = b.compute(OperationKind::kMux, b.type(8), {base, base, base});
        const auto muxOp = b.graph.valueDef(mux);
        const auto view = b.compute(OperationKind::kAssign, b.type(8), {mux});
        b.output("out", view);
        auto& tree = b.graph.getOrCreateSemanticTree("unit.fir", "Circuit", "top");
        const auto semantic = tree.addNode(tree.root(), grh::SemanticNodeKind::Expression, "muxNode");
        b.graph.setOpSemanticIds(muxOp, std::array{semantic});
        b.graph.setValueSemanticIds(mux, std::array{semantic});
        require(run(b.design, std::make_unique<sim::ExprOptPass>()), "F22 ExprOpt failed");
        require(std::find(b.graph.operations().begin(), b.graph.operations().end(), muxOp) != b.graph.operations().end() &&
                    b.graph.opKind(muxOp) == OperationKind::kAssign,
                "F22 ExprOpt did not rewrite the same-arm mux without hidden DCE");
        require(std::find(b.graph.valueSemanticIds(base).begin(), b.graph.valueSemanticIds(base).end(), semantic) !=
                    b.graph.valueSemanticIds(base).end(),
                "F22 ExprOpt dropped the surviving SemanticTree identity");
    }

    // GSIM compares complete expression subtrees, rather than only the
    // ValueId at each mux arm.  Two independently-built but equivalent
    // subtrees must therefore collapse to one arm while carrying the mux
    // source identity to the surviving value.
    {
        Builder b;
        const auto input = b.input("input", 8), select = b.input("select");
        const auto left = b.compute(OperationKind::kNot, b.type(8), {input});
        const auto right = b.compute(OperationKind::kNot, b.type(8), {input});
        const auto rightOp = b.graph.valueDef(right);
        const auto mux = b.compute(OperationKind::kMux, b.type(8), {select, left, right});
        const auto muxOp = b.graph.valueDef(mux);
        const auto view = b.compute(OperationKind::kAssign, b.type(8), {mux});
        b.output("out", view);
        auto& tree = b.graph.getOrCreateSemanticTree("unit.fir", "Circuit", "top");
        const auto semantic = tree.addNode(tree.root(), grh::SemanticNodeKind::Expression, "structuralMux");
        const auto rightSemantic = tree.addNode(tree.root(), grh::SemanticNodeKind::Expression, "structuralMuxCopy");
        b.graph.setOpSemanticIds(muxOp, std::array{semantic});
        b.graph.setValueSemanticIds(mux, std::array{semantic});
        b.graph.setOpSemanticIds(b.graph.valueDef(right), std::array{rightSemantic});
        b.graph.setValueSemanticIds(right, std::array{rightSemantic});
        require(run(b.design, std::make_unique<sim::ExprOptPass>()), "F22 structural ExprOpt failed");
        require(std::find(b.graph.operations().begin(), b.graph.operations().end(), muxOp) !=
                    b.graph.operations().end() && b.graph.opKind(muxOp) == OperationKind::kAssign,
                "F22 ExprOpt did not rewrite a structurally redundant mux without hidden DCE");
        require(std::find(b.graph.valueSemanticIds(left).begin(), b.graph.valueSemanticIds(left).end(), semantic) !=
                    b.graph.valueSemanticIds(left).end(),
                "F22 structural ExprOpt dropped the mux SemanticTree identity");
        require(std::find(b.graph.valueSemanticIds(left).begin(), b.graph.valueSemanticIds(left).end(), rightSemantic) !=
                    b.graph.valueSemanticIds(left).end(),
                "F22 structural ExprOpt dropped the equivalent arm SemanticTree identity");
        require(run(b.design, std::make_unique<transform::DeadCodeElimPass>()),
                "explicit DCE after F22 ExprOpt failed");
        require(std::find(b.graph.operations().begin(), b.graph.operations().end(), rightOp) ==
                    b.graph.operations().end(),
                "explicit DCE did not reclaim the discarded equivalent arm");
    }

    // UsedBits deliberately ignores declared symbols by default, matching
    // GSIM's liveness roots.  A declared wide state used only through a low
    // slice therefore narrows to that slice width.
    {
        Builder b;
        const auto clk = b.input("clk"), enable = b.input("enable"), data = b.input("data", 8);
        auto twoState = b.type(8); twoState.domain = grh::LogicDomain::TwoState;
        auto dataType = b.graph.valueLogicalType(data); dataType.domain = grh::LogicDomain::TwoState;
        b.graph.setValueLogicalType(data, dataType);
        const auto state = b.state("declared_wide", twoState, "8'h00", true);
        const auto readValue = b.read(state);
        const auto lowValue = b.slice(readValue, 0, 3);
        b.output("low", lowValue);
        b.write(OperationKind::kRegisterWritePort, state, {enable, data, clk});
        const auto deadPure = b.compute(OperationKind::kNot, b.type(8), {data});
        const auto deadPureOp = b.graph.valueDef(deadPure);
        const bool usedBitsOk = run(b.design, std::make_unique<sim::UsedBitsPass>(), nullptr, false);
        require(usedBitsOk, "F22 UsedBits failed");
        require(grh::stateLogicalType(b.graph, state).width == 3,
                "F22 UsedBits kept a declared state at its original width");
        require(std::find(b.graph.operations().begin(), b.graph.operations().end(), deadPureOp) !=
                    b.graph.operations().end(), "F22 UsedBits performed hidden DCE");
        require(run(b.design, std::make_unique<transform::DeadCodeElimPass>(), nullptr, false),
                "explicit DCE after UsedBits failed");
        require(std::find(b.graph.operations().begin(), b.graph.operations().end(), deadPureOp) ==
                    b.graph.operations().end(), "explicit DCE did not remove the dead pure cone");
    }

    // Random scalar initialization is width-parametric, so it remains
    // eligible for the same prefix narrowing as a literal initializer.
    {
        Builder b;
        auto twoState = b.type(8); twoState.domain = grh::LogicDomain::TwoState;
        const auto state = b.state("random_wide", twoState, "$random", true);
        b.output("low", b.slice(b.read(state), 0, 3));
        require(run(b.design, std::make_unique<sim::UsedBitsPass>(), nullptr, false), "F22 random UsedBits failed");
        require(grh::stateLogicalType(b.graph, state).width == 3,
                "F22 UsedBits rejected a random scalar initializer");
    }
}

void testF24UsedBitsIndexedReadKeepsFullIndex() {
    Builder b;
    const auto rawIndex = b.input("index", 4);
    const auto computedIndex = b.compute(OperationKind::kOr, b.type(4),
                                          {rawIndex, b.constant(4, "4'b0000")});
    const auto indexView = b.compute(OperationKind::kAssign, b.type(4), {computedIndex});
    const auto state = b.state("rows", b.type(128, {16}),
        "128'h0f0e0d0c0b0a09080706050403020100", false);
    b.output("selected", b.read(state, {indexView}));
    b.output("index_low", b.slice(computedIndex, 0, 1));
    auto reference = roundTrip(b.design);

    // The low-bit output alone would narrow computedIndex to one bit. The
    // indexed read must demand all four address bits through the assign view.
    require(run(b.design, std::make_unique<sim::UsedBitsPass>(), nullptr, false),
            "F24 UsedBits indexed-read fixture failed");
    require(b.graph.valueWidth(computedIndex) == 4 && b.graph.valueWidth(indexView) == 4,
            "F24 UsedBits narrowed an observable indexed-read address");
    require(run(b.design, std::make_unique<transform::GrhVerifyPass>()),
            "F24 UsedBits left a live invalid assign before the DCE boundary");
    {
        grhsim_test::Oracle original(*reference.findGraph("top")), rewritten(b.graph);
        for (uint64_t index = 0; index < 16; ++index) {
            const std::map<std::string, uint64_t> inputs{{"index", index}};
            require(original.eval(inputs) == rewritten.eval(inputs),
                    "F24 UsedBits changed an indexed state read before DCE");
        }
    }
    require(run(b.design, std::make_unique<transform::DeadCodeElimPass>(), nullptr, false),
            "F24 explicit DCE failed after indexed-read demand propagation");
    require(run(b.design, std::make_unique<transform::GrhVerifyPass>()),
            "F24 indexed-read fixture became invalid after explicit DCE");
    grhsim_test::Oracle original(*reference.findGraph("top")), rewritten(b.graph);
    for (uint64_t index = 0; index < 16; ++index) {
        const std::map<std::string, uint64_t> inputs{{"index", index}};
        require(original.eval(inputs) == rewritten.eval(inputs),
                "F24 UsedBits/DCE changed the selected row for a four-bit address");
    }
}

void testF24UsedBitsPreservesFourStateArithmetic() {
    for (const auto kind : {OperationKind::kAdd, OperationKind::kSub, OperationKind::kMul}) {
        Builder b;
        const auto packed = b.compute(OperationKind::kConcat, b.type(8),
                                      {b.constant(4, "4'bxxxx"), b.constant(4, "4'b0011")});
        const bool multiply = kind == OperationKind::kMul;
        const auto lhs = multiply ? packed : b.compute(OperationKind::kConcat, b.type(9),
                                                        {b.constant(1, "1'b0"), packed});
        const auto rhs = b.constant(multiply ? 8 : 9, multiply ? "8'd3" : "9'd1");
        const auto result = b.compute(kind, b.type(multiply ? 16 : 9), {lhs, rhs});
        b.output("low", b.slice(result, 0, 4));
        auto reference = roundTrip(b.design);
        const auto allX = slang::SVInt::createFillX(4, false).toString(slang::LiteralBase::Hex, true, 4);
        const auto checkUnknowns = [&] {
            grhsim_test::Oracle original(*reference.findGraph("top"), {}, nullptr, true);
            grhsim_test::Oracle rewritten(b.graph, {}, nullptr, true);
            const auto expected = original.eval({});
            require(expected.outputs.at("low") == allX,
                    "F24 high-X arithmetic fixture did not produce an unknown low result");
            require(expected == rewritten.eval({}),
                    "F24 UsedBits discarded high unknown arithmetic dependencies");
        };

        require(run(b.design, std::make_unique<sim::UsedBitsPass>(), nullptr, false),
                "F24 FourState arithmetic UsedBits failed");
        require(b.graph.valueWidth(packed) == 8 && b.graph.valueWidth(lhs) == (multiply ? 8 : 9) &&
                    b.graph.valueWidth(result) == (multiply ? 16 : 9),
                "F24 UsedBits narrowed FourState arithmetic or its operand cone");
        require(run(b.design, std::make_unique<transform::GrhVerifyPass>()),
                "F24 FourState arithmetic became invalid before explicit DCE");
        checkUnknowns();
        require(run(b.design, std::make_unique<transform::DeadCodeElimPass>(), nullptr, false),
                "F24 explicit DCE failed after FourState arithmetic UsedBits");
        require(run(b.design, std::make_unique<transform::GrhVerifyPass>()),
                "F24 FourState arithmetic became invalid after explicit DCE");
        checkUnknowns();

        // Folding before TwoState projection must retain the original X
        // result, which the conversion policy then projects to zero.
        require(run(reference, std::make_unique<transform::ConstantAnalysisPass>()) &&
                    run(b.design, std::make_unique<transform::ConstantAnalysisPass>()),
                "F24 high-X arithmetic constant analysis failed");
        convert(reference); convert(b.design);
        grhsim_test::Oracle original(*reference.findGraph("top")), rewritten(b.graph);
        const auto projected = original.eval({});
        require(projected.outputs.at("low") == "4'h0" && projected == rewritten.eval({}),
                "F24 UsedBits changed arithmetic after folding and TwoState projection");
    }

    // TwoState arithmetic retains the modular low-prefix optimization.
    for (const auto kind : {OperationKind::kAdd, OperationKind::kSub, OperationKind::kMul}) {
        Builder b;
        const auto lhs = b.input("lhs", 8), rhs = b.input("rhs", 8);
        const auto result = b.compute(kind, b.type(kind == OperationKind::kMul ? 16 : 8), {lhs, rhs});
        b.output("low", b.slice(result, 0, 4));
        for (const auto value : b.graph.values()) {
            auto type = b.graph.valueLogicalType(value); type.domain = grh::LogicDomain::TwoState;
            b.graph.setValueLogicalType(value, type);
        }
        auto reference = roundTrip(b.design);
        require(run(b.design, std::make_unique<sim::UsedBitsPass>(), nullptr, false),
                "F24 TwoState arithmetic UsedBits failed");
        require(b.graph.valueWidth(result) == 4,
                "F24 UsedBits stopped narrowing TwoState arithmetic");
        require(run(b.design, std::make_unique<transform::GrhVerifyPass>()),
                "F24 TwoState arithmetic became invalid after UsedBits");
        grhsim_test::Oracle original(*reference.findGraph("top")), rewritten(b.graph);
        for (uint64_t lhsBits = 0; lhsBits < 256; ++lhsBits) {
            const std::map<std::string, uint64_t> inputs{{"lhs", lhsBits}, {"rhs", 255 - lhsBits}};
            require(original.eval(inputs) == rewritten.eval(inputs),
                    "F24 UsedBits changed TwoState arithmetic low bits");
        }
    }
}

void testGrhToGrhsimSemanticTree() {
    Builder b;
    const auto clk = b.input("clk"), enable = b.input("enable"), data = b.input("data", 8);
    const auto state = b.state("semantic_state", b.type(8), "8'h00");
    const auto readValue = b.read(state);
    b.output("out", readValue);
    const auto write = b.write(OperationKind::kRegisterWritePort, state, {enable, data, clk});

    auto& tree = b.graph.getOrCreateSemanticTree("unit.fir", "Circuit", "top");
    const auto stateSemantic = tree.addNode(tree.root(), grh::SemanticNodeKind::Declaration, "semantic_state");
    const auto writeSemantic = tree.addNode(tree.root(), grh::SemanticNodeKind::Expression, "write");
    const auto clockSemantic = tree.addNode(tree.root(), grh::SemanticNodeKind::Declaration, "clk");
    b.graph.setOpSemanticIds(state, std::array{stateSemantic});
    b.graph.setOpSemanticIds(write, std::array{writeSemantic});
    b.graph.setValueSemanticIds(readValue, std::array{stateSemantic});
    b.graph.setValueSemanticIds(clk, std::array{clockSemantic});

    convert(b.design);
    bool sawEdge = false;
    for (const auto op : b.graph.operations()) {
        if (b.graph.opKind(op) != sim::kEdgeDetect) continue;
        sawEdge = true;
        const auto opIds = b.graph.opSemanticIds(op);
        require(std::find(opIds.begin(), opIds.end(), writeSemantic) != opIds.end(),
                "grh-to-grhsim dropped source SemanticId from edge detector");
        require(!b.graph.opResults(op).empty(), "edge detector has no result");
        const auto resultIds = b.graph.valueSemanticIds(b.graph.opResults(op).front());
        require(std::find(resultIds.begin(), resultIds.end(), writeSemantic) != resultIds.end(),
                "grh-to-grhsim dropped SemanticId from edge detector result");
        require(std::find(opIds.begin(), opIds.end(), clockSemantic) != opIds.end() &&
                    std::find(resultIds.begin(), resultIds.end(), clockSemantic) != resultIds.end(),
                "grh-to-grhsim dropped clock value SemanticId while growing the semantic pool");
    }
    require(sawEdge, "grh-to-grhsim did not synthesize an edge detector");

    // A plain SV-style graph has no SemanticTree. Conversion must keep that
    // representation valid and must not invent source identities.
    Builder plain;
    const auto plainClk = plain.input("clk"), plainEnable = plain.input("enable"), plainData = plain.input("data", 8);
    const auto plainState = plain.state("plain_state", plain.type(8), "8'h00");
    const auto plainRead = plain.read(plainState);
    plain.output("out", plainRead);
    plain.write(OperationKind::kRegisterWritePort, plainState, {plainEnable, plainData, plainClk});
    convert(plain.design);
    require(!plain.graph.semanticTree(), "grh-to-grhsim created a SemanticTree for a plain graph");
    for (const auto op : plain.graph.operations())
        require(plain.graph.opSemanticIds(op).empty(), "plain conversion invented operation SemanticIds");
    for (const auto value : plain.graph.values())
        require(plain.graph.valueSemanticIds(value).empty(), "plain conversion invented value SemanticIds");
}

void testF23SplitNodes() {
    Builder b;
    const auto lhs = b.input("lhs", 8), rhs = b.input("rhs", 8);
    const auto wide = b.compute(OperationKind::kAnd, b.type(8), {lhs, rhs});
    const auto low = b.slice(wide, 0, 3), high = b.slice(wide, 5, 2);
    b.output("low", low); b.output("high", high);

    const auto wideOp = b.graph.valueDef(wide);
    require(run(b.design, std::make_unique<transform::SplitNodesPass>()),
            "F23 SplitNodes failed");
    require(b.graph.opKind(b.graph.valueDef(low)) == OperationKind::kAssign &&
                b.graph.opKind(b.graph.valueDef(high)) == OperationKind::kAssign,
            "F23 SplitNodes did not retarget static slices");
    require(b.graph.valueWidth(b.graph.opOperands(b.graph.valueDef(low))[0]) == 3 &&
                b.graph.valueWidth(b.graph.opOperands(b.graph.valueDef(high))[0]) == 2,
            "F23 SplitNodes did not create narrow expression results");
    require(std::find(b.graph.operations().begin(), b.graph.operations().end(), wideOp) !=
                b.graph.operations().end(),
            "F23 SplitNodes performed hidden DCE");

    require(run(b.design, std::make_unique<transform::DeadCodeElimPass>(), nullptr, false),
            "F23 explicit DCE after SplitNodes failed");
    require(std::find(b.graph.operations().begin(), b.graph.operations().end(), wideOp) ==
                b.graph.operations().end(),
            "F23 explicit DCE did not reclaim the unused wide producer");

    convert(b.design);
    roundTrip(b.design);
}

void testF24SplitNodesShapedInputView() {
    for (const bool sign : {false, true}) {
        Builder b;
        const auto input = b.graph.createValue(b.graph.internSymbol("array"), b.type(24, {3}, sign));
        b.graph.bindInputPort("array", input);
        const auto flat = b.compute(OperationKind::kAssign, b.type(24), {input});
        const auto selected = b.slice(flat, 9, 5);
        b.output("selected", selected);
        auto reference = roundTrip(b.design);

        require(run(b.design, std::make_unique<transform::SplitNodesPass>()),
                "SplitNodes failed to slice an array through a scalar assignment");
        const auto selectedOp = b.graph.valueDef(selected);
        require(b.graph.opKind(selectedOp) == OperationKind::kAssign,
                "SplitNodes did not replace the scalar view's slice");
        const auto replacement = b.graph.opOperands(selectedOp)[0];
        require(b.graph.valueLogicalType(replacement) == b.type(5),
                "SplitNodes retained array shape or signedness on a bit-range view");
        const auto view = b.graph.valueDef(replacement);
        require(b.graph.opKind(view) == OperationKind::kSliceStatic &&
                    b.graph.opOperands(view)[0] == input,
                "SplitNodes did not preserve the shaped source of the scalar slice");

        grhsim_test::Oracle original(*reference.findGraph("top")), rewritten(b.graph);
        for (uint64_t bits = 0; bits < 256; ++bits) {
            const std::map<std::string, uint64_t> inputs{{"array", bits * 0x010101u}};
            require(original.eval(inputs) == rewritten.eval(inputs),
                    "SplitNodes changed the bits extracted from a shaped input");
        }
    }
}

void testF24SplitNodesRecursiveOperandsAndAttributes() {
    Builder b;
    const auto a = b.input("a", 16), c = b.input("c", 16);
    const auto select = b.input("select");
    auto left = a, right = c;
    for (int i = 0; i < 32; ++i) {
        left = b.compute(OperationKind::kXor, b.type(16), {left, c});
        right = b.compute(OperationKind::kAnd, b.type(16), {right, a});
    }
    const auto joined = b.compute(OperationKind::kMux, b.type(16), {select, left, right});
    const auto source = b.graph.valueDef(joined);
    b.graph.setAttr(source, "originModules", std::vector<std::string>{"top$source"});
    b.graph.setAttr(source, "splitFixture", std::string("copied"));
    b.graph.setAttr(source, "splitFixtureBits", std::vector<int64_t>{2, 3, 5});
    const auto low = b.slice(joined, 0, 3), high = b.slice(joined, 9, 5);
    b.output("low", low); b.output("high", high);
    auto reference = roundTrip(b.design);
    require(run(b.design, std::make_unique<transform::SplitNodesPass>()),
            "SplitNodes failed while recursive slicing grew shared graph pools");
    for (const auto value : {low, high}) {
        const auto replacement = b.graph.opOperands(b.graph.valueDef(value))[0];
        const auto clone = b.graph.valueDef(replacement);
        require(b.graph.opKind(clone) == OperationKind::kMux &&
                    sim::attribute<std::string>(b.graph, clone, "splitFixture") == "copied" &&
                    sim::attribute<std::vector<int64_t>>(b.graph, clone, "splitFixtureBits") ==
                        std::vector<int64_t>{2, 3, 5} &&
                    sim::attribute<std::vector<std::string>>(b.graph, clone, "originModules") ==
                        std::vector<std::string>{"top$source"},
                "SplitNodes did not preserve source attributes across graph pool growth");
    }
    grhsim_test::Oracle original(*reference.findGraph("top")), rewritten(b.graph);
    for (uint64_t bits = 0; bits < 512; ++bits) {
        const std::map<std::string, uint64_t> inputs{
            {"a", bits * 0x0101u}, {"c", (bits * 0x132du) & 0xffffu}, {"select", bits & 1}};
        require(original.eval(inputs) == rewritten.eval(inputs),
                "SplitNodes changed a recursively sliced mux expression");
    }
}

void testF23PatternDetect() {
    Builder b;
    const auto high = b.input("high", 4), low = b.input("low", 4);
    const auto joined = b.compute(OperationKind::kConcat, b.type(8), {high, low}, "joined");
    const auto expected = b.constant(8, "8'hA5");
    const auto match = b.compute(OperationKind::kEq, b.type(1), {joined, expected}, "match");
    b.output("match", match);
    const auto joinedOp = b.graph.valueDef(joined), matchOp = b.graph.valueDef(match);
    require(run(b.design, std::make_unique<transform::PatternDetectPass>()),
            "F23 PatternDetect failed");
    require(b.graph.opKind(matchOp) == OperationKind::kAnd &&
                b.graph.opOperands(matchOp).size() == 2,
            "F23 PatternDetect did not split concat equality");
    for (const auto operand : b.graph.opOperands(matchOp))
        require(b.graph.opKind(b.graph.valueDef(operand)) == OperationKind::kEq,
                "F23 PatternDetect did not create equality tests");
    require(std::find(b.graph.operations().begin(), b.graph.operations().end(), joinedOp) !=
                b.graph.operations().end(),
            "F23 PatternDetect performed hidden DCE");
    require(run(b.design, std::make_unique<transform::DeadCodeElimPass>(), nullptr, false),
            "explicit DCE after F23 PatternDetect failed");
    require(std::find(b.graph.operations().begin(), b.graph.operations().end(), joinedOp) ==
                b.graph.operations().end(),
            "explicit DCE did not reclaim the replaced concat");
}

void testF23AliasAnalysis() {
    Builder b;
    const auto input = b.input("input", 8);
    const auto alias = b.compute(OperationKind::kAssign, b.type(8), {input}, "alias");
    const auto full = b.slice(alias, 0, 8);
    const auto consumer = b.compute(OperationKind::kNot, b.type(8), {full}, "consumer");
    b.output("out", consumer);
    const auto aliasOp = b.graph.valueDef(alias), fullOp = b.graph.valueDef(full);
    auto& tree = b.graph.getOrCreateSemanticTree("unit.fir", "Circuit", "top");
    const auto aliasSemantic = tree.addNode(tree.root(), grh::SemanticNodeKind::Expression, "aliasNode");
    b.graph.setOpSemanticIds(aliasOp, std::array{aliasSemantic});
    b.graph.setValueSemanticIds(alias, std::array{aliasSemantic});
    require(run(b.design, std::make_unique<transform::AliasAnalysisPass>()),
            "F23 AliasAnalysis failed");
    require(std::find(b.graph.operations().begin(), b.graph.operations().end(), aliasOp) !=
                b.graph.operations().end() &&
                std::find(b.graph.operations().begin(), b.graph.operations().end(), fullOp) !=
                b.graph.operations().end(),
            "F23 AliasAnalysis performed hidden DCE");
    require(b.graph.opOperands(b.graph.valueDef(consumer))[0] == input,
            "F23 AliasAnalysis did not redirect identity views");
    require(std::find(b.graph.valueSemanticIds(input).begin(), b.graph.valueSemanticIds(input).end(), aliasSemantic) !=
                b.graph.valueSemanticIds(input).end(),
            "F23 AliasAnalysis dropped the surviving SemanticTree identity");
    require(run(b.design, std::make_unique<transform::DeadCodeElimPass>(), nullptr, false),
            "explicit DCE after F23 AliasAnalysis failed");
    require(std::find(b.graph.operations().begin(), b.graph.operations().end(), aliasOp) ==
                b.graph.operations().end() &&
                std::find(b.graph.operations().begin(), b.graph.operations().end(), fullOp) ==
                b.graph.operations().end(),
            "explicit DCE did not reclaim aliases");
}

void testF23ConstantAnalysis() {
    // A pure scalar expression whose complete input cone is literal constants
    // becomes a kConstant in place.  The pass must preserve the output value
    // identity so callers can inspect the folded producer directly.
    {
        Builder b;
        const auto sum = b.compute(OperationKind::kAdd, b.type(8),
                                   {b.constant(8, "8'h03"), b.constant(8, "8'h04")}, "sum");
        b.output("sum", sum);
        require(run(b.design, std::make_unique<transform::ConstantAnalysisPass>()),
                "F23 constant analysis failed on a pure expression");
        const auto op = b.graph.valueDef(sum);
        require(b.graph.opKind(op) == OperationKind::kConstant,
                "F23 pure expression was not rewritten to a constant");
        const auto folded = slang::SVInt::fromString(sim::attribute<std::string>(b.graph, op, "constValue").value());
        require(folded.as<uint64_t>() == std::optional<uint64_t>(7),
                "F23 pure expression produced the wrong literal");
    }

    // Folding keeps the producer identity, and also carries source node
    // identities from constant operands that the later explicit DCE may
    // reclaim.
    {
        Builder b;
        const auto lhs = b.constant(8, "8'h03"), rhs = b.constant(8, "8'h04");
        const auto sum = b.compute(OperationKind::kAdd, b.type(8), {lhs, rhs}, "semantic_sum");
        b.output("sum", sum);
        auto& tree = b.graph.getOrCreateSemanticTree("unit.fir", "Circuit", "top");
        const auto lhsSemantic = tree.addNode(tree.root(), grh::SemanticNodeKind::Expression, "lhs");
        const auto rhsSemantic = tree.addNode(tree.root(), grh::SemanticNodeKind::Expression, "rhs");
        const auto sumSemantic = tree.addNode(tree.root(), grh::SemanticNodeKind::Expression, "sum");
        b.graph.setValueSemanticIds(lhs, std::array{lhsSemantic});
        b.graph.setOpSemanticIds(b.graph.valueDef(lhs), std::array{lhsSemantic});
        b.graph.setValueSemanticIds(rhs, std::array{rhsSemantic});
        b.graph.setOpSemanticIds(b.graph.valueDef(rhs), std::array{rhsSemantic});
        b.graph.setValueSemanticIds(sum, std::array{sumSemantic});
        b.graph.setOpSemanticIds(b.graph.valueDef(sum), std::array{sumSemantic});
        require(run(b.design, std::make_unique<transform::ConstantAnalysisPass>()),
                "F23 semantic constant analysis failed");
        const auto semantics = b.graph.valueSemanticIds(sum);
        require(std::find(semantics.begin(), semantics.end(), lhsSemantic) != semantics.end() &&
                    std::find(semantics.begin(), semantics.end(), rhsSemantic) != semantics.end() &&
                    std::find(semantics.begin(), semantics.end(), sumSemantic) != semantics.end(),
                "F23 constant analysis dropped folded source SemanticTree identities");
    }

    // Pure system functions are still represented by the ordinary GRH kind
    // before conversion.  ConstantAnalysis may fold the evaluator-supported
    // subset, while effectful/sampled functions remain opaque.
    {
        Builder b;
        const auto input = b.constant(32, "32'd17");
        const auto pure = b.compute(OperationKind::kSystemFunction, b.type(32), {input}, "clog2");
        b.graph.setAttr(b.graph.valueDef(pure), "name", std::string("clog2"));
        const auto sampled = b.compute(OperationKind::kSystemFunction, b.type(32), {input}, "random");
        b.graph.setAttr(b.graph.valueDef(sampled), "name", std::string("random"));
        b.output("pure", pure); b.output("sampled", sampled);
        require(run(b.design, std::make_unique<transform::ConstantAnalysisPass>()),
                "F23 system-function constant analysis failed");
        require(b.graph.opKind(b.graph.valueDef(pure)) == OperationKind::kConstant,
                "F23 pure system function was not folded");
        require(b.graph.opKind(b.graph.valueDef(sampled)) == OperationKind::kSystemFunction,
                "F23 sampled system function was folded unsafely");
    }

    // A constant select can choose between two constant arms.  With a dynamic
    // select, the same pass must leave a differing-arm mux intact because its
    // result is not a compile-time constant.
    {
        Builder b;
        const auto selected = b.compute(OperationKind::kMux, b.type(8),
                                        {b.constant(1, "1'b1"), b.constant(8, "8'h11"),
                                         b.constant(8, "8'h22")}, "selected");
        const auto select = b.input("select");
        const auto dynamic = b.compute(OperationKind::kMux, b.type(8),
                                       {select, b.constant(8, "8'h11"), b.constant(8, "8'h22")}, "dynamic");
        b.output("selected", selected); b.output("dynamic", dynamic);
        require(run(b.design, std::make_unique<transform::ConstantAnalysisPass>()),
                "F23 constant analysis failed on muxes");
        require(b.graph.opKind(b.graph.valueDef(selected)) == OperationKind::kConstant,
                "F23 constant-select mux was not folded");
        require(b.graph.opKind(b.graph.valueDef(dynamic)) == OperationKind::kMux,
                "F23 dynamic-select mux was folded unsafely");
    }

    // Constant analysis owns no liveness policy: an unobserved pure cone is
    // folded but remains present, and a declared operation remains anchored
    // until an explicit DCE pass is requested.
    {
        Builder b;
        const auto dead = b.compute(OperationKind::kAdd, b.type(8),
                                    {b.constant(8, "8'h09"), b.constant(8, "8'h01")}, "declared_dead");
        b.graph.addDeclaredSymbol(b.graph.operationSymbol(b.graph.valueDef(dead)));
        const auto deadOp = b.graph.valueDef(dead);
        const auto live = b.compute(OperationKind::kNot, b.type(8), {b.constant(8, "8'h00")}, "live");
        b.output("live", live);
        require(run(b.design, std::make_unique<transform::ConstantAnalysisPass>()),
                "F23 constant analysis failed on an anchored dead cone");
        require(std::find(b.graph.operations().begin(), b.graph.operations().end(), deadOp) !=
                    b.graph.operations().end() && b.graph.opKind(deadOp) == OperationKind::kConstant,
                "F23 constant analysis performed hidden DCE or lost the declared cone");
        require(b.graph.validateDeclaredSymbols().empty(),
                "F23 constant analysis invalidated a declared symbol");
    }
}

void testConstantAnalysisTopology() {
    {
        Builder b;
        constexpr int32_t repetitions = 32768;
        const auto one = b.constant(1, "1'b1");
        const auto concat = b.graph.createOperation(OperationKind::kConcat);
        const auto value = b.graph.createValue(b.type(repetitions));
        b.graph.addResult(concat, value);
        for (int32_t i = 0; i < repetitions; ++i) b.graph.addOperand(concat, one);
        b.output("out", value);
        require(run(b.design, std::make_unique<transform::GrhVerifyPass>()),
                "ConstantAnalysis repeated-input fixture is invalid GRH");
        require(run(b.design, std::make_unique<transform::ConstantAnalysisPass>()),
                "ConstantAnalysis failed on a high-fanin repeated input");
        require(b.graph.opKind(concat) == OperationKind::kConstant,
                "ConstantAnalysis did not fold the high-fanin concat");
        const auto folded = slang::SVInt::fromString(sim::attribute<std::string>(b.graph, concat, "constValue").value());
        require(folded.getBitWidth() == repetitions && !folded.hasUnknown() && folded.countOnes() == repetitions,
                "ConstantAnalysis produced incorrect repeated concat bits");
    }
    {
        Builder b;
        constexpr int32_t count = 4096;
        std::vector<grh::ValueId> inputs;
        for (int32_t i = 0; i < count; ++i) inputs.push_back(b.graph.createValue(b.type(1)));
        const auto concat = b.graph.createOperation(OperationKind::kConcat);
        const auto value = b.graph.createValue(b.type(count));
        b.graph.addResult(concat, value);
        for (const auto input : inputs) b.graph.addOperand(concat, input);
        for (int32_t i = count; i-- > 0;) {
            const auto constant = b.graph.createOperation(OperationKind::kConstant);
            b.graph.addResult(constant, inputs[i]);
            b.graph.setAttr(constant, "constValue", std::string(i % 2 == 0 ? "1'b1" : "1'b0"));
        }
        b.output("out", value);
        require(run(b.design, std::make_unique<transform::GrhVerifyPass>()),
                "ConstantAnalysis reverse-defined fixture is invalid GRH");
        require(run(b.design, std::make_unique<transform::ConstantAnalysisPass>()),
                "ConstantAnalysis failed on reverse-defined distinct producers");
        require(b.graph.opKind(concat) == OperationKind::kConstant,
                "ConstantAnalysis did not topologically fold the reverse-defined concat");
        const auto folded = slang::SVInt::fromString(sim::attribute<std::string>(b.graph, concat, "constValue").value());
        require(folded.countOnes() == count / 2 &&
                    folded.slice(63, 0).as<uint64_t>() == std::optional<uint64_t>(0xaaaaaaaaaaaaaaaaULL),
                "ConstantAnalysis changed reverse-defined concat operand order");
    }
    {
        Builder b;
        const auto input = b.input("input", 8);
        const auto lhs = b.graph.createValue(b.type(8)), rhs = b.graph.createValue(b.type(8));
        // A real external producer has several results. Its consumer is
        // deliberately defined first and repeats each result, plus an input
        // with no producer, so decrement multiplicities must match uses.
        const auto dynamic = b.compute(OperationKind::kConcat, b.type(56), {lhs, lhs, rhs, input, lhs, rhs, rhs});
        b.output("dynamic", dynamic);
        const auto imported = b.graph.createOperation(OperationKind::kDpicImport, b.graph.internSymbol("dpi"));
        b.graph.setAttr(imported, "argsName", std::vector<std::string>{"lhs", "rhs"});
        b.graph.setAttr(imported, "argsDirection", std::vector<std::string>{"output", "output"});
        b.graph.setAttr(imported, "argsWidth", std::vector<int64_t>{8, 8});
        b.graph.setAttr(imported, "argsSigned", std::vector<bool>{false, false});
        b.graph.setAttr(imported, "hasReturn", false);
        const auto call = b.graph.createOperation(OperationKind::kDpicCall);
        b.graph.addOperand(call, b.constant(1, "1'b1"));
        b.graph.addResult(call, lhs);
        b.graph.addResult(call, rhs);
        b.graph.setAttr(call, "targetImportSymbol", std::string("dpi"));
        const auto sum = b.compute(OperationKind::kAdd, b.type(8), {b.constant(8, "8'd7"), b.constant(8, "8'd8")});
        b.output("sum", sum);
        require(run(b.design, std::make_unique<transform::GrhVerifyPass>()),
                "ConstantAnalysis multi-result fixture is invalid GRH");
        require(run(b.design, std::make_unique<transform::ConstantAnalysisPass>()),
                "ConstantAnalysis incorrectly rejected multi-result/repeated input topology");
        require(b.graph.opKind(b.graph.valueDef(dynamic)) == OperationKind::kConcat &&
                    b.graph.opOperands(b.graph.valueDef(dynamic)).size() == 7,
                "ConstantAnalysis folded a dynamic multi-result producer");
        const auto folded = slang::SVInt::fromString(sim::attribute<std::string>(b.graph, b.graph.valueDef(sum), "constValue").value());
        require(folded.as<uint64_t>() == std::optional<uint64_t>(15),
                "ConstantAnalysis lost an independent constant cone");
    }
    {
        Builder b;
        const auto lhs = b.graph.createValue(b.type(8)), rhs = b.graph.createValue(b.type(8));
        const auto left = b.graph.createOperation(OperationKind::kNot);
        b.graph.addOperand(left, rhs);
        b.graph.addResult(left, lhs);
        const auto right = b.graph.createOperation(OperationKind::kNot);
        b.graph.addOperand(right, lhs);
        b.graph.addResult(right, rhs);
        b.output("out", lhs);
        transform::PassManager manager;
        manager.addPass(std::make_unique<transform::ConstantAnalysisPass>());
        transform::PassDiagnostics diagnostics;
        require(!manager.run(b.design, diagnostics).success,
                "ConstantAnalysis accepted a combinational cycle");
        require(std::any_of(diagnostics.messages().begin(), diagnostics.messages().end(), [](const auto& message) {
                    return message.message.find("requires an acyclic expanded graph") != std::string::npos;
                }), "ConstantAnalysis lost its cycle rejection diagnostic");
    }
}

void testF23CommonExpr() {
    Builder b;
    const auto input = b.input("input", 8);
    auto& tree = b.graph.getOrCreateSemanticTree("unit.fir", "Circuit", "top");
    std::vector<grh::SemanticId> semantics;
    std::vector<grh::OperationId> duplicateOps;
    for (int i = 0; i < 5; ++i) {
        const auto value = b.compute(OperationKind::kNot, b.type(8), {input}, "common" + std::to_string(i));
        duplicateOps.push_back(b.graph.valueDef(value));
        const auto semantic = tree.addNode(tree.root(), grh::SemanticNodeKind::Expression,
                                           "common" + std::to_string(i));
        semantics.push_back(semantic);
        b.graph.setOpSemanticIds(duplicateOps.back(), std::array{semantic});
        b.graph.setValueSemanticIds(value, std::array{semantic});
        b.graph.setAttr(duplicateOps.back(), "originModules", std::vector<std::string>{"origin" + std::to_string(i)});
        if (i == 4) b.graph.addDeclaredSymbol(b.graph.operationSymbol(duplicateOps.back()));
        b.output("out" + std::to_string(i), b.compute(OperationKind::kAssign, b.type(8), {value}));
    }
    require(run(b.design, std::make_unique<transform::CommonExprPass>()),
            "F23 CommonExpr failed on repeated pure expressions");
    for (const auto op : duplicateOps)
        require(std::find(b.graph.operations().begin(), b.graph.operations().end(), op) !=
                    b.graph.operations().end(),
                "F23 CommonExpr performed hidden DCE");
    // The five expression rows remain until the caller requests liveness
    // cleanup, but only the representative keeps a user after sharing.
    std::size_t liveNot = 0;
    for (const auto op : duplicateOps)
        if (!b.graph.valueUsers(b.graph.opResults(op)[0]).empty()) ++liveNot;
    require(liveNot == 1, "F23 CommonExpr did not redirect duplicate users");
    const auto representative = duplicateOps.front();
    for (const auto semantic : semantics) {
        const auto opIds = b.graph.opSemanticIds(representative);
        const auto valueIds = b.graph.valueSemanticIds(b.graph.opResults(representative)[0]);
        require(std::find(opIds.begin(), opIds.end(), semantic) != opIds.end() &&
                    std::find(valueIds.begin(), valueIds.end(), semantic) != valueIds.end(),
                "F23 CommonExpr dropped a merged operation/value SemanticId");
    }
    require(transform::getOriginModules(b.graph, representative).value_or(std::vector<std::string>{}).size() == 5,
            "F23 CommonExpr dropped merged origins");
    require(run(b.design, std::make_unique<transform::DeadCodeElimPass>(), nullptr, false),
            "explicit DCE after CommonExpr failed");
    std::size_t survivingNot = 0;
    for (const auto op : duplicateOps)
        if (std::find(b.graph.operations().begin(), b.graph.operations().end(), op) !=
            b.graph.operations().end()) ++survivingNot;
    require(survivingNot == 1, "explicit DCE did not reclaim CommonExpr duplicates");
}

void testCommonExprSharedDagAndDeepChain() {
    {
        Builder b;
        auto value = b.input("input", 8);
        for (int i = 0; i < 48; ++i)
            value = b.compute(OperationKind::kAdd, b.type(8), {value, value});
        b.output("out", b.compute(OperationKind::kAssign, b.type(8), {value}));
        const auto size = b.graph.operations().size();
        bool changed = true;
        require(run(b.design, std::make_unique<transform::CommonExprPass>(), &changed),
                "CommonExpr failed on a 48-level shared doubling DAG");
        require(!changed && b.graph.operations().size() == size,
                "CommonExpr unexpectedly rewrote the unique shared DAG");
    }
    {
        Builder b;
        constexpr std::size_t depth = 16000;
        std::vector<grh::ValueId> values{b.input("input", 8)};
        for (std::size_t i = 0; i < depth; ++i) values.push_back(b.graph.createValue(b.type(8)));
        // Define consumers first so the first candidate requires the complete
        // postorder traversal rather than hitting already-memoized children.
        for (std::size_t i = depth; i != 0; --i) {
            const auto op = b.graph.createOperation(OperationKind::kNot);
            b.graph.addOperand(op, values[i - 1]);
            b.graph.addResult(op, values[i]);
        }
        b.output("out", b.compute(OperationKind::kAssign, b.type(8), {values.back()}));
        bool changed = true;
        require(run(b.design, std::make_unique<transform::CommonExprPass>(), &changed),
                "CommonExpr failed on a reverse-defined deep unary chain");
        require(!changed, "CommonExpr unexpectedly merged distinct chain depths");
    }
}

void testCommonExprPolicyAndExactAttributes() {
    const auto policy = [](int copies, int width, bool fanout, bool expected) {
        Builder b;
        const auto input = b.input("input", width);
        std::vector<grh::OperationId> ops;
        for (int i = 0; i < copies; ++i) {
            const auto value = b.compute(OperationKind::kNot, b.type(width), {input});
            ops.push_back(b.graph.valueDef(value));
            b.output("out" + std::to_string(i), b.compute(OperationKind::kAssign, b.type(width), {value}));
            if (fanout && i == 0)
                b.output("extra", b.compute(OperationKind::kAssign, b.type(width), {value}));
        }
        bool changed = false;
        require(run(b.design, std::make_unique<transform::CommonExprPass>(), &changed),
                "CommonExpr policy fixture failed");
        require(changed == expected, "CommonExpr changed the count/width/fanout merge threshold");
        require(std::all_of(ops.begin(), ops.end(), [&](auto op) {
                    return std::find(b.graph.operations().begin(), b.graph.operations().end(), op) != b.graph.operations().end();
                }), "CommonExpr policy fixture performed hidden DCE");
    };
    policy(4, 256, false, false);
    policy(5, 256, false, true);
    policy(2, 257, false, true);
    policy(2, 256, true, true);
    {
        Builder b;
        const auto input = b.input("input", 8);
        for (int i = 0; i < 2; ++i) {
            const auto value = b.compute(OperationKind::kNot, b.type(8), {input});
            b.output("out" + std::to_string(i), b.compute(OperationKind::kAdd, b.type(8), {value, value}));
        }
        bool changed = true;
        require(run(b.design, std::make_unique<transform::CommonExprPass>(), &changed),
                "CommonExpr repeated-operand fanout fixture failed");
        require(!changed, "CommonExpr counted two operands of one consumer as fanout two");
    }
    for (const bool reverseDefinitions : {false, true}) {
        Builder b;
        const auto input = b.input("input", 8);
        const std::array values{b.graph.createValue(b.type(8)), b.graph.createValue(b.type(8))};
        const auto defineSources = [&]() {
            for (const auto value : values) {
                const auto op = b.graph.createOperation(OperationKind::kNot);
                b.graph.addOperand(op, input);
                b.graph.addResult(op, value);
            }
        };
        if (!reverseDefinitions) defineSources();
        std::vector<grh::OperationId> sums, users;
        for (int i = 0; i < 2; ++i) {
            const auto sum = b.compute(OperationKind::kAdd, b.type(8), {values[i], values[i]});
            sums.push_back(b.graph.valueDef(sum));
            for (int j = 0; j < 2; ++j) {
                const auto user = b.compute(OperationKind::kAssign, b.type(8), {sum});
                users.push_back(b.graph.valueDef(user));
                b.output("out" + std::to_string(users.size()), user);
            }
        }
        if (reverseDefinitions) defineSources();
        require(run(b.design, std::make_unique<transform::CommonExprPass>()),
                "CommonExpr interacting-group fanout fixture failed");
        require(b.graph.opOperands(sums[0])[0] != b.graph.opOperands(sums[1])[0],
                "CommonExpr changed the single-consumer source-group decision");
        require(b.graph.opOperands(users[0])[0] == b.graph.opOperands(users[2])[0],
                "CommonExpr failed to share the downstream group with two distinct consumers");
    }
    for (const bool vectorAttribute : {false, true}) {
        Builder b;
        const auto input = b.input("input", 8);
        for (int i = 0; i < 5; ++i) {
            const auto value = b.compute(OperationKind::kNot, b.type(8), {input});
            if (vectorAttribute)
                b.graph.setAttr(b.graph.valueDef(value), "exact", std::vector<double>{2.0000001 + i * 0.0000001});
            else
                b.graph.setAttr(b.graph.valueDef(value), "exact", 1.0000001 + i * 0.0000001);
            b.output("out" + std::to_string(i), b.compute(OperationKind::kAssign, b.type(8), {value}));
        }
        bool changed = true;
        require(run(b.design, std::make_unique<transform::CommonExprPass>(), &changed),
                "CommonExpr exact-attribute fixture failed");
        require(!changed, "CommonExpr merged unequal floating attributes after lossy formatting");
    }
    {
        Builder b;
        const auto lhs = b.input("lhs", 8), rhs = b.input("rhs", 8);
        std::vector<grh::OperationId> users;
        for (int direction = 0; direction < 2; ++direction)
            for (int i = 0; i < 5; ++i) {
                const auto value = b.compute(OperationKind::kSub, b.type(8),
                                              {direction ? rhs : lhs, direction ? lhs : rhs});
                const auto user = b.compute(OperationKind::kAssign, b.type(8), {value});
                users.push_back(b.graph.valueDef(user));
                b.output("out" + std::to_string(users.size()), user);
            }
        require(run(b.design, std::make_unique<transform::CommonExprPass>()),
                "CommonExpr ordered-operand fixture failed");
        require(b.graph.opOperands(users[0])[0] != b.graph.opOperands(users[5])[0],
                "CommonExpr ignored operand order in its exact signature");
    }
}

} // namespace

int main() {
    try {
        testOperandReplacement();
        testGenerateGroupMembershipIndex();
        testF22Passes();
        testF24UsedBitsIndexedReadKeepsFullIndex();
        testF24UsedBitsPreservesFourStateArithmetic();
        testGrhToGrhsimSemanticTree();
        testF23ConstantAnalysis();
        testConstantAnalysisTopology();
        testF23CommonExpr();
        testCommonExprSharedDagAndDeepChain();
        testCommonExprPolicyAndExactAttributes();
        testF23SplitNodes();
        testF24SplitNodesShapedInputView();
        testF24SplitNodesRecursiveOperandsAndAttributes();
        testF23PatternDetect();
        testF23AliasAnalysis();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "GRH-based GrhSIM conversion/optimization observations agree\n";
    return 0;
}
