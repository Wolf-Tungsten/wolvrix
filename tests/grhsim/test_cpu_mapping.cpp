#include "grhsim/backend/cpu.hpp"
#include "grhsim/dialect/registry.hpp"
#include "grhsim/io/json.hpp"
#include "grhsim/ir/verifier.hpp"
#include "grhsim/pass/pass.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace
{
    using namespace wolvrix::lib;
    using namespace grhsim;

    void require(bool condition, const std::string &message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    void runPass(GrhSimModel &model, std::string_view name,
                 std::span<const std::string_view> args = {})
    {
        std::string error;
        auto pass = defaultPassRegistry().create(name, args, error);
        require(bool(pass), error);
        PassManager manager(defaultDialectRegistry());
        manager.addPass(std::move(pass));
        diag::Diagnostics diagnostics;
        auto result = manager.run(model, diagnostics);
        for (const auto &message : diagnostics.messages())
            std::cout << message.context << ": " << message.message << '\n';
        require(result.success && result.changed && !model.poisoned(), "mapping pass failed: " + std::string(name));
    }

    StateId state(GrhSimModel &model, TypeId type)
    {
        auto id = model.addState("s" + std::to_string(model.states().size()), type);
        const std::array params{Parameter{model.intern("value"), std::string("1'b0")}};
        const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}};
        model.addInit(id, steps, params);
        return id;
    }

    GrhSimModel fixture()
    {
        GrhSimModel model("multiclock");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        auto bit = model.logicType(1, false, LogicDomain::TwoState);
        std::vector<ValueId> inputs;
        for (std::string_view name : {"clk_a", "clk_b", "enable", "data", "mask"})
        {
            auto input = model.addInput(name, bit);
            auto value = model.addValue(bit, name);
            const std::array results{value};
            const std::array refs{ObjectRef::input(input)};
            model.addOperation("core.input.read", {}, results, refs);
            inputs.push_back(value);
        }
        const auto derived = model.addValue(bit, "gated_clock");
        const std::array gateOperands{inputs[0], inputs[2]};
        const std::array gateResults{derived};
        model.addOperation("core.compute.and", gateOperands, gateResults);
        const auto addWrite = [&](std::vector<ValueId> events, std::vector<std::string> edges,
                                  bool latch = false) {
            std::vector<ValueId> operands{inputs[2], inputs[3], inputs[4]};
            operands.insert(operands.end(), events.begin(), events.end());
            std::vector<ObjectRef> refs{ObjectRef::state(state(model, bit))};
            for (auto event : events)
            {
                (void)event;
                refs.push_back(ObjectRef::state(state(model, bit)));
            }
            const std::array params{Parameter{model.intern("event_edges"), std::move(edges)}};
            model.addOperation(latch ? "core.state.latchWrite" : "core.state.regWrite",
                               operands, {}, refs, params);
        };
        addWrite({inputs[0]}, {"posedge"});
        addWrite({inputs[0]}, {"posedge"});
        addWrite({inputs[0]}, {"posedge"});
        addWrite({inputs[0]}, {"negedge"});
        addWrite({inputs[1]}, {"posedge"});
        addWrite({derived}, {"posedge"});
        addWrite({inputs[0], inputs[1], inputs[0]}, {"posedge", "negedge", "posedge"});
        addWrite({inputs[1], inputs[0]}, {"negedge", "posedge"});
        addWrite({}, {}, true);
        const std::array taskOperands{inputs[2], inputs[0]};
        const std::array taskRefs{ObjectRef::state(state(model, bit))};
        const std::array taskParams{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}},
                                   Parameter{model.intern("name"), std::string("$finish")}};
        model.addOperation("core.system.task", taskOperands, {}, taskRefs, taskParams);
        return model;
    }

    void unitTests()
    {
        for (auto type : {"core.state.regWrite", "core.state.latchWrite", "core.state.memWrite",
                          "core.state.memFill", "core.state.memAssign", "core.state.memWriteSeq"})
            require(isCpuCommitOp(type), "state writer omitted from commit classification");
        require(!isCpuCommitOp("core.dpi.call") && !isCpuCommitOp("core.system.task"),
                "side effects must remain compute ops");
        auto model = fixture();
        auto original = model.clone();
        const auto revision = model.semanticRevision();
        runPass(model, "cpu.st.split-phase");
        require(model.cpuMapping()->partitionTree.partitions.size() == 3, "split-phase tree shape");
        require(model.cpuMapping()->partitionTree.partitions[1].ops.size() == 7, "compute phase count");
        require(model.cpuMapping()->partitionTree.partitions[2].ops.size() == 9, "commit phase count");
        const std::array<std::string_view, 2> options{"--max-op-in-commit-supernode", "2"};
        runPass(model, "cpu.st.form-event-domains", options);
        const auto mapping = *model.cpuMapping();
        require(model.semanticRevision() == revision && model.operations().size() == original.operations().size() &&
                model.states().size() == original.states().size(), "mapping mutated semantic model");
        std::size_t inputDomains = 0, derivedDomains = 0, generalDomains = 0, chunks = 0;
        for (const auto &partition : mapping.partitionTree.partitions)
        {
            if (partition.attrs.kind == CpuPartitionKind::EventDomain)
            {
                if (!partition.attrs.eventGate) ++generalDomains;
                else if (partition.attrs.eventGate->source == CpuEventSource::Input) ++inputDomains;
                else ++derivedDomains;
            }
            if (partition.attrs.kind == CpuPartitionKind::Supernode)
            {
                require(partition.ops.size() <= 2, "chunk limit ignored");
                ++chunks;
            }
        }
        require(inputDomains == 4 && derivedDomains == 1 && generalDomains == 1 && chunks == 7,
                "event canonicalization, source classification or chunk packing failed");
        std::ostringstream first, second;
        diag::Diagnostics ioDiagnostics;
        require(writeGrhSimJson(model, first, defaultDialectRegistry(), ioDiagnostics), "mapping store failed");
        std::istringstream input(first.str());
        auto loaded = readGrhSimJson(input, defaultDialectRegistry(), ioDiagnostics);
        if (!loaded)
            for (const auto &message : ioDiagnostics.messages()) std::cerr << message.message << '\n';
        require(bool(loaded) && loaded->cpuMapping(), "mapping load lost typed payload");
        require(writeGrhSimJson(*loaded, second, defaultDialectRegistry(), ioDiagnostics) && first.str() == second.str(),
                "mapping JSON round trip is unstable");
        auto clone = model.clone();
        diag::Diagnostics cloneDiagnostics;
        require(clone.identity() != model.identity() && verifyGrhSimModel(clone, defaultDialectRegistry(), cloneDiagnostics),
                "clone did not rebind CPU mapping identity");
        clone.commitSemanticMutation();
        require(!clone.cpuMapping() && clone.mappings().empty(), "semantic mutation retained stale CPU mapping");

        const auto reject = [&](CpuBackendMapping bad) {
            auto broken = model.clone();
            broken.setCpuMapping(std::move(bad));
            diag::Diagnostics diagnostics;
            require(!verifyGrhSimModel(broken, defaultDialectRegistry(), diagnostics) && diagnostics.hasError(),
                    "verifier accepted corrupted CPU mapping");
        };
        auto bad = mapping;
        bad.partitionTree.partitions[1].ops.pop_back(); reject(bad);
        bad = mapping;
        bad.partitionTree.partitions[1].ops.push_back(bad.partitionTree.partitions[1].ops.front()); reject(bad);
        bad = mapping;
        bad.partitionTree.partitions[1].attrs.phase = CpuPhase::Commit; reject(bad);
        bad = mapping;
        bad.partitionTree.partitions.back().parent = {}; reject(bad);
        bad = mapping;
        bad.partitionTree.partitions[3].attrs.eventGate->events[0].edge = CpuEventEdge::Negedge; reject(bad);
        bad = mapping;
        bad.partitionTree.partitions[3].attrs.eventGate->source = CpuEventSource::Derived; reject(bad);
        bad = mapping;
        bad.partitionTree.partitions[1].children.push_back(mapping.partitionTree.root); reject(bad);

        std::string error;
        const std::array<std::string_view, 2> invalidOptions{"--max-op-in-commit-supernode", "0"};
        require(!defaultPassRegistry().create("cpu.st.form-event-domains", invalidOptions, error), "accepted zero chunk limit");
        auto pass = defaultPassRegistry().create("cpu.st.form-event-domains", {}, error);
        diag::Diagnostics missingDiagnostics;
        require(!pass->run(original, missingDiagnostics).success && !original.cpuMapping(),
                "event-domain pass accepted a missing prerequisite");
        runPass(model, "cpu.st.split-phase");
        require(model.cpuMapping()->stage == CpuMappingStage::SplitPhase &&
                model.cpuMapping()->partitionTree.partitions.size() == 3, "split did not invalidate downstream tree");
        GrhSimModel empty("empty");
        empty.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        runPass(empty, "cpu.st.split-phase");
        runPass(empty, "cpu.st.form-event-domains");
    }

    void roundTrip(const GrhSimModel &model)
    {
        diag::Diagnostics diagnostics;
        std::ostringstream first, second;
        require(writeGrhSimJson(model, first, defaultDialectRegistry(), diagnostics), "partition stage store failed");
        std::istringstream input(first.str());
        auto loaded = readGrhSimJson(input, defaultDialectRegistry(), diagnostics);
        if (!loaded)
            for (const auto &message : diagnostics.messages()) std::cerr << message.message << '\n';
        require(bool(loaded), "partition stage load failed");
        require(writeGrhSimJson(*loaded, second, defaultDialectRegistry(), diagnostics) && first.str() == second.str(),
                "partition stage round trip changed bytes");
    }

    GrhSimModel independentOutputs(uint32_t count)
    {
        GrhSimModel model("independent_outputs");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto type = model.logicType(8, false, LogicDomain::TwoState);
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto input = model.addInput("i" + std::to_string(i), type);
            const auto output = model.addOutput("o" + std::to_string(i), type);
            const auto value = model.addValue(type);
            const std::array values{value};
            const std::array inRefs{ObjectRef::input(input)}, outRefs{ObjectRef::output(output)};
            // Deliberately store users before definitions. Mapping order must follow dependencies.
            model.addOperation("core.output.write", values, {}, outRefs);
            model.addOperation("core.input.read", {}, values, inRefs);
        }
        return model;
    }

    std::size_t countKind(const GrhSimModel &model, CpuPartitionKind kind)
    {
        const auto &partitions = model.cpuMapping()->partitionTree.partitions;
        return std::count_if(partitions.begin(), partitions.end(), [kind](const auto &partition) {
            return partition.attrs.kind == kind;
        });
    }

    void partitionTests()
    {
        auto model = independentOutputs(41);
        const auto revision = model.semanticRevision();
        runPass(model, "cpu.st.split-phase");
        runPass(model, "cpu.st.form-event-domains");
        const std::array<std::string_view, 2> singleNode{"--max-op-in-compute-node", "1"};
        runPass(model, "cpu.st.build-compute-nodes", singleNode);
        require(countKind(model, CpuPartitionKind::Node) == 82, "node op limit was not respected");
        roundTrip(model);
        auto wrongOrder = model.clone();
        auto wrongMapping = *wrongOrder.cpuMapping();
        auto &children = wrongMapping.partitionTree.partitions[1].children;
        std::swap(children[0], children[1]);
        wrongOrder.setCpuMapping(std::move(wrongMapping));
        diag::Diagnostics orderDiagnostics;
        require(!verifyGrhSimModel(wrongOrder, defaultDialectRegistry(), orderDiagnostics), "accepted use-before-definition node order");

        auto packed = model.clone();
        const std::array<std::string_view, 2> fourOps{"--max-op-in-compute-supernode", "4"};
        runPass(packed, "cpu.st.merge-compute-supernodes", fourOps);
        require(countKind(packed, CpuPartitionKind::Supernode) == 21,
                "coarsen plus DP failed to pack independent two-op cones into four-op supernodes");
        roundTrip(packed);
        const std::array<std::string_view, 2> oneOp{"--max-op-in-compute-supernode", "1"};
        runPass(model, "cpu.st.merge-compute-supernodes", oneOp);
        require(countKind(model, CpuPartitionKind::Supernode) == 82, "supernode op limit was not respected");
        const std::array<std::string_view, 2> helperLimit{"--helper-max-estimated-lines", "1"};
        runPass(model, "cpu.st.pack-active-words", helperLimit);
        require(countKind(model, CpuPartitionKind::ActiveWord) == 11, "active word packing did not use eight lanes");
        roundTrip(model);
        auto corrupted = *model.cpuMapping();
        for (auto &partition : corrupted.partitionTree.partitions)
            if (partition.attrs.activeId) { partition.attrs.activeId = 999; break; }
        auto bad = model.clone(); bad.setCpuMapping(std::move(corrupted));
        diag::Diagnostics activityDiagnostics;
        require(!verifyGrhSimModel(bad, defaultDialectRegistry(), activityDiagnostics), "accepted non-contiguous activity IDs");
        corrupted = *model.cpuMapping();
        for (auto &partition : corrupted.partitionTree.partitions)
            if (!partition.attrs.helperChunks.empty()) { partition.attrs.helperChunks.front().offset = 1; break; }
        bad = model.clone(); bad.setCpuMapping(std::move(corrupted));
        diag::Diagnostics helperDiagnostics;
        require(!verifyGrhSimModel(bad, defaultDialectRegistry(), helperDiagnostics), "accepted helper coverage gap");
        const std::array<std::string_view, 6> functionLimits{
            "--batch-max-ops", "1", "--batch-max-estimated-lines", "1", "--target-batch-count", "0"};
        runPass(model, "cpu.st.pack-emit-functions", functionLimits);
        require(countKind(model, CpuPartitionKind::EmitFunction) == 11, "function packing split an active word");
        require(model.semanticRevision() == revision && model.operations().size() == 82,
                "partition passes mutated semantic IR");
        roundTrip(model);

        auto multi = fixture();
        runPass(multi, "cpu.st.split-phase"); runPass(multi, "cpu.st.form-event-domains");
        for (auto name : {"cpu.st.build-compute-nodes", "cpu.st.merge-compute-supernodes",
                          "cpu.st.pack-active-words", "cpu.st.pack-emit-functions", "cpu.st.layout-data"})
        { runPass(multi, name); roundTrip(multi); }
        auto empty = independentOutputs(0);
        for (auto name : {"cpu.st.split-phase", "cpu.st.form-event-domains", "cpu.st.build-compute-nodes",
                          "cpu.st.merge-compute-supernodes", "cpu.st.pack-active-words", "cpu.st.pack-emit-functions",
                          "cpu.st.layout-data"})
        { runPass(empty, name); roundTrip(empty); }

        GrhSimModel cycle("cycle");
        cycle.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto value = cycle.addValue(cycle.logicType(1, false, LogicDomain::TwoState));
        const std::array values{value};
        cycle.addOperation("core.compute.not", values, values);
        runPass(cycle, "cpu.st.split-phase"); runPass(cycle, "cpu.st.form-event-domains");
        std::string error;
        PassManager manager(defaultDialectRegistry());
        manager.addPass(defaultPassRegistry().create("cpu.st.build-compute-nodes", {}, error));
        diag::Diagnostics diagnostics;
        require(!manager.run(cycle, diagnostics).success && !cycle.poisoned() &&
                cycle.cpuMapping()->stage == CpuMappingStage::EventDomains, "cycle rejection corrupted input mapping");
    }

    void prepareLayout(GrhSimModel &model, bool separate = false)
    {
        runPass(model, "cpu.st.split-phase"); runPass(model, "cpu.st.form-event-domains");
        const std::array<std::string_view, 2> nodeLimit{"--max-op-in-compute-node", "1"};
        const std::array<std::string_view, 2> superLimit{"--max-op-in-compute-supernode", "1"};
        runPass(model, "cpu.st.build-compute-nodes", separate ? std::span<const std::string_view>(nodeLimit) : std::span<const std::string_view>{});
        runPass(model, "cpu.st.merge-compute-supernodes", separate ? std::span<const std::string_view>(superLimit) : std::span<const std::string_view>{});
        const std::array<std::string_view, 2> helperLimit{"--helper-max-estimated-lines", "1"};
        runPass(model, "cpu.st.pack-active-words", helperLimit);
        runPass(model, "cpu.st.pack-emit-functions");
    }

    void layoutTests()
    {
        require(defaultDialectRegistry().hasType("cpu.array") && defaultDialectRegistry().hasType("cpu.uint64") &&
                !defaultDialectRegistry().hasOp("cpu.compute.add"), "CPU type-only dialect is not registered");
        auto model = independentOutputs(1);
        std::vector<TypeId> semanticTypes;
        for (uint32_t width : {1, 7, 8, 9, 16, 17, 32, 33, 64, 65, 129})
            for (bool sign : {false, true})
                for (auto domain : {LogicDomain::TwoState, LogicDomain::FourState})
                {
                    const auto type = model.logicType(width, sign, domain);
                    model.addInput("typed" + std::to_string(semanticTypes.size()), type);
                    semanticTypes.push_back(type);
                }
        const auto wide = model.logicType(65, true, LogicDomain::FourState);
        const auto nested = model.arrayType(model.arrayType(wide, 3), 5);
        const auto nestedInput = model.addInput("nested", nested);
        const auto realInput = model.addInput("real", model.realType());
        const auto stringOutput = model.addOutput("string", model.stringType());
        const auto function = model.addExternFunction("dpi", "core.dpi", "dpi", {}, model.logicType(8, false, LogicDomain::TwoState));
        const auto dpiValue = model.addValue(model.logicType(8, false, LogicDomain::TwoState));
        const std::array dpiResults{dpiValue};
        const std::array dpiRefs{ObjectRef::function(function)};
        model.addOperation("core.dpi.call", {}, dpiResults, dpiRefs);
        const auto revision = model.semanticRevision();
        const auto semanticTypeCount = model.types().size();
        prepareLayout(model);
        runPass(model, "cpu.st.layout-data");
        const auto &layout = *model.cpuMapping()->dataLayout;
        require(model.semanticRevision() == revision && model.types().size() == semanticTypeCount,
                "layout modified semantic types/revision");
        require(layout.objects.size() == model.inputs().size() + model.outputs().size(), "layout covered function as data");
        for (std::size_t i = 0; i < semanticTypes.size(); ++i)
        {
            const auto &semantic = model.types()[semanticTypes[i].index - 1];
            const auto &slot = layout.objects[i + 1].slot;
            const auto &physical = layout.types[slot.type.index - 1];
            const uint64_t scalarBytes = semantic.width <= 8 ? 1 : semantic.width <= 16 ? 2 :
                                         semantic.width <= 32 ? 4 : semantic.width <= 64 ? 8 :
                                         ((uint64_t(semantic.width) + 63) / 64) * 8;
            require(physical.size == scalarBytes * (semantic.domain == LogicDomain::FourState ? 2 : 1),
                    "wrong scalar/four-state CPU byte size");
            const auto &scalar = semantic.domain == LogicDomain::FourState ?
                                 layout.types[physical.elementType.index - 1] : physical;
            require(scalar.kind == (semantic.isSigned ? CpuTypeKind::SInt :
                                    semantic.width == 1 ? CpuTypeKind::Bool : CpuTypeKind::UInt), "wrong CPU signedness");
            require(slot.offset % physical.alignment == 0, "unaligned object");
        }
        require(layout.types[layout.objects[nestedInput.index - 1].slot.type.index - 1].size == 480,
                "recursive array stride/size is wrong");
        require(layout.types[layout.objects[realInput.index - 1].slot.type.index - 1].kind == CpuTypeKind::F64,
                "real did not map to f64");
        require(layout.types[layout.objects[model.inputs().size() + stringOutput.index - 1].slot.type.index - 1].size == 8,
                "string pointer ABI is wrong");
        require(layout.values[0].kind == CpuStorageKind::PartitionLocal, "helper-crossing local value became boundary");
        require(layout.values[dpiValue.index - 1].kind == CpuStorageKind::Boundary, "DPI result lost persistent storage");
        roundTrip(model);

        const auto reject = [&](CpuBackendMapping bad) {
            auto broken = model.clone(); broken.setCpuMapping(std::move(bad));
            diag::Diagnostics diagnostics;
            require(!verifyGrhSimModel(broken, defaultDialectRegistry(), diagnostics), "accepted malformed CPU layout");
        };
        auto bad = *model.cpuMapping(); bad.dataLayout->objects.pop_back(); reject(bad);
        bad = *model.cpuMapping(); bad.dataLayout->values[0].offset++; reject(bad);
        bad = *model.cpuMapping(); bad.dataLayout->values[0].owner = {}; reject(bad);
        bad = *model.cpuMapping(); bad.dataLayout->values[dpiValue.index - 1].kind = CpuStorageKind::PartitionLocal; reject(bad);
        bad = *model.cpuMapping(); bad.dataLayout->types[0].size++; reject(bad);
        bad = *model.cpuMapping(); bad.dataLayout->pointerBytes = 4; reject(bad);
        bad = *model.cpuMapping(); bad.dataLayout->runtime.pop_back(); reject(bad);
        bad = *model.cpuMapping(); bad.dataLayout->localFrames[0].size++; reject(bad);
        bad = *model.cpuMapping(); bad.dataLayout.reset(); reject(bad);
        bad = *model.cpuMapping(); bad.stage = CpuMappingStage::EmitFunctions; reject(bad);

        auto separated = independentOutputs(1);
        prepareLayout(separated, true); runPass(separated, "cpu.st.layout-data");
        require(separated.cpuMapping()->dataLayout->values[0].kind == CpuStorageKind::Boundary,
                "cross-supernode value allocated locally");
        auto multi = fixture(); prepareLayout(multi); runPass(multi, "cpu.st.layout-data");
        const auto &multiLayout = *multi.cpuMapping()->dataLayout;
        require(std::count_if(multiLayout.runtime.begin(), multiLayout.runtime.end(), [](const auto &slot) {
                    return slot.kind == CpuRuntimeKind::DomainArm;
                }) == 5, "general domain got an arm or edge domain missed one");
        for (const auto &partition : multi.cpuMapping()->partitionTree.partitions)
            if (partition.attrs.eventGate)
                for (auto event : partition.attrs.eventGate->events)
                    require(multiLayout.values[event.value.index - 1].kind == CpuStorageKind::Boundary,
                            "event value lost persistent storage");
        roundTrip(multi);

        auto overflow = independentOutputs(0);
        overflow.addInput("huge", overflow.arrayType(overflow.logicType(64, false, LogicDomain::TwoState),
                                                    std::numeric_limits<uint64_t>::max()));
        prepareLayout(overflow);
        std::string error;
        const std::array<std::string_view, 1> invalid{"--unknown"};
        require(!defaultPassRegistry().create("cpu.st.layout-data", invalid, error), "layout accepted unknown option");
        PassManager manager(defaultDialectRegistry());
        manager.addPass(defaultPassRegistry().create("cpu.st.layout-data", {}, error));
        diag::Diagnostics diagnostics;
        require(!manager.run(overflow, diagnostics).success && !overflow.poisoned() &&
                overflow.cpuMapping()->stage == CpuMappingStage::EmitFunctions && !overflow.cpuMapping()->dataLayout,
                "layout overflow was accepted or corrupted input mapping");
        auto arenaOverflow = independentOutputs(0);
        arenaOverflow.addInput("large", arenaOverflow.arrayType(arenaOverflow.logicType(8, false, LogicDomain::TwoState),
                                                               std::numeric_limits<uint64_t>::max() - 7));
        arenaOverflow.addInput("tail", arenaOverflow.logicType(64, false, LogicDomain::TwoState));
        prepareLayout(arenaOverflow);
        diag::Diagnostics arenaDiagnostics;
        require(!manager.run(arenaOverflow, arenaDiagnostics).success && !arenaOverflow.poisoned() &&
                !arenaOverflow.cpuMapping()->dataLayout, "arena allocation overflow corrupted mapping");
        auto missing = independentOutputs(0);
        auto pass = defaultPassRegistry().create("cpu.st.layout-data", {}, error);
        diag::Diagnostics missingDiagnostics;
        require(!pass->run(missing, missingDiagnostics).success && !missing.cpuMapping(),
                "layout accepted a missing partition prerequisite");
        runPass(model, "cpu.st.split-phase");
        require(!model.cpuMapping()->dataLayout, "upstream rerun retained stale layout");
    }

    void densifyBoundaryTests()
    {
        const auto offsetsOf = [](const GrhSimModel &model) {
            std::vector<uint64_t> offsets;
            for (const auto &slot : model.cpuMapping()->dataLayout->values) offsets.push_back(slot.offset);
            return offsets;
        };
        const auto build = [&](bool gated, std::vector<ValueId> &narrow, std::vector<ValueId> &excluded) {
            GrhSimModel model("densify");
            model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto wide = model.logicType(64, false, LogicDomain::TwoState);
            const auto quad = model.logicType(1, false, LogicDomain::FourState);
            const auto clk = model.addInput("clk", bit);
            const auto data = model.addInput("data", bit);
            const auto wideIn = model.addInput("wide", wide);
            const auto quadIn = model.addInput("quad", quad);
            const auto read = [&](ObjectRef ref, TypeId type) {
                const auto value = model.addValue(type);
                const std::array results{value};
                const std::array refs{ref};
                model.addOperation("core.input.read", {}, results, refs);
                return value;
            };
            const auto clkV = read(ObjectRef::input(clk), bit);
            const auto dataV = read(ObjectRef::input(data), bit);
            const auto wideV = read(ObjectRef::input(wideIn), wide);
            const auto quadV = read(ObjectRef::input(quadIn), quad);
            const auto join = [&](ValueId a, ValueId b, TypeId type) {
                const auto value = model.addValue(type);
                const std::array operands{a, b};
                const std::array results{value};
                model.addOperation("core.compute.and", operands, results);
                return value;
            };
            const auto n0 = join(clkV, dataV, bit);
            const auto n1 = join(n0, dataV, bit);
            const auto n2 = join(n1, dataV, bit);
            const auto w0 = join(wideV, wideV, wide);
            const auto ev = join(clkV, n2, bit);
            const std::array operands{n0, n1, n2, w0, quadV, ev};
            const std::array refs{ObjectRef::state(state(model, bit))};
            std::vector<Parameter> params{Parameter{model.intern("name"), std::string("$endpoint")}};
            if (gated) params.push_back(Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}});
            model.addOperation("core.system.task", operands, {}, refs, params);
            prepareLayout(model, true);
            runPass(model, "cpu.st.layout-data");
            narrow = {n0, n1, n2, ev};
            excluded = {w0, quadV};
            return model;
        };
        std::vector<ValueId> narrow, excluded;
        auto gated = build(true, narrow, excluded);
        const auto &layout = *gated.cpuMapping()->dataLayout;
        for (auto value : narrow)
        {
            require(layout.values[value.index - 1].kind == CpuStorageKind::Boundary,
                    "test endpoint input is not a boundary value");
            require(layout.values[value.index - 1].offset < narrow.size(),
                    "event-gated endpoint boundary input was not front-packed");
        }
        for (auto value : excluded)
        {
            require(layout.values[value.index - 1].kind == CpuStorageKind::Boundary,
                    "test excluded endpoint input is not a boundary value");
            require(layout.values[value.index - 1].offset >= narrow.size(),
                    "wide or four-state endpoint input entered the dense tier");
        }
        std::set<uint64_t> seen;
        for (const auto &slot : layout.values)
            if (slot.kind == CpuStorageKind::Boundary)
                require(seen.insert(slot.offset).second, "densified layout lost offset uniqueness");
        auto control = build(false, narrow, excluded);
        const auto &controlLayout = *control.cpuMapping()->dataLayout;
        bool allFront = true;
        for (auto value : narrow)
            allFront = allFront && controlLayout.values[value.index - 1].offset < narrow.size();
        require(!allFront, "ungated endpoint inputs must keep the legacy tiers");
        auto again = build(true, narrow, excluded);
        require(offsetsOf(gated) == offsetsOf(again), "densified layout is not deterministic");
        roundTrip(gated);

        const uint32_t groupSize = 9000;  // two groups exceed the 16 KiB budget together
        GrhSimModel budget("densify_budget");
        budget.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = budget.logicType(1, false, LogicDomain::TwoState);
        const auto clk = budget.addInput("clk", bit);
        const auto data = budget.addInput("data", bit);
        const auto read = [&](ObjectRef ref) {
            const auto value = budget.addValue(bit);
            const std::array results{value};
            const std::array refs{ref};
            budget.addOperation("core.input.read", {}, results, refs);
            return value;
        };
        const auto clkV = read(ObjectRef::input(clk));
        const auto dataV = read(ObjectRef::input(data));
        std::vector<std::vector<ValueId>> groups(2);
        for (auto &group : groups)
        {
            std::vector<ValueId> operands;
            for (uint32_t i = 0; i + 1 < groupSize; ++i)
            {
                const auto value = budget.addValue(bit);
                const std::array ops{clkV, dataV};
                const std::array results{value};
                budget.addOperation("core.compute.and", ops, results);
                operands.push_back(value);
                group.push_back(value);
            }
            const auto ev = budget.addValue(bit);
            const std::array ops{clkV, dataV};
            const std::array results{ev};
            budget.addOperation("core.compute.and", ops, results);
            operands.push_back(ev);
            group.push_back(ev);
            const std::array refs{ObjectRef::state(state(budget, bit))};
            const std::array params{Parameter{budget.intern("name"), std::string("$endpoint")},
                                    Parameter{budget.intern("event_edges"), std::vector<std::string>{"posedge"}}};
            budget.addOperation("core.system.task", operands, {}, refs, params);
        }
        prepareLayout(budget, true);
        runPass(budget, "cpu.st.layout-data");
        const auto &budgetLayout = *budget.cpuMapping()->dataLayout;
        const auto frontCount = [&](const std::vector<ValueId> &group) {
            uint64_t count = 0;
            for (auto value : group)
                if (budgetLayout.values[value.index - 1].offset < groupSize) ++count;
            return count;
        };
        const auto packedA = frontCount(groups[0]) == groupSize, packedB = frontCount(groups[1]) == groupSize;
        require(packedA != packedB, "budget must select exactly one whole group");
        require(frontCount(packedA ? groups[1] : groups[0]) == 0, "over-budget group was partially densified");
        uint64_t frontTotal = 0;
        for (const auto &slot : budgetLayout.values)
            if (slot.kind == CpuStorageKind::Boundary && slot.offset < groupSize) ++frontTotal;
        require(frontTotal == groupSize, "dense tier carries non-group values");
    }

    void layoutCheckpoint(const std::filesystem::path &input, const std::filesystem::path &output)
    {
        diag::Diagnostics diagnostics;
        auto model = loadGrhSimModel(input, defaultDialectRegistry(), diagnostics);
        require(bool(model), "layout checkpoint load failed");
        runPass(*model, "cpu.st.layout-data");
        require(storeGrhSimModel(*model, output, defaultDialectRegistry(), diagnostics), "layout checkpoint store failed");
        auto loaded = loadGrhSimModel(output, defaultDialectRegistry(), diagnostics);
        require(bool(loaded) && loaded->cpuMapping()->dataLayout == model->cpuMapping()->dataLayout,
                "layout checkpoint reload differs");
        require(storeGrhSimModel(*loaded, output.string() + ".roundtrip.json", defaultDialectRegistry(), diagnostics),
                "layout checkpoint roundtrip store failed");
    }

    void checkpointTest(const std::filesystem::path &input, const std::filesystem::path &output)
    {
        diag::Diagnostics diagnostics;
        auto model = loadGrhSimModel(input, defaultDialectRegistry(), diagnostics);
        require(bool(model), "checkpoint load failed");
        runPass(*model, "cpu.st.split-phase");
        runPass(*model, "cpu.st.form-event-domains");
        std::vector<OpId> producers(model->values().size() + 1);
        for (const auto &op : model->operations())
            for (auto value : model->results(op)) producers[value.index] = op.id;
        std::size_t domains = 0;
        for (const auto &partition : model->cpuMapping()->partitionTree.partitions)
        {
            if (partition.attrs.kind != CpuPartitionKind::EventDomain) continue;
            ++domains;
            std::size_t writes = 0;
            for (auto child : partition.children)
                writes += model->cpuMapping()->partitionTree.partitions[child.index - 1].ops.size();
            std::cout << "domain " << partition.id.index << " writes=" << writes;
            if (partition.attrs.eventGate)
            {
                for (auto event : partition.attrs.eventGate->events)
                {
                    std::cout << " event=" << event.value.index << ':' << (event.edge == CpuEventEdge::Posedge ? "pos" : "neg");
                    auto value = event.value;
                    for (unsigned depth = 0; depth < 5; ++depth)
                    {
                        const auto &producer = model->operations()[producers[value.index].index - 1];
                        std::cout << " <- " << model->text(producer.opType);
                        for (auto ref : model->objectRefs(producer))
                        {
                            if (ref.kind == ObjectKind::Input) std::cout << '(' << model->text(model->inputs()[ref.index - 1].name) << ')';
                            if (ref.kind == ObjectKind::State) std::cout << '(' << model->text(model->states()[ref.index - 1].name) << ')';
                        }
                        auto operands = model->operands(producer);
                        if (operands.size() != 1) break;
                        value = operands[0];
                    }
                }
            }
            else std::cout << " general";
            std::cout << '\n';
        }
        for (auto name : {"cpu.st.build-compute-nodes", "cpu.st.merge-compute-supernodes",
                          "cpu.st.pack-active-words", "cpu.st.pack-emit-functions"}) runPass(*model, name);
        require(storeGrhSimModel(*model, output, defaultDialectRegistry(), diagnostics), "mapped checkpoint store failed");
        model.reset();
        auto loaded = loadGrhSimModel(output, defaultDialectRegistry(), diagnostics);
        require(bool(loaded) && loaded->cpuMapping(), "mapped checkpoint reload failed");
        const auto roundtrip = output.string() + ".roundtrip.json";
        require(storeGrhSimModel(*loaded, roundtrip, defaultDialectRegistry(), diagnostics), "mapped checkpoint round trip failed");
        require(std::filesystem::file_size(output) == std::filesystem::file_size(roundtrip), "checkpoint size changed");
        std::ifstream a(output, std::ios::binary), b(roundtrip, std::ios::binary);
        std::array<char, 65536> abuf{}, bbuf{};
        while (a)
        {
            a.read(abuf.data(), abuf.size()); b.read(bbuf.data(), bbuf.size());
            require(a.gcount() == b.gcount() && std::equal(abuf.begin(), abuf.begin() + a.gcount(), bbuf.begin()),
                    "checkpoint bytes changed");
        }
        std::cout << "CPU mapping checkpoint: " << domains << " event domains, stable round trip\n";
    }

    void inspectCheckpoint(const std::filesystem::path &input)
    {
        diag::Diagnostics diagnostics;
        auto model = loadGrhSimModel(input, defaultDialectRegistry(), diagnostics);
        require(bool(model) && model->cpuMapping(), "mapping statistics require a valid CPU checkpoint");
        const auto &tree = model->cpuMapping()->partitionTree;
        std::vector<uint32_t> nodeSizes, supernodeSizes, functionSizes;
        uint64_t helpers = 0, helperSupernodes = 0, computeFunctions = 0, commitFunctions = 0;
        const auto opCount = [&](PartitionId root) {
            uint32_t count = 0;
            std::vector<PartitionId> stack{root};
            while (!stack.empty())
            {
                const auto &p = tree.partitions[stack.back().index - 1]; stack.pop_back();
                count += p.ops.size(); stack.insert(stack.end(), p.children.begin(), p.children.end());
            }
            return count;
        };
        for (const auto &p : tree.partitions)
        {
            if (p.attrs.kind == CpuPartitionKind::Node) nodeSizes.push_back(p.ops.size());
            if (p.attrs.activeId) supernodeSizes.push_back(opCount(p.id));
            if (!p.attrs.helperChunks.empty()) { ++helperSupernodes; helpers += p.attrs.helperChunks.size(); }
            if (p.attrs.kind == CpuPartitionKind::EmitFunction)
            {
                functionSizes.push_back(opCount(p.id));
                if (tree.partitions[p.parent.index - 1].attrs.kind == CpuPartitionKind::EventDomain) ++commitFunctions;
                else ++computeFunctions;
            }
        }
        const auto distribution = [](std::string_view label, std::vector<uint32_t> values) {
            std::sort(values.begin(), values.end());
            uint64_t total = 0;
            for (auto value : values) total += value;
            const auto percentile = [&](uint32_t p) { return values.empty() ? 0 : values[(values.size() - 1) * p / 100]; };
            std::cout << label << " count=" << values.size() << " ops=" << total << " p50=" << percentile(50)
                      << " p90=" << percentile(90) << " max=" << percentile(100) << '\n';
        };
        distribution("nodes", std::move(nodeSizes));
        distribution("compute_supernodes", std::move(supernodeSizes));
        distribution("functions", std::move(functionSizes));
        std::cout << "compute_functions=" << computeFunctions << " commit_functions=" << commitFunctions
                  << " active_words=" << countKind(*model, CpuPartitionKind::ActiveWord)
                  << " helper_supernodes=" << helperSupernodes << " helper_chunks=" << helpers << '\n';
        if (model->cpuMapping()->dataLayout)
        {
            const auto &layout = *model->cpuMapping()->dataLayout;
            uint64_t localBytes = 0, maxFrame = 0;
            for (const auto &frame : layout.localFrames)
            { localBytes += frame.size; maxFrame = std::max(maxFrame, frame.size); }
            std::cout << "cpu_types=" << layout.types.size() << " objects=" << layout.objects.size()
                      << " values=" << layout.values.size() << " boundary_values="
                      << std::count_if(layout.values.begin(), layout.values.end(), [](const auto &slot) {
                             return slot.kind == CpuStorageKind::Boundary;
                         })
                      << " object_bytes=" << layout.objectBytes << " boundary_bytes=" << layout.boundaryBytes
                      << " local_frame_bytes_sum=" << localBytes << " local_frame_bytes_max=" << maxFrame
                      << " runtime_bytes=" << layout.runtimeBytes << '\n';
        }
    }
}

int main(int argc, char **argv)
{
    try
    {
        if (argc == 3 && std::string_view(argv[1]) == "--inspect")
        { inspectCheckpoint(argv[2]); return 0; }
        unitTests();
        partitionTests();
        layoutTests();
        densifyBoundaryTests();
        if (argc == 4 && std::string_view(argv[1]) == "--layout")
        { layoutCheckpoint(argv[2], argv[3]); return 0; }
        if (argc == 3) checkpointTest(argv[1], argv[2]);
        else require(argc == 1, "usage: grhsim-cpu-mapping-tests [xiangshan-ir.json mapped-output.json]");
        std::cout << "CPU mapping tests passed\n";
        return 0;
    }
    catch (const std::exception &ex)
    {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
