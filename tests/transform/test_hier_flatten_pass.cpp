#include "core/grh.hpp"
#include "core/store.hpp"
#include "core/transform.hpp"
#include "transform/hier_flatten.hpp"

#include <initializer_list>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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

// leaf: declared wire w (in -> w -> out) plus a declared kMemory op.
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
        leaf.createOperation(wolvrix::lib::grh::OperationKind::kMemory, leafMemSym);
    leaf.setAttr(leafMem, "width", static_cast<int64_t>(8));
    leaf.setAttr(leafMem, "row", static_cast<int64_t>(16));
    leaf.setAttr(leafMem, "isSigned", false);
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

// child: declared wire cw (ci -> cw -> co) and a declared kMemory op.
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
        child.createOperation(wolvrix::lib::grh::OperationKind::kMemory, memSym);
    child.setAttr(mem, "width", static_cast<int64_t>(8));
    child.setAttr(mem, "row", static_cast<int64_t>(16));
    child.setAttr(mem, "isSigned", false);
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

} // namespace

int main()
{
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
    const auto memOp = child.createOperation(wolvrix::lib::grh::OperationKind::kMemory, memSym);
    child.setAttr(memOp, "width", static_cast<int64_t>(8));
    child.setAttr(memOp, "row", static_cast<int64_t>(16));
    child.setAttr(memOp, "isSigned", false);

    const auto memReadVal = child.createValue(child.makeInternalValSym(), 8, false);
    const auto memReadOp =
        child.createOperation(wolvrix::lib::grh::OperationKind::kMemoryReadPort,
                              child.makeInternalOpSym());
    child.addOperand(memReadOp, childAddr);
    child.addResult(memReadOp, memReadVal);
    child.setAttr(memReadOp, "memSymbol", std::string("mem"));

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
        case wolvrix::lib::grh::OperationKind::kMemory:
            foundMem = opId;
            if (std::string(op.symbolText()) != expectedMem)
            {
                return fail("Flattened memory symbol mismatch");
            }
            break;
        case wolvrix::lib::grh::OperationKind::kMemoryReadPort:
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
        return fail("Flattened memory ops missing");
    }
    const auto memReadOpResolved = top.getOperation(foundRead);
    const auto memSymbol = getAttrString(memReadOpResolved, "memSymbol");
    if (!memSymbol || *memSymbol != expectedMem)
    {
        return fail("memSymbol not rewritten during flatten");
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
