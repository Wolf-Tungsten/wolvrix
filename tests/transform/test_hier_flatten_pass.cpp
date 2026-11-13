#include "core/grh.hpp"
#include "core/semantic_tree.hpp"
#include "core/store.hpp"
#include "core/transform.hpp"
#include "ingest/node_attributes.hpp"
#include "transform/hier_flatten.hpp"
#include "transform/xmr_resolve.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace wolvrix::lib::transform;

namespace
{

int fail(const std::string &message)
{
    std::cerr << "[transform-hier-flatten] " << message << '\n';
    return 1;
}

std::optional<std::string> getAttrString(const wolvrix::lib::grh::Operation &op,
                                         std::string_view key)
{
    auto attr = op.attr(key);
    if (!attr)
    {
        return std::nullopt;
    }
    if (const auto *value = std::get_if<std::string>(&*attr))
    {
        return *value;
    }
    return std::nullopt;
}

bool expectDeclaredValue(const wolvrix::lib::grh::Graph &graph, std::string_view name)
{
    const wolvrix::lib::grh::SymbolId sym = graph.lookupSymbol(name);
    return sym.valid() && graph.findValue(sym).valid() && graph.isDeclaredSymbol(sym);
}

bool expectDeclaredOp(const wolvrix::lib::grh::Graph &graph, std::string_view name)
{
    const wolvrix::lib::grh::SymbolId sym = graph.lookupSymbol(name);
    return sym.valid() && graph.findOperation(sym).valid() && graph.isDeclaredSymbol(sym);
}

const wolvrix::lib::grh::Graph::GenerateGroup *
findGenerateGroup(const wolvrix::lib::grh::Graph &graph, std::string_view scope,
                  std::string_view name)
{
    for (const auto &group : graph.generateGroups())
    {
        if (graph.symbolText(group.scope) == scope && graph.symbolText(group.name) == name)
        {
            return &group;
        }
    }
    return nullptr;
}

bool generateGroupMembersMatch(const wolvrix::lib::grh::Graph &graph,
                               const wolvrix::lib::grh::Graph::GenerateGroup &group,
                               std::initializer_list<std::string_view> expected)
{
    if (group.symbols.size() != expected.size())
    {
        return false;
    }
    std::size_t index = 0;
    for (const wolvrix::lib::grh::SymbolId sym : group.symbols)
    {
        if (graph.symbolText(sym) != *(expected.begin() + index))
        {
            return false;
        }
        ++index;
    }
    return true;
}

std::vector<std::string> generateGroupTexts(const wolvrix::lib::grh::Graph &graph)
{
    std::vector<std::string> out;
    for (const auto &group : graph.generateGroups())
    {
        std::string text = std::string(graph.symbolText(group.scope));
        text.push_back('|');
        text.append(graph.symbolText(group.name));
        for (const wolvrix::lib::grh::SymbolId sym : group.symbols)
        {
            text.push_back('|');
            text.append(graph.symbolText(sym));
        }
        out.push_back(std::move(text));
    }
    return out;
}

// leaf: declared wire w (in -> w -> out) plus a declared array register op.
// mid: instantiates leaf as u_leaf with declared wires mw/mo around it.
// top: instantiates mid twice (u_m1/u_m2) and pre-claims "u_m1$u_leaf$w"
// (8-bit) to force `_N` dedup against leaf's 1-bit w during flatten.
void buildNestedDesign(wolvrix::lib::grh::Design &design)
{
    wolvrix::lib::grh::Graph &leaf = design.createGraph("leaf");
    wolvrix::lib::grh::Graph &mid = design.createGraph("mid");
    wolvrix::lib::grh::Graph &top = design.createGraph("top");
    design.markAsTop("top");

    const auto leafIn = leaf.createValue(leaf.internSymbol("in"), 1, false);
    const auto leafOut = leaf.createValue(leaf.internSymbol("out"), 1, false);
    const auto leafW = leaf.createValue(leaf.internSymbol("w"), 1, false);
    leaf.bindInputPort("in", leafIn);
    leaf.bindOutputPort("out", leafOut);
    leaf.addDeclaredSymbol(leaf.getValue(leafIn).symbol());
    leaf.addDeclaredSymbol(leaf.getValue(leafOut).symbol());
    leaf.addDeclaredSymbol(leaf.getValue(leafW).symbol());
    const auto leafMemSym = leaf.internSymbol("mem");
    leaf.addDeclaredSymbol(leafMemSym);
    const auto leafMem =
        leaf.createOperation(wolvrix::lib::grh::OperationKind::kRegister, leafMemSym);
    leaf.setAttr(leafMem, "width", static_cast<int64_t>(128));
    leaf.setAttr(leafMem, "isSigned", false);
    leaf.setAttr(leafMem, "shape", std::vector<int64_t>{16});
    const auto leafDriveW = leaf.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                 leaf.makeInternalOpSym());
    leaf.addOperand(leafDriveW, leafIn);
    leaf.addResult(leafDriveW, leafW);
    const auto leafDriveOut = leaf.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                   leaf.makeInternalOpSym());
    leaf.addOperand(leafDriveOut, leafW);
    leaf.addResult(leafDriveOut, leafOut);

    const auto midIn = mid.createValue(mid.internSymbol("min"), 1, false);
    const auto midOut = mid.createValue(mid.internSymbol("mout"), 1, false);
    const auto midMw = mid.createValue(mid.internSymbol("mw"), 1, false);
    const auto midMo = mid.createValue(mid.internSymbol("mo"), 1, false);
    mid.bindInputPort("min", midIn);
    mid.bindOutputPort("mout", midOut);
    for (const auto value : {midIn, midOut, midMw, midMo})
    {
        mid.addDeclaredSymbol(mid.getValue(value).symbol());
    }
    const auto midDriveMw = mid.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                mid.makeInternalOpSym());
    mid.addOperand(midDriveMw, midIn);
    mid.addResult(midDriveMw, midMw);
    const auto midDriveOut = mid.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                 mid.makeInternalOpSym());
    mid.addOperand(midDriveOut, midMo);
    mid.addResult(midDriveOut, midOut);
    const auto midInst = mid.createOperation(wolvrix::lib::grh::OperationKind::kInstance,
                                             mid.internSymbol("_op_leaf"));
    mid.addOperand(midInst, midMw);
    mid.addResult(midInst, midMo);
    mid.setAttr(midInst, "moduleName", std::string("leaf"));
    mid.setAttr(midInst, "instanceName", std::string("u_leaf"));

    const auto topIn = top.createValue(top.internSymbol("tin"), 1, false);
    const auto topOut1 = top.createValue(top.internSymbol("tout1"), 1, false);
    const auto topOut2 = top.createValue(top.internSymbol("tout2"), 1, false);
    const auto topT = top.createValue(top.internSymbol("t"), 1, false);
    const auto topConflict = top.createValue(top.internSymbol("u_m1$u_leaf$w"), 8, false);
    top.bindInputPort("tin", topIn);
    top.bindOutputPort("tout1", topOut1);
    top.bindOutputPort("tout2", topOut2);
    for (const auto value : {topIn, topOut1, topOut2, topT, topConflict})
    {
        top.addDeclaredSymbol(top.getValue(value).symbol());
    }
    const auto topDriveT = top.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                               top.makeInternalOpSym());
    top.addOperand(topDriveT, topIn);
    top.addResult(topDriveT, topT);
    const auto instM1 = top.createOperation(wolvrix::lib::grh::OperationKind::kInstance,
                                            top.internSymbol("_op_m1"));
    top.addOperand(instM1, topIn);
    top.addResult(instM1, topOut1);
    top.setAttr(instM1, "moduleName", std::string("mid"));
    top.setAttr(instM1, "instanceName", std::string("u_m1"));
    const auto instM2 = top.createOperation(wolvrix::lib::grh::OperationKind::kInstance,
                                            top.internSymbol("_op_m2"));
    top.addOperand(instM2, topIn);
    top.addResult(instM2, topOut2);
    top.setAttr(instM2, "moduleName", std::string("mid"));
    top.setAttr(instM2, "instanceName", std::string("u_m2"));
}

int testNestedFlattenDeclared()
{
    wolvrix::lib::grh::Design design;
    buildNestedDesign(design);

    PassManager manager;
    manager.addPass(std::make_unique<HierFlattenPass>());
    PassDiagnostics diags;
    const PassManagerResult result = manager.run(design, diags);
    if (!result.success || diags.hasError())
    {
        return fail("nested design hier-flatten failed");
    }
    if (design.graphs().size() != 1)
    {
        return fail("nested flatten: expected a single graph");
    }
    wolvrix::lib::grh::Graph &top = *design.findGraph("top");
    for (const auto opId : top.operations())
    {
        if (top.getOperation(opId).kind() == wolvrix::lib::grh::OperationKind::kInstance)
        {
            return fail("nested flatten: kInstance left behind");
        }
    }

    // Top-graph symbols keep their original names.
    for (const std::string_view name : {"tin", "tout1", "tout2", "t", "u_m1$u_leaf$w"})
    {
        if (!expectDeclaredValue(top, name))
        {
            return fail("nested flatten: top symbol not preserved: " + std::string(name));
        }
    }
    if (top.getValue(top.findValue("u_m1$u_leaf$w")).width() != 8)
    {
        return fail("nested flatten: pre-claimed top value was clobbered");
    }

    // Inlined declared symbols use `$`-joined hierarchical paths and resolve.
    for (const std::string_view name : {"u_m1$mw", "u_m1$mo", "u_m2$mw", "u_m2$mo",
                                        "u_m1$u_leaf$w_1", "u_m2$u_leaf$w"})
    {
        if (!expectDeclaredValue(top, name))
        {
            return fail("nested flatten: missing declared value: " + std::string(name));
        }
    }
    if (top.getValue(top.findValue("u_m1$u_leaf$w_1")).width() != 1)
    {
        return fail("nested flatten: `_N` dedup target has wrong width");
    }
    for (const std::string_view name : {"u_m1$u_leaf$mem", "u_m2$u_leaf$mem"})
    {
        if (!expectDeclaredOp(top, name))
        {
            return fail("nested flatten: missing declared op: " + std::string(name));
        }
    }
    // Child port symbols are superseded by the parent-side names (D1).
    if (top.findValue("u_m1$min").valid() || top.findValue("u_m1$u_leaf$in").valid())
    {
        return fail("nested flatten: child port symbol unexpectedly present");
    }
    return 0;
}

// child: declared wire cw (ci -> cw -> co) and a declared array register op.
// top: instance u_c driven through an undeclared link value so the
// All/Hierarchy parent-side port rename stays observable.
void buildPortRenameDesign(wolvrix::lib::grh::Design &design)
{
    wolvrix::lib::grh::Graph &child = design.createGraph("child");
    wolvrix::lib::grh::Graph &top = design.createGraph("top");
    design.markAsTop("top");

    const auto childIn = child.createValue(child.internSymbol("ci"), 1, false);
    const auto childOut = child.createValue(child.internSymbol("co"), 1, false);
    const auto childW = child.createValue(child.internSymbol("cw"), 1, false);
    child.bindInputPort("ci", childIn);
    child.bindOutputPort("co", childOut);
    child.addDeclaredSymbol(child.getValue(childIn).symbol());
    child.addDeclaredSymbol(child.getValue(childOut).symbol());
    child.addDeclaredSymbol(child.getValue(childW).symbol());
    const auto memSym = child.internSymbol("mem");
    child.addDeclaredSymbol(memSym);
    const auto mem =
        child.createOperation(wolvrix::lib::grh::OperationKind::kRegister, memSym);
    child.setAttr(mem, "width", static_cast<int64_t>(128));
    child.setAttr(mem, "isSigned", false);
    child.setAttr(mem, "shape", std::vector<int64_t>{16});
    const auto driveW = child.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                              child.makeInternalOpSym());
    child.addOperand(driveW, childIn);
    child.addResult(driveW, childW);
    const auto driveOut = child.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                child.makeInternalOpSym());
    child.addOperand(driveOut, childW);
    child.addResult(driveOut, childOut);

    const auto topIn = top.createValue(top.internSymbol("pi"), 1, false);
    const auto topOut = top.createValue(top.internSymbol("po"), 1, false);
    const auto link = top.createValue(top.makeInternalValSym(), 1, false);
    top.bindInputPort("pi", topIn);
    top.bindOutputPort("po", topOut);
    top.addDeclaredSymbol(top.getValue(topIn).symbol());
    top.addDeclaredSymbol(top.getValue(topOut).symbol());
    const auto driveLink = top.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                               top.makeInternalOpSym());
    top.addOperand(driveLink, topIn);
    top.addResult(driveLink, link);
    const auto inst = top.createOperation(wolvrix::lib::grh::OperationKind::kInstance,
                                          top.internSymbol("_op_child"));
    top.addOperand(inst, link);
    top.addResult(inst, topOut);
    top.setAttr(inst, "moduleName", std::string("child"));
    top.setAttr(inst, "instanceName", std::string("u_c"));
}

int runSymProtectScenario(HierFlattenOptions::SymProtectMode mode, bool expectPortRename,
                          const char *label)
{
    wolvrix::lib::grh::Design design;
    buildPortRenameDesign(design);

    HierFlattenOptions options;
    options.symProtect = mode;
    PassManager manager;
    manager.addPass(std::make_unique<HierFlattenPass>(options));
    PassDiagnostics diags;
    const PassManagerResult result = manager.run(design, diags);
    if (!result.success || diags.hasError())
    {
        return fail(std::string(label) + ": hier-flatten failed");
    }
    wolvrix::lib::grh::Graph &top = *design.findGraph("top");
    // Declared child symbols survive with hierarchical names in every mode.
    if (!expectDeclaredValue(top, "u_c$cw"))
    {
        return fail(std::string(label) + ": declared child wire not preserved");
    }
    if (!expectDeclaredOp(top, "u_c$mem"))
    {
        return fail(std::string(label) + ": declared child memory not preserved");
    }
    if (!expectDeclaredValue(top, "pi") || !expectDeclaredValue(top, "po"))
    {
        return fail(std::string(label) + ": top symbols renamed unexpectedly");
    }
    if (expectDeclaredValue(top, "u_c$ci") != expectPortRename)
    {
        return fail(std::string(label) + ": parent-side port-rename behavior mismatch");
    }
    return 0;
}

int testSymProtectModesKeepDeclared()
{
    if (const int rc =
            runSymProtectScenario(HierFlattenOptions::SymProtectMode::All, true, "all"))
    {
        return rc;
    }
    if (const int rc = runSymProtectScenario(HierFlattenOptions::SymProtectMode::Hierarchy,
                                             true, "hierarchy"))
    {
        return rc;
    }
    if (const int rc = runSymProtectScenario(HierFlattenOptions::SymProtectMode::Stateful,
                                             false, "stateful"))
    {
        return rc;
    }
    if (const int rc =
            runSymProtectScenario(HierFlattenOptions::SymProtectMode::None, false, "none"))
    {
        return rc;
    }
    return 0;
}

