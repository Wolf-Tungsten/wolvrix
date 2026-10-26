#include "core/grh.hpp"
#include "core/load.hpp"
#include "core/op_descriptor.hpp"
#include "core/state.hpp"
#include "core/store.hpp"
#include "core/transform.hpp"
#include "transform/simplify.hpp"
#include "transform/hier_flatten.hpp"
#include "transform/instance_inline.hpp"
#include "transform/verify.hpp"

#include <functional>
#include <iostream>
#include <stdexcept>

namespace grh = wolvrix::lib::grh;
namespace transform = wolvrix::lib::transform;
namespace store = wolvrix::lib::store;

namespace {
void require(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

void rejects(const std::function<void()>& action, std::string_view message)
{
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    require(rejected, message);
}

bool verify(grh::Design& design)
{
    transform::PassManager manager;
    manager.addPass(std::make_unique<transform::GrhVerifyPass>());
    transform::PassDiagnostics diagnostics;
    return manager.run(design, diagnostics).success && !diagnostics.hasError();
}

void testTypesAndAssign()
{
    grh::LogicalType array{grh::ValueType::Logic, 48, true, grh::LogicDomain::TwoState, {2, 3}};
    require(array.leafWidth() == 8, "array geometry");
    for (const std::vector<int64_t> shape : {std::vector<int64_t>{0}, {-1}, {5}, {INT64_MAX, 2}})
    {
        auto invalid = array;
        invalid.shape = shape;
        rejects([&] { invalid.validate(); }, "invalid geometry accepted");
    }
    auto invalid = array;
    invalid.kind = grh::ValueType::String;
    rejects([&] { invalid.validate(); }, "string array accepted");

    grh::Design design;
    auto& graph = design.createGraph("top");
    design.markAsTop("top");
    const auto source = graph.createValue(graph.internSymbol("source"), array);
    graph.bindInputPort("source", source);
    auto flatType = array;
    flatType.shape.clear();
    flatType.isSigned = false;
    const auto flat = graph.createValue(graph.internSymbol("flat"), flatType);
    const auto assign = graph.createOperation(grh::OperationKind::kAssign);
    graph.addOperand(assign, source);
    graph.addResult(assign, flat);
    graph.bindOutputPort("flat", flat);
    require(verify(design), "equal-width array reinterpretation rejected");
    rejects([&] { graph.replaceAllUses(flat, source); }, "different full types aliased");
    const auto view = graph.replaceAllUsesWithView(flat, source);
    require(graph.sameValueType(flat, view) && graph.valueShape(source).size() == 2,
            "view changed the source or dropped its result type");
    require(graph.opKind(graph.valueDef(view)) == grh::OperationKind::kAssign,
            "view must use assign");
    require(verify(design), "typed view failed verification");
    auto wrong = flatType;
    wrong.width = 24;
    graph.setValueLogicalType(flat, wrong);
    require(!verify(design), "unequal-width assign accepted");
    graph.setValueLogicalType(flat, flatType);
    wrong = flatType;
    wrong.domain = grh::LogicDomain::FourState;
    graph.setValueLogicalType(flat, wrong);
    require(!verify(design), "implicit domain projection accepted");
    require(!grh::parseOperationKind("kAssignAs"), "removed kind still parsed");
}

void testLifecycleAndJson()
{
    grh::Design design;
    auto& graph = design.createGraph("top");
    design.markAsTop("top");
    auto type = grh::LogicalType{grh::ValueType::Logic, 8, false, grh::LogicDomain::TwoState, {1}};
    const auto unused = graph.createValue(graph.internSymbol("unused"), type);
    const auto value = graph.createValue(graph.internSymbol("input"), type);
    graph.bindInputPort("input", value);
    auto stamp = transform::DesignStamp::capture(graph);
    graph.setMetadata("test.stage", std::string("complete"));
    require(!stamp.matches(graph), "metadata did not invalidate stamp");
    stamp = transform::DesignStamp::capture(graph);
    graph.eraseValue(unused);
    require(!stamp.matches(graph), "erase did not invalidate stamp");
    stamp = transform::DesignStamp::capture(graph);
    graph.freeze();
    require(!stamp.matches(graph), "freeze did not invalidate remapped IDs");
    const auto live = graph.findValue("input");
    require(live.index == 1 && live.generation == 0 && graph.valueLogicalType(live) == type,
            "freeze lost type/domain or failed to compact IDs");
    stamp = transform::DesignStamp::capture(graph);
    graph.setValueSymbol(live, graph.internSymbol("renamed"));
    require(!stamp.matches(graph), "rename did not invalidate stamp");
    stamp = transform::DesignStamp::capture(graph);
    const auto result = graph.createValue(type);
    require(!stamp.matches(graph), "create did not invalidate stamp");
    stamp = transform::DesignStamp::capture(graph);
    graph.bindOutputPort("out", result);
    require(!stamp.matches(graph), "port mutation did not invalidate stamp");

    auto clone = design.clone();
    require(clone.instanceId() != design.instanceId(), "clone inherited identity");
    require(!transform::DesignStamp::capture(graph).matches(*clone.findGraph("top")), "clone reused stamp");
    require(clone.findGraph("top")->valueLogicalType(clone.findGraph("top")->findValue("renamed")) == type,
            "clone lost full type");
    for (const auto mode : {store::JsonPrintMode::Compact, store::JsonPrintMode::Pretty,
                            store::JsonPrintMode::PrettyCompact})
    {
        store::StoreOptions options;
        options.jsonMode = mode;
        store::StoreJson serializer;
        const auto json = serializer.storeToString(design, options);
        require(json.has_value(), "store failed");
        auto restored = grh::Design::fromJsonString(*json);
        auto& loaded = *restored.findGraph("top");
        require(loaded.valueLogicalType(loaded.findValue("renamed")) == type, "JSON lost domain/shape");
        require(loaded.metadata("test.stage") == graph.metadata("test.stage"), "JSON lost graph metadata");
        require(restored.instanceId() != design.instanceId(), "load inherited design identity");
        std::string invalid = *json;
        const auto at = invalid.find("2state");
        invalid.replace(at, 6, "8state");
        rejects([&] { grh::Design::fromJsonString(invalid); }, "unknown domain accepted");
    }
    const uint64_t oldIdentity = design.instanceId();
    design = clone.clone();
    require(design.instanceId() != oldIdentity && design.instanceId() != clone.instanceId(),
            "replacement reused identity");
}

void testSessionAndCompatibility()
{
    transform::SessionStore session;
    rejects([&] { transform::requireSessionValue<int>(nullptr, "x"); }, "null session accepted");
    rejects([&] { transform::requireSessionValue<int>(&session, "x"); }, "missing slot accepted");
    session["x"] = nullptr;
    rejects([&] { transform::requireSessionValue<int>(&session, "x"); }, "null slot accepted");
    session["x"] = std::make_unique<transform::SessionSlotValue<std::string>>(std::string("x"));
    rejects([&] { transform::requireSessionValue<int>(&session, "x"); }, "wrong slot type accepted");
    session["x"] = std::make_unique<transform::SessionSlotValue<int>>(7);
    require(transform::requireSessionValue<int>(&session, "x") == 7, "typed session read");

    grh::Design design;
    auto& graph = design.createGraph("top");
    graph.setMetadata("grhsim.stage", std::string("converted"));
    transform::PassManager manager;
    manager.addPass(std::make_unique<transform::SimplifyPass>());
    transform::PassDiagnostics diagnostics;
    require(!manager.run(design, diagnostics).success, "unadapted simplify accepted simulation form");
    require(verify(design), "public verifier rejected an empty simulation graph");
}

void testStateTypesAndInitialization()
{
    grh::Design design;
    auto& graph = design.createGraph("state_types");
    design.markAsTop("state_types");
    const grh::LogicalType type{grh::ValueType::Logic, 16, false, grh::LogicDomain::TwoState, {2}};
    const auto state = graph.createOperation(grh::OperationKind::kRegister, graph.internSymbol("q"));
    grh::setStateLogicalType(graph, state, type);
    grh::setStateInitSpec(graph, state, {{grh::InitKind::Fill, "8'h0"}});
    const auto read = graph.createOperation(grh::OperationKind::kRegisterReadPort);
    const auto value = graph.createValue(type);
    graph.setAttr(read, "regSymbol", std::string("q"));
    graph.addResult(read, value);
    graph.bindOutputPort("q", value);
    require(verify(design), "valid typed state rejected");
    auto wrong = type;
    wrong.domain = grh::LogicDomain::FourState;
    graph.setValueLogicalType(value, wrong);
    require(!verify(design), "read domain mismatch accepted");
    graph.setValueLogicalType(value, type);
    graph.eraseAttr(state, "initSpecValues");
    require(!verify(design), "incomplete typed initializer accepted");
    grh::setStateInitSpec(graph, state, {{grh::InitKind::Fill, "8'h0"}});
    graph.setMetadata("grhsim.stage", std::string("converted"));
    require(verify(design), "canonical converted state rejected");
    graph.eraseAttr(state, "domain");
    require(!verify(design), "converted state with implicit domain accepted");

    for (const auto kind : {grh::ValueType::Real, grh::ValueType::String})
    {
        const auto declaration = graph.createOperation(grh::OperationKind::kRegister);
        grh::setStateLogicalType(graph, declaration, {kind, 1, false, grh::LogicDomain::FourState, {}});
        const auto init = grh::stateInitSpec(graph, declaration);
        require(init.size() == 1 && init[0].kind == grh::InitKind::Constant &&
                init[0].value == (kind == grh::ValueType::Real ? "0" : ""),
                "non-logic state inherited an unknown-bit default");
    }
}

void testHierarchyTypeBoundaries()
{
    for (const bool flatten : {false, true})
    for (const bool shapedParent : {false, true})
    for (const bool wrongDomain : {false, true})
    {
        grh::Design design;
        auto& child = design.createGraph("child");
        auto& top = design.createGraph("top");
        design.markAsTop("top");
        const grh::LogicalType childType{grh::ValueType::Logic, 16, true, grh::LogicDomain::TwoState, {2}};
        auto parentType = childType;
        parentType.isSigned = false;
        parentType.shape = shapedParent ? std::vector<int64_t>{4} : std::vector<int64_t>{};
        auto inputType = parentType;
        if (wrongDomain) inputType.domain = grh::LogicDomain::FourState;
        const auto input = child.createValue(child.internSymbol("i"), childType);
        const auto output = child.createValue(child.internSymbol("o"), childType);
        child.bindInputPort("i", input);
        child.bindOutputPort("o", output);
        const auto invert = child.createOperation(grh::OperationKind::kNot);
        child.addOperand(invert, input);
        child.addResult(invert, output);
        const auto source = top.createValue(top.internSymbol("source"), inputType);
        const auto result = top.createValue(top.internSymbol("result"), parentType);
        top.bindInputPort("source", source);
        top.bindOutputPort("result", result);
        const auto instance = top.createOperation(grh::OperationKind::kInstance);
        top.addOperand(instance, source);
        top.addResult(instance, result);
        top.setAttr(instance, "moduleName", std::string("child"));
        top.setAttr(instance, "instanceName", std::string("u"));
        top.setAttr(instance, "inputPortName", std::vector<std::string>{"i"});
        top.setAttr(instance, "outputPortName", std::vector<std::string>{"o"});
        transform::PassManager manager;
        if (flatten) manager.addPass(std::make_unique<transform::HierFlattenPass>());
        else manager.addPass(std::make_unique<transform::InstanceInlinePass>(transform::InstanceInlineOptions{"top.u"}));
        manager.addPass(std::make_unique<transform::GrhVerifyPass>());
        transform::PassDiagnostics diagnostics;
        const auto status = manager.run(design, diagnostics);
        require(status.success != wrongDomain, "hierarchy boundary accepted implicit domain conversion or rejected legal retyping");
        if (wrongDomain) continue;
        require(top.valueLogicalType(source) == inputType && top.valueLogicalType(result) == parentType,
                "hierarchy expansion mutated an existing parent value's type");
        bool found = false;
        for (const auto op : top.operations())
            if (top.opKind(op) == grh::OperationKind::kNot)
            {
                found = true;
                require(top.valueLogicalType(top.opOperands(op)[0]) == childType &&
                        top.valueLogicalType(top.opResults(op)[0]) == childType,
                        "hierarchy expansion dropped the child's complete type");
            }
        require(found && top.opKind(top.valueDef(result)) == grh::OperationKind::kAssign,
                "hierarchy output conversion was not represented by equal-width assign");
    }
}
}

int main()
{
    try
    {
        testTypesAndAssign();
        testLifecycleAndJson();
        testSessionAndCompatibility();
        testStateTypesAndInitialization();
        testHierarchyTypeBoundaries();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