// gen_mod: generate-style declared copies with group annotation:
//  - (gen_loop, sig): two 8-bit wire copies;
//  - (gen_loop, konst): two kConstant op copies (op-symbol members);
//  - (outer$inner, x): nested-scope copies;
//  - (gen_loop, portmix): wire copy plus the `gi` port symbol — the port
//    member is dropped at flatten because the parent-side name wins;
//  - (gen_if, only): only the `go` port symbol — the group turns empty at
//    flatten and must not propagate.
// top instantiates gen_mod twice (u_g1/u_g2).
void buildGenerateGroupDesign(wolvrix::lib::grh::Design &design)
{
    wolvrix::lib::grh::Graph &mod = design.createGraph("gen_mod");
    wolvrix::lib::grh::Graph &top = design.createGraph("top");
    design.markAsTop("top");

    const auto gi = mod.createValue(mod.internSymbol("gi"), 1, false);
    const auto go = mod.createValue(mod.internSymbol("go"), 1, false);
    mod.bindInputPort("gi", gi);
    mod.bindOutputPort("go", go);
    mod.addDeclaredSymbol(mod.getValue(gi).symbol());
    mod.addDeclaredSymbol(mod.getValue(go).symbol());

    const auto sig0 = mod.createValue(mod.internSymbol("gen_loop$0$sig"), 8, false);
    const auto sig1 = mod.createValue(mod.internSymbol("gen_loop$1$sig"), 8, false);
    mod.addDeclaredSymbol(mod.getValue(sig0).symbol());
    mod.addDeclaredSymbol(mod.getValue(sig1).symbol());
    const auto sigGroup =
        mod.addGenerateGroup(mod.internSymbol("gen_loop"), mod.internSymbol("sig"));
    mod.addGenerateGroupSymbol(sigGroup, mod.getValue(sig0).symbol());
    mod.addGenerateGroupSymbol(sigGroup, mod.getValue(sig1).symbol());

    const auto konst0 = mod.createOperation(wolvrix::lib::grh::OperationKind::kConstant,
                                            mod.internSymbol("gen_loop$0$konst"));
    mod.setAttr(konst0, "constValue", wolvrix::lib::grh::AttributeValue(std::string("8'h0")));
    const auto konst1 = mod.createOperation(wolvrix::lib::grh::OperationKind::kConstant,
                                            mod.internSymbol("gen_loop$1$konst"));
    mod.setAttr(konst1, "constValue", wolvrix::lib::grh::AttributeValue(std::string("8'h1")));
    mod.addDeclaredSymbol(mod.getOperation(konst0).symbol());
    mod.addDeclaredSymbol(mod.getOperation(konst1).symbol());
    const auto konstGroup =
        mod.addGenerateGroup(mod.internSymbol("gen_loop"), mod.internSymbol("konst"));
    mod.addGenerateGroupSymbol(konstGroup, mod.getOperation(konst0).symbol());
    mod.addGenerateGroupSymbol(konstGroup, mod.getOperation(konst1).symbol());

    const auto x0 = mod.createValue(mod.internSymbol("outer$0$inner$0$x"), 4, false);
    const auto x1 = mod.createValue(mod.internSymbol("outer$1$inner$0$x"), 4, false);
    mod.addDeclaredSymbol(mod.getValue(x0).symbol());
    mod.addDeclaredSymbol(mod.getValue(x1).symbol());
    const auto xGroup =
        mod.addGenerateGroup(mod.internSymbol("outer$inner"), mod.internSymbol("x"));
    mod.addGenerateGroupSymbol(xGroup, mod.getValue(x0).symbol());
    mod.addGenerateGroupSymbol(xGroup, mod.getValue(x1).symbol());

    const auto pw = mod.createValue(mod.internSymbol("gen_loop$0$pw"), 1, false);
    mod.addDeclaredSymbol(mod.getValue(pw).symbol());
    const auto drivePw = mod.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                             mod.makeInternalOpSym());
    mod.addOperand(drivePw, gi);
    mod.addResult(drivePw, pw);
    const auto portmixGroup =
        mod.addGenerateGroup(mod.internSymbol("gen_loop"), mod.internSymbol("portmix"));
    mod.addGenerateGroupSymbol(portmixGroup, mod.getValue(pw).symbol());
    mod.addGenerateGroupSymbol(portmixGroup, mod.getValue(gi).symbol());

    const auto onlyGroup =
        mod.addGenerateGroup(mod.internSymbol("gen_if"), mod.internSymbol("only"));
    mod.addGenerateGroupSymbol(onlyGroup, mod.getValue(go).symbol());

    const auto topIn = top.createValue(top.internSymbol("tin"), 1, false);
    const auto topOut1 = top.createValue(top.internSymbol("tout1"), 1, false);
    const auto topOut2 = top.createValue(top.internSymbol("tout2"), 1, false);
    top.bindInputPort("tin", topIn);
    top.bindOutputPort("tout1", topOut1);
    top.bindOutputPort("tout2", topOut2);
    top.addDeclaredSymbol(top.getValue(topIn).symbol());
    top.addDeclaredSymbol(top.getValue(topOut1).symbol());
    top.addDeclaredSymbol(top.getValue(topOut2).symbol());
    const auto instG1 = top.createOperation(wolvrix::lib::grh::OperationKind::kInstance,
                                            top.internSymbol("_op_g1"));
    top.addOperand(instG1, topIn);
    top.addResult(instG1, topOut1);
    top.setAttr(instG1, "moduleName", std::string("gen_mod"));
    top.setAttr(instG1, "instanceName", std::string("u_g1"));
    const auto instG2 = top.createOperation(wolvrix::lib::grh::OperationKind::kInstance,
                                            top.internSymbol("_op_g2"));
    top.addOperand(instG2, topIn);
    top.addResult(instG2, topOut2);
    top.setAttr(instG2, "moduleName", std::string("gen_mod"));
    top.setAttr(instG2, "instanceName", std::string("u_g2"));
}

int testGenerateGroupsFlatten()
{
    wolvrix::lib::grh::Design design;
    buildGenerateGroupDesign(design);

    PassManager manager;
    manager.addPass(std::make_unique<HierFlattenPass>());
    PassDiagnostics diags;
    const PassManagerResult result = manager.run(design, diags);
    if (!result.success || diags.hasError())
    {
        return fail("generate-group hier-flatten failed");
    }
    wolvrix::lib::grh::Graph &top = *design.findGraph("top");

    // Two instances times four surviving groups; the port-only group is dropped.
    if (top.generateGroups().size() != 8)
    {
        return fail("generate-group flatten: unexpected group count");
    }
    for (const std::string_view inst : {"u_g1", "u_g2"})
    {
        const std::string prefix(inst);
        const auto *sigGroup = findGenerateGroup(top, prefix + "$gen_loop", "sig");
        if (!sigGroup ||
            !generateGroupMembersMatch(top, *sigGroup,
                                       {prefix + "$gen_loop$0$sig", prefix + "$gen_loop$1$sig"}))
        {
            return fail("generate-group flatten: sig group mismatch for " + prefix);
        }
        const auto *konstGroup = findGenerateGroup(top, prefix + "$gen_loop", "konst");
        if (!konstGroup ||
            !generateGroupMembersMatch(top, *konstGroup,
                                       {prefix + "$gen_loop$0$konst",
                                        prefix + "$gen_loop$1$konst"}))
        {
            return fail("generate-group flatten: konst group mismatch for " + prefix);
        }
        const auto *xGroup = findGenerateGroup(top, prefix + "$outer$inner", "x");
        if (!xGroup ||
            !generateGroupMembersMatch(top, *xGroup,
                                       {prefix + "$outer$0$inner$0$x",
                                        prefix + "$outer$1$inner$0$x"}))
        {
            return fail("generate-group flatten: nested-scope group mismatch for " + prefix);
        }
        const auto *portmixGroup = findGenerateGroup(top, prefix + "$gen_loop", "portmix");
        if (!portmixGroup ||
            !generateGroupMembersMatch(top, *portmixGroup, {prefix + "$gen_loop$0$pw"}))
        {
            return fail("generate-group flatten: dropped port member mismatch for " + prefix);
        }
        if (findGenerateGroup(top, prefix + "$gen_if", "only") != nullptr)
        {
            return fail("generate-group flatten: emptied group propagated for " + prefix);
        }
        if (!expectDeclaredValue(top, prefix + "$gen_loop$0$sig") ||
            !expectDeclaredValue(top, prefix + "$gen_loop$1$sig") ||
            !expectDeclaredValue(top, prefix + "$outer$0$inner$0$x") ||
            !expectDeclaredValue(top, prefix + "$outer$1$inner$0$x") ||
            !expectDeclaredValue(top, prefix + "$gen_loop$0$pw"))
        {
            return fail("generate-group flatten: value member not resolvable for " + prefix);
        }
        if (!expectDeclaredOp(top, prefix + "$gen_loop$0$konst") ||
            !expectDeclaredOp(top, prefix + "$gen_loop$1$konst"))
        {
            return fail("generate-group flatten: op member not resolvable for " + prefix);
        }
    }

    // JSON round-trip preserves the flattened group shapes.
    const std::vector<std::string> before = generateGroupTexts(top);
    wolvrix::lib::store::StoreDiagnostics storeDiags;
    wolvrix::lib::store::StoreJson emitter(&storeDiags);
    wolvrix::lib::store::StoreOptions storeOptions;
    auto jsonOpt = emitter.storeToString(design, storeOptions);
    if (!jsonOpt || storeDiags.hasError())
    {
        return fail("generate-group flatten: JSON store failed");
    }
    try
    {
        wolvrix::lib::grh::Design reparsed = wolvrix::lib::grh::Design::fromJsonString(*jsonOpt);
        const wolvrix::lib::grh::Graph *rtTop = reparsed.findGraph("top");
        if (!rtTop)
        {
            return fail("generate-group flatten: round-trip missing top graph");
        }
        if (generateGroupTexts(*rtTop) != before)
        {
            return fail("generate-group flatten: groups differ after JSON round-trip");
        }
    }
    catch (const std::exception &ex)
    {
        return fail(std::string("generate-group flatten: JSON reload failed: ") + ex.what());
    }
    return 0;
}

int testSharedScalarIndexShape()
{
    wolvrix::lib::grh::Design design;
    auto &child = design.createGraph("child_shape");
    auto &top = design.createGraph("top_shape");
    design.markAsTop("top_shape");

    const std::array<int64_t, 1> one{1};
    const auto childArray = child.createValue(child.internSymbol("array"), 1, false,
                                               wolvrix::lib::grh::ValueType::Logic, one);
    const auto childIndex = child.createValue(child.internSymbol("index"), 1, false);
    const auto childResult = child.createValue(child.internSymbol("result"), 1, false);
    child.bindInputPort("array", childArray);
    child.bindInputPort("index", childIndex);
    child.bindOutputPort("result", childResult);
    const auto slice = child.createOperation(wolvrix::lib::grh::OperationKind::kSliceArray,
                                              child.makeInternalOpSym());
    child.addOperand(slice, childArray);
    child.addOperand(slice, childIndex);
    child.addResult(slice, childResult);

    const auto zero = top.createValue(top.internSymbol("zero"), 1, false);
    const auto zeroOp = top.createOperation(wolvrix::lib::grh::OperationKind::kConstant,
                                             top.makeInternalOpSym());
    top.addResult(zeroOp, zero);
    top.setAttr(zeroOp, "constValue", std::string("1'b0"));
    const auto mem = top.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                         top.internSymbol("mem"));
    top.setAttr(mem, "width", int64_t{2});
    top.setAttr(mem, "isSigned", false);
    top.setAttr(mem, "shape", std::vector<int64_t>{2});
    const auto readValue = top.createValue(top.internSymbol("read"), 1, false);
    const auto read = top.createOperation(wolvrix::lib::grh::OperationKind::kRegisterReadArrayPort,
                                          top.makeInternalOpSym());
    top.addOperand(read, zero);
    top.addResult(read, readValue);
    top.setAttr(read, "regSymbol", std::string("mem"));
    top.bindOutputPort("read", readValue);

    const auto childOutput = top.createValue(top.internSymbol("child_output"), 1, false);
    const auto instance = top.createOperation(wolvrix::lib::grh::OperationKind::kInstance,
                                              top.makeInternalOpSym());
    top.addOperand(instance, zero);
    top.addOperand(instance, zero);
    top.addResult(instance, childOutput);
    top.setAttr(instance, "moduleName", std::string("child_shape"));
    top.setAttr(instance, "instanceName", std::string("u_child"));
    top.bindOutputPort("child_output", childOutput);

    PassManager manager;
    manager.addPass(std::make_unique<HierFlattenPass>());
    PassDiagnostics diagnostics;
    const PassManagerResult result = manager.run(design, diagnostics);
    if (!result.success || diagnostics.hasError() || !result.changed)
        return fail("shared scalar index flatten failed");
    if (!top.valueShape(zero).empty() || top.opOperands(read)[0] != zero)
        return fail("flatten changed the shared scalar array index shape");
    bool foundShapedView = false;
    for (const auto op : top.operations())
    {
        if (top.opKind(op) != wolvrix::lib::grh::OperationKind::kSliceArray)
            continue;
        const auto base = top.opOperands(op)[0];
        const auto baseDef = top.valueDef(base);
        foundShapedView = base != zero && top.valueShape(base).size() == 1 &&
                          top.valueShape(base)[0] == 1 && baseDef.valid() &&
                          top.opKind(baseDef) == wolvrix::lib::grh::OperationKind::kAssign &&
                          top.opOperands(baseDef)[0] == zero &&
                          top.valueShape(top.opOperands(op)[1]).empty();
    }
    return foundShapedView ? 0 : fail("flatten did not isolate the child array view");
}

} // namespace

int testDpiImportOriginMerge()
{
    // Two child graphs declare the same DPI import (shared declaration file,
    // XiangShan assert.v pattern) and the top graph has its own copy. The
    // flatten-time merge compares only the signature and does not reintroduce
    // the retired originModules annotation.
    wolvrix::lib::grh::Design design;
    wolvrix::lib::grh::Graph &childA = design.createGraph("childA");
    wolvrix::lib::grh::Graph &childB = design.createGraph("childB");
    wolvrix::lib::grh::Graph &top = design.createGraph("top");
    design.markAsTop("top");

    auto setupChild = [](wolvrix::lib::grh::Graph &child) {
        const auto sig = child.createValue(child.internSymbol("x"), 1, false);
        child.bindInputPort("x", sig);
        child.addDeclaredSymbol(child.getValue(sig).symbol());
        const auto dpiSym = child.internSymbol("xs_assert_v2");
        const auto dpiImport =
            child.createOperation(wolvrix::lib::grh::OperationKind::kDpicImport, dpiSym);
        child.setAttr(dpiImport, "argsDirection",
                      std::vector<std::string>{"input", "input"});
        child.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{1, 64});
        child.setAttr(dpiImport, "argsName",
                      std::vector<std::string>{"filename", "line"});
        child.setAttr(dpiImport, "argsSigned", std::vector<bool>{false, true});
        child.setAttr(dpiImport, "argsType",
                      std::vector<std::string>{"string", "longint"});
        child.setAttr(dpiImport, "hasReturn", false);
    };
    setupChild(childA);
    setupChild(childB);

    const auto topX = top.createValue(top.internSymbol("x"), 1, false);
    top.bindInputPort("x", topX);
    top.addDeclaredSymbol(top.getValue(topX).symbol());
    const auto topDpiSym = top.internSymbol("xs_assert_v2");
    const auto topDpi =
        top.createOperation(wolvrix::lib::grh::OperationKind::kDpicImport, topDpiSym);
    top.setAttr(topDpi, "argsDirection", std::vector<std::string>{"input", "input"});
    top.setAttr(topDpi, "argsWidth", std::vector<int64_t>{1, 64});
    top.setAttr(topDpi, "argsName", std::vector<std::string>{"filename", "line"});
    top.setAttr(topDpi, "argsSigned", std::vector<bool>{false, true});
    top.setAttr(topDpi, "argsType", std::vector<std::string>{"string", "longint"});
    top.setAttr(topDpi, "hasReturn", false);

    for (const char *module : {"childA", "childB"})
    {
        const auto instOp = top.createOperation(wolvrix::lib::grh::OperationKind::kInstance,
                                                top.makeInternalOpSym());
        top.addOperand(instOp, topX);
        top.setAttr(instOp, "moduleName", std::string(module));
        top.setAttr(instOp, "instanceName", std::string("u_") + module);
        top.setAttr(instOp, "inputPortName", std::vector<std::string>{"x"});
        top.setAttr(instOp, "outputPortName", std::vector<std::string>{});
    }

    PassManager manager;
    manager.addPass(std::make_unique<HierFlattenPass>());
    PassDiagnostics diags;
    const PassManagerResult result = manager.run(design, diags);
    if (!result.success || diags.hasError())
    {
        return fail("hier-flatten failed on duplicate DPI imports with origin attrs");
    }

    wolvrix::lib::grh::OperationId found = wolvrix::lib::grh::OperationId::invalid();
    unsigned count = 0;
    for (const auto opId : top.operations())
    {
        const wolvrix::lib::grh::Operation op = top.getOperation(opId);
        if (op.kind() == wolvrix::lib::grh::OperationKind::kDpicImport &&
            op.symbolText() == std::string_view("xs_assert_v2"))
        {
            found = opId;
            ++count;
        }
    }
    if (count != 1 || !found.valid())
    {
        return fail("Duplicate DPI imports must merge into a single op");
    }
    const auto origins = getOriginModules(top, found);
    if (origins && !origins->empty())
        return fail("Merged DPI import reintroduced retired originModules");
    return 0;
}

// Reverse port order and dead slots expose both positional-map changes and
// stale IDs when definitions are compacted before inlining shared instances.
void buildSourceFreezeDesign(wolvrix::lib::grh::Design &design, int namedInstances)
{
    namespace grh = wolvrix::lib::grh;
    namespace node = wolvrix::lib::ingest::node;
    auto &child = design.createGraph("child");
    auto &top = design.createGraph("top");
    design.markAsTop("top");

    const auto dead = child.createOperation(grh::OperationKind::kNot);
    const auto deadValue = child.createValue(8, false);
    child.addResult(dead, deadValue);
    child.eraseOp(dead);
    child.eraseValue(deadValue);

    const auto z = child.createValue(child.internSymbol("z"), 8, false);
    const auto a = child.createValue(child.internSymbol("a"), 8, false);
    const auto y = child.createValue(child.internSymbol("y"), 8, false);
    child.bindInputPort("z", z);
    child.bindInputPort("a", a);
    child.bindOutputPort("y", y);
    const auto difference = child.createOperation(grh::OperationKind::kSub, child.internSymbol("difference"));
    child.addDeclaredSymbol(child.operationSymbol(difference));
    child.addOperand(difference, z);
    child.addOperand(difference, a);
    child.addResult(difference, y);
    child.setAttr(difference, "fixture", std::string("source-freeze"));
    grh::SrcLoc location;
    location.file = "source-freeze.fir";
    location.line = 41;
    child.setOpSrcLoc(difference, location);
    location.line = 42;
    child.setValueSrcLoc(y, location);
    child.setMetadata("fixture.source", std::string("preserved"));
    const auto group = child.addGenerateGroup(child.internSymbol("gen"), child.lookupSymbol("difference"));
    child.addGenerateGroupSymbol(group, child.operationSymbol(difference));
    const auto identity = node::key({"source-freeze.fir", "Circuit", "child", {"local"}, "difference", {}});
    node::write(child, difference, {{identity}, {{identity, 0}}, {identity}});
    node::establishPolicy(child);

    const auto topZ = top.createValue(top.internSymbol("z"), 8, false);
    const auto topA = top.createValue(top.internSymbol("a"), 8, false);
    top.bindInputPort("z", topZ);
    top.bindInputPort("a", topA);
    top.addDeclaredSymbol(top.valueSymbol(topZ));
    top.addDeclaredSymbol(top.valueSymbol(topA));
    for (int i = 0; i < 2; ++i)
    {
        const auto output = top.createValue(top.internSymbol("y" + std::to_string(i)), 8, false);
        top.bindOutputPort("y" + std::to_string(i), output);
        top.addDeclaredSymbol(top.valueSymbol(output));
        const auto instance = top.createOperation(grh::OperationKind::kInstance,
                                                  top.internSymbol("instance" + std::to_string(i)));
        top.setAttr(instance, "moduleName", std::string("child"));
        top.setAttr(instance, "instanceName", "u" + std::to_string(i));
        top.addOperand(instance, topZ);
        top.addOperand(instance, topA);
        top.addResult(instance, output);
        if (i < namedInstances)
        {
            top.setAttr(instance, "inputPortName", std::vector<std::string>{"z", "a"});
            top.setAttr(instance, "outputPortName", std::vector<std::string>{"y"});
        }
    }
}

int checkSourceFreezeWiring(const wolvrix::lib::grh::Design &design)
{
    namespace node = wolvrix::lib::ingest::node;
    const auto &top = *design.findGraph("top");
    for (int i = 0; i < 2; ++i)
    {
        const auto op = top.findOperation("u" + std::to_string(i) + "$difference");
        if (!op.valid()) return fail("source freeze lost a shared instance operation");
        const auto operands = top.opOperands(op);
        if (operands.size() != 2 || operands[0] != top.findValue("z") || operands[1] != top.findValue("a") ||
            top.valueDef(top.findValue("y" + std::to_string(i))) != op)
            return fail("source freeze changed reverse-order instance wiring");
        if (getAttrString(top.getOperation(op), "fixture") != "source-freeze" ||
            !top.opSrcLoc(op) || top.opSrcLoc(op)->line != 41 ||
            !findGenerateGroup(top, "u" + std::to_string(i) + "$gen", "difference"))
            return fail("source freeze changed attributes, locations, or generate groups");
        const auto *group = findGenerateGroup(top, "u" + std::to_string(i) + "$gen", "difference");
        if (group->symbols.size() != 1 || group->symbols.front() != top.operationSymbol(op))
            return fail("source freeze generated group did not retain its instance's target operation symbol");
    }
    const auto inspection = node::inspect(top);
    if (!inspection.errors.empty() || !inspection.nodes.empty())
        return fail("source freeze reintroduced retired FIRRTL node attributes");
    return 0;
}

int testSourceFreezeEquivalence()
{
    std::optional<std::string> baseline;
    for (const int namedInstances : {0, 2})
    {
        wolvrix::lib::grh::Design design;
        buildSourceFreezeDesign(design, namedInstances);
        auto *const source = design.findGraph("child");
        auto *const top = design.findGraph("top");
        const auto oldIndex = source->findOperation("difference").index;
        const auto inventory = source->metadata(wolvrix::lib::ingest::node::kInventory);
        HierFlattenOptions options;
        options.preserveFlattenedModules = true;
        PassManager manager;
        manager.addPass(std::make_unique<HierFlattenPass>(options));
        PassDiagnostics diags;
        if (!manager.run(design, diags).success || diags.hasError())
            return fail("source freeze equivalence flatten failed");
        if (design.findGraph("top") != top || design.findGraph("child") != source ||
            source->frozen() || !source->metadata("firrtl.nodeRetention") ||
            source->metadata("fixture.source") != wolvrix::lib::grh::AttributeValue(std::string("preserved")) ||
            source->metadata(wolvrix::lib::ingest::node::kInventory) != inventory ||
            !findGenerateGroup(*source, "gen", "difference") ||
            !source->valueSrcLoc(source->findValue("y")) ||
            source->valueSrcLoc(source->findValue("y"))->line != 42)
            return fail("source freeze changed preserved graph identity or metadata");
        if (namedInstances == 2 && source->findOperation("difference").index >= oldIndex)
            return fail("named source definition was not compacted before flatten");
        if (const int result = checkSourceFreezeWiring(design)) return result;
        design.deleteGraph("child");
        wolvrix::lib::store::StoreDiagnostics storeDiags;
        wolvrix::lib::store::StoreJson store(&storeDiags);
        const auto json = store.storeToString(design);
        if (!json || storeDiags.hasError()) return fail("source freeze equivalence JSON store failed");
        if (baseline && *baseline != *json)
            return fail("frozen and positional source flatten results differ");
        baseline = json;
    }
    return 0;
}

int testSourceFreezeFallbackAndFailure()
{
    namespace grh = wolvrix::lib::grh;
    // A named peer cannot make a source safe when another reference uses the
    // original, non-lexical positional port order (including legacy bad types).
    for (const bool wrongType : {false, true})
    {
        grh::Design design;
        buildSourceFreezeDesign(design, 1);
        auto &top = *design.findGraph("top");
        if (wrongType) top.setAttr(top.findOperation("instance1"), "inputPortName", int64_t(7));
        HierFlattenOptions options;
        options.preserveFlattenedModules = true;
        PassManager manager;
        manager.addPass(std::make_unique<HierFlattenPass>(options));
        PassDiagnostics diags;
        if (!manager.run(design, diags).success || design.findGraph("child")->frozen())
            return fail("shared positional source was frozen or rejected");
        if (const int result = checkSourceFreezeWiring(design)) return result;
    }

    grh::Design design;
    buildSourceFreezeDesign(design, 2);
    auto *const source = design.findGraph("child");
    auto *const top = design.findGraph("top");
    design.createGraph("unused");
    const auto missing = top->createOperation(grh::OperationKind::kInstance);
    top->setAttr(missing, "moduleName", std::string("missing"));
    PassManager manager;
    manager.addPass(std::make_unique<HierFlattenPass>());
    PassDiagnostics diags;
    if (manager.run(design, diags).success || !diags.hasError() || design.graphs().size() != 3 ||
        design.findGraph("child") != source || design.findGraph("top") != top || !source->frozen())
        return fail("source freeze changed the failed flatten source graph set");
    if (diags.messages().back().message != "Instance module not found: missing")
        return fail("source freeze changed missing-module diagnostics");
    if (const int result = checkSourceFreezeWiring(design)) return result;
    const auto inventory = top->metadata(wolvrix::lib::ingest::node::kInventory);
    if (inventory && !std::get<std::vector<std::string>>(*inventory).empty())
        return fail("failed source freeze flatten reintroduced retired node inventory");
    return 0;
}

int testSourceFreezeBlackboxRetention()
{
    namespace grh = wolvrix::lib::grh;
    for (const bool namedBlackbox : {false, true})
    {
        grh::Design design;
        buildSourceFreezeDesign(design, 2);
        auto *const source = design.findGraph("child");
        const auto oldIndex = source->findOperation("difference").index;
        auto &top = *design.findGraph("top");
        const auto blackbox = top.createOperation(grh::OperationKind::kBlackbox);
        top.setAttr(blackbox, "moduleName", std::string("child"));
        top.setAttr(blackbox, "instanceName", std::string("opaque"));
        if (namedBlackbox)
        {
            top.setAttr(blackbox, "inputPortName", std::vector<std::string>{"z", "a"});
            top.setAttr(blackbox, "outputPortName", std::vector<std::string>{"y"});
        }
        top.addOperand(blackbox, top.findValue("z"));
        top.addOperand(blackbox, top.findValue("a"));
        top.addResult(blackbox, top.createValue(8, false));
        PassManager manager;
        manager.addPass(std::make_unique<HierFlattenPass>());
        PassDiagnostics diags;
        if (!manager.run(design, diags).success || design.graphs().size() != 2 ||
            design.findGraph("child") != source || source->frozen() ||
            !source->metadata("firrtl.nodeRetention") ||
            (namedBlackbox && source->findOperation("difference").index >= oldIndex) ||
            top.opKind(blackbox) != grh::OperationKind::kBlackbox ||
            (!namedBlackbox && source->inputPorts().front().name != "z"))
            return fail("source freeze changed blackbox definition retention or positional ports");
        const auto operands = top.opOperands(blackbox);
        if (operands.size() != 2 || operands[0] != top.findValue("z") || operands[1] != top.findValue("a"))
            return fail("source freeze changed retained blackbox wiring");
        if (const int result = checkSourceFreezeWiring(design)) return result;
    }
    return 0;
}

int testSourceFreezeDiagnosticFallback()
{
    namespace grh = wolvrix::lib::grh;
    const std::array<std::vector<std::string>, 3> badNames{{{"z"}, {"z", "z"}, {"z", "missing"}}};
    const std::array<std::string_view, 3> errors{{"Instance port list does not match module ports",
        "Flatten missing mapping for port value (input)", "Flatten missing input port: missing"}};
    for (std::size_t i = 0; i < badNames.size(); ++i)
    {
        grh::Design design;
        buildSourceFreezeDesign(design, 2);
        auto &top = *design.findGraph("top");
        top.setAttr(top.findOperation("instance1"), "inputPortName", badNames[i]);
        PassManager manager;
        manager.addPass(std::make_unique<HierFlattenPass>());
        PassDiagnostics diags;
        if (manager.run(design, diags).success || !diags.hasError() ||
            design.graphs().size() != 2 || design.findGraph("child")->frozen() ||
            !diags.messages().back().message.starts_with(errors[i]))
            return fail("source freeze changed malformed port-map diagnostics");
    }

    grh::Design design;
    buildSourceFreezeDesign(design, 2);
    auto &source = *design.findGraph("child");
    const auto erasedOperand = source.createValue(8, false);
    source.replaceOperand(source.findOperation("difference"), 0, erasedOperand);
    source.eraseValueUnchecked(erasedOperand);
    PassManager manager;
    manager.addPass(std::make_unique<HierFlattenPass>());
    PassDiagnostics diags;
    if (manager.run(design, diags).success || !diags.hasError() || source.frozen() ||
        design.graphs().size() != 2 || diags.messages().back().message != "Flatten missing operand mapping")
        return fail("failed source freeze replaced existing invalid-graph diagnostics");
    const auto summary = std::find_if(diags.messages().begin(), diags.messages().end(), [](const auto &diagnostic) {
        return diagnostic.message == "source freeze eligible=1 frozen=0 already-frozen=0 skipped-positional=0 failed-freeze=1";
    });
    return summary != diags.messages().end() ? 0 : fail("failed source freeze summary missing");
}

// Different port names may share a child input ValueId. Preserve the
// original unordered_map::emplace rule: the first instance operand wins.
int testSharedPortInputFirstMapping()
{
    namespace grh = wolvrix::lib::grh;
    for (const bool inout : {false, true})
    {
        for (const unsigned portCase : {0U, 1U, 2U, 3U})
        {
            const bool named = (portCase & 1U) != 0;
            const std::vector<std::string> portNames = portCase < 2U
                ? std::vector<std::string>{"p0", "p1"}
                : std::vector<std::string>{"z", "a"};
            grh::Design design;
            auto &child = design.createGraph("child");
            auto &top = design.createGraph("top");
            design.markAsTop("top");
            const auto shared = child.createValue(child.internSymbol("shared"), 8, false);
            std::array<grh::ValueId, 2> childOutputs;
            std::array<grh::ValueId, 2> childOes;
            for (std::size_t i = 0; i < 2; ++i)
            {
                childOutputs[i] = child.createValue(8, false);
                if (inout)
                {
                    childOes[i] = child.createValue(1, false);
                    child.bindInoutPort(portNames[i], shared, childOutputs[i], childOes[i]);
                    const auto one = child.createOperation(grh::OperationKind::kConstant);
                    child.setAttr(one, "constValue", std::string("1'b1"));
                    child.addResult(one, childOes[i]);
                }
                else
                {
                    child.bindInputPort(portNames[i], shared);
                    child.bindOutputPort("y" + std::to_string(i), childOutputs[i]);
                }
                const auto observe = child.createOperation(grh::OperationKind::kAssign,
                    child.internSymbol("observe" + std::to_string(i)));
                child.addDeclaredSymbol(child.operationSymbol(observe));
                child.addOperand(observe, shared);
                child.addResult(observe, childOutputs[i]);
            }

            const auto first = top.createValue(top.internSymbol("first"), 8, false);
            const auto second = top.createValue(top.internSymbol("second"), 8, false);
            top.bindInputPort("first", first);
            top.bindInputPort("second", second);
            std::array<grh::ValueId, 2> outputs;
            for (std::size_t i = 0; i < 2; ++i)
            {
                outputs[i] = top.createValue(top.internSymbol("y" + std::to_string(i)), 8, false);
                top.bindOutputPort("y" + std::to_string(i), outputs[i]);
            }
            const auto instance = top.createOperation(grh::OperationKind::kInstance);
            top.setAttr(instance, "moduleName", std::string("child"));
            top.setAttr(instance, "instanceName", std::string("u"));
            top.addOperand(instance, first);
            top.addOperand(instance, second);
            for (const auto output : outputs) top.addResult(instance, output);
            if (inout)
            {
                for (std::size_t i = 0; i < 2; ++i)
                    top.addResult(instance, top.createValue(1, false));
            }
            if (named)
            {
                top.setAttr(instance, inout ? "inoutPortName" : "inputPortName",
                    portNames);
                if (!inout)
                    top.setAttr(instance, "outputPortName", std::vector<std::string>{"y0", "y1"});
            }
            HierFlattenOptions options;
            options.symProtect = HierFlattenOptions::SymProtectMode::None;
            PassManager manager;
            manager.addPass(std::make_unique<HierFlattenPass>(options));
            PassDiagnostics diags;
            if (!manager.run(design, diags).success || diags.hasError())
                return fail("flatten rejected aliased child port inputs");
            for (std::size_t i = 0; i < 2; ++i)
            {
                const auto observe = top.findOperation("u$observe" + std::to_string(i));
                if (!observe.valid() || top.opOperands(observe).size() != 1 ||
                    top.opOperands(observe)[0] != first || top.valueDef(outputs[i]) != observe)
                    return fail("flatten overwrote the first aliased input/inout mapping");
            }
        }
    }
    return 0;
}

int testForwardSymbolReferences()
{
    namespace grh = wolvrix::lib::grh;
    struct Reference
    {
        grh::OperationKind kind;
        const char *name;
        const char *attr;
        const char *target;
        grh::OperationKind targetKind;
    };
    const std::array<Reference, 3> references{{
        {grh::OperationKind::kRegisterReadPort, "reg_read", "regSymbol", "state", grh::OperationKind::kRegister},
        {grh::OperationKind::kLatchReadPort, "latch_read", "latchSymbol", "lat", grh::OperationKind::kLatch},
        {grh::OperationKind::kAssign, "ordinary_ref", "regSymbol", "ordinary", grh::OperationKind::kNot}}};
    for (const bool namedPorts : {false, true})
    {
        grh::Design design;
        auto &child = design.createGraph("child");
        auto &top = design.createGraph("top");
        design.markAsTop("top");
        const auto input = child.createValue(child.internSymbol("x"), 1, false);
        const auto ordinaryResult = child.createValue(1, false);
        child.bindInputPort("x", input);
        for (std::size_t i = 0; i < references.size(); ++i)
        {
            const auto &reference = references[i];
            const auto output = child.createValue(child.internSymbol("y" + std::to_string(i)), 1, false);
            child.bindOutputPort("y" + std::to_string(i), output);
            const auto op = child.createOperation(reference.kind, child.internSymbol(reference.name));
            child.addDeclaredSymbol(child.operationSymbol(op));
            if (reference.kind == grh::OperationKind::kAssign) child.addOperand(op, ordinaryResult);
            child.addResult(op, output);
            child.setAttr(op, reference.attr, std::string(reference.target));
        }
        // Every target follows its reference. The ordinary target is deliberately
        // undeclared: arbitrary operation symbols must survive the rename filter.
        for (const auto &reference : references)
        {
            const auto op = child.createOperation(reference.targetKind, child.internSymbol(reference.target));
            if (reference.targetKind == grh::OperationKind::kNot)
            {
                child.addOperand(op, input);
                child.addResult(op, ordinaryResult);
            }
            else
            {
                child.addDeclaredSymbol(child.operationSymbol(op));
                child.setAttr(op, "width", int64_t{1});
                child.setAttr(op, "isSigned", false);
            }
        }
        const auto topInput = top.createValue(top.internSymbol("x"), 1, false);
        top.bindInputPort("x", topInput);
        top.addDeclaredSymbol(top.valueSymbol(topInput));
        const auto instance = top.createOperation(grh::OperationKind::kInstance);
        top.addOperand(instance, topInput);
        for (std::size_t i = 0; i < references.size(); ++i)
        {
            const auto name = "y" + std::to_string(i);
            const auto output = top.createValue(top.internSymbol(name), 1, false);
            top.bindOutputPort(name, output);
            top.addDeclaredSymbol(top.valueSymbol(output));
            top.addResult(instance, output);
        }
        top.setAttr(instance, "moduleName", std::string("child"));
        top.setAttr(instance, "instanceName", std::string("u_child"));
        if (namedPorts)
        {
            top.setAttr(instance, "inputPortName", std::vector<std::string>{"x"});
            top.setAttr(instance, "outputPortName", std::vector<std::string>{"y0", "y1", "y2"});
        }
        PassManager manager;
        manager.addPass(std::make_unique<HierFlattenPass>());
        PassDiagnostics diags;
        if (!manager.run(design, diags).success || diags.hasError() || design.graphs().size() != 1)
            return fail("forward symbol-reference flatten failed");
        for (std::size_t i = 0; i < references.size(); ++i)
        {
            const auto &reference = references[i];
            const auto op = top.findOperation("u_child$" + std::string(reference.name));
            if (!op.valid() || top.valueDef(top.findValue("y" + std::to_string(i))) != op)
                return fail("forward symbol reference lost its result mapping");
            const auto targetName = getAttrString(top.getOperation(op), reference.attr);
            const auto target = targetName ? top.findOperation(*targetName) : grh::OperationId::invalid();
            if (!target.valid() || *targetName == reference.target || top.opKind(target) != reference.targetKind)
                return fail("forward symbol reference did not resolve to the cloned target");
            if (reference.targetKind == grh::OperationKind::kNot)
            {
                if (top.opOperands(op).size() != 1 || top.valueDef(top.opOperands(op)[0]) != target)
                    return fail("forward reference to an ordinary operation resolved incorrectly");
            }
            else if (*targetName != "u_child$" + std::string(reference.target))
                return fail("forward state reference lost its declared hierarchical name");
        }
    }
    return 0;
}

int testDpiCallBeforeImportSiblingDedup()
{
    namespace grh = wolvrix::lib::grh;
    for (const bool namedPorts : {false, true})
    {
        grh::Design design;
        auto &child = design.createGraph("child");
        auto &top = design.createGraph("top");
        design.markAsTop("top");
        const auto input = child.createValue(child.internSymbol("x"), 1, false);
        const auto output = child.createValue(child.internSymbol("y"), 1, false);
        child.bindInputPort("x", input);
        child.bindOutputPort("y", output);
        const auto call = child.createOperation(grh::OperationKind::kDpicCall);
        child.addOperand(call, input);
        child.addResult(call, output);
        child.setAttr(call, "targetImportSymbol", std::string("dpi_forward"));
        child.setAttr(call, "inArgName", std::vector<std::string>{"x"});
        child.setAttr(call, "outArgName", std::vector<std::string>{});
        child.setAttr(call, "hasReturn", true);
        const auto dpiImport = child.createOperation(grh::OperationKind::kDpicImport, child.internSymbol("dpi_forward"));
        child.setAttr(dpiImport, "argsDirection", std::vector<std::string>{"input"});
        child.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{1});
        child.setAttr(dpiImport, "argsName", std::vector<std::string>{"x"});
        child.setAttr(dpiImport, "argsSigned", std::vector<bool>{false});
        child.setAttr(dpiImport, "argsType", std::vector<std::string>{"logic"});
        child.setAttr(dpiImport, "hasReturn", true);
        child.setAttr(dpiImport, "returnWidth", int64_t{1});
        child.setAttr(dpiImport, "returnSigned", false);
        const auto topInput = top.createValue(top.internSymbol("x"), 1, false);
        top.bindInputPort("x", topInput);
        top.addDeclaredSymbol(top.valueSymbol(topInput));
        for (int i = 0; i < 2; ++i)
        {
            const auto name = "y" + std::to_string(i);
            const auto topOutput = top.createValue(top.internSymbol(name), 1, false);
            top.bindOutputPort(name, topOutput);
            top.addDeclaredSymbol(top.valueSymbol(topOutput));
            const auto instance = top.createOperation(grh::OperationKind::kInstance);
            top.addOperand(instance, topInput);
            top.addResult(instance, topOutput);
            top.setAttr(instance, "moduleName", std::string("child"));
            top.setAttr(instance, "instanceName", "u" + std::to_string(i));
            if (namedPorts)
            {
                top.setAttr(instance, "inputPortName", std::vector<std::string>{"x"});
                top.setAttr(instance, "outputPortName", std::vector<std::string>{"y"});
            }
        }
        PassManager manager;
        manager.addPass(std::make_unique<HierFlattenPass>());
        PassDiagnostics diags;
        if (!manager.run(design, diags).success || diags.hasError() || design.graphs().size() != 1)
            return fail("DPI call-before-import sibling flatten failed");
        unsigned imports = 0;
        unsigned calls = 0;
        for (const auto op : top.operations())
        {
            imports += top.opKind(op) == grh::OperationKind::kDpicImport;
            calls += top.opKind(op) == grh::OperationKind::kDpicCall;
        }
        const auto resolvedImport = top.findOperation("dpi_forward");
        if (imports != 1 || calls != 2 || !resolvedImport.valid())
            return fail("DPI sibling calls did not share exactly one import");
        for (int i = 0; i < 2; ++i)
        {
            const auto resolvedCall = top.valueDef(top.findValue("y" + std::to_string(i)));
            if (!resolvedCall.valid() || top.opKind(resolvedCall) != grh::OperationKind::kDpicCall ||
                getAttrString(top.getOperation(resolvedCall), "targetImportSymbol") != "dpi_forward" ||
                top.opOperands(resolvedCall).size() != 1 || top.opOperands(resolvedCall)[0] != topInput)
                return fail("DPI call-before-import lost its target or sibling port mapping");
        }
    }
    return 0;
}

int testUnresolvedSymbolReferenceDiagnostics()
{
    namespace grh = wolvrix::lib::grh;
    for (const bool namedPorts : {false, true})
    for (const char *attr : {"regSymbol", "latchSymbol", "targetImportSymbol"})
    for (const std::string target : {"missing", "unbound", "value", "instance"})
    {
        grh::Design design;
        auto &child = design.createGraph("child");
        auto &top = design.createGraph("top");
        design.markAsTop("top");
        const auto input = child.createValue(child.internSymbol("value"), 1, false);
        const auto output = child.createValue(child.internSymbol("y"), 1, false);
        child.bindInputPort("value", input);
        child.bindOutputPort("y", output);
        const auto reference = child.createOperation(grh::OperationKind::kAssign);
        child.addOperand(reference, input);
        child.addResult(reference, output);
        child.setAttr(reference, attr, target);
        if (target == "unbound") child.internSymbol(target);
        if (target == "instance")
        {
            design.createGraph("leaf");
            const auto nested = child.createOperation(grh::OperationKind::kInstance, child.internSymbol(target));
            child.setAttr(nested, "moduleName", std::string("leaf"));
            child.setAttr(nested, "instanceName", std::string("u_leaf"));
            child.setAttr(nested, "inputPortName", std::vector<std::string>{});
            child.setAttr(nested, "outputPortName", std::vector<std::string>{});
        }
        const auto sourceSymbol = child.lookupSymbol(target);
        if ((target == "missing" && sourceSymbol.valid()) ||
            (target != "missing" && !sourceSymbol.valid()) ||
            (target == "unbound" && (child.findValue(sourceSymbol).valid() || child.findOperation(sourceSymbol).valid())))
            return fail("unresolved reference fixture has the wrong symbol binding");
        const auto topInput = top.createValue(top.internSymbol("x"), 1, false);
        const auto topOutput = top.createValue(top.internSymbol("y"), 1, false);
        top.bindInputPort("x", topInput);
        top.bindOutputPort("y", topOutput);
        const auto instance = top.createOperation(grh::OperationKind::kInstance);
        top.addOperand(instance, topInput);
        top.addResult(instance, topOutput);
        top.setAttr(instance, "moduleName", std::string("child"));
        top.setAttr(instance, "instanceName", std::string("u_child"));
        if (namedPorts)
        {
            top.setAttr(instance, "inputPortName", std::vector<std::string>{"value"});
            top.setAttr(instance, "outputPortName", std::vector<std::string>{"y"});
        }
        PassManager manager;
        manager.addPass(std::make_unique<HierFlattenPass>());
        PassDiagnostics diags;
        const auto result = manager.run(design, diags);
        const auto errors = std::count_if(diags.messages().begin(), diags.messages().end(), [](const auto &diagnostic) {
            return diagnostic.kind == PassDiagnosticKind::Error;
        });
        if (result.success || !diags.hasError() || errors != 1 ||
            diags.messages().back().message != "Flatten unresolved symbol reference: " + target)
            return fail("flatten changed the unresolved " + std::string(attr) + " diagnostic for " + target);
    }
    return 0;
}

int testCloneMetadataPreservation()
{
    namespace grh = wolvrix::lib::grh;
    for (const bool namedPorts : {false, true})
    for (int provenance = 0; provenance < 4; ++provenance)
    {
        grh::Design design;
        auto &child = design.createGraph("child");
        auto &top = design.createGraph("top");
        design.markAsTop("top");
        const auto input = child.createValue(child.internSymbol("x"), 8, false);
        const auto intermediate = child.createValue(8, false);
        const auto output = child.createValue(child.internSymbol("y"), 8, false);
        child.bindInputPort("x", input);
        child.bindOutputPort("y", output);
        const auto producer = child.createOperation(grh::OperationKind::kNot, child.internSymbol("producer"));
        child.addDeclaredSymbol(child.operationSymbol(producer));
        child.addOperand(producer, input);
        child.addResult(producer, intermediate);
        const auto forward = child.createOperation(grh::OperationKind::kAssign);
        child.addOperand(forward, intermediate);
        child.addResult(forward, output);
        child.setAttr(producer, "before", std::string("preserve-before"));
        if (provenance == 1) child.setAttr(producer, kOriginModulesKey, std::vector<std::string>{});
        if (provenance == 2) child.setAttr(producer, kOriginModulesKey, std::vector<std::string>{"leaf", "u0", "leaf"});
        // A retired annotation is dropped regardless of its old payload type.
        if (provenance == 3) child.setAttr(producer, kOriginModulesKey, int64_t{7});
        child.setAttr(producer, "after", std::string("preserve-after"));
        grh::SrcLoc location;
        location.file = "a/long/source/path/whose/storage/must/outlive/each/clone/source.fir";
        location.line = 12; location.column = 4; location.endLine = 14; location.endColumn = 9;
        location.origin = "original-frontend-location";
        location.pass = "ingest-firrtl";
        location.note = "a source location note with independently owned string storage";
        child.setOpSrcLoc(producer, location);
        child.setValueSrcLoc(intermediate, location);
        const auto sameLocation = [&](const std::optional<grh::SrcLoc> &actual) {
            return actual && actual->file == location.file && actual->line == location.line &&
                   actual->column == location.column && actual->endLine == location.endLine &&
                   actual->endColumn == location.endColumn && actual->origin == location.origin &&
                   actual->pass == location.pass && actual->note == location.note;
        };
        const auto topInput = top.createValue(top.internSymbol("x"), 8, false);
        top.bindInputPort("x", topInput);
        top.addDeclaredSymbol(top.valueSymbol(topInput));
        for (int i = 0; i < 2; ++i)
        {
            const auto name = "y" + std::to_string(i);
            const auto value = top.createValue(top.internSymbol(name), 8, false);
            top.bindOutputPort(name, value);
            top.addDeclaredSymbol(top.valueSymbol(value));
            const auto instance = top.createOperation(grh::OperationKind::kInstance);
            top.addOperand(instance, topInput);
            top.addResult(instance, value);
            top.setAttr(instance, "moduleName", std::string("child"));
            top.setAttr(instance, "instanceName", "u" + std::to_string(i));
            if (namedPorts)
            {
                top.setAttr(instance, "inputPortName", std::vector<std::string>{"x"});
                top.setAttr(instance, "outputPortName", std::vector<std::string>{"y"});
            }
        }
        HierFlattenOptions options;
        options.preserveFlattenedModules = true;
        PassManager manager;
        manager.addPass(std::make_unique<HierFlattenPass>(options));
        PassDiagnostics diags;
        if (!manager.run(design, diags).success || diags.hasError())
            return fail("metadata clone flatten failed");
        const auto sourceProducer = child.findOperation("producer");
        if (!sameLocation(child.opSrcLoc(sourceProducer)) ||
            !sameLocation(child.valueSrcLoc(child.opResults(sourceProducer)[0])))
            return fail("cloning moved metadata out of its preserved source definition");
        std::vector<std::string> expectedKeys;
        for (const auto &attr : child.opAttrRange(sourceProducer))
            if (attr.key() != kOriginModulesKey) expectedKeys.emplace_back(attr.key());
        for (int i = 0; i < 2; ++i)
        {
            const std::string prefix = "u" + std::to_string(i);
            const auto clone = top.findOperation(prefix + "$producer");
            if (!clone.valid() || !sameLocation(top.opSrcLoc(clone)) ||
                !sameLocation(top.valueSrcLoc(top.opResults(clone)[0])))
                return fail("cloned value or operation source location changed");
            if (top.getOperation(clone).attr(kOriginModulesKey))
                return fail("clone retained retired originModules annotation");
            std::vector<std::string> actualKeys;
            for (const auto &attr : top.opAttrRange(clone)) actualKeys.emplace_back(attr.key());
            if (actualKeys != expectedKeys ||
                getAttrString(top.getOperation(clone), "before") != "preserve-before" ||
                getAttrString(top.getOperation(clone), "after") != "preserve-after")
                return fail("cloning changed generic attributes or their insertion order");
        }
    }
    return 0;
}

int testFrozenNestedDefinitionsAndPassReuse()
{
    namespace grh = wolvrix::lib::grh;
    grh::Design design;
    // Keep outer reference sets live while enough distinct inner definitions
    // are visited to grow the cache, then revisit every definition as a sibling.
    constexpr int depth = 24;
    std::vector<grh::Graph *> modules;
    for (int i = 0; i < depth; ++i) modules.push_back(&design.createGraph("m" + std::to_string(i)));
    auto &top = design.createGraph("top");
    design.markAsTop("top");
    const auto instantiate = [](grh::Graph &graph, std::string module, std::string name,
                                grh::ValueId input, grh::ValueId output) {
        const auto instance = graph.createOperation(grh::OperationKind::kInstance);
        graph.addOperand(instance, input);
        graph.addResult(instance, output);
        graph.setAttr(instance, "moduleName", std::move(module));
        graph.setAttr(instance, "instanceName", std::move(name));
        graph.setAttr(instance, "inputPortName", std::vector<std::string>{"x"});
        graph.setAttr(instance, "outputPortName", std::vector<std::string>{"y"});
    };
    const auto state = [](grh::Graph &graph, const std::string &name) {
        const auto op = graph.createOperation(grh::OperationKind::kRegister, graph.internSymbol(name));
        graph.addDeclaredSymbol(graph.operationSymbol(op));
        graph.setAttr(op, "width", int64_t{8});
        graph.setAttr(op, "isSigned", false);
        graph.setAttr(op, "initValue", std::string("8'b0"));
    };
    for (int i = 0; i < depth; ++i)
    {
        auto &graph = *modules[i];
        const auto input = graph.createValue(graph.internSymbol("x"), 8, false);
        const auto output = graph.createValue(graph.internSymbol("y"), 8, false);
        graph.bindInputPort("x", input);
        graph.bindOutputPort("y", output);
        auto nested = input;
        if (i + 1 < depth)
        {
            nested = graph.createValue(8, false);
            instantiate(graph, "m" + std::to_string(i + 1), "nested", input, nested);
        }
        // These ops use the outer cached set after recursively adding entries.
        state(graph, "state");
        const auto readValue = graph.createValue(8, false);
        const auto read = graph.createOperation(grh::OperationKind::kRegisterReadPort, graph.internSymbol("read"));
        graph.addDeclaredSymbol(graph.operationSymbol(read));
        graph.addResult(read, readValue);
        graph.setAttr(read, "regSymbol", std::string("state"));
        const auto combine = graph.createOperation(grh::OperationKind::kXor);
        graph.addOperand(combine, nested);
        graph.addOperand(combine, readValue);
        graph.addResult(combine, output);
    }
    const auto topInput = top.createValue(top.internSymbol("x"), 8, false);
    top.bindInputPort("x", topInput);
    top.addDeclaredSymbol(top.valueSymbol(topInput));
    const auto addTopInstance = [&](int ordinal) {
        const auto name = "y" + std::to_string(ordinal);
        const auto output = top.createValue(top.internSymbol(name), 8, false);
        top.bindOutputPort(name, output);
        top.addDeclaredSymbol(top.valueSymbol(output));
        instantiate(top, "m0", "u" + std::to_string(ordinal), topInput, output);
    };
    addTopInstance(0);
    // One complete 24-level expansion exercises consuming every definition.
    // Keep this separate from the sibling/preserved-source pass-reuse checks.
    std::string consumedReference;
    std::vector<std::string> consumedDiagnostics;
    for (const auto [global, threads] : {std::pair{false, std::size_t{1}},
                                        std::pair{true, std::size_t{1}},
                                        std::pair{true, std::size_t{4}}}) {
        auto candidate = design.clone();
        HierFlattenOptions consumeOptions;
        consumeOptions.useGlobalPlan = global;
        consumeOptions.numThreads = threads;
        PassManager consumeManager;
        consumeManager.addPass(std::make_unique<HierFlattenPass>(consumeOptions));
        PassDiagnostics diagnostics;
        if (!consumeManager.run(candidate, diagnostics).success || diagnostics.hasError() ||
            candidate.graphs().size() != 1 || candidate.findGraph("m0") || candidate.findGraph("m23"))
            return fail("24-level single-use flatten did not consume all source definitions");
        wolvrix::lib::store::StoreDiagnostics storeDiagnostics;
        wolvrix::lib::store::StoreJson store(&storeDiagnostics);
        const auto json = store.storeToString(candidate);
        if (!json || storeDiagnostics.hasError()) return fail("24-level consumed JSON serialization failed");
        std::vector<std::string> messages;
        for (const auto &message : diagnostics.messages()) messages.push_back(message.message);
        if (!global) {
            consumedReference = *json;
            consumedDiagnostics = std::move(messages);
        } else if (*json != consumedReference || messages != consumedDiagnostics) {
            return fail("24-level consuming global plan differs from eager JSON or diagnostics");
        }
    }
    addTopInstance(1);
    HierFlattenOptions options;
    options.preserveFlattenedModules = true;
    PassManager manager;
    manager.addPass(std::make_unique<HierFlattenPass>(options));
    PassDiagnostics first;
    if (!manager.run(design, first).success || first.hasError())
        return fail("frozen nested-definition flatten failed");
    for (const auto *module : modules)
        if (!module->frozen()) return fail("nested definition did not exercise frozen-source caching");
    const auto checkReads = [&](int ordinal, bool updated) {
        std::string prefix = "u" + std::to_string(ordinal);
        for (int i = 0; i < depth; ++i)
        {
            const auto read = top.findOperation(prefix + "$read");
            const auto targetName = prefix + (updated && i == 0 ? "$fresh_state" : "$state");
            const auto target = top.findOperation(targetName);
            if (!read.valid() || !target.valid() || top.opKind(target) != grh::OperationKind::kRegister ||
                getAttrString(top.getOperation(read), "regSymbol") != targetName)
                return false;
            prefix += "$nested";
        }
        return true;
    };
    if (!checkReads(0, false) || !checkReads(1, false))
        return fail("nested cache growth or sibling reuse changed symbol-reference remapping");

    // Thaw and change the preserved source, then reuse the same pass object.
    // A stale per-pass symbol set would omit the new forward reference target.
    auto &source = *modules.front();
    source.setAttr(source.findOperation("read"), "regSymbol", std::string("fresh_state"));
    state(source, "fresh_state");
    if (source.frozen()) return fail("source edit did not exercise a thawed definition");
    addTopInstance(2);
    PassDiagnostics second;
    if (!manager.run(design, second).success || second.hasError() || !source.frozen())
        return fail("same-pass flatten failed after modifying a retained definition");
    if (!checkReads(0, false) || !checkReads(1, false) || !checkReads(2, true))
        return fail("same-pass source mutation reused stale mappings or changed earlier instances");
    return 0;
}

int testBlockRunsSerialParallelIdentity()
{
    namespace grh = wolvrix::lib::grh;
    // Exercise the retained eager reference, the global event plan and its
    // persistent executor on runs interrupted by nested instances.
    constexpr std::size_t runLength = 32768;
    constexpr std::size_t tailLength = 3;
    const auto build = [&](grh::Design& design, std::size_t instances) {
        auto& leaf = design.createGraph("leaf");
        auto& middle = design.createGraph("middle");
        auto& top = design.createGraph("top");
        design.markAsTop("top");
        const auto ports = [](grh::Graph& graph) {
            const auto input = graph.createValue(graph.internSymbol("x"), 8, false);
            const auto output = graph.createValue(graph.internSymbol("y"), 8, false);
            graph.bindInputPort("x", input);
            graph.bindOutputPort("y", output);
            return std::pair(input, output);
        };
        const auto [leafInput, leafOutput] = ports(leaf);
        const auto leafOp = leaf.createOperation(grh::OperationKind::kAssign, leaf.internSymbol("leaf"));
        leaf.addDeclaredSymbol(leaf.operationSymbol(leafOp));
        leaf.addOperand(leafOp, leafInput);
        leaf.addResult(leafOp, leafOutput);
        const auto [middleInput, middleOutput] = ports(middle);
        grh::SrcLoc location;
        location.file = "a/shared/long/source/path/for/ordinary/operation/block/runs.fir";
        location.pass = "ingest-firrtl";
        location.note = "provenance shared across values and operation runs";
        const auto chain = [&](grh::ValueId first, grh::ValueId final, bool before) {
            auto previous = first;
            const auto count = before ? runLength : tailLength;
            for (std::size_t index = 0; index < count; ++index) {
                const bool named = before ? index == 0 : index + 1 == count;
                const auto symbol = named ? middle.internSymbol(before ? "before" : "after") : middle.makeInternalOpSym();
                const auto operation = middle.createOperation(grh::OperationKind::kAssign, symbol);
                if (named) middle.addDeclaredSymbol(symbol);
                const auto result = index + 1 == count ? final : middle.createValue(8, false);
                middle.addOperand(operation, previous);
                middle.addResult(operation, result);
                location.line = static_cast<uint32_t>(index + (before ? 1 : runLength + 1));
                middle.setOpAndValueSrcLocView(operation, result, grh::SrcLocView(location));
                middle.setAttr(operation, "fixture.payload", std::vector<int64_t>{1, 2, 3});
                previous = result;
            }
        };
        const auto beforeOutput = middle.createValue(8, false);
        const auto childOutput = middle.createValue(8, false);
        chain(middleInput, beforeOutput, true);
        const auto instantiate = [](grh::Graph& graph, std::string module, std::string name,
                                    grh::ValueId input, grh::ValueId output) {
            const auto instance = graph.createOperation(grh::OperationKind::kInstance);
            graph.addOperand(instance, input);
            graph.addResult(instance, output);
            graph.setAttr(instance, "moduleName", std::move(module));
            graph.setAttr(instance, "instanceName", std::move(name));
            graph.setAttr(instance, "inputPortName", std::vector<std::string>{"x"});
            graph.setAttr(instance, "outputPortName", std::vector<std::string>{"y"});
        };
        instantiate(middle, "leaf", "nested", beforeOutput, childOutput);
        chain(childOutput, middleOutput, false);
        const auto input = top.createValue(top.internSymbol("x"), 8, false);
        top.bindInputPort("x", input);
        // Force the protected operation name to use the old collision suffix.
        top.createValue(top.internSymbol("u$before"), 8, false);
        for (std::size_t index = 0; index < instances; ++index) {
            const auto name = index ? "v" : "u";
            const auto output = top.createValue(top.internSymbol(std::string(name) + "_out"), 8, false);
            top.bindOutputPort(std::string(name) + "_out", output);
            instantiate(top, "middle", name, input, output);
        }
    };
    for (const std::size_t instances : {std::size_t{1}, std::size_t{2}}) {
        for (const bool preserve : {false, true}) {
            std::string serialJson;
            std::vector<std::string> serialDiagnostics;
            for (const auto [global, threads] : {std::pair{false, std::size_t{1}},
                                                std::pair{true, std::size_t{1}},
                                                std::pair{true, std::size_t{4}}}) {
                grh::Design design;
                build(design, instances);
                HierFlattenOptions options;
                options.preserveFlattenedModules = preserve;
                options.numThreads = threads;
                options.useGlobalPlan = global;
                PassManager manager;
                manager.addPass(std::make_unique<HierFlattenPass>(options));
                PassDiagnostics diagnostics;
                if (!manager.run(design, diagnostics).success || diagnostics.hasError())
                    return fail("ordinary block runs failed across an instance boundary");
                const auto& top = *design.findGraph("top");
                const auto before = top.findOperation("u$before_1");
                const auto nested = top.findOperation("u$nested$leaf");
                const auto after = top.findOperation("u$after");
                if (before.index != instances + 1 || nested.index != instances + runLength + 1 ||
                    after.index != instances + runLength + tailLength + 1)
                    return fail("block planning changed legacy parent/child operation creation order");
                for (const auto operation : top.operations()) {
                    const auto operands = top.opOperands(operation);
                    for (std::size_t index = 0; index < operands.size(); ++index) {
                        const auto users = top.valueUsers(operands[index]);
                        if (std::count_if(users.begin(), users.end(), [&](const auto& user) {
                            return user.operation == operation && user.operandIndex == index;
                        }) != 1)
                            return fail("block use-list rebuild lost an operand backlink");
                    }
                }
                if (bool(design.findGraph("middle")) != preserve || bool(design.findGraph("leaf")) != preserve)
                    return fail("block flatten changed source preservation policy");
                wolvrix::lib::store::StoreDiagnostics storeDiagnostics;
                wolvrix::lib::store::StoreJson store(&storeDiagnostics);
                const auto json = store.storeToString(design);
                if (!json || storeDiagnostics.hasError()) return fail("block flatten JSON serialization failed");
                std::vector<std::string> messages;
                for (const auto& diagnostic : diagnostics.messages()) messages.push_back(diagnostic.message);
                if (!global) {
                    serialJson = *json;
                    serialDiagnostics = std::move(messages);
                } else if (*json != serialJson || messages != serialDiagnostics) {
                    return fail("global event plan, eager reference or parallel executor changed JSON bytes or diagnostic order");
                }
            }
        }
    }
    return 0;
}

int testGlobalBoundaryPlanReference()
{
    namespace grh = wolvrix::lib::grh;
    const auto build = [](grh::Design &design) {
        auto &leaf = design.createGraph("leaf_view");
        auto &middle = design.createGraph("middle_view");
        auto &top = design.createGraph("top_view");
        design.markAsTop("top_view");
        const std::array<int64_t, 1> shape{1};
        const auto x = leaf.createValue(leaf.internSymbol("x"), 8, false, grh::ValueType::Logic, shape);
        const auto y = leaf.createValue(leaf.internSymbol("y"), 8, false, grh::ValueType::Logic, shape);
        leaf.bindInputPort("x", x);
        leaf.bindOutputPort("copy", x); // Input/output alias requires an assign at each boundary.
        leaf.bindOutputPort("y", y);
        const auto assign = leaf.createOperation(grh::OperationKind::kAssign);
        leaf.addOperand(assign, x);
        leaf.addResult(assign, y);
        leaf.setAttr(assign, "fixture.payload", std::vector<int64_t>{7, 11});
        const auto instantiate = [](grh::Graph &graph, std::string module, std::string name,
                                    grh::ValueId input, grh::ValueId copy, grh::ValueId output) {
            const auto op = graph.createOperation(grh::OperationKind::kInstance);
            graph.addOperand(op, input);
            graph.addResult(op, copy);
            graph.addResult(op, output);
            graph.setAttr(op, "moduleName", std::move(module));
            graph.setAttr(op, "instanceName", std::move(name));
            graph.setAttr(op, "inputPortName", std::vector<std::string>{"x"});
            graph.setAttr(op, "outputPortName", std::vector<std::string>{"copy", "y"});
        };
        const auto midX = middle.createValue(8, false);
        const auto midCopy = middle.createValue(8, false);
        const auto midY = middle.createValue(8, false);
        middle.bindInputPort("x", midX);
        middle.bindOutputPort("copy", midCopy);
        middle.bindOutputPort("y", midY);
        instantiate(middle, "leaf_view", "inner", midX, midCopy, midY);
        const auto input = top.createValue(top.internSymbol("x"), 8, false);
        top.bindInputPort("x", input);
        for (const std::string name : {"u", "v"}) {
            const auto copy = top.createValue(8, false);
            const auto output = top.createValue(8, false);
            top.bindOutputPort(name + "_copy", copy);
            top.bindOutputPort(name + "_y", output);
            instantiate(top, "middle_view", name, input, copy, output);
        }
    };
    std::string reference;
    for (const auto [global, threads] : {std::pair{false, std::size_t{1}},
                                        std::pair{true, std::size_t{1}},
                                        std::pair{true, std::size_t{4}}}) {
        grh::Design design;
        build(design);
        HierFlattenOptions options;
        options.useGlobalPlan = global;
        options.numThreads = threads;
        options.symProtect = HierFlattenOptions::SymProtectMode::None;
        PassManager manager;
        manager.addPass(std::make_unique<HierFlattenPass>(options));
        PassDiagnostics diagnostics;
        if (!manager.run(design, diagnostics).success || diagnostics.hasError())
            return fail("global boundary plan failed on nested aliases and shared type views");
        wolvrix::lib::store::StoreDiagnostics storeDiagnostics;
        wolvrix::lib::store::StoreJson store(&storeDiagnostics);
        const auto json = store.storeToString(design);
        if (!json || storeDiagnostics.hasError()) return fail("boundary plan JSON serialization failed");
        if (!global) reference = *json;
        else if (*json != reference)
            return fail("global port union changed view/alias boundary IDs, ownership or operands");
    }
    return 0;
}

int testResolvedXmrGlobalBlocksReference()
{
    namespace grh = wolvrix::lib::grh;
    namespace node = wolvrix::lib::ingest::node;
    const auto build = [](grh::Design &design) {
        // The real register/latch read/write fixture from
        // test_xmr_resolve_storage, nested inside interrupted parent runs.
        auto &leaf = design.createGraph("leaf");
        auto &middle = design.createGraph("middle");
        auto &top = design.createGraph("top");
        design.markAsTop("top");
        const auto reg = leaf.createOperation(grh::OperationKind::kRegister, leaf.internSymbol("reg_a"));
        leaf.setAttr(reg, "width", int64_t{8});
        leaf.setAttr(reg, "isSigned", false);
        const auto latch = leaf.createOperation(grh::OperationKind::kLatch, leaf.internSymbol("lat_b"));
        leaf.setAttr(latch, "width", int64_t{4});
        leaf.setAttr(latch, "isSigned", false);
        const auto constant = [](grh::Graph &graph) {
            const auto value = graph.createValue(1, false);
            const auto op = graph.createOperation(grh::OperationKind::kConstant);
            graph.addResult(op, value);
            graph.setAttr(op, "constValue", std::string("1'b0"));
        };
        const auto instantiate = [](grh::Graph &graph, std::string module, std::string name) {
            const auto op = graph.createOperation(grh::OperationKind::kInstance);
            graph.setAttr(op, "moduleName", std::move(module));
            graph.setAttr(op, "instanceName", std::move(name));
            graph.setAttr(op, "inputPortName", std::vector<std::string>{});
            graph.setAttr(op, "outputPortName", std::vector<std::string>{});
            graph.setAttr(op, "inoutPortName", std::vector<std::string>{});
        };
        constant(middle);
        instantiate(middle, "leaf", "u_leaf");
        constant(middle);
        instantiate(top, "middle", "u_middle");
        const auto makeInput = [&](std::string_view name, int32_t width) {
            const auto value = top.createValue(top.internSymbol(name), width, false);
            top.bindInputPort(std::string(name), value);
            return value;
        };
        const auto cond = makeInput("cond", 1), data = makeInput("data", 8);
        const auto mask = makeInput("mask", 8), clock = makeInput("clock", 1);
        const auto latchCond = makeInput("latch_cond", 1), latchData = makeInput("latch_data", 4);
        const auto latchMask = makeInput("latch_mask", 4);
        const auto observed = top.createValue(top.internSymbol("observed"), 8, false);
        const auto read = top.createOperation(grh::OperationKind::kXMRRead);
        top.addResult(read, observed);
        top.setAttr(read, "xmrPath", std::string("u_middle.u_leaf.reg_a"));
        top.bindOutputPort("observed", observed);
        const auto key = node::key({"fixture", "Circuit", "top", {}, "xmr", {}});
        node::write(top, read, {{key}, {{key, 0}}, {key}});
        node::establishPolicy(top);
        const auto regWrite = top.createOperation(grh::OperationKind::kXMRWrite);
        for (const auto value : {cond, data, mask, clock}) top.addOperand(regWrite, value);
        top.setAttr(regWrite, "xmrPath", std::string("u_middle.u_leaf.reg_a"));
        top.setAttr(regWrite, "eventEdge", std::vector<std::string>{"posedge"});
        const auto latchWrite = top.createOperation(grh::OperationKind::kXMRWrite);
        for (const auto value : {latchCond, latchData, latchMask}) top.addOperand(latchWrite, value);
        top.setAttr(latchWrite, "xmrPath", std::string("u_middle.u_leaf.lat_b"));
    };
    std::string reference;
    std::vector<std::string> referenceDiagnostics;
    for (const auto [global, threads] : {std::pair{false, std::size_t{1}},
                                        std::pair{true, std::size_t{1}},
                                        std::pair{true, std::size_t{4}}}) {
        grh::Design design;
        build(design);
        HierFlattenOptions options;
        options.useGlobalPlan = global;
        options.numThreads = threads;
        PassDiagnostics diagnostics;
        PassManager resolver;
        resolver.addPass(std::make_unique<XmrResolvePass>());
        if (!resolver.run(design, diagnostics).success || diagnostics.hasError())
            return fail("nested storage XMR fixture could not resolve its read/write paths");
        const auto &resolvedTop = *design.findGraph("top");
        const auto resolvedNodes = node::inspect(resolvedTop);
        if (!resolvedNodes.errors.empty() || resolvedNodes.nodes.size() != 1 ||
            resolvedNodes.nodes[0].roots.size() != 1 ||
            resolvedNodes.nodes[0].roots[0].value != resolvedTop.outputPortValue("observed"))
            return fail("nested XMR resolution changed the observed node root");
        PassManager flatten;
        flatten.addPass(std::make_unique<HierFlattenPass>(options));
        if (!flatten.run(design, diagnostics).success || diagnostics.hasError() || design.graphs().size() != 1)
            return fail("resolved storage XMR did not flatten across nested block boundaries");
        const auto &top = *design.findGraph("top");
        for (const auto op : top.operations())
            if (top.opKind(op) == grh::OperationKind::kXMRRead || top.opKind(op) == grh::OperationKind::kXMRWrite)
                return fail("resolved cross-block flatten retained an XMR operation");
        const auto inspection = node::inspect(top);
        if (!inspection.errors.empty()) return fail("resolved cross-block flatten produced invalid node metadata");
        // The old flatten drops optional associations on the erased instance.
        // Check XMR's actual storage read after that boundary; full JSON below
        // also compares the exact surviving associations against the old path.
        const auto observedDef = top.valueDef(top.outputPortValue("observed"));
        if (!observedDef.valid() || top.opKind(observedDef) != grh::OperationKind::kRegisterReadPort)
            return fail("resolved cross-block observed output is no longer a register read");
        const auto regSymbol = getAttrString(top.getOperation(observedDef), "regSymbol");
        const auto reg = regSymbol ? top.findOperation(*regSymbol) : grh::OperationId{};
        if (!reg.valid() || top.opKind(reg) != grh::OperationKind::kRegister)
            return fail("resolved cross-block register read lost its cloned storage reference");
        wolvrix::lib::store::StoreDiagnostics storeDiagnostics;
        wolvrix::lib::store::StoreJson store(&storeDiagnostics);
        const auto json = store.storeToString(design);
        if (!json || storeDiagnostics.hasError()) return fail("resolved XMR JSON serialization failed");
        std::vector<std::string> messages;
        for (const auto &message : diagnostics.messages()) messages.push_back(message.message);
        if (!global) {
            reference = *json;
            referenceDiagnostics = std::move(messages);
        } else if (*json != reference || messages != referenceDiagnostics) {
            return fail("resolved storage XMR eager/global/parallel flatten differs in JSON or diagnostics");
        }
    }
    return 0;
}

int testGlobalPlanBoundaryDiagnosticsReference()
{
    namespace grh = wolvrix::lib::grh;
    for (int scenario = 0; scenario < 5; ++scenario) {
        std::vector<std::string> reference;
        std::string referenceJson;
        for (const bool global : {false, true}) {
            grh::Design design;
            auto &child = design.createGraph("child");
            auto &top = design.createGraph("top");
            design.markAsTop("top");
            grh::LogicalType childType{grh::ValueType::Logic, 8, false, grh::LogicDomain::FourState, {}};
            auto parentInputType = childType;
            auto parentOutputType = childType;
            if (scenario == 0) parentInputType.width = 16;
            if (scenario == 1) {
                childType.kind = grh::ValueType::Real;
                childType.width = 1;
                parentInputType.width = 1;
                parentOutputType = childType;
            }
            if (scenario == 2) parentInputType.domain = grh::LogicDomain::TwoState;
            if (scenario == 3) parentOutputType.width = 16;
            const auto input = child.createValue(child.internSymbol("x"), childType);
            const auto output = child.createValue(child.internSymbol("y"), childType);
            child.bindInputPort("x", input);
            child.bindOutputPort("y", output);
            const auto instance = [](grh::Graph &graph, grh::ValueId in, grh::ValueId out) {
                const auto op = graph.createOperation(grh::OperationKind::kInstance);
                graph.addOperand(op, in);
                graph.addResult(op, out);
                graph.setAttr(op, "moduleName", std::string("child"));
                graph.setAttr(op, "instanceName", std::string("u"));
                graph.setAttr(op, "inputPortName", std::vector<std::string>{"x"});
                graph.setAttr(op, "outputPortName", std::vector<std::string>{"y"});
            };
            if (scenario == 4) instance(child, input, output);
            else {
                const auto assign = child.createOperation(grh::OperationKind::kAssign);
                child.addOperand(assign, input);
                child.addResult(assign, output);
            }
            const auto parentInput = top.createValue(top.internSymbol("x"), parentInputType);
            const auto parentOutput = top.createValue(top.internSymbol("y"), parentOutputType);
            top.bindInputPort("x", parentInput);
            top.bindOutputPort("y", parentOutput);
            instance(top, parentInput, parentOutput);
            HierFlattenOptions options;
            options.useGlobalPlan = global;
            options.numThreads = 4;
            PassManager manager;
            manager.addPass(std::make_unique<HierFlattenPass>(options));
            PassDiagnostics diagnostics;
            if (manager.run(design, diagnostics).success || !diagnostics.hasError())
                return fail("invalid port type or recursive instance escaped flatten diagnostics");
            std::vector<std::string> messages;
            for (const auto &message : diagnostics.messages()) messages.push_back(message.message);
            const auto expected = scenario == 4 ? "Recursive instance detected during hier-flatten" :
                scenario == 3 ? "Flatten output port requires equal width and compatible kind/domain" :
                "Flatten input port requires equal width and compatible kind/domain";
            if (std::find(messages.begin(), messages.end(), expected) == messages.end())
                return fail("invalid boundary produced the wrong flatten diagnostic");
            wolvrix::lib::store::StoreDiagnostics storeDiagnostics;
            wolvrix::lib::store::StoreJson store(&storeDiagnostics);
            const auto json = store.storeToString(design);
            if (!json || storeDiagnostics.hasError()) return fail("failed-boundary JSON serialization failed");
            if (!global) {
                reference = std::move(messages);
                referenceJson = *json;
            } else if (messages != reference || *json != referenceJson) {
                return fail("global plan changed boundary failure order, message or partial graph JSON");
            }
        }
    }
    return 0;
}

int testResultLocationInheritanceReference()
{
    namespace grh = wolvrix::lib::grh;
    const grh::SrcLoc opLocation{"SimTop.fir", 8794404, 11, 8794404, 19,
        "FIRRTL", "ingest-firrtl", "RocketDebugWrapper.scala 132:13"};
    const grh::SrcLoc ownLocation{"value.fir", 7, 2, 7, 8, "value", "lower", "keep explicit location"};
    const std::array<std::string, 5> names{"missing_string", "explicit_logic", "empty_location", "no_locations", "missing_logic"};
    const auto build = [&](grh::Design &design, std::size_t instances) {
        auto &child = design.createGraph("child");
        auto &top = design.createGraph("top");
        design.markAsTop("top");
        for (std::size_t index = 0; index < names.size(); ++index) {
            const auto type = index == 0 ? grh::ValueType::String : grh::ValueType::Logic;
            const auto value = child.createValue(child.internSymbol(names[index]), 1, false, type);
            child.addDeclaredSymbol(child.valueSymbol(value));
            if (index == 1) child.setValueSrcLoc(value, ownLocation);
            if (index == 2) child.setValueSrcLoc(value, grh::SrcLoc{});
            const auto op = child.createOperation(grh::OperationKind::kConstant,
                child.internSymbol("make_" + names[index]));
            child.addDeclaredSymbol(child.operationSymbol(op));
            child.addResult(op, value);
            child.setAttr(op, "constValue", index == 0 ? std::string("debug.scala") : std::string("1'b0"));
            // Ingest can attach provenance after adding the result. Such a
            // result intentionally has no source record until flatten clones
            // the located op and calls the old scalar addResult route.
            if (index != 3) child.setOpSrcLoc(op, opLocation);
        }
        if (child.valueSrcLoc(child.findValue(names[0])))
            throw std::runtime_error("inheritance fixture unexpectedly set the source String value location");
        for (std::size_t index = 0; index < instances; ++index) {
            const auto op = top.createOperation(grh::OperationKind::kInstance);
            top.setAttr(op, "moduleName", std::string("child"));
            top.setAttr(op, "instanceName", index == 0 ? std::string("u") : std::string("v"));
            top.setAttr(op, "inputPortName", std::vector<std::string>{});
            top.setAttr(op, "outputPortName", std::vector<std::string>{});
        }
    };
    const auto scalarFlatten = [&](grh::Design &design) {
        auto &source = *design.findGraph("child");
        auto &target = *design.findGraph("top");
        const std::vector<grh::OperationId> instances(target.operations().begin(), target.operations().end());
        for (std::size_t index = 0; index < instances.size(); ++index) {
            const std::string prefix = index == 0 ? "u" : "v";
            target.eraseOpUnchecked(instances[index]);
            std::vector<grh::ValueId> values(source.values().size() + 1);
            for (const auto sourceValue : source.values()) {
                const auto name = prefix + "$" + std::string(source.symbolText(source.valueSymbol(sourceValue)));
                const auto symbol = target.internSymbol(name);
                target.addDeclaredSymbol(symbol);
                const auto value = target.createValue(symbol, source.valueLogicalType(sourceValue));
                target.copyValueSrcLoc(value, source, sourceValue);
                values[sourceValue.index] = value;
            }
            for (const auto sourceOp : source.operations()) {
                const auto name = prefix + "$" + std::string(source.symbolText(source.operationSymbol(sourceOp)));
                const auto symbol = target.internSymbol(name);
                target.addDeclaredSymbol(symbol);
                const auto op = target.createOperation(source.opKind(sourceOp), symbol);
                target.copyOpSrcLoc(op, source, sourceOp);
                for (const auto attr : source.opAttrRange(sourceOp)) target.setAttrView(op, attr.key(), attr.value());
                for (const auto value : source.opResults(sourceOp)) target.addResult(op, values[value.index]);
            }
        }
        design.deleteGraph("child");
    };
    for (const std::size_t instances : {std::size_t{1}, std::size_t{2}}) {
        grh::Design scalar;
        build(scalar, instances);
        scalarFlatten(scalar);
        wolvrix::lib::store::StoreDiagnostics storeDiagnostics;
        wolvrix::lib::store::StoreJson store(&storeDiagnostics);
        const auto expected = store.storeToString(scalar);
        if (!expected || storeDiagnostics.hasError()) return fail("scalar provenance reference did not serialize");
        for (const auto [global, threads] : {std::pair{false, std::size_t{1}},
                                            std::pair{true, std::size_t{1}},
                                            std::pair{true, std::size_t{4}}}) {
            grh::Design design;
            build(design, instances);
            HierFlattenOptions options;
            options.useGlobalPlan = global;
            options.numThreads = threads;
            PassManager manager;
            manager.addPass(std::make_unique<HierFlattenPass>(options));
            PassDiagnostics diagnostics;
            if (!manager.run(design, diagnostics).success || diagnostics.hasError())
                return fail("bulk result provenance inheritance flatten failed");
            const auto &top = *design.findGraph("top");
            for (std::size_t index = 0; index < instances; ++index) {
                const std::string prefix = index == 0 ? "u$" : "v$";
                const auto inherited = top.valueSrcLoc(top.findValue(prefix + names[0]));
                const auto explicitValue = top.valueSrcLoc(top.findValue(prefix + names[1]));
                const auto empty = top.valueSrcLoc(top.findValue(prefix + names[2]));
                const auto inheritedLogic = top.valueSrcLoc(top.findValue(prefix + names[4]));
                if (!inherited || inherited->file != opLocation.file || inherited->line != opLocation.line ||
                    inherited->note != opLocation.note || !explicitValue || explicitValue->file != ownLocation.file ||
                    !empty || !empty->file.empty() || empty->line != 0 ||
                    !inheritedLogic || inheritedLogic->file != opLocation.file ||
                    top.valueSrcLoc(top.findValue(prefix + names[3])))
                    return fail("bulk result location inheritance differs from scalar missing/explicit/empty semantics");
            }
            const auto actual = store.storeToString(design);
            if (!actual || storeDiagnostics.hasError() || *actual != *expected)
                return fail("eager/global/parallel provenance JSON differs from scalar copyOpSrcLoc then addResult");
        }
    }
    return 0;
}

int testSemanticTreeBelowUnannotatedTop()
{
    namespace grh = wolvrix::lib::grh;
    std::string reference;
    for (const bool global : {false, true})
    {
        grh::Design design;
        auto& leaf = design.createGraph("FirrtlLeaf");
        auto& wrapper = design.createGraph("SvWrapper");
        auto& top = design.createGraph("svtop");
        design.markAsTop("svtop");
        auto& tree = leaf.getOrCreateSemanticTree("fir", "Original", "FirrtlLeaf");
        const auto semantic = tree.addNode(tree.root(), "n");
        const auto constant = [](grh::Graph& graph, std::string_view name) {
            const auto value = graph.createValue(graph.internSymbol(name), 1, false);
            const auto operation = graph.createOperation(grh::OperationKind::kConstant);
            graph.setAttr(operation, "value", std::string("1'b0"));
            graph.addResult(operation, value);
            return std::pair(value, operation);
        };
        const auto [sourceValue, sourceOperation] = constant(leaf, "n");
        leaf.setValueSemanticIds(sourceValue, std::span<const grh::SemanticId>(&semantic, 1));
        leaf.setOpSemanticIds(sourceOperation, std::span<const grh::SemanticId>(&semantic, 1));
        tree.addNode(tree.root(), "empty");
        constant(wrapper, "local");
        const auto [topValue, topOperation] = constant(top, "original");
        const auto instance = [](grh::Graph& graph, std::string_view module, std::string_view name) {
            const auto operation = graph.createOperation(grh::OperationKind::kInstance);
            graph.setAttr(operation, "moduleName", std::string(module));
            graph.setAttr(operation, "instanceName", std::string(name));
        };
        for (const auto name : {"f0", "f1"}) instance(wrapper, "FirrtlLeaf", name);
        for (const auto name : {"w0", "w1"}) instance(top, "SvWrapper", name);
        HierFlattenOptions options;
        options.useGlobalPlan = global;
        PassManager manager;
        manager.addPass(std::make_unique<HierFlattenPass>(options));
        PassDiagnostics diagnostics;
        if (!manager.run(design, diagnostics).success || diagnostics.hasError())
            return fail("FIRRTL source below an unannotated SV top did not flatten");
        if (!top.semanticTree() || top.semanticTree()->sourceUnit() != "fir" ||
            top.semanticTree()->circuit() != "Original" ||
            top.semanticTree()->pathName(top.semanticTree()->root()) != "svtop")
            return fail("mixed target tree did not retain first imported source context and target root");
        if (!top.opSemanticIds(topOperation).empty() || !top.valueSemanticIds(topValue).empty())
            return fail("mixed target tree invented provenance for original SV operations");
        std::size_t annotated = 0, unannotated = 0;
        for (const auto operation : top.operations())
        {
            const auto ids = top.opSemanticIds(operation);
            if (ids.empty()) { ++unannotated; continue; }
            ++annotated;
            const auto name = top.semanticTree()->name(ids.front());
            if (ids.size() != 1 ||
                (name != "w0.f0.n" && name != "w0.f1.n" && name != "w1.f0.n" && name != "w1.f1.n"))
                return fail("mixed source lost an unannotated ancestor or independent instance path");
        }
        if (annotated != 4 || unannotated != 3 || top.semanticTree()->pathCount() != 1 ||
            top.semanticTree()->size() != 8 || !top.validateSemanticTree().empty())
            return fail("mixed source retained internal paths or invented SV ownership");
        std::size_t completeNodes = 0, emptyNodes = 0;
        for (const auto id : top.semanticTree()->nodeIds())
        {
            if (top.semanticTree()->node(id).parent != top.semanticTree()->root())
                return fail("flatten retained a semantic container");
            const auto ops = top.semanticNodeOperations(id);
            const auto values = top.semanticNodeAnchors(id);
            if (top.semanticTree()->name(id).ends_with(".empty") && ops.empty() && values.empty())
                ++emptyNodes;
            else if (ops.size() == 1 && values.size() == 1 && top.valueDef(values.front()) == ops.front())
                ++completeNodes;
            else return fail("flatten did not relocate Node members and anchors together");
        }
        if (completeNodes != 4 || emptyNodes != 4)
            return fail("flatten lost a complete or empty Node");
        const auto serialized = wolvrix::lib::store::StoreJson().storeToString(design);
        if (!serialized) return fail("mixed source tree could not be serialized");
        if (reference.empty()) reference = *serialized;
        else if (reference != *serialized) return fail("mixed source tree differs between scalar and global flatten");
        PassDiagnostics second;
        if (!manager.run(design, second).success || second.hasError() ||
            wolvrix::lib::store::StoreJson().storeToString(design) != serialized)
            return fail("repeated mixed flatten changed the imported SemanticTree");
    }
    return 0;
}

int testExternalImplementationBindings()
{
    namespace grh = wolvrix::lib::grh;
    std::string reference;
    for (const bool global : {false, true})
    {
        grh::Design design;
        auto& leaf = design.createGraph("SvImplementation");
        leaf.setMetadata("ingest.sourceModule", std::string("SharedImplementation"));
        leaf.setMetadata("ingest.link.sourceModule", std::string("FirrtlExternal"));
        leaf.setMetadata("ingest.link.implementationModule", std::string("ResolvedImplementation"));
        auto& middle = design.createGraph("FirrtlMiddle");
        auto& top = design.createGraph("top");
        design.markAsTop("top");
        const auto leafInput = leaf.createValue(leaf.internSymbol("data"), 8, false);
        const auto leafOutput = leaf.createValue(leaf.internSymbol("out"), 8, false);
        leaf.bindInputPort("data", leafInput);
        leaf.bindOutputPort("out", leafOutput);
        const auto drive = leaf.createOperation(grh::OperationKind::kAssign, leaf.internSymbol("drive"));
        leaf.addOperand(drive, leafInput);
        leaf.addResult(drive, leafOutput);

        auto& middleTree = middle.getOrCreateSemanticTree("fixture", "top", "FirrtlMiddle");
        const auto externalPath = middleTree.addPath(middleTree.root(), "external");
        const auto inputSemantic = middleTree.addNode(externalPath, "data");
        const auto outputSemantic = middleTree.addNode(externalPath, "out");
        const auto middleInput = middle.createValue(middle.internSymbol("data"), 8, false);
        const auto middleOutput = middle.createValue(middle.internSymbol("out"), 8, false);
        middle.bindInputPort("data", middleInput);
        middle.bindOutputPort("out", middleOutput);
        middle.setValueSemanticIds(middleInput, std::span<const grh::SemanticId>(&inputSemantic, 1));
        middle.setValueSemanticIds(middleOutput, std::span<const grh::SemanticId>(&outputSemantic, 1));
        const auto external = middle.createOperation(grh::OperationKind::kInstance);
        middle.addOperand(external, middleInput);
        middle.addResult(external, middleOutput);
        middle.setAttr(external, "moduleName", std::string("SvImplementation"));
        middle.setAttr(external, "instanceName", std::string("external"));
        middle.setAttr(external, "firrtl.line", int64_t{57});
        middle.setAttr(external, "inputPortName", std::vector<std::string>{"data"});
        middle.setAttr(external, "outputPortName", std::vector<std::string>{"out"});
        const std::vector<grh::SemanticId> externalSemantics{inputSemantic, outputSemantic};
        middle.setOpSemanticIds(external, externalSemantics);

        auto& topTree = top.getOrCreateSemanticTree("fixture", "top", "top");
        const auto topInput = top.createValue(top.internSymbol("data"), 8, false);
        top.bindInputPort("data", topInput);
        for (const auto& name : {std::string("m0"), std::string("m1")})
        {
            const auto output = top.createValue(top.internSymbol(name + "out"), 8, false);
            top.bindOutputPort(name + "out", output);
            const auto instance = top.createOperation(grh::OperationKind::kInstance);
            top.addOperand(instance, topInput);
            top.addResult(instance, output);
            top.setAttr(instance, "moduleName", std::string("FirrtlMiddle"));
            top.setAttr(instance, "instanceName", name);
            top.setAttr(instance, "firrtl.line", int64_t{100});
        }

        HierFlattenOptions options;
        options.useGlobalPlan = global;
        PassManager manager;
        manager.addPass(std::make_unique<HierFlattenPass>(options));
        PassDiagnostics diagnostics;
        if (!manager.run(design, diagnostics).success || diagnostics.hasError())
            return fail("external implementation binding flatten failed");
        const auto expect = [&](std::string_view key, grh::AttributeValue value) {
            return top.metadata(key) == std::optional<grh::AttributeValue>(std::move(value));
        };
        if (!expect("firrtl.externalInstancePaths", std::vector<std::string>{"m0$external", "m1$external"}) ||
            !expect("firrtl.externalInstanceModules", std::vector<std::string>{"FirrtlExternal", "FirrtlExternal"}) ||
            !expect("firrtl.externalInstanceImplementations", std::vector<std::string>{"ResolvedImplementation", "ResolvedImplementation"}) ||
            !expect("firrtl.externalInstanceLines", std::vector<int64_t>{57, 57}) ||
            !expect("firrtl.externalInstanceInputOffsets", std::vector<int64_t>{0, 1, 2}) ||
            !expect("firrtl.externalInstanceOutputOffsets", std::vector<int64_t>{0, 1, 2}) ||
            !expect("firrtl.externalInstanceInoutOffsets", std::vector<int64_t>{0, 0, 0}) ||
            !expect("firrtl.externalInstanceInputPorts", std::vector<std::string>{"m0$external$data", "m1$external$data"}) ||
            !expect("firrtl.externalInstanceOutputPorts", std::vector<std::string>{"m0$external$out", "m1$external$out"}) ||
            !expect("firrtl.externalInstanceInoutPorts", std::vector<std::string>{}))
            return fail("external implementation binding did not preserve complete per-instance boundaries");
        for (const auto operation : top.operations())
        {
            if (top.opKind(operation) != grh::OperationKind::kAssign) continue;
            const auto ids = top.opSemanticIds(operation);
            if (!ids.empty())
                return fail("external SV implementation was assigned to a FIRRTL boundary Node");
        }
        if (topTree.size() != 4 || !topTree.isFlat() || !top.validateSemanticTree().empty())
            return fail("external boundary Nodes or relocated anchors were lost");
        const auto serialized = wolvrix::lib::store::StoreJson().storeToString(design);
        if (!serialized) return fail("external implementation metadata serialization failed");
        if (reference.empty()) reference = *serialized;
        else if (reference != *serialized) return fail("external bindings differ between legacy and global flatten");
        PassDiagnostics second;
        if (!manager.run(design, second).success || second.hasError() ||
            wolvrix::lib::store::StoreJson().storeToString(design) != serialized)
            return fail("repeated flatten duplicated external implementation bindings");
    }
    return 0;
}

int testSemanticPortNodeRelocation()
{
    namespace grh = wolvrix::lib::grh;
    std::string reference;
    for (const bool global : {false, true})
    {
        grh::Design design;
        auto& child = design.createGraph("child");
        auto& childTree = child.getOrCreateSemanticTree("fixture", "top", "child");
        const auto xNode = childTree.addNode(childTree.root(), "x");
        const auto yNode = childTree.addNode(childTree.root(), "y");
        const auto x = child.createValue(child.internSymbol("x"), 8, false);
        const auto y = child.createValue(child.internSymbol("y"), 8, false);
        child.bindInputPort("x", x);
        child.bindOutputPort("y", y);
        child.setValueSemanticIds(x, std::span<const grh::SemanticId>(&xNode, 1));
        child.setValueSemanticIds(y, std::span<const grh::SemanticId>(&yNode, 1));
        const auto drive = child.createOperation(grh::OperationKind::kAssign);
        child.addOperand(drive, x);
        child.addResult(drive, y);
        child.setOpSemanticIds(drive, std::span<const grh::SemanticId>(&yNode, 1));

        auto& top = design.createGraph("top");
        design.markAsTop("top");
        auto& tree = top.getOrCreateSemanticTree("fixture", "top", "top");
        const auto inputNode = tree.addNode(tree.root(), "a");
        const auto input = top.createValue(top.internSymbol("a"), 8, false);
        top.bindInputPort("a", input);
        top.setValueSemanticIds(input, std::span<const grh::SemanticId>(&inputNode, 1));
        std::vector<grh::SemanticId> originalIds{inputNode};
        for (const std::string name : {"u", "v"})
        {
            const auto path = tree.addPath(tree.root(), name);
            const auto inNode = tree.addNode(path, "x");
            const auto outNode = tree.addNode(path, "y");
            originalIds.push_back(inNode);
            originalIds.push_back(outNode);
            const auto output = top.createValue(top.internSymbol(name + "out"), 8, false);
            top.bindOutputPort(name + "out", output);
            const auto oldInput = top.valueSemanticIds(input);
            std::vector<grh::SemanticId> inputIds(oldInput.begin(), oldInput.end());
            inputIds.push_back(inNode);
            top.setValueSemanticIds(input, inputIds);
            top.setValueSemanticIds(output, std::span<const grh::SemanticId>(&outNode, 1));
            const auto instance = top.createOperation(grh::OperationKind::kInstance);
            top.setAttr(instance, "moduleName", std::string("child"));
            top.setAttr(instance, "instanceName", name);
            top.addOperand(instance, input);
            top.addResult(instance, output);
            top.setOpSemanticIds(instance, std::vector<grh::SemanticId>{inNode, outNode});
        }
        HierFlattenOptions options;
        options.useGlobalPlan = global;
        PassManager manager;
        manager.addPass(std::make_unique<HierFlattenPass>(options));
        PassDiagnostics diagnostics;
        if (!manager.run(design, diagnostics).success || diagnostics.hasError())
            return fail("semantic port Node relocation failed");
        if (tree.size() != 5 || !tree.isFlat() || tree.nodeIds() != originalIds ||
            !top.validateSemanticTree().empty())
            return fail("flatten duplicated placeholders, changed Node IDs, or retained paths");
        for (const auto id : tree.nodeIds())
        {
            const auto ops = top.semanticNodeOperations(id);
            const auto anchors = top.semanticNodeAnchors(id);
            if (anchors.size() != 1) return fail("port Node lost its final boundary anchor");
            if (tree.name(id).ends_with(".y"))
            {
                if (ops.size() != 1 || top.valueDef(anchors.front()) != ops.front())
                    return fail("output Node did not acquire the child's actual operation");
            }
            else if (!ops.empty() || anchors.front() != input)
                return fail("input Node acquired circuitry or an incorrect alias representative");
        }
        const auto serialized = wolvrix::lib::store::StoreJson().storeToString(design);
        if (!serialized) return fail("semantic port Nodes could not be serialized");
        if (reference.empty()) reference = *serialized;
        else if (reference != *serialized) return fail("semantic port Nodes differ between flatten implementations");
    }
    return 0;
}

int testSemanticQualifierAndInvalidPort()
{
    namespace grh = wolvrix::lib::grh;
    for (const bool sourceFlat : {false, true})
    {
        grh::Design design;
        auto& child = design.createGraph("child");
        auto& source = child.getOrCreateSemanticTree("fixture", "top", "child");
        const auto path = source.addPath(source.root(), "a.b");
        const auto id = source.addNode(path, "c[0]");
        if (sourceFlat) source.flatten();
        auto& top = design.createGraph("top");
        design.markAsTop("top");
        const auto instance = top.createOperation(grh::OperationKind::kInstance);
        top.setAttr(instance, "moduleName", std::string("child"));
        top.setAttr(instance, "instanceName", std::string("u.dot"));
        PassManager manager;
        manager.addPass(std::make_unique<HierFlattenPass>());
        PassDiagnostics diagnostics;
        if (!manager.run(design, diagnostics).success || !top.semanticTree() ||
            top.semanticTree()->size() != 1 ||
            top.semanticTree()->name(top.semanticTree()->nodeIds().front()) != "u\\.dot.a\\.b.c\\[0\\]")
            return fail("hier-flatten lost reversible names or re-escaped a flattened source");
        (void)id;
    }
    for (const bool global : {false, true})
    for (const bool invalidAnchor : {false, true})
    {
        grh::Design design;
        auto& child = design.createGraph("child");
        auto& source = child.getOrCreateSemanticTree("fixture", "top", "child");
        const auto inputNode = source.addNode(source.root(), "x");
        const auto input = child.createTemporaryValue(8, false);
        child.bindInputPort("x", input);
        child.setValueSemanticIds(input, std::span<const grh::SemanticId>(&inputNode, 1));
        auto& top = design.createGraph("top");
        design.markAsTop("top");
        auto& tree = top.getOrCreateSemanticTree("fixture", "top", "top");
        const auto placeholder = tree.addNode(tree.addPath(tree.root(), "u"), "x");
        const auto expected = top.createTemporaryValue(8, false);
        const auto actual = top.createTemporaryValue(8, false);
        top.bindInputPort("a", expected);
        top.bindInputPort("b", actual);
        top.setValueSemanticIds(expected, std::span<const grh::SemanticId>(&placeholder, 1));
        // Expand a valid instance before the second one fails, so rollback
        // must restore operations, values, members and the original Paths.
        const auto good = top.createOperation(grh::OperationKind::kInstance);
        top.setAttr(good, "moduleName", std::string("child"));
        top.setAttr(good, "instanceName", std::string("good"));
        top.addOperand(good, expected);
        const auto instance = top.createOperation(grh::OperationKind::kInstance);
        top.setAttr(instance, "moduleName", std::string(invalidAnchor ? "child" : "missing"));
        top.setAttr(instance, "instanceName", std::string("u"));
        top.addOperand(instance, actual);
        top.setOpSemanticIds(instance, std::span<const grh::SemanticId>(&placeholder, 1));
        HierFlattenOptions options;
        options.useGlobalPlan = global;
        PassManager manager;
        manager.addPass(std::make_unique<HierFlattenPass>(options));
        PassDiagnostics diagnostics;
        const auto before = wolvrix::lib::store::StoreJson().storeToString(design);
        if (!before) return fail("rollback fixture could not be serialized");
        bool rejected = false;
        try { rejected = !manager.run(design, diagnostics).success; }
        catch (const std::invalid_argument& error)
        {
            rejected = std::string_view(error.what()).find("placeholder has no port mapping") != std::string_view::npos;
        }
        if (!rejected) return fail("flatten accepted invalid semantic instance correspondence");
        if (design.findGraph("top") != &top || !design.findGraph("child") ||
            top.valueWidth(expected) != 8 || top.valueWidth(actual) != 8 ||
            top.opKind(good) != grh::OperationKind::kInstance ||
            top.opKind(instance) != grh::OperationKind::kInstance ||
            !top.validateSemanticTree().empty() ||
            wolvrix::lib::store::StoreJson().storeToString(design) != before)
            return fail("failed flatten published partial IR or SemanticTree changes");
    }
    return 0;
}

int main()
{
    if (const int result = testSemanticQualifierAndInvalidPort()) return result;
    if (const int result = testSemanticPortNodeRelocation()) return result;
    if (const int result = testSemanticTreeBelowUnannotatedTop()) return result;
    if (const int result = testExternalImplementationBindings()) return result;
    if (const int result = testBlockRunsSerialParallelIdentity()) return result;
    if (const int result = testGlobalBoundaryPlanReference()) return result;
    if (const int result = testResolvedXmrGlobalBlocksReference()) return result;
    if (const int result = testGlobalPlanBoundaryDiagnosticsReference()) return result;
    if (const int result = testResultLocationInheritanceReference()) return result;
    if (const int result = testFrozenNestedDefinitionsAndPassReuse()) return result;
    if (const int result = testCloneMetadataPreservation()) return result;
    if (const int result = testSharedPortInputFirstMapping()) return result;
    if (const int result = testForwardSymbolReferences()) return result;
    if (const int result = testDpiCallBeforeImportSiblingDedup()) return result;
    if (const int result = testUnresolvedSymbolReferenceDiagnostics()) return result;
    if (const int result = testSourceFreezeEquivalence()) return result;
    if (const int result = testSourceFreezeFallbackAndFailure()) return result;
    if (const int result = testSourceFreezeBlackboxRetention()) return result;
    if (const int result = testSourceFreezeDiagnosticFallback()) return result;
    if (const int result = testSharedScalarIndexShape(); result != 0)
        return result;
    if (const int result = testDpiImportOriginMerge(); result != 0)
        return result;
    wolvrix::lib::grh::Design design;
    wolvrix::lib::grh::Graph &child = design.createGraph("child");
    wolvrix::lib::grh::Graph &top = design.createGraph("top");
    design.markAsTop("top");

    const auto childA = child.createValue(child.internSymbol("a"), 1, false);
    const auto childAddr = child.createValue(child.internSymbol("addr"), 4, false);
    const auto childY = child.createValue(child.internSymbol("y"), 1, false);
    const auto childIoIn = child.createValue(child.internSymbol("io__in"), 1, false);
    const auto childIoOut = child.createValue(child.internSymbol("io__out"), 1, false);
    const auto childIoOe = child.createValue(child.internSymbol("io__oe"), 1, false);

    child.bindInputPort("a", childA);
    child.bindInputPort("addr", childAddr);
    child.bindOutputPort("y", childY);
    child.bindInoutPort("io", childIoIn, childIoOut, childIoOe);

    child.addDeclaredSymbol(child.getValue(childA).symbol());
    child.addDeclaredSymbol(child.getValue(childAddr).symbol());
    child.addDeclaredSymbol(child.getValue(childY).symbol());
    child.addDeclaredSymbol(child.getValue(childIoIn).symbol());
    child.addDeclaredSymbol(child.getValue(childIoOut).symbol());
    child.addDeclaredSymbol(child.getValue(childIoOe).symbol());

    const auto memSym = child.internSymbol("mem");
    child.addDeclaredSymbol(memSym);
    const auto memOp = child.createOperation(wolvrix::lib::grh::OperationKind::kRegister, memSym);
    child.setAttr(memOp, "width", static_cast<int64_t>(128));
    child.setAttr(memOp, "isSigned", false);
    child.setAttr(memOp, "shape", std::vector<int64_t>{16});

    const auto memReadVal = child.createValue(child.makeInternalValSym(), 8, false);
    const auto memReadOp =
        child.createOperation(wolvrix::lib::grh::OperationKind::kRegisterReadArrayPort,
                              child.makeInternalOpSym());
    child.addOperand(memReadOp, childAddr);
    child.addResult(memReadOp, memReadVal);
    child.setAttr(memReadOp, "regSymbol", std::string("mem"));

    const auto dpiSym = child.internSymbol("dpi_add");
    const auto dpiImport =
        child.createOperation(wolvrix::lib::grh::OperationKind::kDpicImport, dpiSym);
    child.setAttr(dpiImport, "argsDirection", std::vector<std::string>{"input"});
    child.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{1});
    child.setAttr(dpiImport, "argsName", std::vector<std::string>{"x"});
    child.setAttr(dpiImport, "argsSigned", std::vector<bool>{false});
    child.setAttr(dpiImport, "argsType", std::vector<std::string>{"logic"});
    child.setAttr(dpiImport, "hasReturn", true);
    child.setAttr(dpiImport, "returnWidth", static_cast<int64_t>(1));
    child.setAttr(dpiImport, "returnSigned", false);

    const auto dpiCall =
        child.createOperation(wolvrix::lib::grh::OperationKind::kDpicCall,
                              child.makeInternalOpSym());
    child.addOperand(dpiCall, childA);
    const auto dpiRet = child.createValue(child.makeInternalValSym(), 1, false);
    child.addResult(dpiCall, dpiRet);
    child.setAttr(dpiCall, "targetImportSymbol", std::string("dpi_add"));
    child.setAttr(dpiCall, "inArgName", std::vector<std::string>{"x"});
    child.setAttr(dpiCall, "outArgName", std::vector<std::string>{});
    child.setAttr(dpiCall, "hasReturn", true);

    const auto assignOp =
        child.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                              child.makeInternalOpSym());
    child.addOperand(assignOp, childIoOut);
    child.addResult(assignOp, childY);

    const auto topA = top.createValue(top.internSymbol("a"), 1, false);
    const auto topAddr = top.createValue(top.internSymbol("addr"), 4, false);
    const auto topY = top.createValue(top.internSymbol("y"), 1, false);
    const auto topAddrInternal = top.createValue(top.makeInternalValSym(), 4, false);
    const auto topIoIn = top.createValue(top.internSymbol("io__in"), 1, false);
    const auto topIoOut = top.createValue(top.internSymbol("io__out"), 1, false);
    const auto topIoOe = top.createValue(top.internSymbol("io__oe"), 1, false);

    top.bindInputPort("a", topA);
    top.bindInputPort("addr", topAddr);
    top.bindOutputPort("y", topY);
    top.bindInoutPort("io", topIoIn, topIoOut, topIoOe);

    top.addDeclaredSymbol(top.getValue(topA).symbol());
    top.addDeclaredSymbol(top.getValue(topAddr).symbol());
    top.addDeclaredSymbol(top.getValue(topY).symbol());
    top.addDeclaredSymbol(top.getValue(topIoIn).symbol());
    top.addDeclaredSymbol(top.getValue(topIoOut).symbol());
    top.addDeclaredSymbol(top.getValue(topIoOe).symbol());

    const auto instOp =
        top.createOperation(wolvrix::lib::grh::OperationKind::kInstance,
                            top.internSymbol("_op_child"));
    top.addOperand(instOp, topA);
    top.addOperand(instOp, topAddrInternal);
    top.addOperand(instOp, topIoIn);
    top.addResult(instOp, topY);
    top.addResult(instOp, topIoOut);
    top.addResult(instOp, topIoOe);
    top.setAttr(instOp, "moduleName", std::string("child"));
    top.setAttr(instOp, "instanceName", std::string("u_child"));
    top.setAttr(instOp, "inputPortName", std::vector<std::string>{"a", "addr"});
    top.setAttr(instOp, "outputPortName", std::vector<std::string>{"y"});
    top.setAttr(instOp, "inoutPortName", std::vector<std::string>{"io"});

    PassManager manager;
    manager.addPass(std::make_unique<HierFlattenPass>());
    PassDiagnostics diags;
    const PassManagerResult result = manager.run(design, diags);
    if (!result.success || diags.hasError())
    {
        return fail("hier-flatten pass failed");
    }

    if (design.graphs().size() != 1)
    {
        return fail("Expected flattened design to contain one graph");
    }

    for (const auto opId : top.operations())
    {
        const wolvrix::lib::grh::Operation op = top.getOperation(opId);
        if (op.kind() == wolvrix::lib::grh::OperationKind::kInstance)
        {
            return fail("Expected no kInstance operations after flatten");
        }
    }

    const std::string expectedMem = "u_child$mem";
    wolvrix::lib::grh::OperationId foundMem = wolvrix::lib::grh::OperationId::invalid();
    wolvrix::lib::grh::OperationId foundRead = wolvrix::lib::grh::OperationId::invalid();
    wolvrix::lib::grh::OperationId foundDpiImport = wolvrix::lib::grh::OperationId::invalid();
    wolvrix::lib::grh::OperationId foundDpiCall = wolvrix::lib::grh::OperationId::invalid();
    bool foundAssign = false;

    for (const auto opId : top.operations())
    {
        const wolvrix::lib::grh::Operation op = top.getOperation(opId);
        switch (op.kind())
        {
        case wolvrix::lib::grh::OperationKind::kRegister:
            if (std::string(op.symbolText()) == expectedMem)
            {
                foundMem = opId;
            }
            break;
        case wolvrix::lib::grh::OperationKind::kRegisterReadArrayPort:
            foundRead = opId;
            break;
        case wolvrix::lib::grh::OperationKind::kDpicImport:
            if (op.symbolText() == std::string_view("dpi_add"))
            {
                foundDpiImport = opId;
            }
            break;
        case wolvrix::lib::grh::OperationKind::kDpicCall:
            foundDpiCall = opId;
            break;
        case wolvrix::lib::grh::OperationKind::kAssign:
            if (!op.operands().empty() && !op.results().empty() &&
                op.operands()[0] == topIoOut &&
                op.results()[0] == topY)
            {
                foundAssign = true;
            }
            break;
        default:
            break;
        }
    }

    if (!foundMem.valid() || !foundRead.valid())
    {
        return fail("Flattened array register ops missing");
    }
    const auto memReadOpResolved = top.getOperation(foundRead);
    const auto memSymbol = getAttrString(memReadOpResolved, "regSymbol");
    if (!memSymbol || *memSymbol != expectedMem)
    {
        return fail("regSymbol not rewritten during flatten");
    }
    if (memReadOpResolved.operands().size() != 1 ||
        memReadOpResolved.operands()[0] != topAddrInternal)
    {
        return fail("Memory read port operand mismatch after flatten");
    }

    if (!foundDpiImport.valid() || !foundDpiCall.valid())
    {
        return fail("DPI ops missing after flatten");
    }
    const auto dpiCallOp = top.getOperation(foundDpiCall);
    const auto targetImport = getAttrString(dpiCallOp, "targetImportSymbol");
    if (!targetImport || *targetImport != "dpi_add")
    {
        return fail("DPI call targetImportSymbol mismatch after flatten");
    }
    if (dpiCallOp.operands().empty() || dpiCallOp.operands()[0] != topA)
    {
        return fail("DPI call operand not mapped to top input");
    }

    if (!foundAssign)
    {
        return fail("Inout-connected assign op missing after flatten");
    }

    if (std::string(top.getValue(topA).symbolText()) != "a")
    {
        return fail("Top input symbol was renamed unexpectedly");
    }
    if (std::string(top.getValue(topAddrInternal).symbolText()) != "u_child$addr")
    {
        return fail("Flattened internal port mapping symbol mismatch");
    }

    if (const int rc = testNestedFlattenDeclared())
    {
        return rc;
    }
    if (const int rc = testSymProtectModesKeepDeclared())
    {
        return rc;
    }
    if (const int rc = testGenerateGroupsFlatten())
    {
        return rc;
    }

    return 0;
}
