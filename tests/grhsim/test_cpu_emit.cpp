#include "grhsim/backend/cpu_emit.hpp"
#include "grhsim/backend/cpu_block_share.hpp"
#include "grhsim/backend/cpu_shape_share.hpp"
#include "emit/readmem.hpp"
#include "grhsim/dialect/registry.hpp"
#include "grhsim/io/json.hpp"
#include "grhsim/ir/model.hpp"
#include "grhsim/ir/verifier.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

namespace
{
    using namespace wolvrix::lib;
    using namespace grhsim;

    void require(bool condition, std::string_view message)
    { if (!condition) throw std::runtime_error(std::string(message)); }

    std::string quote(std::string_view text)
    {
        std::string result = "'";
        for (char c : text) result += c == '\'' ? "'\\''" : std::string(1, c);
        return result + "'";
    }

    void command(const std::string &cmd)
    { std::cout << cmd << std::endl; require(std::system(cmd.c_str()) == 0, "generated CPU compilation or simulation failed"); }

    std::set<uint32_t> emittedDirectStates(const std::filesystem::path &directory)
    {
        std::set<uint32_t> result;
        constexpr std::string_view marker = "// cpu_direct_commit state=";
        for (const auto &entry : std::filesystem::directory_iterator(directory))
        {
            if (entry.path().extension() != ".cpp") continue;
            std::ifstream stream(entry.path());
            for (std::string line; std::getline(stream, line);)
                if (const auto pos = line.find(marker); pos != std::string::npos)
                    result.insert(static_cast<uint32_t>(std::stoul(line.substr(pos + marker.size()))));
        }
        return result;
    }

    unsigned checkSamplingTasks(const GrhSimModel &model, const std::filesystem::path &directory)
    {
        unsigned count = 0;
        for (const auto &task : model.cpuMapping()->schedule->numaNodes[0].cores[0].tasks)
        {
            const auto path = directory / ("grhsim_" + std::string(model.text(model.name())) +
                "_task_" + std::to_string(task.id.index) + ".cpp");
            std::ifstream stream(path); require(bool(stream), "sampling check could not read task");
            const std::string source{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
            const bool sampled = source.find("// cpu_inactive_edge_sample task=") != std::string::npos;
            const auto &tree = model.cpuMapping()->partitionTree;
            const auto &function = tree.partitions[task.partition.index - 1];
            const auto &gate = tree.partitions[function.parent.index - 1].attrs.eventGate;
            require(sampled == (task.execution == CpuExecution::DomainGatedCommit && gate->events.size() <= 8),
                    "small edge domain missed sampling path or general/fallback domain used it");
            count += sampled;
        }
        return count;
    }

    unsigned checkActivityGuards(const GrhSimModel &model, const std::filesystem::path &directory)
    {
        std::string source;
        for (const auto &entry : std::filesystem::directory_iterator(directory))
            if (entry.path().extension() == ".cpp")
            {
                std::ifstream stream(entry.path());
                source.append(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
            }
        const auto &mapping = *model.cpuMapping();
        const auto &tree = mapping.partitionTree;
        const auto &layout = *mapping.dataLayout;
        std::vector<uint64_t> wordOffsets(tree.partitions.size() + 1);
        for (const auto &slot : layout.runtime)
            if (slot.kind == CpuRuntimeKind::ActiveWord) wordOffsets[slot.owner.index] = slot.offset;
        unsigned count = 0;
        for (const auto &task : mapping.schedule->numaNodes[0].cores[0].tasks)
        {
            if (task.execution != CpuExecution::ActivityDrivenCompute) continue;
            std::string expected = "if(";
            bool first = true;
            for (const auto word : tree.partitions[task.partition.index - 1].children)
            {
                if (!first) expected += "||";
                expected += "cpu_flags[" + std::to_string(wordOffsets[word.index]) + "]";
                first = false;
            }
            expected += ")cpu_task_" + std::to_string(task.id.index) + "();";
            require(source.find(expected) != std::string::npos, "activity-driven task is missing its outer activity guard");
            ++count;
        }
        require(count != 0, "fixture did not produce an activity-driven task");
        return count;
    }

    void checkBufferLocals(const std::filesystem::path &directory)
    {
        std::string source;
        for (const auto &entry : std::filesystem::directory_iterator(directory))
            if (entry.path().extension() == ".cpp" && entry.path().filename().string().find("_task_") != std::string::npos)
            {
                std::ifstream stream(entry.path());
                source.append(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
            }
        require(source.find("std::byte *__restrict const cpu_obj_=cpu_objects.get();") != std::string::npos,
                "task body missed the restrict-qualified objects buffer local");
        require(source.find("std::byte *__restrict const cpu_bnd_=cpu_boundary.get();") != std::string::npos,
                "task body missed the restrict-qualified boundary buffer local");
        require(source.find("(cpu_obj_,") != std::string::npos, "state accesses were not routed through the objects local");
        require(source.find("(cpu_bnd_,") != std::string::npos, "boundary accesses were not routed through the boundary local");
    }

    GrhSimModel fixture()
    {
        GrhSimModel model("cpu_chain"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState), byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto input = [&](const char *name, TypeId type) {
            const auto id = model.addInput(name, type); const auto value = model.addValue(type);
            const std::array results{value}; const std::array refs{ObjectRef::input(id)};
            model.addOperation("core.input.read", {}, results, refs); return value;
        };
        const auto clock = input("clock", bit), clockB = input("clock_b", bit), reset = input("reset", bit), enable = input("enable", bit);
        const auto data = input("data", byte);
        const auto constant = [&](TypeId type, const char *literal) {
            const auto value = model.addValue(type); const std::array results{value};
            const std::array params{Parameter{model.intern("value"), std::string(literal)}};
            model.addOperation("core.compute.constant", {}, results, {}, params); return value;
        };
        const auto one = constant(bit, "1'b1"), zero = constant(byte, "8'h00"), mask = constant(byte, "8'hff");
        const auto lowMask = constant(byte, "8'h0f"), highMask = constant(byte, "8'hf0");
        const auto state = [&](TypeId type) {
            const auto id = model.addState("s" + std::to_string(model.states().size()), type);
            const std::array params{Parameter{model.intern("value"), std::string("1'b0")}};
            const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}}; model.addInit(id, steps, params); return id;
        };
        const auto compute = [&](const char *type, TypeId resultType, std::initializer_list<ValueId> operands) {
            const auto value = model.addValue(resultType); const std::array results{value};
            model.addOperation(type, {operands.begin(), operands.size()}, results); return value;
        };
        const auto gate = compute("core.compute.and", bit, {clock, enable});
        const auto inverse = compute("core.compute.not", byte, {data});
        std::vector<StateId> registers; std::vector<ValueId> reads;
        for (const char *name : {"q1", "q2", "qg", "qn", "latched", "merged", "cancelled"})
        {
            const auto reg = state(byte); registers.push_back(reg); const auto value = model.addValue(byte); reads.push_back(value);
            const std::array results{value}; const std::array refs{ObjectRef::state(reg)};
            model.addOperation("core.state.read", {}, results, refs);
            const auto output = model.addOutput(name, byte); const std::array outputRefs{ObjectRef::output(output)};
            model.addOperation("core.output.write", results, {}, outputRefs);
        }
        const auto write = [&](uint32_t target, ValueId value, ValueId maskValue, ValueId event, const char *edge) {
            const auto next = compute("core.compute.mux", byte, {reset, zero, value});
            const std::array operands{one, next, maskValue, event, reset};
            const std::array refs{ObjectRef::state(registers[target]), ObjectRef::state(state(bit)), ObjectRef::state(state(bit))};
            const std::array params{Parameter{model.intern("event_edges"), std::vector<std::string>{edge, "posedge"}}};
            model.addOperation("core.state.regWrite", operands, {}, refs, params);
        };
        write(0, data, mask, clock, "posedge"); write(1, reads[0], mask, clock, "posedge");
        write(2, reads[1], mask, gate, "posedge"); write(3, data, mask, clockB, "negedge");
        write(5, data, lowMask, clock, "posedge"); write(5, inverse, highMask, clock, "posedge");
        write(6, data, mask, clock, "posedge"); write(6, zero, mask, clock, "posedge");
        const std::array latchOperands{enable, data, mask}; const std::array latchRefs{ObjectRef::state(registers[4])};
        model.addOperation("core.state.latchWrite", latchOperands, {}, latchRefs);
        return model;
    }

    void map(GrhSimModel &model, std::string_view maxSuperOps = "2", std::string_view helperLines = "1")
    {
        PassManager manager(defaultDialectRegistry()); std::string error;
        for (auto name : {"cpu.st.split-phase", "cpu.st.form-event-domains", "cpu.st.build-compute-nodes",
                          "cpu.st.merge-compute-supernodes", "cpu.st.pack-active-words", "cpu.st.pack-emit-functions",
                          "cpu.st.layout-data", "cpu.st.build-schedule"})
        {
            const std::array<std::string_view, 2> node{"--max-op-in-compute-node", "1"}, super{"--max-op-in-compute-supernode", maxSuperOps},
                helper{"--helper-max-estimated-lines", helperLines};
            std::span<const std::string_view> args;
            if (std::string_view(name) == "cpu.st.build-compute-nodes") args = node;
            if (std::string_view(name) == "cpu.st.merge-compute-supernodes") args = super;
            if (std::string_view(name) == "cpu.st.pack-active-words") args = helper;
            manager.addPass(defaultPassRegistry().create(name, args, error));
        }
        diag::Diagnostics diagnostics; const auto result = manager.run(model, diagnostics);
        for (const auto &message : diagnostics.messages()) std::cout << message.message << '\n';
        require(result.success, "fixture mapping failed");
    }

    void audit(const GrhSimModel &model)
    {
        std::map<std::string, uint64_t> counts;
        for (const auto &op : model.operations())
        {
            auto name = std::string(model.text(op.opType));
            if (name.starts_with("core.system.")) for (const auto &param : model.parameters(op))
                if (model.text(param.name) == "name") name += ":" + std::get<std::string>(param.value);
            ++counts[name];
            if (name.starts_with("core.system.") || name == "core.dpi.call")
            {
                auto detail = name;
                if (name == "core.dpi.call") detail += ":" + std::string(model.text(model.functions()[model.objectRefs(op)[0].index - 1].symbol));
                for (const auto &param : model.parameters(op))
                {
                    if (model.text(param.name) == "proc_kind") detail += ":" + std::get<std::string>(param.value);
                    if (model.text(param.name) == "event_edges") detail += ":events=" + std::to_string(std::get<std::vector<std::string>>(param.value).size());
                }
                ++counts["call_detail:" + detail];
            }
        }
        for (const auto &record : model.initRecords()) for (const auto &step : model.steps(record)) ++counts[std::string(model.text(step.kind))];
        for (const auto &[name, count] : counts) std::cout << name << '=' << count << '\n';
        for (const auto &function : model.functions())
        {
            const auto printType = [&](TypeId id) {
                if (!id) { std::cout << "void"; return; }
                const auto &type = model.types()[id.index - 1];
                std::cout << "kind" << unsigned(type.kind) << ':' << type.width << (type.isSigned ? 's' : 'u');
            };
            std::cout << "DPI " << model.text(function.symbol) << " return="; printType(function.returnType);
            for (const auto &arg : model.arguments(function))
            { std::cout << " arg=" << model.text(arg.name) << ':' << unsigned(arg.direction) << ':'; printType(arg.type); }
            std::cout << '\n';
        }
    }

    GrhSimModel scalarFixture()
    {
        GrhSimModel model("cpu_scalar"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState),
                   word = model.logicType(64, false, LogicDomain::TwoState),
                   signedWord = model.logicType(64, true, LogicDomain::TwoState),
                   byte = model.logicType(8, false, LogicDomain::TwoState);
        std::map<std::string, ValueId> inputs;
        for (auto name : {"a", "b", "sa", "sb", "sm"})
        {
            const auto type = std::string_view(name) == "sm" ? byte : name[0] == 's' ? signedWord : word;
            const auto input = model.addInput(name, type); const auto value = model.addValue(type);
            const std::array results{value}; const std::array refs{ObjectRef::input(input)};
            model.addOperation("core.input.read", {}, results, refs); inputs[name] = value;
        }
        const auto output = [&](std::string_view name, std::string_view operation, TypeId type,
                                std::initializer_list<ValueId> operands, std::span<const Parameter> parameters = {}) {
            const auto result = model.addValue(type); const std::array results{result};
            model.addOperation("core.compute." + std::string(operation), {operands.begin(), operands.size()}, results, {}, parameters);
            const auto object = model.addOutput(name, type); const std::array refs{ObjectRef::output(object)};
            model.addOperation("core.output.write", results, {}, refs);
        };
        const auto a = inputs.at("a"), b = inputs.at("b"), sa = inputs.at("sa"), sb = inputs.at("sb"), small = inputs.at("sm");
        for (auto op : {"add", "sub", "mul", "div", "mod", "and", "or", "xor", "xnor", "shl", "lshr"})
            output("out_" + std::string(op), op, word, {a, b});
        for (auto op : {"eq", "ne", "caseEq", "caseNe", "lt", "le", "gt", "ge", "logicAnd", "logicOr"})
            output("out_" + std::string(op), op, bit, {a, b});
        for (auto op : {"div", "mod", "ashr"}) output("out_s" + std::string(op), op, signedWord, {sa, sb});
        for (auto op : {"lt", "le", "gt", "ge"}) output("out_s" + std::string(op), op, bit, {sa, sb});
        for (auto op : {"reduceAnd", "reduceOr", "reduceXor", "reduceNand", "reduceNor", "reduceXnor", "logicNot"})
            output("out_" + std::string(op), op, bit, {a});
        output("out_not", "not", word, {a}); output("out_assign", "assign", word, {small});
        output("out_mux", "mux", word, {small, a, b});
        const auto pair = model.logicType(16, false, LogicDomain::TwoState);
        output("out_concat", "concat", pair, {small, small});
        const std::array rep{Parameter{model.intern("rep"), int64_t(2)}};
        output("out_replicate", "replicate", pair, {small}, rep);
        const std::array slice{Parameter{model.intern("sliceStart"), int64_t(31)}, Parameter{model.intern("sliceEnd"), int64_t(38)}};
        output("out_static", "sliceStatic", byte, {a}, slice);
        const std::array sliceWidth{Parameter{model.intern("sliceWidth"), int64_t(8)}};
        output("out_dynamic", "sliceDynamic", byte, {a, b}, sliceWidth);
        output("out_array", "sliceArray", byte, {a, b}, sliceWidth);
        return model;
    }

    GrhSimModel wideFixture()
    {
        GrhSimModel model("cpu_wide"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto logic = [&](uint32_t width) { return model.logicType(width, false, LogicDomain::TwoState); };
        const auto input = [&](const char *name, uint32_t width, bool isSigned = false) {
            const auto type = model.logicType(width, isSigned, LogicDomain::TwoState); const auto id = model.addInput(name, type);
            const auto value = model.addValue(type); const std::array results{value};
            const std::array refs{ObjectRef::input(id)};
            model.addOperation("core.input.read", {}, results, refs); return value;
        };
        const auto small = input("narrow", 28), bit = input("bit_in", 1), wide = input("wide", 129), shift = input("shift", 64);
        const auto other = input("other", 129), signedWide = input("signed_wide", 129, true),
            signedShort = input("signed_short", 67, true), signedByte = input("signed_byte", 8, true),
            wideShift = input("wide_shift", 512);
        const auto output = [&](const char *name, const char *op, uint32_t width, std::vector<ValueId> operands,
                                std::span<const Parameter> params = {}) {
            const auto type = logic(width); const auto result = model.addValue(type); const std::array results{result};
            model.addOperation(op, operands, results, {}, params);
            const auto id = model.addOutput(name, type); const std::array refs{ObjectRef::output(id)};
            model.addOperation("core.output.write", results, {}, refs);
            return result;
        };
        output("deep", "core.compute.concat", 1024, std::vector<ValueId>(1024, bit));
        const auto mixed = output("mixed", "core.compute.concat", 185, {small, wide, small});
        output("mixed_parity", "core.compute.reduceXor", 1, {mixed});
        output("shifted", "core.compute.shl", 448, {small, shift});
        output("sum", "core.compute.add", 129, {small, small});
        output("parity", "core.compute.reduceXor", 1, {wide});
        const std::array rep{Parameter{model.intern("rep"), int64_t(137)}};
        output("repeated", "core.compute.replicate", 137, {bit}, rep);
        for (const auto op : {"eq", "ne", "lt", "le", "gt", "ge"})
        {
            output(("cmp_" + std::string(op)).c_str(), ("core.compute." + std::string(op)).c_str(), 1, {wide, other});
            output(("scmp_" + std::string(op)).c_str(), ("core.compute." + std::string(op)).c_str(), 1, {signedWide, signedShort});
        }
        output("mixed_lt", "core.compute.lt", 1, {signedByte, wide});
        output("scalar_signed_lt", "core.compute.lt", 1, {signedByte, signedWide});
        output("wide_shl", "core.compute.shl", 129, {wide, wideShift});
        output("wide_lshr", "core.compute.lshr", 129, {wide, wideShift});
        output("wide_ashr", "core.compute.ashr", 129, {signedWide, wideShift});
        output("scalar_shl", "core.compute.shl", 28, {small, wideShift});
        return model;
    }

    void testWideBitwise(const std::filesystem::path &directory)
    {
        for (bool tracked : {true, false})
        {
            GrhSimModel model("cpu_bitwise"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto wide = model.logicType(129, false, LogicDomain::TwoState);
            const auto small = model.logicType(28, false, LogicDomain::TwoState);
            const auto input = [&](const char *name, TypeId type) {
                const auto id = model.addInput(name, type); const auto value = model.addValue(type);
                const std::array results{value}; const std::array refs{ObjectRef::input(id)};
                model.addOperation("core.input.read", {}, results, refs); return value;
            };
            const auto lhs = input("lhs", wide), rhs = input("rhs", wide), narrow = input("narrow", small);
            for (const auto name : {"and", "or", "xor", "not"})
            {
                const auto value = model.addValue(wide); const std::array results{value};
                const std::vector operands = std::string_view(name) == "not" ? std::vector{lhs} :
                    std::vector{lhs, std::string_view(name) == "or" ? narrow : rhs};
                model.addOperation("core.compute." + std::string(name), operands, results);
                const auto id = model.addOutput("out_" + std::string(name), wide);
                const std::array refs{ObjectRef::output(id)};
                model.addOperation("core.output.write", results, {}, refs);
            }
            map(model, tracked ? "1" : "1024");
            const auto path = directory / (tracked ? "tracked" : "local");
            diag::Diagnostics diagnostics;
            require(emitCpuCpp(model, path, diagnostics).success, "wide bitwise emit failed");
            std::string generated;
            for (const auto &entry : std::filesystem::directory_iterator(path))
            {
                if (entry.path().extension() != ".cpp") continue;
                std::ifstream stream(entry.path());
                generated.append(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
            }
            std::size_t count = 0;
            for (std::size_t pos = 0; (pos = generated.find("|=cpu_bitwise_words_changed<", pos)) != std::string::npos; ++pos) ++count;
            require(count == (tracked ? 4u : 0u), "bitwise tracked/local helper selection differs");
            for (const auto name : {"and", "or", "xor", "not"})
                require((generated.find("grhsim_" + std::string(name) + "_words(") != std::string::npos) == !tracked,
                        "bitwise local legacy helper path differs");
            const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_bitwise.mk";
            command("make --no-print-directory -C " + quote(path.string()) + " -f " + quote(makefile.string()) +
                    " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                    " CXXFLAGS='-std=c++20 -O0 -g -fsanitize=address,undefined -fno-sanitize-recover=all'");
        }
    }

    void testWideActivity(const std::filesystem::path &directory)
    {
        for (bool helpers : {true, false})
        for (bool tracked : {true, false})
        {
            GrhSimModel model("cpu_wide_activity"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto wide = model.logicType(129, false, LogicDomain::TwoState);
            const auto small = model.logicType(28, false, LogicDomain::TwoState);
            const auto input = [&](const char *name, TypeId type) {
                const auto id = model.addInput(name, type); const auto value = model.addValue(type);
                const std::array results{value}; const std::array refs{ObjectRef::input(id)};
                model.addOperation("core.input.read", {}, results, refs); return value;
            };
            const auto lhs = input("lhs", wide), rhs = input("rhs", wide), narrow = input("narrow", small);
            for (const auto name : {"add", "sub", "shl", "lshr", "ashr", "mixed_add", "same_add", "mixed_shl"})
            {
                const std::string_view operation(name);
                const auto value = model.addValue(wide); const std::array results{value};
                const std::array operands = operation == "mixed_add" ? std::array{lhs, narrow} :
                    operation == "same_add" ? std::array{narrow, narrow} :
                    operation == "mixed_shl" ? std::array{narrow, rhs} : std::array{lhs, rhs};
                const auto opName = operation == "mixed_add" || operation == "same_add" ? "add" :
                    operation == "mixed_shl" ? "shl" : name;
                model.addOperation("core.compute." + std::string(opName), operands, results);
                const auto id = model.addOutput("out_" + std::string(name), wide);
                const std::array refs{ObjectRef::output(id)};
                model.addOperation("core.output.write", results, {}, refs);
            }
            map(model, tracked ? "1" : "1024", helpers ? "1" : "65536");
            const auto path = directory / (helpers ? "helpers" : "inline") / (tracked ? "tracked" : "local");
            diag::Diagnostics diagnostics;
            require(emitCpuCpp(model, path, diagnostics).success, "wide activity emit failed");
            std::string generated;
            for (const auto &entry : std::filesystem::directory_iterator(path))
            {
                if (entry.path().extension() != ".cpp") continue;
                std::ifstream stream(entry.path());
                generated.append(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
            }
            for (const auto helper : {"cpu_arithmetic_words_changed", "cpu_shift_words_changed"})
            {
                std::size_t count = 0;
                for (std::size_t pos = 0; (pos = generated.find("|=" + std::string(helper) + "<", pos)) != std::string::npos; ++pos) ++count;
                require(count == (tracked ? 4u : 0u), "wide activity tracked/local selection differs");
            }
            require(generated.find("std::uint8_t cpu_active_word=cpu_flags[") != std::string::npos,
                    "compute word must load a local activity byte");
            require((generated.find("(cpu_local,cpu_active_word)") != std::string::npos) == helpers,
                    "split helpers must share the current local activity byte");
            if (tracked) require(generated.find("cpu_active_word |= ") != std::string::npos,
                                 "later same-word fanout must use the local activity byte");
            for (const auto name : {"add", "sub", "shl", "lshr", "ashr"})
                require((generated.find("grhsim_" + std::string(name) + "_words(") != std::string::npos) == !tracked,
                        "wide activity local legacy helper path differs");
            const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_wide_activity.mk";
            command("make --no-print-directory -C " + quote(path.string()) + " -f " + quote(makefile.string()) +
                    " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                    " CXXFLAGS='-std=c++20 -O0 -g -fsanitize=address,undefined -fno-sanitize-recover=all'");
        }
    }

    GrhSimModel stateFixture()
    {
        GrhSimModel model("cpu_wide_state"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState),
            addressType = model.logicType(2, false, LogicDomain::TwoState),
            wide = model.logicType(129, false, LogicDomain::TwoState);
        const auto input = [&](const char *name, TypeId type) {
            const auto id = model.addInput(name, type); const auto value = model.addValue(type);
            const std::array results{value}; const std::array refs{ObjectRef::input(id)};
            model.addOperation("core.input.read", {}, results, refs); return value;
        };
        const auto clock = input("clock", bit), enA = input("en_a", bit), enB = input("en_b", bit), fill = input("fill", bit);
        const auto address = input("address", addressType), addressB = input("address_b", addressType);
        const auto dataA = input("data_a", wide), dataB = input("data_b", wide), mask = input("mask", wide);
        const auto invert = model.addValue(wide); const std::array maskOperand{mask}, invertResult{invert};
        model.addOperation("core.compute.not", maskOperand, invertResult);
        const auto state = [&](TypeId type) {
            const auto id = model.addState("s" + std::to_string(model.states().size()), type);
            const std::array params{Parameter{model.intern("value"), std::string("1'b0")}};
            const std::array steps{InitStep{model.intern(model.types()[type.index - 1].kind == TypeKind::Array ? "core.init.fill" : "core.init.const"), {0, 1}}};
            model.addInit(id, steps, params); return id;
        };
        const auto reg = state(wide), latch = state(wide), memory = state(model.arrayType(wide, 4)),
            sequence = state(model.arrayType(wide, 4)), filled = state(model.arrayType(wide, 4));
        const auto write = [&](const char *operation, StateId target, std::vector<ValueId> operands, bool event = true) {
            std::vector<ObjectRef> refs{ObjectRef::state(target)};
            std::vector<Parameter> params;
            if (event)
            {
                operands.push_back(clock); refs.push_back(ObjectRef::state(state(bit)));
                params.push_back({model.intern("event_edges"), std::vector<std::string>{"posedge"}});
            }
            model.addOperation(operation, operands, {}, refs, params);
        };
        write("core.state.regWrite", reg, {enA, dataA, mask});
        write("core.state.regWrite", reg, {enB, dataB, invert});
        write("core.state.latchWrite", latch, {enA, dataA, mask}, false);
        write("core.state.memWrite", memory, {enA, address, dataA, mask});
        write("core.state.memWrite", memory, {enB, address, dataB, invert});
        write("core.state.memWriteSeq", sequence, {enA, address, dataA, enB, addressB, dataB});
        write("core.state.memFill", filled, {fill, dataB});
        const auto output = [&](const char *name, StateId target, bool isArray) {
            const auto result = model.addValue(wide); const std::array results{result};
            const std::array refs{ObjectRef::state(target)};
            const std::vector<ValueId> operands = isArray ? std::vector<ValueId>{address} : std::vector<ValueId>{};
            model.addOperation(isArray ? "core.state.memRead" : "core.state.read", operands, results, refs);
            const auto id = model.addOutput(name, wide); const std::array outRefs{ObjectRef::output(id)};
            model.addOperation("core.output.write", results, {}, outRefs);
        };
        output("q", reg, false); output("latched", latch, false);
        output("memory_out", memory, true); output("sequence_out", sequence, true); output("fill_out", filled, true);
        return model;
    }

    void testEmitShape(const std::filesystem::path &directory)
    {
        for (const bool helpers : {false, true})
        {
            GrhSimModel model("cpu_emit_shape"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState), byte = model.logicType(8, false, LogicDomain::TwoState);
            const auto input = [&](const char *name, TypeId type) {
                const auto id = model.addInput(name, type);
                const auto v = model.addValue(type); const std::array results{v}; const std::array refs{ObjectRef::input(id)};
                model.addOperation("core.input.read", {}, results, refs); return v;
            };
            const auto clock = input("clock", bit), fill = input("fill", bit), enable = input("enable", bit);
            const auto a = input("address_a", byte), b = input("address_b", byte), r = input("read_address", byte);
            const auto data = input("data", byte), mask = input("mask", byte);
            const auto constant = [&](TypeId type, const char *text) {
                const auto v = model.addValue(type); const std::array results{v};
                const std::array params{Parameter{model.intern("value"), std::string(text)}};
                model.addOperation("core.compute.constant", {}, results, {}, params); return v;
            };
            const auto one = constant(bit, "1"), full = constant(byte, "255"), zero = constant(byte, "0");
            const auto state = [&](TypeId type) {
                const auto id = model.addState("s" + std::to_string(model.states().size()), type);
                const std::array params{Parameter{model.intern("value"), std::string("0")}};
                const std::array steps{InitStep{model.intern(model.types()[type.index - 1].kind == TypeKind::Array ? "core.init.fill" : "core.init.const"), {0, 1}}};
                model.addInit(id, steps, params); return id;
            };
            const auto q0 = state(byte), q1 = state(byte), mem = state(model.arrayType(byte, 4));
            const auto read = [&](StateId s, bool memory = false) {
                const auto v = model.addValue(byte); const std::array results{v}; const std::array refs{ObjectRef::state(s)};
                const std::vector<ValueId> operands = memory ? std::vector<ValueId>{r} : std::vector<ValueId>{};
                model.addOperation(memory ? "core.state.memRead" : "core.state.read", operands, results, refs); return v;
            };
            const auto snapshot = read(q0), alias = read(q0), q1Read = read(q1), memRead = read(mem, true);
            const auto write = [&](const char *name, StateId s, std::vector<ValueId> operands) {
                operands.push_back(clock);
                const std::array refs{ObjectRef::state(s), ObjectRef::state(state(bit))};
                const std::array params{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
                model.addOperation(name, operands, {}, refs, params);
            };
            write("core.state.regWrite", q0, {one, data, full});
            write("core.state.regWrite", q1, {one, snapshot, full});
            write("core.state.memFill", mem, {fill, data});
            write("core.state.memWrite", mem, {enable, a, data, mask});
            write("core.state.memWriteSeq", mem, {enable, b, data, enable, b, zero});
            for (const auto &[name, v] : std::vector<std::pair<const char *, ValueId>>{{"q0", alias}, {"q1", q1Read}, {"memory_out", memRead}})
            {
                const auto output = model.addOutput(name, byte); const std::array refs{ObjectRef::output(output)}; const std::array operands{v};
                model.addOperation("core.output.write", operands, {}, refs);
            }
            map(model, "2", helpers ? "1" : "1000000");
            const auto path = directory / (helpers ? "helpers" : "inline");
            diag::Diagnostics diagnostics;
            require(emitCpuCpp(model, path, diagnostics).success, "emit shape fixture failed");
            bool aliases = false;
            for (const auto &message : diagnostics.messages()) aliases |= message.message.find("state_read_aliases=2 ") != std::string::npos;
            require(aliases, "compute-only state reads were not aliased or commit snapshot was aliased");
            std::string source;
            for (const auto &entry : std::filesystem::directory_iterator(path))
                if (entry.path().extension() == ".cpp")
                {
                    std::ifstream file(entry.path()); source.append(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
                }
            require(source.find("cpu_stage_bytes(" + std::to_string(mem.index) + ",") == std::string::npos &&
                    source.find("cpu_write_cell<") != std::string::npos,
                    "memory writes still stage an entire array");
            require(source.find("cpu_changed_") != std::string::npos && source.find("cpu_read_offsets[i]==p.offset") != std::string::npos,
                    "grouped changes or addressed-reader activation missing");
            const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_emit_shape.mk";
            command("make -C " + quote(path.string()) + " -f " + quote(makefile.string()) +
                    " check CXX=" + quote(WOLVRIX_TEST_CXX) + " CXXFLAGS='-std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer'");
        }
    }

    GrhSimModel cdcFixture()
    {
        GrhSimModel model("cpu_cdc"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState), byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto input = [&](const char *name, TypeId type) {
            const auto id = model.addInput(name, type);
            const auto value = model.addValue(type); const std::array results{value};
            const std::array refs{ObjectRef::input(id)};
            model.addOperation("core.input.read", {}, results, refs); return value;
        };
        const auto clockA = input("clock_a", bit), clockB = input("clock_b", bit), reset = input("reset", bit);
        const auto incA = input("inc_a", bit), incB = input("inc_b", bit);
        const auto constant = [&](TypeId type, const char *text) {
            const auto result = model.addValue(type); const std::array results{result};
            const std::array params{Parameter{model.intern("value"), std::string(text)}};
            model.addOperation("core.compute.constant", {}, results, {}, params); return result;
        };
        const auto one = constant(bit, "1"), zero = constant(byte, "0"), mask = constant(byte, "255");
        const auto state = [&](TypeId type) {
            const auto id = model.addState("s" + std::to_string(model.states().size()), type);
            const std::array params{Parameter{model.intern("value"), std::string("0")}};
            const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}};
            model.addInit(id, steps, params); return id;
        };
        std::vector<StateId> registers;
        std::vector<ValueId> reads;
        for (const char *name : {"count_a", "count_b", "b_sync1", "b_sync2", "a_sync1", "a_sync2"})
        {
            const auto id = state(byte); registers.push_back(id);
            const auto value = model.addValue(byte); reads.push_back(value); const std::array results{value};
            const std::array refs{ObjectRef::state(id)};
            model.addOperation("core.state.read", {}, results, refs);
            const auto output = model.addOutput(name, byte); const std::array outRefs{ObjectRef::output(output)};
            model.addOperation("core.output.write", results, {}, outRefs);
        }
        const auto compute = [&](const char *op, std::initializer_list<ValueId> operands) {
            const auto value = model.addValue(byte); const std::array results{value};
            model.addOperation(op, {operands.begin(), operands.size()}, results); return value;
        };
        const auto write = [&](unsigned index, ValueId data, bool domainA) {
            const auto next = compute("core.compute.mux", {reset, zero, data});
            const std::array operands{one, next, mask, domainA ? clockA : clockB, reset};
            const std::array refs{ObjectRef::state(registers[index]), ObjectRef::state(state(bit)), ObjectRef::state(state(bit))};
            const std::array params{Parameter{model.intern("event_edges"), std::vector<std::string>{domainA ? "posedge" : "negedge", "posedge"}}};
            model.addOperation("core.state.regWrite", operands, {}, refs, params);
        };
        write(0, compute("core.compute.add", {reads[0], incA}), true);
        write(1, compute("core.compute.add", {reads[1], incB}), false);
        write(2, reads[1], true); write(3, reads[2], true);
        write(4, reads[0], false); write(5, reads[4], false);
        return model;
    }

    GrhSimModel dualRamFixture()
    {
        GrhSimModel model("cpu_dual_ram"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState),
            addressType = model.logicType(3, false, LogicDomain::TwoState), word = model.logicType(16, false, LogicDomain::TwoState);
        const auto input = [&](const char *name, TypeId type) {
            const auto id = model.addInput(name, type); const auto value = model.addValue(type);
            const std::array results{value}; const std::array refs{ObjectRef::input(id)};
            model.addOperation("core.input.read", {}, results, refs); return value;
        };
        const auto clockA = input("clock_a", bit), clockB = input("clock_b", bit), reset = input("reset", bit);
        const auto enA = input("en_a", bit), enB = input("en_b", bit);
        const auto addrA = input("addr_a", addressType), addrB = input("addr_b", addressType), probe = input("probe", addressType);
        const auto dataA = input("data_a", word), dataB = input("data_b", word), maskA = input("mask_a", word), maskB = input("mask_b", word);
        const auto state = [&](TypeId type) {
            const auto id = model.addState("s" + std::to_string(model.states().size()), type);
            const std::array params{Parameter{model.intern("value"), std::string("0")}};
            const std::array steps{InitStep{model.intern(model.types()[type.index - 1].kind == TypeKind::Array ? "core.init.fill" : "core.init.const"), {0, 1}}};
            model.addInit(id, steps, params); return id;
        };
        const auto memory = state(model.arrayType(word, 8));
        const auto compute = [&](const char *op, TypeId type, std::initializer_list<ValueId> operands) {
            const auto value = model.addValue(type); const std::array results{value};
            model.addOperation(op, {operands.begin(), operands.size()}, results); return value;
        };
        const auto constant = [&](TypeId type, const char *text) {
            const auto value = model.addValue(type); const std::array results{value};
            const std::array params{Parameter{model.intern("value"), std::string(text)}};
            model.addOperation("core.compute.constant", {}, results, {}, params); return value;
        };
        const auto one = constant(bit, "1"), zero = constant(word, "0"), mask = constant(word, "65535");
        const auto running = compute("core.compute.logicNot", bit, {reset});
        const auto read = [&](ValueId address) {
            const auto value = model.addValue(word); const std::array operands{address}, results{value};
            const std::array refs{ObjectRef::state(memory)};
            model.addOperation("core.state.memRead", operands, results, refs); return value;
        };
        const auto output = [&](const char *name, ValueId value) {
            const auto id = model.addOutput(name, word); const std::array operands{value};
            const std::array refs{ObjectRef::output(id)};
            model.addOperation("core.output.write", operands, {}, refs);
        };
        const auto port = [&](const char *name, ValueId clock, ValueId enabled, ValueId address, ValueId data, ValueId writeMask) {
            const auto cond = compute("core.compute.and", bit, {running, enabled});
            const std::array writeOperands{cond, address, data, writeMask, clock};
            const std::array writeRefs{ObjectRef::state(memory), ObjectRef::state(state(bit))};
            const std::array event{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
            model.addOperation("core.state.memWrite", writeOperands, {}, writeRefs, event);
            const auto next = compute("core.compute.mux", word, {reset, zero, read(address)});
            const auto reg = state(word);
            const std::array operands{one, next, mask, clock, reset};
            const std::array refs{ObjectRef::state(reg), ObjectRef::state(state(bit)), ObjectRef::state(state(bit))};
            const std::array events{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge", "posedge"}}};
            model.addOperation("core.state.regWrite", operands, {}, refs, events);
            const auto value = model.addValue(word); const std::array results{value}; const std::array readRefs{ObjectRef::state(reg)};
            model.addOperation("core.state.read", {}, results, readRefs); output(name, value);
        };
        port("q_a", clockA, enA, addrA, dataA, maskA);
        port("q_b", clockB, enB, addrB, dataB, maskB);
        output("observed", read(probe));
        return model;
    }

    void compileAndCompare(const std::filesystem::path &directory, std::string_view top)
    {
        const auto output = directory.string();
        const auto data = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR);
        command("make --no-print-directory -C " + quote(output) + " -j 2 CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O2 -fsanitize=undefined -fno-sanitize-recover=all'");
        command(quote(WOLVRIX_TEST_VERILATOR) + " --cc --exe --build -j 2 --top-module " + std::string(top) + " --Mdir " + quote(output + "/verilator") +
                " -Wno-fatal --x-initial 0 --x-assign 0 " + quote((data / (std::string(top) + ".sv")).string()) + " " +
                quote((data / (std::string(top) + "_main.cpp")).string()) + " -CFLAGS " + quote("-std=c++20 -O2 -I" + output + " -fsanitize=undefined -fno-sanitize-recover=all") +
                " -LDFLAGS " + quote(output + "/libgrhsim_" + std::string(top) + ".a -fsanitize=undefined"));
        command(quote(output + "/verilator/V" + std::string(top)));
    }

    GrhSimModel startupFixture()
    {
        GrhSimModel model("cpu_startup"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState),
            byte = model.logicType(8, false, LogicDomain::TwoState),
            addressType = model.logicType(24, false, LogicDomain::TwoState), string = model.stringType();
        const auto input = [&](const char *name, TypeId type) {
            const auto id = model.addInput(name, type); const auto value = model.addValue(type);
            const std::array results{value}; const std::array refs{ObjectRef::input(id)};
            model.addOperation("core.input.read", {}, results, refs); return value;
        };
        const auto output = [&](const char *name, ValueId value, TypeId type) {
            const auto id = model.addOutput(name, type); const std::array operands{value};
            const std::array refs{ObjectRef::output(id)};
            model.addOperation("core.output.write", operands, {}, refs);
        };
        const auto text = input("text", string);
        output("text_a", text, string); output("text_b", text, string); output("text_c", text, string);
        const auto overheated = input("cpu_overheated", bit);
        output("cpu_again", overheated, bit); output("cpu_round", overheated, bit);
        const auto clock = input("clock", bit), enable = input("enable", bit),
            address = input("address", addressType), data = input("data", byte);
        const auto state = [&](TypeId type) {
            const auto id = model.addState("s" + std::to_string(model.states().size()), type);
            const bool array = model.types()[type.index - 1].kind == TypeKind::Array;
            const std::array params{Parameter{model.intern("value"), std::string(array ? "8'h3c" : "1'b0")}};
            const std::array steps{InitStep{model.intern(array ? "core.init.fill" : "core.init.const"), {0, 1}}};
            model.addInit(id, steps, params); return id;
        };
        const auto memory = state(model.arrayType(byte, 16 * 1024 * 1024));
        const auto mask = model.addValue(byte); const std::array maskResults{mask};
        const std::array maskParams{Parameter{model.intern("value"), std::string("8'hff")}};
        model.addOperation("core.compute.constant", {}, maskResults, {}, maskParams);
        const std::array operands{enable, address, data, mask, clock};
        const std::array refs{ObjectRef::state(memory), ObjectRef::state(state(bit))};
        const std::array params{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
        model.addOperation("core.state.memWrite", operands, {}, refs, params);
        const auto read = model.addValue(byte); const std::array readResults{read}, readOperands{address};
        const std::array readRefs{ObjectRef::state(memory)};
        model.addOperation("core.state.memRead", readOperands, readResults, readRefs); output("q", read, byte);
        for (const char *name : {"constant_a", "constant_b", "constant_c"})
        {
            const auto value = model.addValue(string); const std::array results{value};
            const std::array literal{Parameter{model.intern("value"), std::string(200, 'x') + name}};
            model.addOperation("core.compute.constant", {}, results, {}, literal); output(name, value, string);
        }
        return model;
    }

    void testStartup(const std::filesystem::path &directory)
    {
        auto model = startupFixture(); map(model);
        const auto &layout = *model.cpuMapping()->dataLayout;
        bool localString = false, boundaryString = false, helperString = false;
        for (const auto &slot : layout.values)
            if (layout.types[slot.type.index - 1].kind == CpuTypeKind::String)
            {
                require(layout.types[slot.type.index - 1].size == sizeof(void *), "string handle size changed");
                if (slot.kind == CpuStorageKind::PartitionLocal)
                {
                    localString = true;
                    helperString |= model.cpuMapping()->partitionTree.partitions[slot.owner.index - 1].attrs.helperChunks.size() > 1;
                }
                else boundaryString = true;
            }
        require(localString && boundaryString && helperString, "startup fixture misses string storage/helper boundaries");
        require(!model.cpuMapping()->schedule->inputShadows.empty(), "startup fixture misses input shadows");
        diag::Diagnostics diagnostics;
        require(emitCpuCpp(model, directory, diagnostics).success, "startup emit failed");
        const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_startup.mk";
        command("make --no-print-directory -C " + quote(directory.string()) + " -f " + quote(makefile.string()) +
                " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O0 -g -fsanitize=address,undefined -fno-sanitize-recover=all'");
    }

    void testSamplingLimit(const std::filesystem::path &directory)
    {
        for (unsigned count : {8u, 9u})
        {
            GrhSimModel model("cpu_sampling_limit"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto state = [&]() {
                const auto id = model.addState("s" + std::to_string(model.states().size()), bit);
                const std::array params{Parameter{model.intern("value"), std::string("0")}};
                const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}};
                model.addInit(id, steps, params); return id;
            };
            const auto one = model.addValue(bit); const std::array results{one};
            const std::array params{Parameter{model.intern("value"), std::string("1")}};
            model.addOperation("core.compute.constant", {}, results, {}, params);
            std::vector<ValueId> operands{one, one, one};
            std::vector<ObjectRef> refs{ObjectRef::state(state())};
            for (unsigned i = 0; i < count; ++i)
            {
                const auto input = model.addInput("clock" + std::to_string(i), bit);
                const auto value = model.addValue(bit); const std::array eventResults{value};
                const std::array inputRefs{ObjectRef::input(input)};
                model.addOperation("core.input.read", {}, eventResults, inputRefs);
                operands.push_back(value); refs.push_back(ObjectRef::state(state()));
            }
            const std::array edges{Parameter{model.intern("event_edges"), std::vector<std::string>(count, "posedge")}};
            model.addOperation("core.state.regWrite", operands, {}, refs, edges);
            std::vector<ObjectRef> secondRefs{ObjectRef::state(state())};
            for (unsigned i = 0; i < count; ++i) secondRefs.push_back(ObjectRef::state(state()));
            model.addOperation("core.state.regWrite", operands, {}, secondRefs, edges);
            map(model); diag::Diagnostics diagnostics;
            const auto output = directory / std::to_string(count);
            require(emitCpuCpp(model, output, diagnostics).success, "sampling limit fixture emission failed");
            require(checkSamplingTasks(model, output) == (count == 8 ? 1u : 0u), "sampling expression cap differs");
            unsigned stable = 0;
            for (const auto &file : std::filesystem::directory_iterator(output))
                if (file.path().extension() == ".cpp")
                {
                    std::ifstream stream(file.path());
                    const std::string source{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
                    stable += source.find("cpu_stable_history_skip") != std::string::npos;
                }
            require(stable == (count == 8 ? 1u : 0u), "stable history event-value cap differs");
        }
    }

    GrhSimModel historyScanFixture(unsigned count, unsigned gapEvery, bool derived, bool observe = true,
                                  std::string_view writeKind = "core.state.regWrite", bool signedHistory = false,
                                  bool randomHistory = false)
    {
        GrhSimModel model("cpu_history_scan"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState), byte = model.logicType(8, false, LogicDomain::TwoState);
        const bool memory = writeKind != "core.state.regWrite";
        const auto registerType = memory ? model.arrayType(byte, 2) : byte;
        const auto historyType = signedHistory ? model.logicType(1, true, LogicDomain::TwoState) : bit;
        const auto input = [&](const char *name, TypeId type) {
            const auto id = model.addInput(name, type); const auto value = model.addValue(type);
            const std::array results{value}; const std::array refs{ObjectRef::input(id)};
            model.addOperation("core.input.read", {}, results, refs); return value;
        };
        const auto clockA = input("clock_a", bit), clockB = input("clock_b", bit),
            enable = input("enable", bit), data = input("data", byte);
        auto eventB = clockB;
        if (derived)
        {
            eventB = model.addValue(bit); const std::array operands{clockB}, results{eventB};
            model.addOperation("core.compute.not", operands, results);
        }
        const auto mask = model.addValue(byte); const std::array maskResults{mask};
        const std::array maskParams{Parameter{model.intern("value"), std::string("255")}};
        model.addOperation("core.compute.constant", {}, maskResults, {}, maskParams);
        ValueId address;
        if (memory)
        {
            address = model.addValue(byte); const std::array results{address};
            const std::array params{Parameter{model.intern("value"), std::string("1")}};
            model.addOperation("core.compute.constant", {}, results, {}, params);
        }
        const auto state = [&](TypeId type, bool initial) {
            const auto id = model.addState("s" + std::to_string(model.states().size()), type);
            const std::array params{Parameter{model.intern("value"), std::string(initial ? "1" : "0")}};
            const bool random = randomHistory && type == historyType;
            const auto init = random ? "core.init.random" :
                model.types()[type.index - 1].kind == TypeKind::Array ? "core.init.fill" : "core.init.const";
            if (random)
            {
                const std::array steps{InitStep{model.intern(init), {0, 0}}};
                model.addInit(id, steps, {}); return id;
            }
            const std::array steps{InitStep{model.intern(init), {0, 1}}};
            model.addInit(id, steps, params); return id;
        };
        std::vector<StateId> registers, historiesA, historiesB;
        for (unsigned i = 0; i < count; ++i) registers.push_back(state(registerType, false));
        for (unsigned i = 0; i < count; ++i)
        {
            historiesA.push_back(state(historyType, i + 1 != count));
            if (gapEvery && (i + 1) % gapEvery == 0 && i + 1 != count) state(byte, false);
        }
        for (unsigned i = 0; i < count; ++i)
        {
            historiesB.push_back(state(historyType, i == 0));
            if (gapEvery && (i + 1) % gapEvery == 0 && i + 1 != count) state(byte, false);
        }
        const auto output = [&](const std::string &name, StateId target, TypeId type) {
            const auto value = model.addValue(type); const std::array results{value};
            const std::array refs{ObjectRef::state(target)};
            if (model.types()[model.states()[target.index - 1].type.index - 1].kind == TypeKind::Array)
            {
                const std::array operands{address};
                model.addOperation("core.state.memRead", operands, results, refs);
            }
            else model.addOperation("core.state.read", {}, results, refs);
            const auto port = model.addOutput(name, type); const std::array outRefs{ObjectRef::output(port)};
            model.addOperation("core.output.write", results, {}, outRefs);
        };
        for (unsigned i = 0; i < count; ++i)
        {
            std::vector<ValueId> operands;
            if (writeKind == "core.state.regWrite") operands = {enable, data, mask};
            else if (writeKind == "core.state.memWrite") operands = {enable, address, data, mask};
            else if (writeKind == "core.state.memFill") operands = {enable, data};
            else operands = {enable, address, mask, enable, address, data};
            operands.push_back(clockA); operands.push_back(eventB);
            const std::array refs{ObjectRef::state(registers[i]), ObjectRef::state(historiesA[i]), ObjectRef::state(historiesB[i])};
            const std::array params{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge", "negedge"}}};
            model.addOperation(writeKind, operands, {}, refs, params);
            output("q" + std::to_string(i), registers[i], byte);
        }
        if (observe)
        {
            output("history_a", historiesA.back(), bit); output("history_b", historiesB.front(), bit);
            // Observe every history to preserve the independent range scans.
            for (unsigned i = 0; i < count; ++i)
            {
                output("history_a_" + std::to_string(i), historiesA[i], bit);
                output("history_b_" + std::to_string(i), historiesB[i], bit);
            }
        }
        return model;
    }

    void testHistoryScan(const std::filesystem::path &directory)
    {
        struct Case { const char *name; unsigned count, gap; bool derived; };
        for (const auto &test : {Case{"compact", 32, 0, false}, Case{"fragmented", 32, 8, false},
                                Case{"sparse", 32, 1, false}, Case{"derived", 32, 0, true},
                                Case{"below_minimum", 15, 0, false}, Case{"minimum", 16, 0, false}})
        {
            auto model = historyScanFixture(test.count, test.gap, test.derived); map(model);
            diag::Diagnostics diagnostics; const auto path = directory / test.name;
            require(emitCpuCpp(model, path, diagnostics).success, "history scan emit failed");
            require(checkSamplingTasks(model, path) == 1, "history scan lost its event-domain sampling path");
            std::string source;
            for (const auto &file : std::filesystem::directory_iterator(path))
                if (file.path().extension() == ".cpp")
                {
                    std::ifstream stream(file.path());
                    source.append(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
                }
            const bool scanned = test.count >= 16 && test.gap != 1;
            const auto marker = "/* cpu_history_edge_scan histories=" + std::to_string(test.count) +
                                " ranges=" + std::to_string(test.gap ? test.count / test.gap : 1) + " */";
            require((source.find(marker) != std::string::npos) == scanned, "history scan eligibility/range count differs");
            if (!scanned) require(source.find("cpu_history_edge_scan") == std::string::npos, "unprofitable history scan was emitted");
            require(source.find("cpu_stable_history_skip") == std::string::npos, "observed history used whole-task skip");
            require(source.find("cpu_edge_snapshot") == std::string::npos, "observed history used an edge snapshot");
            if (test.count != 32) continue;
            const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_history_scan.mk";
            command("make --no-print-directory -C " + quote(path.string()) + " -f " + quote(makefile.string()) +
                    " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                    " CXXFLAGS='-std=c++20 -O0 -g -fsanitize=address,undefined -fno-sanitize-recover=all'" +
                    (test.derived ? " CPU_HISTORY_SCAN_ARGS=--derived" : ""));
        }
    }

    void testStableHistorySkip(const std::filesystem::path &directory)
    {
        struct Case { const char *name; unsigned count, gap; bool derived; const char *kind = "core.state.regWrite"; bool signedHistory = false; };
        for (const auto &test : {Case{"compact", 32, 0, false}, Case{"fragmented", 32, 8, false},
                                Case{"sparse", 32, 1, false}, Case{"derived", 32, 0, true},
                                Case{"below_minimum", 7, 0, false}, Case{"minimum", 8, 0, false},
                                Case{"memory_write", 32, 1, false, "core.state.memWrite"},
                                Case{"memory_fill", 32, 0, true, "core.state.memFill"},
                                Case{"memory_sequence", 32, 8, true, "core.state.memWriteSeq"},
                                Case{"signed_fallback", 32, 0, false, "core.state.regWrite", true}})
        {
            auto model = historyScanFixture(test.count, test.gap, test.derived, false, test.kind, test.signedHistory); map(model);
            diag::Diagnostics diagnostics; std::stringstream json;
            require(writeGrhSimJson(model, json, defaultDialectRegistry(), diagnostics), "stable history JSON write failed");
            auto restored = readGrhSimJson(json, defaultDialectRegistry(), diagnostics);
            require(bool(restored), "stable history fresh load failed");
            const auto path = directory / test.name;
            require(emitCpuCpp(*restored, path, diagnostics).success, "stable history emit failed");
            require(checkSamplingTasks(*restored, path) == 1, "stable history lost original sampling path");
            std::string source;
            for (const auto &file : std::filesystem::directory_iterator(path))
                if (file.path().extension() == ".cpp")
                {
                    std::ifstream stream(file.path());
                    source.append(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
                }
            const bool eligible = test.count >= 8 && !test.signedHistory;
            const auto marker = "/* cpu_stable_history_scan histories=" + std::to_string(test.count * 2) + " groups=2 */";
            require((source.find(marker) != std::string::npos) == eligible, "private stable history threshold differs");
            require((source.find("cpu_stable_history_skip") != std::string::npos) == eligible, "private history skip eligibility differs");
            require((source.find("static constexpr std::size_t cpu_histories[]") != std::string::npos) == eligible,
                    "sparse history skip did not use exact offsets");
            bool sharing = false;
            const auto shared = test.signedHistory ? 0u : test.count * 2 - 4;
            for (const auto &message : diagnostics.messages())
                sharing |= message.message.find("history_shared_states=" + std::to_string(shared) + " ") != std::string::npos;
            require(sharing, "history sharing merged distinct initializers or missed equivalent private histories");
            require((source.find("const bool cpu_edge_snapshot_0=") != std::string::npos) == !test.signedHistory,
                    "edge snapshot eligibility differs");
            if (!test.signedHistory)
            {
                require(source.find("// cpu_edge_snapshot uses=" + std::to_string(test.count - 2) + "\n") != std::string::npos,
                        "edge snapshot merged distinct initial histories or missed repeated guards");
                require(source.find("const bool cpu_edge_snapshot_1=") == std::string::npos,
                        "edge snapshot cached a non-repeated predicate");
                require(source.find("if((cpu_edge_snapshot_0) &&") != std::string::npos ||
                        source.find("if(cpu_edge_snapshot_0){") != std::string::npos,
                        "commit payload did not consume the edge snapshot");
            }
            if (test.count != 32) continue;
            const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_history_scan.mk";
            command("make --no-print-directory -C " + quote(path.string()) + " -f " + quote(makefile.string()) +
                    " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                    " CXXFLAGS='-std=c++20 -O0 -g -DCPU_HISTORY_SCAN_PRIVATE=1 -fsanitize=address,undefined -fno-sanitize-recover=all'" +
                    (test.derived ? " CPU_HISTORY_SCAN_ARGS=--derived" : ""));
        }
    }

    void testRandomHistorySharingFallback(const std::filesystem::path &directory)
    {
        auto model = historyScanFixture(32, 0, false, false, "core.state.regWrite", false, true);
        map(model); diag::Diagnostics diagnostics;
        require(emitCpuCpp(model, directory, diagnostics).success, "random history emit failed");
        bool excluded = false;
        for (const auto &message : diagnostics.messages())
            excluded |= message.message.find("history_shared_states=0 history_shared_tasks=0 ") != std::string::npos;
        require(excluded, "random histories were merged despite independent initialization");
        bool overwritten = false;
        for (const auto &file : std::filesystem::directory_iterator(directory))
            if (file.path().extension() == ".cpp")
            {
                std::ifstream stream(file.path());
                const std::string source{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
                overwritten |= source.find("cpu_stage_bytes_overwrite(") != std::string::npos;
            }
        require(overwritten, "independent random histories lost batch overwrite coverage");
        const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_history_scan.mk";
        command("make --no-print-directory -C " + quote(directory.string()) + " -f " + quote(makefile.string()) +
                " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O0 -g -DCPU_HISTORY_SCAN_PRIVATE=1 -fsanitize=address,undefined -fno-sanitize-recover=all'"
                " CPU_HISTORY_SCAN_ARGS=--random");
    }

    void testComputeHistorySharing(const std::filesystem::path &directory)
    {
        GrhSimModel model("cpu_compute_history"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto input = [&](const char *name) {
            const auto id = model.addInput(name, bit); const auto value = model.addValue(bit);
            const std::array results{value}; const std::array refs{ObjectRef::input(id)};
            model.addOperation("core.input.read", {}, results, refs); return value;
        };
        const auto clock = input("clock"), enable = input("enable");
        const auto history = [&](bool initial) {
            const auto id = model.addState("h" + std::to_string(model.states().size()), bit);
            const std::array params{Parameter{model.intern("value"), std::string(initial ? "1" : "0")}};
            const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}};
            model.addInit(id, steps, params); return id;
        };
        const auto call = [&](StateId target) {
            const std::array operands{enable, clock};
            const std::array refs{ObjectRef::state(target)};
            const std::array params{Parameter{model.intern("name"), std::string("display")},
                Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
            model.addOperation("core.system.task", operands, {}, refs, params);
        };
        std::array<StateId, 8> zeroHistories;
        for (auto &entry : zeroHistories) { entry = history(false); call(entry); }
        std::array<StateId, 2> oneHistories;
        for (auto &entry : oneHistories) { entry = history(true); call(entry); }
        const auto observed = history(false); call(observed); call(observed);
        map(model);
        diag::Diagnostics diagnostics;
        require(emitCpuCpp(model, directory, diagnostics).success, "compute history sharing emit failed");
        // The mapper spreads the twelve calls over six same-task units: four zero-initializer pairs,
        // one one-initializer pair, and one observed pair. Only same-unit lockstep pairs may merge.
        bool sharing = false;
        for (const auto &message : diagnostics.messages())
            sharing |= message.message.find("compute_history_aliases=5 compute_history_alias_units=5 ") != std::string::npos;
        require(sharing, "compute history sharing merged across units/groups or missed same-unit pairs");
        std::string source;
        for (const auto &file : std::filesystem::directory_iterator(directory))
            if (file.path().extension() == ".cpp")
            {
                std::ifstream stream(file.path());
                source.append(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
            }
        const auto count = [&](const std::string &needle) {
            std::size_t found = 0, at = 0;
            while ((at = source.find(needle, at)) != std::string::npos) { ++found; at += needle.size(); }
            return found;
        };
        require(count("cpu_system_task(\"display\"") == 12, "compute history sharing dropped a guarded system task");
        require(count("cpu_write_scalar<bool>(") == 0,
                "unit-private history samples still pay the staged write path");
        require(count("cpu_dsample=") == 6,
                "unit-private history representatives were not deferred-sampled once per unit block");
        require(count("cpu_dsample=") != 0 && count("cpu_write_scalar<bool>(cpu_obj_,cpu_shadow_," + std::to_string(observed.index) + ",") == 0,
                "history referenced by two calls lost its sample");
        // Every unit holds a same-key call pair, so each pair's repeated event guard collapses to one local.
        bool hoisted = false;
        for (const auto &message : diagnostics.messages())
            hoisted |= message.message.find("compute_guard_snapshots=6 compute_guard_snapshot_uses=12 ") != std::string::npos;
        require(hoisted, "compute guard hoisting missed same-unit repeated event guards");
        require(count("const bool cpu_cevent_") == 6, "compute guard locals were not emitted once per unit key");
        require(count("cpu_cold_gate") == 12, "constant-body guarded calls did not get the cold-body hint");
        require(count("(false ||") == 6, "repeated event guard expressions were not collapsed");
    }

    void testGateCompaction(const std::filesystem::path &directory)
    {
        GrhSimModel model("cpu_gate_compaction"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto input = [&](const char *name) {
            const auto id = model.addInput(name, bit); const auto value = model.addValue(bit);
            const std::array results{value}; const std::array refs{ObjectRef::input(id)};
            model.addOperation("core.input.read", {}, results, refs); return value;
        };
        const auto clock = input("clock"), enableA = input("enableA"), enableB = input("enableB");
        const auto dataId = model.addInput("data", byte); const auto data = model.addValue(byte);
        {   const std::array results{data}; const std::array refs{ObjectRef::input(dataId)};
            model.addOperation("core.input.read", {}, results, refs); }
        const auto history = [&] {
            const auto id = model.addState("h" + std::to_string(model.states().size()), bit);
            const std::array params{Parameter{model.intern("value"), std::string("0")}};
            const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}};
            model.addInit(id, steps, params); return id;
        };
        const auto display = [&](ValueId condition) {
            const std::array operands{condition, clock};
            const std::array refs{ObjectRef::state(history())};
            const std::array params{Parameter{model.intern("name"), std::string("display")},
                Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
            model.addOperation("core.system.task", operands, {}, refs, params);
        };
        // Same-condition constant-body pair: must merge under one hoisted guard.
        display(enableA); display(enableA);
        // Distinct constant-body condition: hoisted, hinted, not merged.
        display(enableB);
        // Runtime-argument gate: hoisted but never hinted.
        {
            const std::array operands{enableB, data, clock};
            const std::array refs{ObjectRef::state(history())};
            const std::array params{Parameter{model.intern("name"), std::string("display")},
                Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
            model.addOperation("core.system.task", operands, {}, refs, params);
        }
        map(model, "2", "1000000");
        diag::Diagnostics diagnostics;
        require(emitCpuCpp(model, directory, diagnostics).success, "gate compaction emit failed");
        bool stats = false;
        for (const auto &message : diagnostics.messages())
        {
            if (message.message.find("gate_hoisted_runs=") != std::string::npos) std::cout << message.message << '\n';
            stats |= message.message.find("gate_hoisted_runs=1 gate_hoisted_gates=2 gate_merged_gates=1 gate_cold_hints=2") != std::string::npos;
        }
        require(stats, "gate compaction statistics did not match the fixture shape");
        std::string source;
        for (const auto &file : std::filesystem::directory_iterator(directory))
            if (file.path().extension() == ".cpp")
            {
                std::ifstream stream(file.path());
                source.append(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
            }
        const auto count = [&](const std::string &needle) {
            std::size_t found = 0, at = 0;
            while ((at = source.find(needle, at)) != std::string::npos) { ++found; at += needle.size(); }
            return found;
        };
        require(count("cpu_system_task(\"display\"") == 4, "gate compaction dropped a guarded system task");
        require(count("if(cpu_cevent_") == 1, "same-guard calls were not grouped under one hoisted guard per unit");
        require(count("cpu_gate_merge ops=2") == 1, "same-condition call pair was not merged");
        require(count("__builtin_expect") == 1, "constant-body gates did not get exactly one cold hint each");
        require(count("cpu_cold_gate") == 0, "run members must not use the singleton cold-gate form");
        require(count("){ // cpu_edge_direction") == 3, "single-event posedge units did not get the edge-direction fast path");
    }

    void testHistoryCohorts(const std::filesystem::path &directory)
    {
        GrhSimModel model("cpu_history_batch"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState), byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto input = [&](const char *name, TypeId type) {
            const auto id = model.addInput(name, type); const auto value = model.addValue(type);
            const std::array results{value}; const std::array refs{ObjectRef::input(id)};
            model.addOperation("core.input.read", {}, results, refs); return value;
        };
        const auto clock = input("clock", bit), data = input("data", byte);
        const auto constant = [&](TypeId type, const char *text) {
            const auto value = model.addValue(type); const std::array results{value};
            const std::array params{Parameter{model.intern("value"), std::string(text)}};
            model.addOperation("core.compute.constant", {}, results, {}, params); return value;
        };
        const auto one = constant(bit, "1"), zero = constant(bit, "0"), mask = constant(byte, "255");
        const auto state = [&](TypeId type, bool initial) {
            const auto id = model.addState("s" + std::to_string(model.states().size()), type);
            const std::array params{Parameter{model.intern("value"), std::string(initial ? "1" : "0")}};
            const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}};
            model.addInit(id, steps, params); return id;
        };
        std::array<StateId, 16> registers, histories;
        for (auto &reg : registers) reg = state(byte, false);
        for (unsigned i = 0; i < histories.size(); ++i) histories[i] = state(bit, i & 1);
        const auto output = [&](const std::string &name, StateId target, TypeId type) {
            const auto value = model.addValue(type); const std::array results{value};
            const std::array refs{ObjectRef::state(target)};
            model.addOperation("core.state.read", {}, results, refs);
            const auto port = model.addOutput(name, type); const std::array outRefs{ObjectRef::output(port)};
            model.addOperation("core.output.write", results, {}, outRefs);
        };
        for (unsigned i = 0; i < registers.size(); ++i)
        {
            const std::array operands{one, data, mask, clock};
            const std::array refs{ObjectRef::state(registers[i]), ObjectRef::state(histories[i == 10 ? 9 : i])};
            const std::array params{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
            model.addOperation("core.state.regWrite", operands, {}, refs, params);
            output("q" + std::to_string(i), registers[i], byte);
        }
        output("history_read", histories[8], bit);
        const std::array unusedWrite{zero, zero, one}; const std::array unusedRefs{ObjectRef::state(histories[11])};
        model.addOperation("core.state.latchWrite", unusedWrite, {}, unusedRefs);
        const auto clockB = input("clock_b", bit);
        const std::array sharedRegisters{state(byte, false), state(byte, false)};
        const auto sharedHistory = state(bit, false);
        const std::array sharedClocks{clock, clockB};
        for (unsigned i = 0; i < sharedRegisters.size(); ++i)
        {
            const std::array operands{one, data, mask, sharedClocks[i]};
            const std::array refs{ObjectRef::state(sharedRegisters[i]), ObjectRef::state(sharedHistory)};
            const std::array params{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
            model.addOperation("core.state.regWrite", operands, {}, refs, params);
            output(i == 0 ? "shared_a" : "shared_b", sharedRegisters[i], byte);
        }
        output("shared_history", sharedHistory, bit);
        map(model); diag::Diagnostics diagnostics;
        std::stringstream json;
        require(writeGrhSimJson(model, json, defaultDialectRegistry(), diagnostics), "history batch JSON write failed");
        auto restored = readGrhSimJson(json, defaultDialectRegistry(), diagnostics);
        require(bool(restored), "history batch fresh load failed");
        unsigned fallbackDomains = 0;
        for (const auto &task : restored->cpuMapping()->schedule->numaNodes[0].cores[0].tasks)
        {
            const auto &tree = restored->cpuMapping()->partitionTree;
            const auto &function = tree.partitions[task.partition.index - 1];
            if (!tree.partitions[function.parent.index - 1].attrs.eventGate) continue;
            require(task.execution == CpuExecution::AlwaysScanCommit, "conflicting history retained domain gating");
            ++fallbackDomains;
        }
        require(fallbackDomains == 2, "cross-domain history fixture missed fallback coverage");
        require(emitCpuCpp(*restored, directory, diagnostics).success, "history batch emit failed");
        require(checkSamplingTasks(*restored, directory) == 0, "conflicting-history fallback used sampling fast path");
        bool overwriteBatch = false, eventSnapshot = false, edgeSnapshot = false;
        for (const auto &file : std::filesystem::directory_iterator(directory))
            if (file.path().extension() == ".cpp")
            {
                std::ifstream stream(file.path());
                const std::string source{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
                require(source.find("cpu_stable_history_skip") == std::string::npos, "shared/written history used whole-task skip");
                overwriteBatch |= source.find("cpu_stage_bytes_overwrite(") != std::string::npos;
                eventSnapshot |= source.find("cpu_event_snapshot_") != std::string::npos;
                edgeSnapshot |= source.find("cpu_edge_snapshot uses=") != std::string::npos;
            }
        require(!overwriteBatch, "equivalent history cohorts were still batch copied");
        require(eventSnapshot, "repeated boundary event was not cached at task entry");
        require(edgeSnapshot, "mixed-task private history guards were not shared");
        bool coverage = false;
        for (const auto &message : diagnostics.messages())
        {
            std::cout << message.message << '\n';
            coverage |= message.message.find("history_shared_states=11 history_shared_tasks=1 ") != std::string::npos;
        }
        require(coverage, "history batching missed eligible histories or accepted shared/observed/written histories");
        std::set<uint32_t> direct;
        for (auto reg : registers) direct.insert(reg.index);
        for (auto reg : sharedRegisters) direct.insert(reg.index);
        require(emittedDirectStates(directory) == direct, "direct commit accepted an event history or missed a private register");
        const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_history_batch.mk";
        command("make --no-print-directory -C " + quote(directory.string()) + " -f " + quote(makefile.string()) +
                " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O0 -g -fsanitize=address,undefined -fno-sanitize-recover=all'");
    }

    void testPrivateCommits(const std::filesystem::path &directory)
    {
        GrhSimModel model("cpu_private_commit"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState),
            narrow = model.logicType(5, true, LogicDomain::TwoState), word = model.logicType(64, false, LogicDomain::TwoState);
        const auto input = [&](const std::string &name, TypeId type) {
            const auto id = model.addInput(name, type); const auto value = model.addValue(type);
            const std::array results{value}; const std::array refs{ObjectRef::input(id)};
            model.addOperation("core.input.read", {}, results, refs); return value;
        };
        const auto enable = input("enable", bit);
        const auto one = model.addValue(bit); const std::array oneResult{one};
        const std::array oneParams{Parameter{model.intern("value"), std::string("1")}};
        model.addOperation("core.compute.constant", {}, oneResult, {}, oneParams);
        std::vector<ValueId> callOperands{one};
        std::vector<DpiArgument> arguments;
        std::set<uint32_t> direct;
        for (const auto &[name, type] : std::array<std::pair<const char *, TypeId>, 3>{{{"bit", bit}, {"narrow", narrow}, {"word", word}}})
        {
            const auto data = input(std::string(name) + "_data", type), mask = input(std::string(name) + "_mask", type);
            const auto state = model.addState(name, type); direct.insert(state.index);
            const std::array params{Parameter{model.intern("value"), std::string("0")}};
            const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}}; model.addInit(state, steps, params);
            const auto read = model.addValue(type); const std::array results{read}; const std::array refs{ObjectRef::state(state)};
            model.addOperation("core.state.read", {}, results, refs);
            const std::array writeOperands{enable, data, mask};
            model.addOperation("core.state.latchWrite", writeOperands, {}, refs);
            callOperands.push_back(read);
            arguments.push_back({model.intern(name), DpiDirection::Input, type});
        }
        const auto function = model.addExternFunction("cpu_test_private", "core.dpi", "cpu_test_private", arguments, {});
        const std::array refs{ObjectRef::function(function)};
        const std::array params{Parameter{model.intern("event_edges"), std::vector<std::string>{}}};
        model.addOperation("core.dpi.call", callOperands, {}, refs, params);
        map(model);
        require(model.cpuMapping()->schedule->commitStateFanout.size() == 3, "private fixture lost its state-read commit fanout");
        const auto &privateProjection = model.cpuMapping()->schedule->quiescenceProjection;
        require(std::none_of(privateProjection.begin(), privateProjection.end(), [](bool projected) { return projected; }),
                "private fixture accidentally added a quiescence root");
        diag::Diagnostics diagnostics; std::stringstream json;
        require(writeGrhSimJson(model, json, defaultDialectRegistry(), diagnostics), "private commit JSON write failed");
        auto restored = readGrhSimJson(json, defaultDialectRegistry(), diagnostics);
        require(bool(restored), "private commit fresh load failed");
        require(emitCpuCpp(*restored, directory, diagnostics).success, "private commit emit failed");
        require(emittedDirectStates(directory) == direct, "nonprojected private states did not use direct commit");
        bool specialized = false;
        for (const auto &entry : std::filesystem::directory_iterator(directory))
            if (entry.path().extension() == ".cpp")
            {
                std::ifstream stream(entry.path());
                const std::string source{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
                specialized |= source.find("cpu_direct_state_changed_one(") != std::string::npos;
            }
        require(specialized, "single-target direct commits did not use the specialized notification");
        const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_private_commit.mk";
        command("make --no-print-directory -C " + quote(directory.string()) + " -f " + quote(makefile.string()) +
                " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O0 -g -fsanitize=address,undefined -fno-sanitize-recover=all'");
    }

    void testSharedCommitEdges(const std::filesystem::path &directory, std::string_view superOps,
                                 std::string_view helperLines)
    {
        GrhSimModel model("cpu_notifications"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState), byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto input = [&](const std::string &name, TypeId type) {
            const auto port = model.addInput(name, type);
            const auto value = model.addValue(type);
            const std::array results{value}; const std::array refs{ObjectRef::input(port)};
            model.addOperation("core.input.read", {}, results, refs); return value;
        };
        const auto clock = input("clock", bit), mask = input("mask", byte);
        std::array<ValueId, 8> enables, data;
        for (unsigned i = 0; i < enables.size(); ++i)
        {
            enables[i] = input("en" + std::to_string(i), bit);
            data[i] = input("d" + std::to_string(i), byte);
        }
        ValueId sum;
        for (unsigned i = 0; i < 41; ++i)
        {
            const auto suffix = std::to_string(i);
            const auto state = model.addState("s" + suffix, byte), history = model.addState("h" + suffix, bit);
            const std::array initParams{Parameter{model.intern("value"), std::string("0")}};
            const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}};
            model.addInit(state, steps, initParams); model.addInit(history, steps, initParams);
            const std::array operands{enables[i % enables.size()], data[i % data.size()], mask, clock};
            const std::array refs{ObjectRef::state(state), ObjectRef::state(history)};
            const std::array params{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
            model.addOperation("core.state.regWrite", operands, {}, refs, params);
            const auto read = model.addValue(byte); const std::array result{read};
            const std::array readRefs{ObjectRef::state(state)};
            model.addOperation("core.state.read", {}, result, readRefs);
            const auto adjusted = model.addValue(byte); const std::array adjustedResult{adjusted};
            const std::array adjustedOperands{read, mask};
            model.addOperation("core.compute.add", adjustedOperands, adjustedResult);
            if (!sum) sum = adjusted;
            else
            {
                const auto next = model.addValue(byte); const std::array addResult{next};
                const std::array addOperands{sum, adjusted};
                model.addOperation("core.compute.add", addOperands, addResult); sum = next;
            }
        }
        const auto output = model.addOutput("sum", byte); const std::array outRefs{ObjectRef::output(output)};
        const std::array outOperands{sum}; model.addOperation("core.output.write", outOperands, {}, outRefs);
        map(model, superOps, helperLines); diag::Diagnostics diagnostics;
        require(emitCpuCpp(model, directory, diagnostics).success, "notification fixture emit failed");
        bool sharedEdge = false, partialByte = false;
        for (const auto &file : std::filesystem::directory_iterator(directory))
            if (file.path().extension() == ".cpp")
            {
                std::ifstream stream(file.path());
                const std::string source{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
                sharedEdge |= source.find("cpu_shared_task_edge") != std::string::npos ||
                    source.find("cpu_shared_port_edge") != std::string::npos;
                partialByte |= source.find("cpu_pflags[5]&=~1;") != std::string::npos;
            }
        require(sharedEdge && partialByte, "shared commit edge fixture missed block guard or partial byte consumption");
        const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_notifications.mk";
        command("make --no-print-directory -C " + quote(directory.string()) + " -f " + quote(makefile.string()) +
                " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O0 -g -fsanitize=address,undefined -fno-sanitize-recover=all'");
    }

    void testIdentityAssigns(const std::filesystem::path &directory, bool helpers)
    {
        GrhSimModel model("cpu_identity_assign"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto input = [&](const std::string &name, TypeId type) {
            const auto id = model.addInput(name, type); const auto value = model.addValue(type);
            const std::array results{value}; const std::array refs{ObjectRef::input(id)};
            model.addOperation("core.input.read", {}, results, refs); return value;
        };
        const auto state = [&](TypeId type) {
            const auto id = model.addState("s" + std::to_string(model.states().size()), type);
            const std::array params{Parameter{model.intern("value"), std::string("0")}};
            const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}};
            model.addInit(id, steps, params); return id;
        };
        const auto compute = [&](const char *op, TypeId type, std::initializer_list<ValueId> args) {
            const auto value = model.addValue(type); const std::array results{value};
            model.addOperation(op, {args.begin(), args.size()}, results); return value;
        };
        const auto output = [&](const std::string &name, TypeId type, ValueId value) {
            const auto id = model.addOutput(name, type); const std::array refs{ObjectRef::output(id)};
            const std::array operands{value}; model.addOperation("core.output.write", operands, {}, refs);
        };
        const auto identity = [&](ValueId value) {
            const auto type = model.values()[value.index - 1].type;
            for (unsigned i = 0; i < 3; ++i) value = compute("core.compute.assign", type, {value});
            return value;
        };
        const auto clock = identity(input("clock", bit)), enable = identity(input("enable", bit));
        const std::array types{bit, model.logicType(5, true, LogicDomain::TwoState),
            model.logicType(64, false, LogicDomain::TwoState)};
        for (std::size_t i = 0; i < types.size(); ++i)
        {
            const auto type = types[i]; const auto suffix = std::to_string(i);
            const auto a = input("a" + suffix, type), b = input("b" + suffix, type);
            const auto constant = [&](const char *literal) {
                const auto value = model.addValue(type);
                const std::array params{Parameter{model.intern("value"), std::string(literal)}};
                model.addOperation("core.compute.constant", {}, std::array{value}, {}, params);
                return identity(value);
            };
            const auto zero = constant("0"), one = constant("1"), mask = constant("-1");
            const auto algebraicIdentity = [&](ValueId value) {
                value = compute("core.compute.and", type, {mask, value});
                value = compute("core.compute.add", type, {value, zero});
                value = compute("core.compute.mul", type, {one, value});
                value = compute("core.compute.div", type, {value, one});
                value = compute("core.compute.xor", type, {zero, value});
                const auto absorbed = compute("core.compute.and", type, {value, zero});
                return compute("core.compute.add", type, {value, absorbed});
            };
            const auto sum = algebraicIdentity(identity(compute("core.compute.add", type, {identity(a), b})));
            const auto mix = identity(compute("core.compute.xor", type, {sum, b}));
            const auto sumCopy = compute("core.compute.add", type, {identity(b), a});
            const auto mixCopy = compute("core.compute.xor", type, {sumCopy, b});
            const auto combined = compute("core.compute.add", type, {sumCopy, mixCopy});
            output("sum" + suffix, type, sum); output("mix" + suffix, type, mix);
            output("combined" + suffix, type, combined);
            // Removing aliases must preserve pre-commit snapshots, including a
            // state read whose only commit consumer is behind an identity chain.
            ValueId previous = sum;
            for (unsigned stage = 0; stage < 2; ++stage)
            {
                const auto reg = state(type), history = state(bit);
                const auto mask = model.addValue(type); const std::array maskResult{mask};
                const std::array params{Parameter{model.intern("value"), std::string("-1")}};
                model.addOperation("core.compute.constant", {}, maskResult, {}, params);
                const std::array operands{enable, previous, mask, clock};
                const std::array refs{ObjectRef::state(reg), ObjectRef::state(history)};
                const std::array edges{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
                model.addOperation("core.state.regWrite", operands, {}, refs, edges);
                const auto read = model.addValue(type); const std::array result{read};
                const std::array readRefs{ObjectRef::state(reg)};
                model.addOperation("core.state.read", {}, result, readRefs);
                output("q" + std::to_string(stage) + "_" + suffix, type, read); previous = algebraicIdentity(identity(read));
            }
        }
        PassManager manager(defaultDialectRegistry()); std::string error;
        auto pass = defaultPassRegistry().create("grhsim.canonicalize-compute", {}, error);
        require(bool(pass), "identity assignment pass lookup failed"); manager.addPass(std::move(pass));
        diag::Diagnostics rewrite;
        const auto result = manager.run(model, rewrite);
        require(result.success && result.changed, "identity assignment pass did not rewrite fixture");
        unsigned adds = 0, xors = 0;
        for (const auto &op : model.operations())
        {
            require(model.text(op.opType) != "core.compute.assign", "identity assignment chain remains");
            adds += model.text(op.opType) == "core.compute.add";
            xors += model.text(op.opType) == "core.compute.xor";
        }
        require(adds == 6 && xors == 3, "common expressions were not shared through assignment chains");
        map(model, "128", helpers ? "1" : "10000"); diag::Diagnostics diagnostics;
        require(emitCpuCpp(model, directory, diagnostics).success, "identity assignment emit failed");
        const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_identity_assign.mk";
        command("make --no-print-directory -C " + quote(directory.string()) + " -f " + quote(makefile.string()) +
                " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O2 -g -fsanitize=address,undefined -fno-sanitize-recover=all'");
    }

    void testBitPackingDomains()
    {
        for (unsigned test = 0; test < 7; ++test)
        {
            GrhSimModel model("packing_domains"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto four = model.logicType(1, false, LogicDomain::FourState);
            const auto signedBit = model.logicType(1, true, LogicDomain::TwoState);
            const auto targetType = test == 0 ? four : test == 6 ? model.logicType(2, false, LogicDomain::TwoState) : bit;
            const auto input = [&](const char *name, TypeId type) {
                const auto port = model.addInput(name, type); const auto value = model.addValue(type);
                model.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(port)});
                return value;
            };
            const auto clock = input("clock", test == 2 ? four : bit);
            const auto enable = input("enable", test == 3 ? signedBit : bit);
            const auto mask = input("mask", test == 4 ? four : targetType), data = input("data", targetType);
            for (unsigned copy = 0; copy < 2; ++copy)
            {
                const auto q = model.addState("q" + std::to_string(copy), targetType);
                const auto h = model.addState("h" + std::to_string(copy), test == 1 ? four : bit);
                const std::array init{Parameter{model.intern("value"), std::string("0")}};
                const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}};
                model.addInit(q, steps, init); model.addInit(h, steps, init);
                const auto value = model.addValue(targetType);
                const std::vector<Parameter> readParams = test == 5 ?
                    std::vector<Parameter>{{model.intern("extra"), true}} : std::vector<Parameter>{};
                model.addOperation("core.state.read", {}, std::array{value}, std::array{ObjectRef::state(q)}, readParams);
                const auto port = model.addOutput("q" + std::to_string(copy), targetType);
                model.addOperation("core.output.write", std::array{value}, {}, std::array{ObjectRef::output(port)});
                const std::array edges{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
                model.addOperation("core.state.regWrite", std::array{enable, data, mask, clock}, {},
                                   std::array{ObjectRef::state(q), ObjectRef::state(h)}, edges);
            }
            map(model);
            PassManager manager(defaultDialectRegistry()); diag::Diagnostics diagnostics; std::string error;
            manager.addPass(defaultPassRegistry().create("grhsim.pack-bit-registers", {}, error));
            const auto result = manager.run(model, diagnostics);
            require(result.success && !result.changed && model.cpuMapping() && model.states().size() == 4,
                    "bit packing accepted an unsupported type or parameterized read");
        }
    }

    void testPackedBitRegisters(const std::filesystem::path &directory, bool helpers)
    {
        GrhSimModel model("cpu_packed_bits"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto input = [&](const char *name, TypeId type) {
            const auto port = model.addInput(name, type);
            const auto value = model.addValue(type);
            model.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(port)});
            return value;
        };
        const auto state = [&](const std::string &name, TypeId type, const std::string &literal) {
            const auto id = model.addState(name, type);
            const std::array params{Parameter{model.intern("value"), literal}};
            const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}};
            model.addInit(id, steps, params); return id;
        };
        const auto read = [&](StateId id, TypeId type) {
            const auto value = model.addValue(type);
            model.addOperation("core.state.read", {}, std::array{value}, std::array{ObjectRef::state(id)});
            return value;
        };
        const auto clock = input("clock", bit), aux = input("aux", bit), enable = input("enable", bit);
        const auto enable2 = input("enable2", bit), mask = input("mask", bit);
        const auto data = input("data", model.logicType(64, false, LogicDomain::TwoState));
        std::array<ValueId, 64> dataBits;
        for (unsigned i = 0; i < dataBits.size(); ++i)
        {
            dataBits[i] = model.addValue(bit);
            const std::array params{Parameter{model.intern("sliceStart"), int64_t(i)},
                                    Parameter{model.intern("sliceEnd"), int64_t(i)}};
            model.addOperation("core.compute.sliceStatic", std::array{data}, std::array{dataBits[i]}, {}, params);
        }
        const std::array counts{2u, 64u, 130u, 3u, 3u, 2u};
        std::vector<StateId> privateStates;
        for (unsigned group = 0; group < counts.size(); ++group)
        {
            std::vector<StateId> states;
            std::vector<ValueId> values;
            for (unsigned i = 0; i < counts[group]; ++i)
            {
                const auto q = state("q" + std::to_string(group) + "_" + std::to_string(i), bit,
                                     i % 2 ? "1'b1" : "1'b0");
                states.push_back(q); values.push_back(read(q, bit));
            }
            for (unsigned i = 0; i < counts[group]; ++i)
            {
                auto next = dataBits[i % 64];
                if (group == 2)
                {
                    next = model.addValue(bit);
                    model.addOperation("core.compute.xor", std::array{values[(i + 1) % counts[group]], dataBits[i % 64]},
                                       std::array{next});
                }
                const auto history = state("h" + std::to_string(group) + "_" + std::to_string(i), bit, group == 3 ? "1" : "0");
                std::vector<ValueId> operands{group == 4 ? enable2 : enable, next, mask, clock};
                std::vector<ObjectRef> refs{ObjectRef::state(states[i]), ObjectRef::state(history)};
                std::vector<std::string> edges{group == 1 ? "negedge" : "posedge"};
                if (group == 2)
                {
                    operands.push_back(aux); edges.push_back("negedge");
                    refs.push_back(ObjectRef::state(state("ha" + std::to_string(i), bit, "1")));
                }
                const std::array params{Parameter{model.intern("event_edges"), edges}};
                model.addOperation("core.state.regWrite", operands, {}, refs, params);
            }
            if (group == 5)
            {
                privateStates = states;
                const std::array args{DpiArgument{model.intern("a"), DpiDirection::Input, bit},
                                      DpiArgument{model.intern("b"), DpiDirection::Input, bit}};
                const auto function = model.addExternFunction("packed_private", "core.dpi", "packed_private", args, {});
                const std::array params{Parameter{model.intern("event_edges"), std::vector<std::string>{}}};
                model.addOperation("core.dpi.call", std::array{enable, values[0], values[1]}, {},
                                   std::array{ObjectRef::function(function)}, params);
            }
            else
            {
                std::reverse(values.begin(), values.end());
                const auto type = model.logicType(counts[group], false, LogicDomain::TwoState);
                const auto value = model.addValue(type);
                model.addOperation("core.compute.concat", values, std::array{value});
                const auto output = model.addOutput("out" + std::to_string(group), type);
                model.addOperation("core.output.write", std::array{value}, {}, std::array{ObjectRef::output(output)});
            }
        }
        // Exclude uncertain initialization, observable/shared histories and
        // multiple writers even when all visible controls are identical.
        std::vector<std::string> excluded;
        for (unsigned test = 0; test < 8; ++test)
        {
            StateId shared;
            const auto type = test == 0 ? model.logicType(1, true, LogicDomain::TwoState) : bit;
            const auto next = model.addValue(type), writeMask = model.addValue(type);
            model.addOperation("core.compute.assign", std::array{dataBits[test]}, std::array{next});
            model.addOperation("core.compute.assign", std::array{mask}, std::array{writeMask});
            for (unsigned copy = 0; copy < 2; ++copy)
            {
                const auto name = "excluded" + std::to_string(test) + "_" + std::to_string(copy);
                excluded.push_back(name);
                StateId q;
                if (test == 1)
                {
                    q = model.addState(name, type);
                    const std::array steps{InitStep{model.intern("core.init.random"), {0, 0}}};
                    model.addInit(q, steps, {});
                }
                else q = state(name, type, test == 2 ? "1'bx" : "0");
                const auto history = test == 4 && copy ? shared : state(name + "_hist", bit, test == 3 ? "1'bx" : "0");
                shared = history;
                const auto value = read(q, type);
                const auto output = model.addOutput(name, type);
                model.addOperation("core.output.write", std::array{value}, {}, std::array{ObjectRef::output(output)});
                if (test == 5)
                {
                    const auto historyOut = model.addOutput(name + "_hist", bit);
                    model.addOperation("core.output.write", std::array{read(history, bit)}, {}, std::array{ObjectRef::output(historyOut)});
                }
                const std::array params{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
                const auto write = [&](StateId target, StateId hist) {
                    model.addOperation("core.state.regWrite", std::array{enable, next, writeMask, clock}, {},
                                       std::array{ObjectRef::state(target), ObjectRef::state(hist)}, params);
                };
                write(q, history);
                if (test == 6) write(q, state(name + "_second", bit, "0"));
                if (test == 7) write(history, state(name + "_third", bit, "0"));
            }
        }
        map(model, helpers ? "1" : "128", helpers ? "1" : "10000");
        for (auto id : privateStates)
            require(!model.cpuMapping()->schedule->quiescenceProjection[id.index], "packing fixture private state is projected");
        auto reference = model.clone();
        PassManager manager(defaultDialectRegistry()); std::string error; diag::Diagnostics diagnostics;
        manager.addPass(defaultPassRegistry().create("grhsim.pack-bit-registers", {}, error));
        const auto result = manager.run(model, diagnostics);
        for (const auto &message : diagnostics.messages()) std::cout << message.message << '\n';
        require(result.success && result.changed && !model.cpuMapping(), "bit packing failed or kept stale mapping");
        unsigned writes = 0;
        for (const auto &op : model.operations()) writes += model.text(op.opType) == "core.state.regWrite";
        require(writes == 28, "packing did not preserve control, history initial value, chunk, or projection boundaries");
        for (const auto &name : excluded)
            require(std::any_of(model.states().begin(), model.states().end(), [&](const auto &state) {
                return model.text(state.name) == name;
            }), "bit packing removed an excluded state");
        map(model, helpers ? "1" : "128", helpers ? "1" : "10000");
        const auto stable = manager.run(model, diagnostics);
        require(stable.success && !stable.changed, "bit packing is not idempotent after remapping");
        std::stringstream json;
        require(writeGrhSimJson(model, json, defaultDialectRegistry(), diagnostics), "packed bit JSON write failed");
        auto restored = readGrhSimJson(json, defaultDialectRegistry(), diagnostics);
        require(bool(restored), "packed bit JSON reload failed");
        require(emitCpuCpp(*restored, directory, diagnostics).success &&
                emitCpuCpp(reference, directory / "reference", diagnostics).success, "packed bit emit failed");
        const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_packed_bits.mk";
        command("make --no-print-directory -C " + quote(directory.string()) + " -f " + quote(makefile.string()) +
                " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all'");
    }

    void testBitwisePredicates(const std::filesystem::path &directory, bool helpers)
    {
        GrhSimModel model("cpu_predicates"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto signedBit = model.logicType(1, true, LogicDomain::TwoState);
        const auto input = [&](const char *name, TypeId type) {
            const auto port = model.addInput(name, type); const auto value = model.addValue(type);
            model.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(port)});
            return value;
        };
        const auto compute = [&](const char *name, ValueId a, ValueId b) {
            const auto value = model.addValue(bit);
            model.addOperation(name, std::array{a, b}, std::array{value}); return value;
        };
        const auto output = [&](const char *name, ValueId value) {
            const auto port = model.addOutput(name, bit);
            model.addOperation("core.output.write", std::array{value}, {}, std::array{ObjectRef::output(port)});
        };
        const auto a = input("a", bit), b = input("b", bit), c = input("c", bit);
        const auto x = input("x", byte), y = input("y", byte), s = input("s", signedBit);
        const auto land = compute("core.compute.logicAnd", a, b), lor = compute("core.compute.logicOr", a, b);
        output("land", land); output("lor", lor);
        output("chain", compute("core.compute.logicOr", compute("core.compute.logicAnd", lor, c), land));
        output("wide_and", compute("core.compute.logicAnd", x, y));
        output("wide_or", compute("core.compute.logicOr", x, y));
        output("signed_and", compute("core.compute.logicAnd", s, a));
        const auto q = model.addState("q", bit), history = model.addState("history", bit);
        const std::array initParams{Parameter{model.intern("value"), std::string("0")}};
        const std::array initSteps{InitStep{model.intern("core.init.const"), {0, 1}}};
        model.addInit(q, initSteps, initParams); model.addInit(history, initSteps, initParams);
        const auto old = model.addValue(bit), one = model.addValue(bit);
        model.addOperation("core.state.read", {}, std::array{old}, std::array{ObjectRef::state(q)});
        const std::array literal{Parameter{model.intern("value"), std::string("1")}};
        model.addOperation("core.compute.constant", {}, std::array{one}, {}, literal);
        const std::array edges{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
        model.addOperation("core.state.regWrite", std::array{lor, land, one, c}, {},
                           std::array{ObjectRef::state(q), ObjectRef::state(history)}, edges);
        output("q", old);
        output("state_and", compute("core.compute.logicAnd", old, a));
        map(model, helpers ? "1" : "128", helpers ? "1" : "10000");
        PassManager manager(defaultDialectRegistry()); std::string error; diag::Diagnostics diagnostics;
        manager.addPass(defaultPassRegistry().create("grhsim.bitwise-predicates", {}, error));
        require(manager.run(model, diagnostics).success && !model.cpuMapping(), "predicate pass retained stale mapping");
        map(model, helpers ? "1" : "128", helpers ? "1" : "10000");
        require(emitCpuCpp(model, directory, diagnostics).success, "predicate model emit failed");
        const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_predicates.mk";
        command("make --no-print-directory -C " + quote(directory.string()) + " -f " + quote(makefile.string()) +
                " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O0 -g -fsanitize=address,undefined -fno-sanitize-recover=all'");
    }

    void testBitwiseMuxes(const std::filesystem::path &directory, bool helpers) {
        GrhSimModel model("cpu_bit_select"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto signed5 = model.logicType(5, true, LogicDomain::TwoState);
        const auto word = model.logicType(64, false, LogicDomain::TwoState);
        const auto input = [&](const char *name, TypeId type) {
            const auto port = model.addInput(name, type);
            const auto value = model.addValue(type);
            model.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(port)});
            return value;
        };
        const auto compute = [&](const char *name, TypeId type, ValueId mask, ValueId a, ValueId b) {
            const auto value = model.addValue(type);
            model.addOperation(name, std::array{mask, a, b}, std::array{value}); return value;
        };
        const auto output = [&](const char *name, ValueId value) {
            const auto port = model.addOutput(name, model.values()[value.index - 1].type);
            model.addOperation("core.output.write", std::array{value}, {}, std::array{ObjectRef::output(port)});
        };
        const auto a = input("a", bit), b = input("b", bit), c = input("c", bit);
        const auto clock = input("clock", bit), condition = input("condition", byte);
        const auto y = compute("core.compute.mux", bit, c, a, b);
        const auto nested = compute("core.compute.mux", bit, y, b, c);
        output("selected", y); output("nested", nested);
        output("nonbool", compute("core.compute.mux", bit, condition, a, b));
        const auto mask5 = input("mask5", signed5), a5 = input("a5", signed5), b5 = input("b5", signed5);
        output("selected5", compute("core.compute.bitSelect", signed5, mask5, a5, b5));
        const auto mask64 = input("mask64", word), a64 = input("a64", word), b64 = input("b64", word);
        output("selected64", compute("core.compute.bitSelect", word, mask64, a64, b64));
        const auto state = [&](const char *name) {
            const auto id = model.addState(name, bit);
            const std::array params{Parameter{model.intern("value"), std::string("0")}};
            const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}};
            model.addInit(id, steps, params); return id;
        };
        const auto q = state("q"), r = state("r"), hq = state("hq"), hr = state("hr");
        const auto read = [&](StateId id) {
            const auto value = model.addValue(bit);
            model.addOperation("core.state.read", {}, std::array{value}, std::array{ObjectRef::state(id)});
            return value;
        };
        const auto oldQ = read(q), oldR = read(r), one = model.addValue(bit);
        const std::array literal{Parameter{model.intern("value"), std::string("1")}};
        model.addOperation("core.compute.constant", {}, std::array{one}, {}, literal);
        const std::array edges{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
        model.addOperation("core.state.regWrite", std::array{one, nested, one, clock}, {},
                           std::array{ObjectRef::state(q), ObjectRef::state(hq)}, edges);
        model.addOperation("core.state.regWrite", std::array{one, oldQ, one, clock}, {},
                           std::array{ObjectRef::state(r), ObjectRef::state(hr)}, edges);
        output("registered", oldQ); output("delayed", oldR);
        map(model);
        PassManager manager(defaultDialectRegistry()); std::string error; diag::Diagnostics diagnostics;
        manager.addPass(defaultPassRegistry().create("grhsim.bitwise-muxes", {}, error));
        const auto first = manager.run(model, diagnostics);
        require(first.success && first.changed && !model.cpuMapping(), "bitwise mux pass retained stale mapping");
        const auto second = manager.run(model, diagnostics);
        require(second.success && !second.changed, "bitwise mux pass is not idempotent");
        unsigned converted = 0, remaining = 0;
        for (const auto &op : model.operations()) {
            converted += model.text(op.opType) == "core.compute.bitSelect";
            remaining += model.text(op.opType) == "core.compute.mux";
        }
        require(converted == 4 && remaining == 1, "bitwise mux changed a nonboolean condition");
        map(model, helpers ? "1" : "128", helpers ? "1" : "10000");
        std::stringstream json;
        require(writeGrhSimJson(model, json, defaultDialectRegistry(), diagnostics), "bitSelect JSON write failed");
        auto restored = readGrhSimJson(json, defaultDialectRegistry(), diagnostics);
        require(restored && emitCpuCpp(*restored, directory, diagnostics).success, "bitSelect roundtrip or emit failed");
        const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_bit_select.mk";
        command("make --no-print-directory -C " + quote(directory.string()) + " -f " + quote(makefile.string()) +
                " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all'");
    }

    void testMuxChainFold(const std::filesystem::path &directory, bool helpers)
    {
        GrhSimModel model("cpu_mux_chain"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto input = [&](const char *name, TypeId type) {
            const auto port = model.addInput(name, type);
            const auto value = model.addValue(type);
            model.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(port)});
            return value;
        };
        const auto output = [&](const char *name, ValueId value) {
            const auto port = model.addOutput(name, byte);
            model.addOperation("core.output.write", std::array{value}, {}, std::array{ObjectRef::output(port)});
        };
        const auto mux = [&](ValueId condition, ValueId onTrue, ValueId onFalse) {
            const auto value = model.addValue(byte);
            model.addOperation("core.compute.mux", std::array{condition, onTrue, onFalse}, std::array{value});
            return value;
        };
        std::vector<ValueId> conditions;
        for (const char *name : {"c0", "c1", "c2", "c3", "e0", "e1", "e2", "e3", "e4", "f0", "f1"})
            conditions.push_back(input(name, bit));
        const auto c = conditions.begin();
        std::vector<ValueId> arms;
        for (const char *name : {"a0", "a1", "a2", "a3", "a4"}) arms.push_back(input(name, byte));
        const auto d = input("d", byte);
        const auto clock = input("clock", bit);
        // A four-link priority chain folds into one prioritySelect.
        const auto selected = mux(c[0], arms[0], mux(c[1], arms[1], mux(c[2], arms[2], mux(c[3], arms[3], d))));
        output("sel", selected);
        // Two links stay plain muxes.
        output("pair", mux(c[9], arms[0], mux(c[10], arms[1], d)));
        // A tapped middle link blocks the head; only the three-link suffix folds.
        const auto suffix = mux(c[6], arms[2], mux(c[7], arms[3], mux(c[8], arms[4], d)));
        output("tapped", suffix);
        mux(c[4], arms[0], mux(c[5], arms[1], suffix));
        const auto state = [&](const char *name, TypeId type) {
            const auto id = model.addState(name, type);
            const std::array initParams{Parameter{model.intern("value"), std::string("0")}};
            const std::array initSteps{InitStep{model.intern("core.init.const"), {0, 1}}};
            model.addInit(id, initSteps, initParams); return id;
        };
        const auto q = state("q", byte), hq = state("hq", bit);
        const auto qd = state("qd", byte), hqd = state("hqd", bit);
        const auto oldQ = model.addValue(byte), oldQd = model.addValue(byte);
        model.addOperation("core.state.read", {}, std::array{oldQ}, std::array{ObjectRef::state(q)});
        model.addOperation("core.state.read", {}, std::array{oldQd}, std::array{ObjectRef::state(qd)});
        const auto one = model.addValue(bit), full = model.addValue(byte);
        const std::array oneParam{Parameter{model.intern("value"), std::string("1")}};
        const std::array fullParam{Parameter{model.intern("value"), std::string("255")}};
        model.addOperation("core.compute.constant", {}, std::array{one}, {}, oneParam);
        model.addOperation("core.compute.constant", {}, std::array{full}, {}, fullParam);
        const std::array edges{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
        model.addOperation("core.state.regWrite", std::array{one, selected, full, clock}, {},
                           std::array{ObjectRef::state(q), ObjectRef::state(hq)}, edges);
        model.addOperation("core.state.regWrite", std::array{one, oldQ, full, clock}, {},
                           std::array{ObjectRef::state(qd), ObjectRef::state(hqd)}, edges);
        output("qt", oldQ); output("qd", oldQd);
        map(model, helpers ? "1" : "128", helpers ? "1" : "10000");
        PassManager manager(defaultDialectRegistry()); std::string error; diag::Diagnostics diagnostics;
        manager.addPass(defaultPassRegistry().create("grhsim.mux-chain-fold", {}, error));
        const auto folded = manager.run(model, diagnostics);
        require(folded.success && folded.changed && !model.cpuMapping(), "mux chain fold retained stale mapping");
        unsigned selects = 0, muxes = 0;
        for (const auto &op : model.operations())
        {
            selects += model.text(op.opType) == "core.compute.prioritySelect";
            muxes += model.text(op.opType) == "core.compute.mux";
        }
        require(selects == 2 && muxes == 4, "mux chain fold coverage differs");
        std::stringstream json;
        require(writeGrhSimJson(model, json, defaultDialectRegistry(), diagnostics), "prioritySelect JSON write failed");
        auto restored = readGrhSimJson(json, defaultDialectRegistry(), diagnostics);
        require(bool(restored), "prioritySelect JSON fresh load failed");
        map(*restored, helpers ? "1" : "128", helpers ? "1" : "10000");
        require(emitCpuCpp(*restored, directory, diagnostics).success, "mux chain model emit failed");
        std::string generated;
        for (const auto &entry : std::filesystem::directory_iterator(directory))
            if (entry.path().extension() == ".cpp")
            {
                std::ifstream stream(entry.path());
                generated.append(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
            }
        unsigned nested = 0;
        std::istringstream lines(generated);
        for (std::string line; std::getline(lines, line);)
        {
            unsigned calls = 0;
            for (std::size_t at = 0; (at = line.find("grhsim_mux_u64(", at)) != std::string::npos; at += 15)
                ++calls;
            nested += calls >= 4;
        }
        require(nested >= 1, "prioritySelect lost the nested branchless select form");
        const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_mux_chain.mk";
        command("make --no-print-directory -C " + quote(directory.string()) + " -f " + quote(makefile.string()) +
                " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all'");
    }

    void testCommitCompactWalk(const std::filesystem::path &directory)
    {
        GrhSimModel model("cpu_commit_batch"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto word = model.logicType(64, false, LogicDomain::TwoState);
        const auto input = [&](const char *name, TypeId type) {
            const auto port = model.addInput(name, type);
            const auto value = model.addValue(type);
            model.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(port)});
            return value;
        };
        const auto state = [&](const char *name, TypeId type) {
            const auto id = model.addState(name, type);
            const std::array initParams{Parameter{model.intern("value"), std::string("0")}};
            const std::array initSteps{InitStep{model.intern("core.init.const"), {0, 1}}};
            model.addInit(id, initSteps, initParams); return id;
        };
        const auto compute = [&](const char *op, TypeId type, std::initializer_list<ValueId> args) {
            const auto value = model.addValue(type);
            model.addOperation(op, {args.begin(), args.size()}, std::array{value}); return value;
        };
        const auto constant = [&](const char *text) {
            const auto value = model.addValue(word);
            const std::array params{Parameter{model.intern("value"), std::string(text)}};
            model.addOperation("core.compute.constant", {}, std::array{value}, {}, params); return value;
        };
        const auto clock = input("clock", bit), enw = input("enw", word), dw = input("dw", word);
        const auto fullMask = constant("64'hffffffffffffffff");
        const std::array edges{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
        ValueId folded;
        // 64 uniform u64 full-mask ports form one full compact-walk group.
        for (unsigned i = 0; i < 64; ++i)
        {
            const auto enBit = model.addValue(bit);
            {
                const std::array ops{enw};
                const std::array params{Parameter{model.intern("sliceStart"), static_cast<long long>(i)},
                                        Parameter{model.intern("sliceEnd"), static_cast<long long>(i)}};
                model.addOperation("core.compute.sliceStatic", ops, std::array{enBit}, {}, params);
            }
            const auto step = constant(std::to_string(i).c_str());
            const auto data = compute("core.compute.add", word, {dw, step});
            const auto suffix = std::to_string(i);
            const auto q = state(("q" + suffix).c_str(), word), hq = state(("hq" + suffix).c_str(), bit);
            model.addOperation("core.state.regWrite", std::array{enBit, data, fullMask, clock}, {},
                               std::array{ObjectRef::state(q), ObjectRef::state(hq)}, edges);
            const auto read = model.addValue(word);
            model.addOperation("core.state.read", {}, std::array{read}, std::array{ObjectRef::state(q)});
            if (!folded) folded = read;
            else
            {
                const auto next = model.addValue(word);
                model.addOperation("core.compute.xor", std::array{folded, read}, std::array{next});
                folded = next;
            }
        }
        // One variable-mask port and one constant-enable port share the edge but must
        // stay in the per-bit walk (the latter has no boundary-resident enable byte).
        const auto enm = input("enm", bit), dm = input("dm", word), mm = input("mm", word);
        const auto qm = state("qm", word), hqm = state("hqm", bit);
        model.addOperation("core.state.regWrite", std::array{enm, dm, mm, clock}, {},
                           std::array{ObjectRef::state(qm), ObjectRef::state(hqm)}, edges);
        const auto constEnable = model.addValue(bit);
        {
            const std::array params{Parameter{model.intern("value"), std::string("1")}};
            model.addOperation("core.compute.constant", {}, std::array{constEnable}, {}, params);
        }
        const auto dc = input("dc", word);
        const auto qc = state("qc", word), hqc = state("hqc", bit);
        model.addOperation("core.state.regWrite", std::array{constEnable, dc, fullMask, clock}, {},
                           std::array{ObjectRef::state(qc), ObjectRef::state(hqc)}, edges);
        const auto readM = model.addValue(word);
        model.addOperation("core.state.read", {}, std::array{readM}, std::array{ObjectRef::state(qm)});
        const auto readC = model.addValue(word);
        model.addOperation("core.state.read", {}, std::array{readC}, std::array{ObjectRef::state(qc)});
        const auto mix1 = model.addValue(word);
        model.addOperation("core.compute.xor", std::array{folded, readM}, std::array{mix1});
        const auto mixed = model.addValue(word);
        model.addOperation("core.compute.xor", std::array{mix1, readC}, std::array{mixed});
        const auto out = model.addOutput("x", word);
        model.addOperation("core.output.write", std::array{mixed}, {}, std::array{ObjectRef::output(out)});
        map(model);
        diag::Diagnostics diagnostics;
        const auto emitted = emitCpuCpp(model, directory, diagnostics, false, true);
        require(emitted.success, "commit compact walk emit failed");
        std::string generated;
        for (const auto &entry : std::filesystem::directory_iterator(directory))
            if (entry.path().extension() == ".cpp")
            {
                std::ifstream stream(entry.path());
                generated.append(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
            }
        require(generated.find("// cpu_compact_walk") != std::string::npos, "uniform u64 group was not compact-walked");
        require(generated.find("__builtin_ctzll(cpu_todo)") != std::string::npos, "compact walk lost the ctz iteration");
        require(generated.find("if(cpu_armed&1){") != std::string::npos,
                "variable-mask port left the per-bit walk");
        require(generated.find("grhsim_trunc_u64(UINT64_C(18446744073709551615),1))") != std::string::npos,
                "constant-enable port left the per-bit walk");
        const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_commit_batch.mk";
        command("make --no-print-directory -C " + quote(directory.string()) + " -f " + quote(makefile.string()) +
                " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all'");
    }

    // A run of >=4 same-snapshot-guard memWrite ports hoists the pure-read edge
    // guard once and caches the distinct boundary enable bytes in locals; a short
    // run (<4) keeps the per-port form. Scoreboard checks aliasing address writes
    // stay in program order inside the hoisted block.
    void testCommitMemWalk(const std::filesystem::path &directory)
    {
        GrhSimModel model("cpu_commit_memwalk"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto word = model.logicType(64, false, LogicDomain::TwoState);
        const auto input = [&](const char *name, TypeId type) {
            const auto port = model.addInput(name, type);
            const auto value = model.addValue(type);
            model.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(port)});
            return value;
        };
        const auto state = [&](const char *name, TypeId type) {
            const auto id = model.addState(name, type);
            const std::array initParams{Parameter{model.intern("value"), std::string("0")}};
            const std::array initSteps{InitStep{model.intern(model.types()[type.index - 1].kind == TypeKind::Array ? "core.init.fill" : "core.init.const"), {0, 1}}};
            model.addInit(id, initSteps, initParams); return id;
        };
        const auto compute = [&](const char *op, TypeId type, std::initializer_list<ValueId> args) {
            const auto value = model.addValue(type);
            model.addOperation(op, {args.begin(), args.size()}, std::array{value}); return value;
        };
        const auto constant = [&](TypeId type, const char *text) {
            const auto value = model.addValue(type);
            const std::array params{Parameter{model.intern("value"), std::string(text)}};
            model.addOperation("core.compute.constant", {}, std::array{value}, {}, params); return value;
        };
        const auto slice = [&](ValueId source, long long index) {
            const auto value = model.addValue(bit);
            const std::array ops{source};
            const std::array params{Parameter{model.intern("sliceStart"), index}, Parameter{model.intern("sliceEnd"), index}};
            model.addOperation("core.compute.sliceStatic", ops, std::array{value}, {}, params); return value;
        };
        const auto clock = input("clock", bit), clock2 = input("clock2", bit);
        const auto enw = input("enw", word), dw = input("dw", word), aw = input("aw", byte);
        const auto fullMask = constant(word, "64'hffffffffffffffff");
        const auto memory = state("mem", model.arrayType(word, 16));
        const auto memory2 = state("mem2", model.arrayType(word, 16));
        const auto port = [&](StateId array, ValueId clockEvent, ValueId enable, long long index) {
            const auto stepW = constant(word, std::to_string(index).c_str());
            const auto stepB = constant(byte, std::to_string(index).c_str());
            const auto data = compute("core.compute.add", word, {dw, stepW});
            const auto addr = compute("core.compute.add", byte, {aw, stepB});
            const std::array edges{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
            model.addOperation("core.state.memWrite", std::array{enable, addr, data, fullMask, clockEvent}, {},
                               std::array{ObjectRef::state(array), ObjectRef::state(state("h", bit))}, edges);
        };
        // 12 same-guard ports (first four share one enable) -> one hoisted run with a
        // deduplicated enable cache; 3 same-guard ports stay per-port (<4).
        const auto sharedEnable = slice(enw, 0);
        for (long long i = 0; i < 12; ++i) port(memory, clock, i < 4 ? sharedEnable : slice(enw, i), i);
        for (long long i = 0; i < 3; ++i) port(memory2, clock2, slice(enw, 16 + i), i);
        const auto foldCells = [&](StateId array, ValueId base) {
            ValueId folded = base;
            for (long long i = 0; i < 16; ++i)
            {
                const auto cell = model.addValue(word);
                const std::array readOps{constant(byte, std::to_string(i).c_str())};
                model.addOperation("core.state.memRead", readOps, std::array{cell},
                                   std::array{ObjectRef::state(array)});
                if (!folded) folded = cell;
                else folded = compute("core.compute.xor", word, {folded, cell});
            }
            return folded;
        };
        const auto zeroW = constant(word, "0");
        const auto folded = foldCells(memory, foldCells(memory2, zeroW));
        const auto out = model.addOutput("x", word);
        model.addOperation("core.output.write", std::array{folded}, {}, std::array{ObjectRef::output(out)});
        map(model);
        diag::Diagnostics diagnostics;
        const auto emitted = emitCpuCpp(model, directory, diagnostics, false, false, true);
        require(emitted.success, "commit mem walk emit failed");
        std::string generated;
        for (const auto &entry : std::filesystem::directory_iterator(directory))
            if (entry.path().extension() == ".cpp")
            {
                std::ifstream stream(entry.path());
                generated.append(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
            }
        require(generated.find("// cpu_mem_guard_hoist ops=12") != std::string::npos,
                "same-guard memWrite run was not guard-hoisted");
        require(generated.find("cpu_men_") != std::string::npos, "boundary enables were not cached");
        require(generated.find("ops=3") == std::string::npos, "short memWrite run must stay per-port");
        const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_commit_memwalk.mk";
        command("make --no-print-directory -C " + quote(directory.string()) + " -f " + quote(makefile.string()) +
                " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all'");
    }

    void testReplicateBroadcast(const std::filesystem::path &directory, bool helpers)
    {
        GrhSimModel model("cpu_replicate_broadcast"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto input = [&](const char *name, TypeId type) {
            const auto port = model.addInput(name, type);
            const auto value = model.addValue(type);
            model.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(port)});
            return value;
        };
        const auto output = [&](const char *name, TypeId type, ValueId value) {
            const auto port = model.addOutput(name, type);
            model.addOperation("core.output.write", std::array{value}, {}, std::array{ObjectRef::output(port)});
        };
        const auto b = input("b", bit), clock = input("clock", bit), w8 = input("w8", byte);
        const auto replicate = [&](const char *name, ValueId source, unsigned width, int64_t rep) {
            const auto type = model.logicType(width, false, LogicDomain::TwoState);
            const auto value = model.addValue(type);
            const std::array params{Parameter{model.intern("rep"), rep}};
            model.addOperation("core.compute.replicate", std::array{source}, std::array{value}, {}, params);
            output(name, type, value);
            return value;
        };
        replicate("r1", b, 1, 1);
        replicate("r3", b, 3, 3);
        replicate("r32", b, 32, 32);
        replicate("r64", b, 64, 64);
        replicate("r66", b, 66, 66);
        replicate("r130", b, 130, 130);
        replicate("wide9", w8, 72, 9);
        const auto state = [&](const char *name) {
            const auto id = model.addState(name, bit);
            const std::array params{Parameter{model.intern("value"), std::string("0")}};
            const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}};
            model.addInit(id, steps, params); return id;
        };
        const auto q = state("q"), hq = state("hq");
        const std::array edges{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
        const auto one = model.addValue(bit);
        const std::array literal{Parameter{model.intern("value"), std::string("1")}};
        model.addOperation("core.compute.constant", {}, std::array{one}, {}, literal);
        model.addOperation("core.state.regWrite", std::array{one, b, one, clock}, {},
                           std::array{ObjectRef::state(q), ObjectRef::state(hq)}, edges);
        const auto oldQ = model.addValue(bit);
        model.addOperation("core.state.read", {}, std::array{oldQ}, std::array{ObjectRef::state(q)});
        output("q", bit, oldQ);
        replicate("q32", oldQ, 32, 32);
        map(model, helpers ? "1" : "128", helpers ? "1" : "10000");
        diag::Diagnostics diagnostics;
        require(emitCpuCpp(model, directory, diagnostics).success, "replicate broadcast emit failed");
        const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_replicate_broadcast.mk";
        command("make --no-print-directory -C " + quote(directory.string()) + " -f " + quote(makefile.string()) +
                " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all'");
    }

    void testDynamicStats(const std::filesystem::path &directory)
    {
        GrhSimModel model("cpu_dynamic_stats"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto input = [&](const char *name, TypeId type) {
            const auto port = model.addInput(name, type);
            const auto value = model.addValue(type);
            model.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(port)});
            return value;
        };
        const auto compute = [&](const char *name, TypeId type, std::initializer_list<ValueId> args) {
            const auto value = model.addValue(type);
            model.addOperation(name, {args.begin(), args.size()}, std::array{value}); return value;
        };
        const auto output = [&](const char *name, TypeId type, ValueId value) {
            const auto port = model.addOutput(name, type);
            model.addOperation("core.output.write", std::array{value}, {}, std::array{ObjectRef::output(port)});
        };
        const auto a = input("a", byte), b = input("b", byte);
        const auto clock = input("clock", bit), enable = input("enable", bit);
        const auto sum = compute("core.compute.add", byte, {a, b});
        const auto sel = compute("core.compute.mux", byte, {enable, sum, a});
        const auto dat = compute("core.compute.xor", byte, {sel, a});
        const auto msk = compute("core.compute.or", byte, {a, b});
        const auto en = compute("core.compute.and", bit, {enable, compute("core.compute.reduceOr", byte, {b})});
        output("out_sum", byte, sum); output("out_sel", byte, sel); output("out_dat", byte, dat);
        const auto state = [&](const char *name, TypeId type) {
            const auto id = model.addState(name, type);
            const std::array params{Parameter{model.intern("value"), std::string("0")}};
            const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}};
            model.addInit(id, steps, params); return id;
        };
        const auto q = state("q", byte), hq = state("hq", bit);
        const std::array edges{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
        model.addOperation("core.state.regWrite", std::array{en, dat, msk, clock}, {},
                           std::array{ObjectRef::state(q), ObjectRef::state(hq)}, edges);
        const auto oldQ = model.addValue(byte);
        model.addOperation("core.state.read", {}, std::array{oldQ}, std::array{ObjectRef::state(q)});
        output("out_q", byte, oldQ);
        map(model);
        diag::Diagnostics diagnostics;
        require(emitCpuCpp(model, directory, diagnostics, true).success, "dynamic stats emit failed");
        {
            std::ifstream stream(directory / "grhsim_cpu_dynamic_stats.hpp");
            const std::string header((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            require(header.find("cpu_dyn_wr") != std::string::npos && header.find("cpu_dyn_sn_act") != std::string::npos,
                    "dynamic stats counters were not declared");
        }
        const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_dynamic_stats.mk";
        command("make --no-print-directory -C " + quote(directory.string()) + " -f " + quote(makefile.string()) +
                " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all'");
        {
            GrhSimModel collision("cpu_dyn_collision"); collision.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = collision.logicType(1, false, LogicDomain::TwoState);
            const auto port = collision.addInput("cpu_dyn_wr", bit);
            const auto value = collision.addValue(bit);
            collision.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(port)});
            const auto out = collision.addOutput("o", bit);
            collision.addOperation("core.output.write", std::array{value}, {}, std::array{ObjectRef::output(out)});
            map(collision);
            diag::Diagnostics invalid;
            require(!emitCpuCpp(collision, directory / "collision", invalid, true).success &&
                    !std::filesystem::exists(directory / "collision"), "dynamic stats reserved name was accepted");
        }
    }

    void testHelperReadCaches(const std::filesystem::path &directory, bool helpers)
    {
        GrhSimModel model("cpu_helper_read_cache"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto nibble = model.logicType(4, false, LogicDomain::TwoState);
        const auto signedByte = model.logicType(8, true, LogicDomain::TwoState);
        const auto input = [&](const char *name, TypeId type) {
            const auto port = model.addInput(name, type);
            const auto value = model.addValue(type);
            model.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(port)});
            return value;
        };
        const auto compute = [&](const char *name, TypeId type, std::initializer_list<ValueId> args) {
            const auto value = model.addValue(type);
            model.addOperation(name, {args.begin(), args.size()}, std::array{value}); return value;
        };
        const auto output = [&](const char *name, TypeId type, ValueId value) {
            const auto port = model.addOutput(name, type);
            model.addOperation("core.output.write", std::array{value}, {}, std::array{ObjectRef::output(port)});
        };
        const auto a = input("a", byte), b = input("b", byte);
        const auto clock = input("clock", bit), enable = input("enable", bit);
        const auto sum0 = compute("core.compute.add", byte, {a, b});
        const auto sum1 = compute("core.compute.add", byte, {a, b});
        // This boundary value has both external commit users and local users;
        // combining its producer with those local users must disable caching.
        const auto not0 = compute("core.compute.logicNot", bit, {enable});
        const auto not1 = compute("core.compute.logicNot", bit, {enable});
        output("sum", byte, sum0);
        output("equal", byte, compute("core.compute.xor", byte, {sum0, sum0}));
        output("not_equal", bit, compute("core.compute.xor", bit, {not0, not1}));
        output("ab", byte, compute("core.compute.sub", byte, {a, b}));
        output("ba", byte, compute("core.compute.sub", byte, {b, a}));
        output("small", nibble, compute("core.compute.add", nibble, {a, b}));
        output("signed_sum", signedByte, compute("core.compute.add", signedByte, {a, b}));
        for (unsigned start = 0; start < 2; ++start)
        {
            const auto value = model.addValue(nibble);
            const std::array params{Parameter{model.intern("sliceStart"), int64_t(start)}};
            model.addOperation("core.compute.sliceStatic", std::array{a}, std::array{value}, {}, params);
            output(start ? "slice1" : "slice0", nibble, value);
        }
        const auto state = [&](const char *name, TypeId type) {
            const auto id = model.addState(name, type);
            const std::array params{Parameter{model.intern("value"), std::string("0")}};
            const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}};
            model.addInit(id, steps, params); return id;
        };
        const auto mask = model.addValue(byte);
        const std::array literal{Parameter{model.intern("value"), std::string("255")}};
        model.addOperation("core.compute.constant", {}, std::array{mask}, {}, literal);
        for (unsigned i = 0; i < 2; ++i)
        {
            const auto q = state(i ? "reg1" : "reg0", byte), history = state(i ? "hist1" : "hist0", bit);
            const std::array edges{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
            model.addOperation("core.state.regWrite", std::array{i ? not1 : enable, i ? sum1 : sum0, mask, clock}, {},
                               std::array{ObjectRef::state(q), ObjectRef::state(history)}, edges);
            const auto value = model.addValue(byte);
            model.addOperation("core.state.read", {}, std::array{value}, std::array{ObjectRef::state(q)});
            output(i ? "q1" : "q0", byte, value);
        }
        map(model, "2", helpers ? "1" : "10000");
        const auto &layout = *model.cpuMapping()->dataLayout;
        require(layout.helperReadCaches && !layout.helperReadCaches->empty(), "fixture missed helper read cache plan");
        bool cachedSum = false;
        for (const auto &cache : *layout.helperReadCaches)
            for (const auto value : cache.values)
                cachedSum |= value == sum0;
        require(cachedSum, "repeated stable boundary value was not cached");
        auto combined = model.clone(); map(combined, "128", "10000");
        for (const auto &cache : *combined.cpuMapping()->dataLayout->helperReadCaches)
            for (const auto value : cache.values)
                require(value != sum0, "helper-produced boundary value was read before its definition");
        for (unsigned defect = 0; defect < 3; ++defect)
        {
            auto broken = model.clone(); auto mapping = *broken.cpuMapping();
            auto &plan = *mapping.dataLayout->helperReadCaches;
            if (defect == 0) plan[0].firstOp = {};
            if (defect == 1) plan[0].values.push_back(plan[0].values.front());
            if (defect == 2) plan.pop_back();
            broken.setCpuMapping(std::move(mapping)); diag::Diagnostics diagnostics;
            require(!verifyGrhSimModel(broken, defaultDialectRegistry(), diagnostics), "invalid read cache plan passed verification");
        }
        diag::Diagnostics diagnostics; std::stringstream json;
        require(writeGrhSimJson(model, json, defaultDialectRegistry(), diagnostics), "read cache JSON write failed");
        auto restored = readGrhSimJson(json, defaultDialectRegistry(), diagnostics);
        require(restored && restored->cpuMapping()->dataLayout == model.cpuMapping()->dataLayout,
                "read cache plan did not survive JSON round-trip");
        require(emitCpuCpp(*restored, directory, diagnostics).success, "read cache emit failed");
        auto legacy = model.clone(); auto mapping = *legacy.cpuMapping();
        mapping.dataLayout->helperReadCaches.reset();
        legacy.setCpuMapping(std::move(mapping)); std::stringstream oldJson;
        require(writeGrhSimJson(legacy, oldJson, defaultDialectRegistry(), diagnostics), "legacy layout JSON write failed");
        auto old = readGrhSimJson(oldJson, defaultDialectRegistry(), diagnostics);
        require(old && !old->cpuMapping()->dataLayout->helperReadCaches, "legacy layout gained a fabricated read cache plan");
        const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_helper_read_cache.mk";
        command("make --no-print-directory -C " + quote(directory.string()) + " -f " + quote(makefile.string()) +
                " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all'");
    }

    void testSharedComputeClones(const std::filesystem::path &directory, bool helpers)
    {
        GrhSimModel model("cpu_clones"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto input = [&](const char *name, TypeId type) {
            const auto port = model.addInput(name, type);
            const auto value = model.addValue(type);
            model.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(port)});
            return value;
        };
        const auto compute = [&](const char *name, TypeId type, std::initializer_list<ValueId> args) {
            const auto result = model.addValue(type);
            model.addOperation(name, {args.begin(), args.size()}, std::array{result}); return result;
        };
        const auto output = [&](const char *name, TypeId type, ValueId value) {
            const auto port = model.addOutput(name, type);
            model.addOperation("core.output.write", std::array{value}, {}, std::array{ObjectRef::output(port)});
        };
        const auto state = [&](TypeId type) {
            const auto id = model.addState("s" + std::to_string(model.states().size()), type);
            const std::array params{Parameter{model.intern("value"), std::string("0")}};
            const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}};
            model.addInit(id, steps, params); return id;
        };
        const auto read = [&](StateId state) {
            const auto value = model.addValue(byte);
            model.addOperation("core.state.read", {}, std::array{value}, std::array{ObjectRef::state(state)});
            return value;
        };
        const auto clock = input("clock", bit), inhibit = input("inhibit", bit);
        const auto a = input("a", byte), b = input("b", byte);
        const auto enable = compute("core.compute.logicNot", bit, {inhibit});
        const auto q0 = state(byte), q1 = state(byte);
        const auto current = read(q0), previous = read(q1);
        const auto inverted = compute("core.compute.not", byte, {current});
        const auto mask = model.addValue(byte);
        const std::array literal{Parameter{model.intern("value"), std::string("255")}};
        model.addOperation("core.compute.constant", {}, std::array{mask}, {}, literal);
        const auto write = [&](StateId target, ValueId value) {
            const auto history = state(bit);
            const std::array edges{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
            model.addOperation("core.state.regWrite", std::array{enable, value, mask, clock}, {},
                std::array{ObjectRef::state(target), ObjectRef::state(history)}, edges);
        };
        write(q0, a); write(q1, inverted);
        output("q0", byte, current); output("q1", byte, previous);
        output("left", byte, compute("core.compute.and", byte, {inverted, a}));
        output("right", byte, compute("core.compute.or", byte, {inverted, b}));
        output("duplicate", byte, compute("core.compute.xor", byte, {inverted, inverted}));
        output("enabled_clock", bit, compute("core.compute.and", bit, {enable, clock}));
        output("enabled_or_clock", bit, compute("core.compute.or", bit, {enable, clock}));
        output("raw_inhibit", bit, inhibit);
        const auto word = model.logicType(64, false, LogicDomain::TwoState);
        const auto wordValue = input("word", word), five = model.addValue(word);
        const std::array fiveParams{Parameter{model.intern("value"), std::string("5")}};
        model.addOperation("core.compute.constant", {}, std::array{five}, {}, fiveParams);
        const std::array roots{
            compute("core.compute.not", word, {wordValue}),
            compute("core.compute.xor", word, {wordValue, five}),
            compute("core.compute.add", word, {five, wordValue}),
            compute("core.compute.sub", word, {wordValue, five}),
            compute("core.compute.sub", word, {five, wordValue})};
        for (std::size_t i = 0; i < roots.size(); ++i)
        {
            output(("and" + std::to_string(i)).c_str(), word, compute("core.compute.and", word, {roots[i], wordValue}));
            output(("xor" + std::to_string(i)).c_str(), word, compute("core.compute.xor", word, {roots[i], wordValue}));
        }
        const auto chained = compute("core.compute.add", word, {roots[0], five});
        output("and_chain", word, compute("core.compute.and", word, {chained, wordValue}));
        output("xor_chain", word, compute("core.compute.xor", word, {chained, wordValue}));
        PassManager manager(defaultDialectRegistry()); std::string error; diag::Diagnostics diagnostics;
        manager.addPass(defaultPassRegistry().create("grhsim.clone-shared-compute", {}, error));
        require(manager.run(model, diagnostics).success, "cloning failed");
        unsigned reads = 0, clones = 0;
        for (const auto &op : model.operations())
        {
            reads += model.text(op.opType) == "core.state.read";
            clones += model.text(op.name).find(".local") != std::string_view::npos;
        }
        require(reads == 2 && clones == 19, "clones missed scalar bijections/chains or duplicated state reads/operand uses");
        map(model, helpers ? "2" : "128", helpers ? "1" : "10000");
        std::stringstream serialized;
        require(writeGrhSimJson(model, serialized, defaultDialectRegistry(), diagnostics), "clone JSON write failed");
        auto restored = readGrhSimJson(serialized, defaultDialectRegistry(), diagnostics);
        if (!restored)
            for (const auto &message : diagnostics.messages()) std::cerr << message.context << ": " << message.message << '\n';
        require(bool(restored), "clone JSON reload failed");
        require(emitCpuCpp(*restored, directory, diagnostics).success, "clone emit failed");
        const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_clones.mk";
        command("make --no-print-directory -C " + quote(directory.string()) + " -f " + quote(makefile.string()) +
                " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O2 -g -fsanitize=address,undefined -fno-sanitize-recover=all'");
    }

    void testStateSharing(const std::filesystem::path &directory, bool helpers)
    {
        GrhSimModel model("cpu_state_share"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto input = [&](const std::string &name, TypeId type) {
            const auto id = model.addInput(name, type); const auto value = model.addValue(type);
            const std::array result{value}; const std::array refs{ObjectRef::input(id)};
            model.addOperation("core.input.read", {}, result, refs); return value;
        };
        const auto state = [&](const std::string &name, TypeId type, const char *value = "0", bool random = false) {
            const auto id = model.addState(name, type);
            const std::vector<Parameter> params = random ? std::vector<Parameter>{} :
                std::vector<Parameter>{{model.intern("value"), std::string(value)}};
            const std::array steps{InitStep{model.intern(random ? "core.init.random" : "core.init.const"),
                {0, static_cast<uint32_t>(params.size())}}};
            model.addInit(id, steps, params); return id;
        };
        const auto read = [&](StateId state, TypeId type) {
            const auto value = model.addValue(type); const std::array result{value};
            const std::array refs{ObjectRef::state(state)};
            model.addOperation("core.state.read", {}, result, refs); return value;
        };
        const auto output = [&](const std::string &name, ValueId value) {
            const auto id = model.addOutput(name, model.values()[value.index - 1].type);
            const std::array operands{value}; const std::array refs{ObjectRef::output(id)};
            model.addOperation("core.output.write", operands, {}, refs);
        };
        const auto clock = input("clock", bit), aux = input("aux", bit), enable = input("enable", bit);
        const auto write = [&](StateId target, ValueId en, ValueId data, ValueId mask,
                               std::vector<ValueId> events, std::vector<StateId> histories, const char *edge) {
            std::vector<ValueId> args{en, data, mask}; args.insert(args.end(), events.begin(), events.end());
            std::vector<ObjectRef> refs{ObjectRef::state(target)};
            for (auto history : histories) refs.push_back(ObjectRef::state(history));
            const std::array params{Parameter{model.intern("event_edges"), std::vector<std::string>(events.size(), edge)}};
            model.addOperation("core.state.regWrite", args, {}, refs, params);
        };
        const std::array types{bit, model.logicType(5, true, LogicDomain::TwoState),
            model.logicType(64, false, LogicDomain::TwoState), model.logicType(129, false, LogicDomain::TwoState)};
        for (unsigned lane = 0; lane < types.size(); ++lane)
        {
            const auto type = types[lane];
            const auto data = input("data" + std::to_string(lane), type), mask = input("mask" + std::to_string(lane), type);
            for (unsigned copy = 0; copy < 2; ++copy)
            {
                auto previous = data;
                for (unsigned stage = 0; stage < 2; ++stage)
                {
                    const auto name = "q" + std::to_string(lane) + '_' + std::to_string(stage) + '_' + std::to_string(copy);
                    const auto q = state(name, type), history = state(name + "_hist", bit);
                    if (stage == 0) write(q, enable, previous, mask, {clock, aux}, {history, state(name + "_aux", bit)}, "posedge");
                    else write(q, enable, previous, mask, {clock}, {history}, "negedge");
                    const auto value = read(q, type); output(name, value);
                    const auto next = model.addValue(type); const std::array args{value, data}; const std::array result{next};
                    model.addOperation("core.compute.xor", args, result); previous = next;
                }
            }
        }
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto data = input("negative_data", byte), mask = input("negative_mask", byte);
        for (unsigned test = 0; test < 8; ++test)
        {
            const auto en = input("negative_enable" + std::to_string(test), bit);
            StateId sharedHistory;
            for (unsigned copy = 0; copy < 2; ++copy)
            {
                const auto name = "negative" + std::to_string(test) + '_' + std::to_string(copy);
                const auto q = state(name, byte, test == 0 && copy ? "1" : "0", test == 2);
                const auto value = read(q, byte); output(name, value);
                const auto history = test == 5 && copy ? sharedHistory :
                    state(name + "_hist", bit, test == 1 && copy ? "1" : "0");
                sharedHistory = history;
                if (test == 4 && copy == 0) output("observed_history", read(history, bit));
                write(q, en, test == 6 ? value : data, mask, {test == 7 && copy ? aux : clock}, {history}, "posedge");
                if (test == 3 && copy == 0)
                    write(q, en, mask, data, {clock}, {state(name + "_second", bit)}, "posedge");
            }
        }
        auto reference = model.clone();
        PassManager manager(defaultDialectRegistry()); std::string error; diag::Diagnostics diagnostics;
        manager.addPass(defaultPassRegistry().create("grhsim.canonicalize-compute", {}, error));
        const auto stateCount = model.states().size();
        const auto result = manager.run(model, diagnostics);
        require(result.success && result.changed && model.states().size() == stateCount - 20,
                "equivalent state cascade or private-history exclusions differ");
        const auto stable = manager.run(model, diagnostics);
        require(stable.success && !stable.changed, "state sharing did not reach a fixed point");
        for (unsigned test = 0; test < 8; ++test)
            for (unsigned copy = 0; copy < 2; ++copy)
            {
                const auto name = "negative" + std::to_string(test) + '_' + std::to_string(copy);
                require(std::any_of(model.states().begin(), model.states().end(), [&](const auto &state) {
                    return model.text(state.name) == name;
                }), "state sharing removed an excluded state");
            }
        std::stringstream json;
        require(writeGrhSimJson(model, json, defaultDialectRegistry(), diagnostics) &&
                bool(readGrhSimJson(json, defaultDialectRegistry(), diagnostics)), "shared state model failed round-trip");
        map(model, "128", helpers ? "1" : "10000");
        map(reference, "128", helpers ? "1" : "10000");
        require(emitCpuCpp(model, directory, diagnostics).success &&
                emitCpuCpp(reference, directory / "reference", diagnostics).success, "state sharing emit failed");
        const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_state_share.mk";
        command("make --no-print-directory -C " + quote(directory.string()) + " -f " + quote(makefile.string()) +
                " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all'");
    }

    void testScalarConstants(const std::filesystem::path &directory, bool helpers)
    {
        GrhSimModel model("cpu_constants"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto signedFive = model.logicType(5, true, LogicDomain::TwoState);
        const auto word = model.logicType(64, false, LogicDomain::TwoState);
        const auto input = [&](const char *name, TypeId type) {
            const auto port = model.addInput(name, type);
            const auto value = model.addValue(type);
            const std::array results{value}; const std::array refs{ObjectRef::input(port)};
            model.addOperation("core.input.read", {}, results, refs); return value;
        };
        const auto output = [&](const char *name, ValueId value) {
            const auto port = model.addOutput(name, model.values()[value.index - 1].type);
            const std::array operands{value}; const std::array refs{ObjectRef::output(port)};
            model.addOperation("core.output.write", operands, {}, refs);
        };
        const auto constant = [&](TypeId type, const char *literal) {
            const auto value = model.addValue(type); const std::array results{value};
            const std::array params{Parameter{model.intern("value"), std::string(literal)}};
            model.addOperation("core.compute.constant", {}, results, {}, params); return value;
        };
        const auto state = [&](TypeId type, const char *initial) {
            const auto id = model.addState("s" + std::to_string(model.states().size()), type);
            const std::array params{Parameter{model.intern("value"), std::string(initial)}};
            const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}};
            model.addInit(id, steps, params); return id;
        };
        const auto clock = input("clock", bit), data = input("data", byte);
        const auto unsignedData = input("unsigned_data", word), signedData = input("signed_data", signedFive);
        const auto one = constant(bit, "1'b1"), minusThree = constant(signedFive, "5'h1d");
        const auto allOnes = constant(word, "64'hffffffffffffffff"), unknown = constant(byte, "8'hxz");
        const auto inoutInitial = constant(byte, "8'hx5");
        output("u1", one); output("s1", constant(model.logicType(1, true, LogicDomain::TwoState), "1'b1"));
        output("u5", constant(model.logicType(5, false, LogicDomain::TwoState), "8'hff"));
        output("s5", minusThree); output("u64", allOnes);
        output("s64", constant(model.logicType(64, true, LogicDomain::TwoState), "64'h8000000000000000"));
        output("xz", unknown); output("inout_initial", inoutInitial);
        for (const auto &[name, lhs, rhs, type] : std::array{
                 std::tuple{"unsigned_sum", unsignedData, allOnes, word},
                 std::tuple{"signed_sum", signedData, minusThree, signedFive}})
        {
            const auto value = model.addValue(type); const std::array results{value};
            const std::array operands{lhs, rhs};
            model.addOperation("core.compute.add", operands, results); output(name, value);
        }
        const auto write = [&](const char *name, ValueId event, const char *mask) {
            const auto reg = state(byte, "8'ha0"), history = state(bit, "0");
            const std::array operands{one, data, constant(byte, mask), event};
            const std::array refs{ObjectRef::state(reg), ObjectRef::state(history)};
            const std::array params{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
            model.addOperation("core.state.regWrite", operands, {}, refs, params);
            const auto value = model.addValue(byte); const std::array results{value};
            const std::array readRefs{ObjectRef::state(reg)};
            model.addOperation("core.state.read", {}, results, readRefs); output(name, value);
        };
        write("masked", clock, "8'h0f"); write("constant_event", one, "8'hff");
        const std::array arguments{DpiArgument{model.intern("value"), DpiDirection::Inout, byte}};
        const auto function = model.addExternFunction("cpu_constant_inout", "core.dpi", "cpu_constant_inout", arguments);
        const auto returned = model.addValue(byte); const std::array results{returned};
        const std::array operands{one, inoutInitial, clock};
        const std::array refs{ObjectRef::function(function), ObjectRef::state(state(bit, "0"))};
        const std::array params{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
        model.addOperation("core.dpi.call", operands, results, refs, params); output("inout_result", returned);
        map(model, helpers ? "2" : "128", helpers ? "1" : "10000");
        diag::Diagnostics diagnostics;
        const auto revision = model.semanticRevision();
        require(emitCpuCpp(model, directory, diagnostics).success && model.semanticRevision() == revision,
                "scalar constant emit failed or mutated semantic IR");
        const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_constants.mk";
        command("make --no-print-directory -C " + quote(directory.string()) + " -f " + quote(makefile.string()) +
                " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O2 -g -fsanitize=address,undefined -fno-sanitize-recover=all'");
    }

    void testScalarStaging(const std::filesystem::path &directory)
    {
        GrhSimModel model("cpu_scalar_stage"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto input = [&](const std::string &name, TypeId type) {
            const auto id = model.addInput(name, type); const auto value = model.addValue(type);
            const std::array results{value}; const std::array refs{ObjectRef::input(id)};
            model.addOperation("core.input.read", {}, results, refs); return value;
        };
        const auto state = [&](TypeId type) {
            const auto id = model.addState("s" + std::to_string(model.states().size()), type);
            const std::array params{Parameter{model.intern("value"), std::string("0")}};
            const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}}; model.addInit(id, steps, params); return id;
        };
        const auto output = [&](const std::string &name, StateId id, TypeId type) {
            const auto value = model.addValue(type); const std::array results{value};
            const std::array refs{ObjectRef::state(id)};
            model.addOperation("core.state.read", {}, results, refs);
            const auto port = model.addOutput(name, type); const std::array ports{ObjectRef::output(port)};
            model.addOperation("core.output.write", results, {}, ports);
        };
        const auto clock = input("clock", bit), enable = input("enable", bit);
        const std::array<std::pair<unsigned, bool>, 10> cases{{
            {1, false}, {5, false}, {5, true}, {8, false}, {13, false},
            {13, true}, {32, false}, {32, true}, {64, false}, {64, true}}};
        std::vector<StateId> registers;
        for (std::size_t i = 0; i < cases.size(); ++i)
        {
            const auto [width, isSigned] = cases[i];
            const auto type = model.logicType(width, isSigned, LogicDomain::TwoState);
            const auto reg = state(type); registers.push_back(reg);
            output("q" + std::to_string(i), reg, type);
            for (unsigned writer = 0; writer < 2; ++writer)
            {
                const auto suffix = std::to_string(i) + "_" + std::to_string(writer);
                const auto data = input("data" + suffix, type), mask = input("mask" + suffix, type);
                const auto history = state(bit);
                if (writer == 0) output("history" + std::to_string(i), history, bit);
                const std::array operands{enable, data, mask, clock};
                const std::array refs{ObjectRef::state(reg), ObjectRef::state(history)};
                const std::array params{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
                model.addOperation("core.state.regWrite", operands, {}, refs, params);
            }
        }
        map(model);
        diag::Diagnostics diagnostics;
        require(emitCpuCpp(model, directory, diagnostics).success, "scalar staging emit failed");
        require(emittedDirectStates(directory).empty(), "multiwriter scalar fixture bypassed shadow staging");
        // Expose internals only in this test artifact, before compiling every translation unit.
        const auto headerPath = directory / "grhsim_cpu_scalar_stage.hpp";
        std::ifstream headerInput(headerPath);
        std::string header{std::istreambuf_iterator<char>(headerInput), std::istreambuf_iterator<char>()};
        headerInput.close();
        const auto privatePos = header.find("private:\n");
        require(privatePos != std::string::npos, "scalar staging header has no private section");
        header.replace(privatePos, std::string("private:").size(), "public:");
        std::ofstream(headerPath) << header;
        std::ofstream slots(directory / "scalar_stage_slots.hpp");
        slots << "struct ScalarStageSlot{std::uint32_t state;std::size_t offset;};\n"
              << "inline constexpr ScalarStageSlot scalar_stage_slots[]={\n";
        for (const auto reg : registers)
        {
            bool found = false;
            for (const auto &entry : model.cpuMapping()->dataLayout->objects)
                if (entry.object == ObjectRef::state(reg))
                { slots << '{' << reg.index << ',' << entry.slot.offset << "},\n"; found = true; break; }
            require(found, "scalar fixture state has no object slot");
        }
        slots << "};\n"; slots.close();
        const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_scalar_stage.mk";
        command("make --no-print-directory -C " + quote(directory.string()) + " -f " + quote(makefile.string()) +
                " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O0 -g -fsanitize=address,undefined -fno-sanitize-recover=all'");
    }

    // Two-level event cone (clk & a & b, each op in its own supernode): the
    // falling-edge elision zeroes the event/history bytes, and the next
    // posedge must still propagate through the unchanged intermediate.
    void testFallingEdgeElisionDeepCone(const std::filesystem::path &directory)
    {
        GrhSimModel model("cpu_fp_cone"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto input = [&](const std::string &name) {
            const auto id = model.addInput(name, bit); const auto value = model.addValue(bit);
            const std::array results{value}; const std::array refs{ObjectRef::input(id)};
            model.addOperation("core.input.read", {}, results, refs); return value;
        };
        const auto constant = [&](std::string literal) {
            const auto value = model.addValue(bit); const std::array results{value};
            const std::array params{Parameter{model.intern("value"), std::move(literal)}};
            model.addOperation("core.compute.constant", {}, results, {}, params); return value;
        };
        const auto state = [&] {
            const auto id = model.addState("s" + std::to_string(model.states().size()), bit);
            const std::array params{Parameter{model.intern("value"), std::string("0")}};
            const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}}; model.addInit(id, steps, params); return id;
        };
        const auto clk = input("clk"), a = input("a"), b = input("b"), data = input("data");
        const auto one = constant("1");
        const auto combined = model.addValue(bit); const std::array andOne{clk, a}; const std::array andOneResult{combined};
        model.addOperation("core.compute.and", andOne, andOneResult, {});
        const auto event = model.addValue(bit); const std::array andTwo{combined, b}; const std::array andTwoResult{event};
        model.addOperation("core.compute.and", andTwo, andTwoResult, {});
        const auto reg = state(); const auto history = state();
        const std::array writeOperands{one, data, one, event};
        const std::array writeRefs{ObjectRef::state(reg), ObjectRef::state(history)};
        const std::array writeParams{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
        model.addOperation("core.state.regWrite", writeOperands, {}, writeRefs, writeParams);
        const auto read = model.addValue(bit); const std::array readResults{read}; const std::array readRefs{ObjectRef::state(reg)};
        model.addOperation("core.state.read", {}, readResults, readRefs);
        const auto port = model.addOutput("q", bit); const std::array ports{ObjectRef::output(port)};
        const std::array writeOut{read};
        model.addOperation("core.output.write", writeOut, {}, ports);
        // Toggle register on the same event cone: observes every posedge even
        // when no data input changes in the same eval.
        const auto toggle = state(); const auto toggleHistory = state();
        const auto toggleRead = model.addValue(bit); const std::array toggleReadResults{toggleRead};
        const std::array toggleReadRefs{ObjectRef::state(toggle)};
        model.addOperation("core.state.read", {}, toggleReadResults, toggleReadRefs);
        const auto toggleNext = model.addValue(bit); const std::array notOperands{toggleRead};
        const std::array notResults{toggleNext};
        model.addOperation("core.compute.not", notOperands, notResults, {});
        const std::array toggleOperands{one, toggleNext, one, event};
        const std::array toggleRefs{ObjectRef::state(toggle), ObjectRef::state(toggleHistory)};
        model.addOperation("core.state.regWrite", toggleOperands, {}, toggleRefs, writeParams);
        const auto togglePort = model.addOutput("t", bit); const std::array togglePorts{ObjectRef::output(togglePort)};
        const std::array toggleOut{toggleRead};
        model.addOperation("core.output.write", toggleOut, {}, togglePorts);
        map(model, "1", "1");
        diag::Diagnostics diagnostics;
        require(emitCpuCpp(model, directory, diagnostics).success, "fp cone emit failed");
        for (const auto &message : diagnostics.messages()) std::cout << message.message << '\n';
        const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_fp_cone.mk";
        command("make --no-print-directory -C " + quote(directory.string()) + " -f " + quote(makefile.string()) +
                " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O2 -g -fsanitize=address,undefined -fno-sanitize-recover=all'");
    }

    void testMemoryStaging(const std::filesystem::path &directory)
    {
        GrhSimModel model("cpu_memory_stage"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto input = [&](const std::string &name, TypeId type) {
            const auto id = model.addInput(name, type); const auto value = model.addValue(type);
            const std::array results{value}; const std::array refs{ObjectRef::input(id)};
            model.addOperation("core.input.read", {}, results, refs); return value;
        };
        const auto state = [&](TypeId type) {
            const auto id = model.addState("s" + std::to_string(model.states().size()), type);
            const std::array params{Parameter{model.intern("value"), std::string("0")}};
            const std::array steps{InitStep{model.intern(model.types()[type.index - 1].kind == TypeKind::Array ?
                "core.init.fill" : "core.init.const"), {0, 1}}};
            model.addInit(id, steps, params); return id;
        };
        const auto clock = input("clock", bit), enable = input("enable", bit);
        const auto a = input("address_a", byte), b = input("address_b", byte), read = input("read_address", byte);
        const std::array<std::pair<unsigned, bool>, 10> cases{{
            {1, false}, {5, false}, {5, true}, {8, false}, {13, false},
            {13, true}, {32, false}, {32, true}, {64, false}, {64, true}}};
        std::vector<StateId> memories;
        for (std::size_t i = 0; i < cases.size(); ++i)
        {
            const auto [width, isSigned] = cases[i];
            const auto type = model.logicType(width, isSigned, LogicDomain::TwoState);
            const auto memory = state(model.arrayType(type, 4)); memories.push_back(memory);
            const auto value = model.addValue(type); const std::array results{value};
            const std::array operands{read}; const std::array refs{ObjectRef::state(memory)};
            model.addOperation("core.state.memRead", operands, results, refs);
            const auto output = model.addOutput("q" + std::to_string(i), type);
            const std::array outputRefs{ObjectRef::output(output)};
            model.addOperation("core.output.write", results, {}, outputRefs);
            for (unsigned writer = 0; writer < 2; ++writer)
            {
                const auto suffix = std::to_string(i) + "_" + std::to_string(writer);
                const auto data = input("data" + suffix, type), mask = input("mask" + suffix, type);
                const std::array writeOperands{enable, writer ? b : a, data, mask, clock};
                const std::array writeRefs{ObjectRef::state(memory), ObjectRef::state(state(bit))};
                const std::array params{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
                model.addOperation("core.state.memWrite", writeOperands, {}, writeRefs, params);
            }
        }
        map(model);
        diag::Diagnostics diagnostics;
        require(emitCpuCpp(model, directory, diagnostics).success, "memory staging emit failed");
        const auto headerPath = directory / "grhsim_cpu_memory_stage.hpp";
        std::ifstream inputHeader(headerPath);
        std::string header{std::istreambuf_iterator<char>(inputHeader), std::istreambuf_iterator<char>()};
        inputHeader.close();
        const auto privatePos = header.find("private:\n");
        require(privatePos != std::string::npos, "memory fixture header has no private section");
        header.replace(privatePos, std::string("private:").size(), "public:");
        std::ofstream(headerPath) << header;
        std::ofstream slots(directory / "memory_stage_slots.hpp");
        slots << "struct MemoryStageSlot{std::size_t key,offset;};\ninline constexpr MemoryStageSlot memory_stage_slots[]={\n";
        std::size_t key = model.states().size() + 1;
        for (auto memory : memories)
        {
            bool found = false;
            for (const auto &entry : model.cpuMapping()->dataLayout->objects)
                if (entry.object == ObjectRef::state(memory))
                { slots << '{' << key << ',' << entry.slot.offset << "},\n"; found = true; break; }
            require(found, "memory fixture has no object slot");
            key += 4;
        }
        slots << "};\n"; slots.close();
        const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_memory_stage.mk";
        command("make --no-print-directory -C " + quote(directory.string()) + " -f " + quote(makefile.string()) +
                " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all'");
    }

    using InitDescription = std::pair<std::string, std::vector<std::pair<std::string, ParameterValue>>>;

    void initializedOutput(GrhSimModel &model, const char *name, TypeId type, ValueId address,
                           std::initializer_list<InitDescription> descriptions)
    {
        const auto state = model.addState(name, type);
        std::vector<InitStep> steps;
        std::vector<Parameter> parameters;
        for (const auto &[kind, entries] : descriptions)
        {
            const auto offset = uint32_t(parameters.size());
            for (const auto &[key, value] : entries) parameters.push_back({model.intern(key), value});
            steps.push_back({model.intern(kind), {offset, uint32_t(entries.size())}});
        }
        model.addInit(state, steps, parameters);
        const auto &target = model.types()[type.index - 1];
        const auto element = target.kind == TypeKind::Array ? target.elementType : type;
        const auto value = model.addValue(element); const std::array results{value}, operands{address};
        const std::array refs{ObjectRef::state(state)};
        model.addOperation(target.kind == TypeKind::Array ? "core.state.memRead" : "core.state.read",
                           target.kind == TypeKind::Array ? std::span<const ValueId>(operands) : std::span<const ValueId>{}, results, refs);
        const auto output = model.addOutput(name, element); const std::array outputRefs{ObjectRef::output(output)};
        model.addOperation("core.output.write", results, {}, outputRefs);
    }

    GrhSimModel initFixture()
    {
        GrhSimModel model("cpu_init"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto byte = model.logicType(8, false, LogicDomain::TwoState),
            signedType = model.logicType(5, true, LogicDomain::TwoState),
            wide = model.logicType(129, false, LogicDomain::TwoState), index = model.logicType(3, false, LogicDomain::TwoState);
        const auto memory = model.arrayType(byte, 8), signedMemory = model.arrayType(signedType, 8), wideMemory = model.arrayType(wide, 8);
        const auto input = model.addInput("address", index);
        const auto address = model.addValue(index);
        const std::array results{address}; const std::array refs{ObjectRef::input(input)};
        model.addOperation("core.input.read", {}, results, refs);
        const auto data = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR);
        initializedOutput(model, "filled", memory, address, {
            {"core.init.fill", {{"value", std::string("8'ha5")}}},
            {"core.init.fill", {{"value", std::string("8'h3c")}, {"start", int64_t(2)}, {"count", int64_t(3)}}},
            {"core.init.fill", {{"value", std::string("8'hee")}, {"start", int64_t(6)}}},
            {"core.init.fill", {{"value", std::string("0")}, {"start", int64_t(8)}, {"count", int64_t(0)}}}});
        initializedOutput(model, "hex", memory, address, {
            {"core.init.fill", {{"value", std::string("8'h55")}}},
            {"core.init.readmem", {{"file", (data / "cpu_init_hex.mem").string()}, {"format", std::string("hex")}, {"start", int64_t(2)}, {"count", int64_t(4)}}}});
        initializedOutput(model, "bin", memory, address, {
            {"core.init.const", {{"value", std::vector<std::string>{"0", "1", "2", "3", "4", "5", "6", "7"}}}},
            {"core.init.readmem", {{"file", std::filesystem::relative(data / "cpu_init_bin.mem").string()}, {"format", std::string("bin")}, {"start", int64_t(1)}}}});
        initializedOutput(model, "wide_filled", wideMemory, address, {
            {"core.init.fill", {{"value", std::string("'1")}}},
            {"core.init.fill", {{"value", std::string("129'h10000000000000000123456789abcdef0")}, {"start", int64_t(2)}, {"count", int64_t(2)}}}});
        initializedOutput(model, "wide_const", wideMemory, address, {
            {"core.init.const", {{"value", std::vector<std::string>{"0", "1", "2", "3", "4", "5", "6", "129'h100000000000000000000000000000000"}}}}});
        initializedOutput(model, "wide_readmem", wideMemory, address, {
            {"core.init.fill", {{"value", std::string("0")}}},
            {"core.init.readmem", {{"file", (data / "cpu_init_wide.mem").string()}, {"format", std::string("hex")}}}});
        initializedOutput(model, "fragmented", memory, address, {
            {"core.init.fill", {{"value", std::string("4")}, {"count", int64_t(4)}}},
            {"core.init.fill", {{"value", std::string("8")}, {"start", int64_t(4)}}}});
        initializedOutput(model, "signed_fill", signedMemory, address, {{"core.init.fill", {{"value", std::string("5'h1d")}}}});
        initializedOutput(model, "unknown", memory, address, {{"core.init.fill", {{"value", std::string("x")}}}});
        initializedOutput(model, "random_byte", memory, address, {
            {"core.init.fill", {{"value", std::string("0")}}},
            {"core.init.fill", {{"random", true}, {"start", int64_t(2)}, {"count", int64_t(4)}}}});
        initializedOutput(model, "random_signed", signedMemory, address, {{"core.init.fill", {{"random", true}}}});
        initializedOutput(model, "random_wide", wideMemory, address, {{"core.init.fill", {{"random", true}}}});
        initializedOutput(model, "seeded", wide, address, {{"core.init.random", {{"seed", int64_t(-7)}}}});
        initializedOutput(model, "seeded_signed", signedType, address, {{"core.init.random", {{"seed", int64_t(0)}}}});
        initializedOutput(model, "random_scalar", wide, address, {{"core.init.random", {}}});
        return model;
    }

    void testInit(const std::filesystem::path &directory)
    {
        auto model = initFixture(); map(model);
        diag::Diagnostics diagnostics;
        std::stringstream serialized;
        require(writeGrhSimJson(model, serialized, defaultDialectRegistry(), diagnostics), "initializer JSON write failed");
        auto restored = readGrhSimJson(serialized, defaultDialectRegistry(), diagnostics);
        require(bool(restored), "initializer JSON fresh load failed");
        const auto emitted = emitCpuCpp(*restored, directory, diagnostics);
        for (const auto &message : diagnostics.messages()) std::cout << message.message << '\n';
        require(emitted.success, "array initialization emit failed");
        const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_init.mk";
        command("make --no-print-directory -C " + quote(directory.string()) + " -f " + quote(makefile.string()) +
                " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O0 -g -fsanitize=address,undefined -fno-sanitize-recover=all'");

        const auto rejects = [&](const char *name, std::initializer_list<InitDescription> steps, bool scalar = false) {
            GrhSimModel bad("bad_init"); bad.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto byte = bad.logicType(8, false, LogicDomain::TwoState);
            const auto value = bad.addValue(byte);
            const std::array results{value}; const std::array params{Parameter{bad.intern("value"), std::string("0")}};
            bad.addOperation("core.compute.constant", {}, results, {}, params);
            initializedOutput(bad, "q", scalar ? byte : bad.arrayType(byte, 8), value, steps);
            map(bad); diag::Diagnostics rejected;
            const auto path = directory / name;
            require(!emitCpuCpp(bad, path, rejected).success && !std::filesystem::exists(path), "bad initializer emitted artifacts");
        };
        const InitDescription base{"core.init.fill", {{"value", std::string("0")}}};
        rejects("negative_start", {base, {"core.init.fill", {{"value", std::string("0")}, {"start", int64_t(-1)}}}});
        rejects("negative_count", {base, {"core.init.fill", {{"value", std::string("0")}, {"count", int64_t(-1)}}}});
        rejects("past_end", {base, {"core.init.fill", {{"value", std::string("0")}, {"start", int64_t(9)}}}});
        rejects("overflow_count", {base, {"core.init.fill", {{"value", std::string("0")}, {"start", int64_t(7)}, {"count", INT64_MAX}}}});
        rejects("hole", {{"core.init.fill", {{"value", std::string("0")}, {"start", int64_t(1)}}}});
        rejects("missing_value", {{"core.init.fill", {}}});
        rejects("both", {{"core.init.fill", {{"value", std::string("0")}, {"random", true}}}});
        rejects("false_random", {{"core.init.fill", {{"random", false}}}});
        rejects("array_random", {{"core.init.random", {}}});
        rejects("scalar_fill", {base}, true);
        rejects("short_sequence", {{"core.init.const", {{"value", std::vector<std::string>{"0"}}}}});
        rejects("scalar_sequence", {{"core.init.const", {{"value", std::string("0")}}}});
        rejects("invalid_literal", {{"core.init.fill", {{"value", std::string("not_a_literal")}}}});
        const auto data = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR);
        for (const auto &[name, file, format] : std::vector<std::tuple<std::string, std::string, std::string>>{
                 {"missing_file", "cpu_init_missing.mem", "hex"}, {"bad_comment", "cpu_init_bad.mem", "hex"},
                 {"bad_address", "cpu_init_address_bad.mem", "hex"}, {"bad_format", "cpu_init_hex.mem", "oct"},
                 {"bad_digit", "cpu_init_hex.mem", "bin"}})
            rejects(name.c_str(), {base, {"core.init.readmem", {{"file", (data / file).string()}, {"format", format}}}});
        require(!emit::parseReadmemAddress("") && !emit::parseReadmemAddress("__") && !emit::parseReadmemAddress("10000000000000000") &&
                emit::parseReadmemAddress("f_F") == 255, "shared readmem address parser regression");
        std::cout << "Initialization rejection checks passed\n";
    }

    GrhSimModel callsFixture()
    {
        GrhSimModel model("cpu_calls"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState), byte = model.logicType(8, false, LogicDomain::TwoState),
            word = model.logicType(32, false, LogicDomain::TwoState), wide = model.logicType(129, false, LogicDomain::TwoState),
            string = model.stringType(), real = model.realType();
        const auto input = [&](const char *name, TypeId type) {
            const auto id = model.addInput(name, type); const auto value = model.addValue(type);
            const std::array results{value}; const std::array refs{ObjectRef::input(id)};
            model.addOperation("core.input.read", {}, results, refs); return value;
        };
        const auto output = [&](const char *name, ValueId value, TypeId type) {
            const auto id = model.addOutput(name, type); const std::array operands{value}; const std::array refs{ObjectRef::output(id)};
            model.addOperation("core.output.write", operands, {}, refs);
        };
        const auto constant = [&](TypeId type, std::string text) {
            const auto value = model.addValue(type); const std::array results{value};
            const std::array params{Parameter{model.intern("value"), std::move(text)}};
            model.addOperation("core.compute.constant", {}, results, {}, params); return value;
        };
        const auto state = [&](TypeId type) {
            const auto id = model.addState("s" + std::to_string(model.states().size()), type);
            const std::array params{Parameter{model.intern("value"), std::string("1'b0")}};
            const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}}; model.addInit(id, steps, params); return id;
        };
        const auto clockA = input("clock_a", bit), clockB = input("clock_b", bit), enable = input("enable", word),
            wideEnable = input("wide_enable", wide),
            data = input("data", byte), wideIn = input("wide", wide), text = input("text", string), realIn = input("real_in", real),
            finish = input("finish", bit), fatal = input("fatal", bit), handle = input("handle", word);
        const auto call = [&](const char *symbol, std::vector<DpiArgument> arguments, TypeId returnType,
                              std::vector<ValueId> inputs, std::vector<ValueId> events, std::vector<std::string> edges,
                              ValueId condition = {}) {
            const auto function = model.addExternFunction(symbol, "core.dpi", symbol, arguments, returnType);
            std::vector<ValueId> results;
            if (returnType) results.push_back(model.addValue(returnType));
            for (const auto &arg : arguments) if (arg.direction == DpiDirection::Output) results.push_back(model.addValue(arg.type));
            for (const auto &arg : arguments) if (arg.direction == DpiDirection::Inout) results.push_back(model.addValue(arg.type));
            std::vector<ValueId> operands{condition ? condition : enable}; operands.insert(operands.end(), inputs.begin(), inputs.end());
            operands.insert(operands.end(), events.begin(), events.end());
            std::vector<ObjectRef> refs{ObjectRef::function(function)};
            for (auto event : events) { (void)event; refs.push_back(ObjectRef::state(state(bit))); }
            const std::array params{Parameter{model.intern("event_edges"), std::move(edges)}};
            model.addOperation("core.dpi.call", operands, results, refs, params); return results;
        };
        const auto arg = [&](const char *name, DpiDirection direction, TypeId type) { return DpiArgument{model.intern(name), direction, type}; };
        const auto sharedText = constant(string, std::string(256, 'L') + "\"\\\n\r\t");
        call("cpu_test_literal", {arg("text", DpiDirection::Input, string)}, {}, {sharedText}, {clockA}, {"posedge"});
        output("literal_out", sharedText, string);
        const auto stringIo = call("cpu_test_string_io", {arg("text", DpiDirection::Inout, string)}, {},
                                   {sharedText}, {clockA}, {"posedge"});
        output("literal_inout", stringIo[0], string);
        const auto mixed = call("cpu_test_mixed", {
            arg("out", DpiDirection::Output, byte), arg("io", DpiDirection::Inout, byte),
            arg("data", DpiDirection::Input, byte), arg("wide_out", DpiDirection::Output, wide),
            arg("wide", DpiDirection::Input, wide), arg("text", DpiDirection::Input, string),
            arg("text_out", DpiDirection::Output, string), arg("real", DpiDirection::Input, real),
            arg("real_out", DpiDirection::Output, real), arg("wide_io", DpiDirection::Inout, wide)},
            byte, {data, wideIn, text, realIn, data, wideIn}, {clockA}, {"posedge"});
        output("returned", mixed[0], byte); output("out", mixed[1], byte); output("wide_out", mixed[2], wide);
        output("text_out", mixed[3], string); output("real_out", mixed[4], real);
        output("inout_out", mixed[5], byte); output("wide_inout", mixed[6], wide);
        const auto plain = call("cpu_test_plain", {arg("data", DpiDirection::Input, byte)}, byte, {data}, {}, {});
        output("plain", plain[0], byte);
        const auto signed64 = model.logicType(64, true, LogicDomain::TwoState);
        const auto hostWord = input("host_word", signed64);
        const auto hostCall = call("cpu_test_host_abi", {arg("word", DpiDirection::Input, signed64)}, signed64, {hostWord}, {}, {});
        output("host_result", hostCall[0], signed64);
        const auto signedBit = model.logicType(1, true, LogicDomain::TwoState),
            signedFive = model.logicType(5, true, LogicDomain::TwoState);
        const auto narrow = input("narrow", model.logicType(5, false, LogicDomain::TwoState));
        const auto narrowCall = call("cpu_test_narrow", {arg("io", DpiDirection::Inout, signedFive),
            arg("out", DpiDirection::Output, signedBit)}, signedBit, {narrow}, {}, {});
        output("signed_bit", narrowCall[0], signedBit); output("signed_bit_out", narrowCall[1], signedBit);
        output("narrow_io", narrowCall[2], signedFive);
        call("cpu_test_void", {arg("data", DpiDirection::Input, byte)}, {}, {data}, {}, {}, wideEnable);
        call("cpu_test_edge_void", {arg("data", DpiDirection::Input, byte)}, {}, {data}, {clockB}, {"negedge"});
        call("cpu_test_multi_void", {}, {}, {}, {clockA, clockB}, {"posedge", "negedge"});
        const auto reg = state(byte), history = state(bit);
        const std::array writeOperands{enable, mixed[0], constant(byte, "8'hff"), clockA};
        const std::array writeRefs{ObjectRef::state(reg), ObjectRef::state(history)};
        const std::array writeParams{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
        model.addOperation("core.state.regWrite", writeOperands, {}, writeRefs, writeParams);
        const auto captured = model.addValue(byte); const std::array capturedResults{captured}; const std::array capturedRefs{ObjectRef::state(reg)};
        model.addOperation("core.state.read", {}, capturedResults, capturedRefs); output("captured", captured, byte);
        const auto task = [&](const char *name, ValueId condition, std::vector<ValueId> args,
                              std::vector<ValueId> events, const char *proc = "always") {
            std::vector<ValueId> operands{condition}; operands.insert(operands.end(), args.begin(), args.end());
            operands.insert(operands.end(), events.begin(), events.end()); std::vector<ObjectRef> refs;
            for (auto event : events) { (void)event; refs.push_back(ObjectRef::state(state(bit))); }
            const std::array params{Parameter{model.intern("name"), std::string(name)},
                Parameter{model.intern("proc_kind"), std::string(proc)}, Parameter{model.intern("has_timing"), !events.empty()},
                Parameter{model.intern("event_edges"), std::vector<std::string>(events.size(), "posedge")}};
            model.addOperation("core.system.task", operands, {}, refs, params);
        };
        task("fwrite", wideEnable, {handle, constant(string, "value=%0d\n"), data}, {clockA});
        task("display", enable, {constant(string, "once")}, {clockB}, "initial");
        task("strobe", enable, {constant(string, "strobe=%0d"), data}, {clockA});
        task("finish", finish, {constant(word, "7")}, {});
        task("fatal", fatal, {constant(word, "11"), constant(string, "expected fatal")}, {});
        return model;
    }

    void testCalls(const std::filesystem::path &directory)
    {
        auto model = callsFixture(); map(model);
        diag::Diagnostics diagnostics;
        const auto emitted = emitCpuCpp(model, directory, diagnostics);
        for (const auto &message : diagnostics.messages()) std::cout << message.message << '\n';
        require(emitted.success, "external calls emit failed");
        const auto &layout = *model.cpuMapping()->dataLayout;
        std::string generated;
        for (const auto &file : std::filesystem::directory_iterator(directory))
            if (file.path().extension() == ".cpp")
            {
                std::ifstream stream(file.path());
                generated.append(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
            }
        bool localConstant = false, boundaryConstant = false;
        std::set<uint32_t> constantStrings;
        for (const auto &op : model.operations())
            if (model.text(op.opType) == "core.compute.constant" &&
                model.types()[model.values()[model.results(op)[0].index - 1].type.index - 1].kind == TypeKind::String)
            {
                constantStrings.insert(model.results(op)[0].index);
                const auto &slot = layout.values[model.results(op)[0].index - 1];
                const bool boundary = slot.kind == CpuStorageKind::Boundary;
                localConstant |= !boundary; boundaryConstant |= boundary;
                const auto storage = "cpu_at<std::string>(" + std::string(boundary ? "cpu_boundary.get()" : "cpu_local") +
                                     ',' + std::to_string(slot.offset) + ")=std::string(";
                require(generated.find(storage) == std::string::npos, "string constant still assigned to mutable storage");
                if (boundary)
                    require(generated.find("cpu_at<std::string*>(cpu_boundary.get()," + std::to_string(slot.offset) + ")=") ==
                            std::string::npos, "boundary constant still binds a persistent string object");
            }
        require(localConstant && boundaryConstant, "string constant storage coverage is incomplete");
        std::size_t expectedLocalStrings = 0, actualLocalStrings = 0;
        for (std::size_t i = 0; i < layout.values.size(); ++i)
            if (layout.types[layout.values[i].type.index - 1].kind == CpuTypeKind::String &&
                layout.values[i].kind == CpuStorageKind::PartitionLocal && !constantStrings.contains(i + 1))
                ++expectedLocalStrings;
        for (std::size_t pos = 0; (pos = generated.find("std::string cpu_string_", pos)) != std::string::npos; ++pos)
            ++actualLocalStrings;
        require(actualLocalStrings == expectedLocalStrings, "constant strings retain local lifetimes or dynamic strings lost theirs");
        require(generated.find("::cpu_test_literal((std::string(") != std::string::npos, "DPI literal is not resolved at its call site");
        const auto makefile = std::filesystem::path(WOLVRIX_GRHSIM_TEST_DATA_DIR) / "cpu_calls.mk";
        command("make --no-print-directory -C " + quote(directory.string()) + " -f " + quote(makefile.string()) +
                " -j 2 check CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O0 -g -fsanitize=address,undefined -fno-sanitize-recover=all'");
    }
}

    void testShapeTwinFold()
    {
        const std::string taskA =
            "#include \"grhsim_top.hpp\"\n"
            "void GrhSIM_top::cpu_task_1(){\n"
            "    cpu_at<bool>(cpu_bnd_,100)=static_cast<bool>(grhsim_trunc_u64((cpu_at<bool>(cpu_bnd_,200)&cpu_at<bool>(cpu_bnd_,300)),1));\n"
            "    if(cpu_at<bool>(cpu_bnd_,400)){\n"
            "        cpu_write_cell<std::uint32_t,32>(cpu_obj_,cpu_shadow_,500,600,cpu_at<std::uint16_t>(cpu_bnd_,700),8,1,true,cpu_at<std::uint32_t>(cpu_bnd_,800),static_cast<std::uint32_t>(grhsim_trunc_u64(UINT64_C(4294967295),32)));\n"
            "    }\n"
            "}\n";
        std::string taskB = taskA;
        const std::pair<const char *, const char *> subs[] = {{"100", "101"}, {"200", "201"}, {"300", "301"}, {"400", "401"},
                                                              {"500", "501"}, {"600", "601"}, {"700", "701"}, {"800", "801"}};
        for (const auto &sub : subs)
        {
            const std::string from = sub.first;
            const std::string to = sub.second;
            std::size_t pos = 0;
            while ((pos = taskB.find(from, pos)) != std::string::npos)
            {
                const auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; };
                const char prev = pos > 0 ? taskB[pos - 1] : '\0';
                const char next = pos + from.size() < taskB.size() ? taskB[pos + from.size()] : '\0';
                if (!ident(prev) && !ident(next))
                    taskB.replace(pos, from.size(), to);
                pos += to.size();
            }
        }
        taskB.replace(taskB.find("cpu_task_1"), 10, "cpu_task_2");
        const auto folded = foldShapeTwins({taskA, taskB}, {"cpu_task_1", "cpu_task_2"}, "GrhSIM_top", 100);
        require(folded.groups == 1 && folded.instances == 2, "shape twin group not formed");
        require(folded.taskTexts[0].find("static const std::uint32_t cpu_shape_params32[]={100,200,300,400,500,600,700,800};") != std::string::npos,
                "wrapper params for first instance differ");
        require(folded.taskTexts[1].find("{101,201,301,401,501,601,701,801}") != std::string::npos,
                "wrapper params for second instance differ");
        require(folded.taskTexts[0].find("cpu_shape_0(cpu_shape_params32,nullptr);") != std::string::npos,
                "wrapper call shape differs");
        require(folded.shapesCpp.find("__attribute__((noinline)) void GrhSIM_top::cpu_shape_0(") != std::string::npos,
                "shared definition missing or not noinline");
        require(folded.shapesCpp.find("static_cast<int>(cpu_shape_p32[0])") != std::string::npos,
                "shared body param read missing");
        require(folded.shapesCpp.find("cpu_write_cell<std::uint32_t,32>") != std::string::npos,
                "template argument was parameterized");
        require(folded.shapesCpp.find("UINT64_C(4294967295)") != std::string::npos,
                "uniform literal was parameterized");
        require(folded.shapesCpp.find("),1));") != std::string::npos, "uniform trunc width was parameterized");
        require(folded.decls.size() == 1 && folded.decls[0].find("cpu_shape_0") != std::string::npos,
                "header declaration missing");
        // non-twin tasks stay untouched
        const auto solo = foldShapeTwins({taskA, std::string("void GrhSIM_top::cpu_task_2(){\n}\n")},
                                         {"cpu_task_1", "cpu_task_2"}, "GrhSIM_top", 100);
        require(solo.groups == 0 && solo.taskTexts[0] == taskA, "singleton task was rewritten");
    }

    void testBranchBlockFold(const std::filesystem::path &directory)
    {
        const std::string taskA =
            "void GrhSIM_top::cpu_task_1(){\n"
            "    std::uint8_t cpu_active_word=cpu_flags[10];\n"
            "    if(cpu_active_word){\n"
            "        cpu_flags[10]=0;\n"
            "        if(cpu_active_word&1){\n"
            "            cpu_active_word&=~1;\n"
            "            cpu_at<bool>(cpu_bnd_,100)=static_cast<bool>(grhsim_trunc_u64((cpu_at<bool>(cpu_bnd_,200)&cpu_at<bool>(cpu_bnd_,300)),1));\n"
            "            cpu_at<std::uint16_t>(cpu_bnd_,400)=static_cast<std::uint16_t>(grhsim_trunc_u64((cpu_at<std::uint16_t>(cpu_bnd_,500)+cpu_at<std::uint16_t>(cpu_bnd_,600)),16));\n"
            "            cpu_flags[700] |= (static_cast<std::uint8_t>(-static_cast<std::uint8_t>(cpu_at<bool>(cpu_bnd_,800))) & 1);\n"
            "            if(cpu_active_word&2){\n"
            "                cpu_active_word&=~2;\n"
            "                cpu_flags[900]=1;\n"
            "            }\n"
            "        }\n"
            "    }\n"
            "}\n";
        std::string taskB = taskA;
        const std::pair<const char *, const char *> subs[] = {{"100", "101"}, {"200", "201"}, {"300", "301"}, {"400", "401"},
                                                              {"500", "501"}, {"600", "601"}, {"700", "701"}, {"800", "801"}};
        for (const auto &sub : subs)
        {
            const std::string from = sub.first;
            const std::string to = sub.second;
            std::size_t pos = 0;
            while ((pos = taskB.find(from, pos)) != std::string::npos)
            {
                const auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; };
                const char prev = pos > 0 ? taskB[pos - 1] : '\0';
                const char next = pos + from.size() < taskB.size() ? taskB[pos + from.size()] : '\0';
                if (!ident(prev) && !ident(next))
                    taskB.replace(pos, from.size(), to);
                pos += to.size();
            }
        }
        taskB.replace(taskB.find("cpu_task_1"), 10, "cpu_task_2");

        // default options: fold all groups
        const auto folded = foldBranchBlocks({taskA, taskB}, {"cpu_task_1", "cpu_task_2"}, "GrhSIM_top",
                                             {.minSourceBytes = 100});
        require(folded.groups == 1 && folded.instances == 2 && folded.excludedGroups == 0,
                "branch block group not formed");
        require(folded.taskTexts[0].find("static const std::uint32_t cpu_blk_params32[]={100,200,300,400,500,600,700,800};") != std::string::npos,
                "call-site params for first instance differ");
        require(folded.taskTexts[1].find("{101,201,301,401,501,601,701,801}") != std::string::npos,
                "call-site params for second instance differ");
        require(folded.taskTexts[0].find("cpu_active_word&=~1;") != std::string::npos &&
                    folded.taskTexts[0].find("cpu_blk_0(cpu_blk_params32,nullptr,cpu_active_word);") != std::string::npos,
                "bit clear or by-ref call shape differs");
        require(folded.blocksChunks.size() == 1 &&
                    folded.blocksChunks[0].find("__attribute__((noinline)) void GrhSIM_top::cpu_blk_0(") != std::string::npos,
                "shared block definition missing or not noinline");
        require(folded.blocksChunks[0].find("std::uint8_t &cpu_active_word") != std::string::npos,
                "by-ref active word param missing");
        require(folded.blocksChunks[0].find("if(cpu_active_word&2){") != std::string::npos,
                "nested guard not preserved inside shared body");
        require(folded.blocksChunks[0].find("cpu_flags[static_cast<int>(cpu_blk_p32[6])]") != std::string::npos,
                "varying flag index was not parameterized");
        require(folded.decls.size() == 1 && folded.decls[0].find("cpu_blk_0") != std::string::npos,
                "header declaration missing");

        // hotness-guided exclusion: hot group with zero budget stays inline
        const std::unordered_map<std::string, double> hotness{{"cpu_task_1", 5.0}, {"cpu_task_2", 5.0}};
        const auto cold = foldBranchBlocks({taskA, taskB}, {"cpu_task_1", "cpu_task_2"}, "GrhSIM_top",
                                           {.minSourceBytes = 100, .taskHotness = &hotness, .growthBudget = 0.0});
        require(cold.groups == 0 && cold.excludedGroups == 1 && cold.instances == 0,
                "hot group was not excluded under zero budget");
        require(cold.taskTexts[0] == taskA && cold.taskTexts[1] == taskB,
                "excluded group text was rewritten");
        require(cold.blocksChunks.empty() && cold.decls.empty(), "excluded group emitted helpers");

        // generous budget keeps the fold and reports the estimate
        const auto warm = foldBranchBlocks({taskA, taskB}, {"cpu_task_1", "cpu_task_2"}, "GrhSIM_top",
                                           {.minSourceBytes = 100, .taskHotness = &hotness, .growthBudget = 1.0e9});
        require(warm.groups == 1 && warm.excludedGroups == 0 && warm.estimatedGrowth > 0.0,
                "generous budget changed the fold");

        // hotness TSV loader: tolerant of malformed lines
        std::filesystem::create_directories(directory);
        const std::filesystem::path tsv = directory / "hotness.tsv";
        {
            std::ofstream out(tsv);
            out << "cpu_task_1\t5.0\nmalformed\n\n\tmissing-name\ncpu_task_2\t2.5\n";
        }
        const auto loaded = loadTaskHotnessFile(tsv);
        require(loaded.size() == 2 && loaded.at("cpu_task_1") == 5.0 && loaded.at("cpu_task_2") == 2.5,
                "hotness TSV loader mismatch");
    }

    void testInitZeroElide(const std::filesystem::path &directory)
    {
        GrhSimModel model("zero_elide"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto byte = model.logicType(8, false, LogicDomain::TwoState),
            half = model.logicType(16, false, LogicDomain::TwoState),
            word = model.logicType(32, false, LogicDomain::TwoState),
            wide = model.logicType(129, false, LogicDomain::TwoState),
            index = model.logicType(3, false, LogicDomain::TwoState);
        const auto input = model.addInput("address", index);
        const auto address = model.addValue(index);
        const std::array results{address}; const std::array refs{ObjectRef::input(input)};
        model.addOperation("core.input.read", {}, results, refs);
        initializedOutput(model, "scalar_zero8", byte, address, {{"core.init.const", {{"value", std::string("0")}}}});
        initializedOutput(model, "scalar_one16", half, address, {{"core.init.const", {{"value", std::string("1")}}}});
        initializedOutput(model, "wide_zero", wide, address, {{"core.init.const", {{"value", std::string("0")}}}});
        initializedOutput(model, "wide_ones", wide, address, {{"core.init.const", {{"value", std::string("'1")}}}});
        initializedOutput(model, "arr_zero", model.arrayType(byte, 8), address, {
            {"core.init.fill", {{"value", std::string("0")}}}});
        // A zero fill after a non-zero write to the same state is load-bearing: kept.
        initializedOutput(model, "arr_nz_then_zero", model.arrayType(word, 4), address, {
            {"core.init.fill", {{"value", std::string("32'hdeadbeef")}}},
            {"core.init.fill", {{"value", std::string("0")}}}});
        initializedOutput(model, "arr_zero_then_nz", model.arrayType(half, 4), address, {
            {"core.init.fill", {{"value", std::string("0")}}},
            {"core.init.fill", {{"value", std::string("16'h5")}, {"start", int64_t(1)}, {"count", int64_t(2)}}}});
        initializedOutput(model, "arr_const_zero", model.arrayType(byte, 4), address, {
            {"core.init.const", {{"value", std::vector<std::string>{"0", "0", "0", "0"}}}}});
        initializedOutput(model, "arr_const_mixed", model.arrayType(byte, 4), address, {
            {"core.init.const", {{"value", std::vector<std::string>{"0", "1", "0", "1"}}}}});
        map(model);
        diag::Diagnostics diagnostics;
        require(emitCpuCpp(model, directory, diagnostics).success, "zero-elide emit failed");
        std::string generated;
        for (const auto &entry : std::filesystem::directory_iterator(directory))
            if (entry.path().filename().string().find("_init_") != std::string::npos)
            {
                std::ifstream stream(entry.path());
                generated.append(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
            }
        const auto count = [&](const std::string &needle) {
            std::size_t total = 0, pos = 0;
            while ((pos = generated.find(needle, pos)) != std::string::npos) { ++total; pos += needle.size(); }
            return total;
        };
        require(count("std::memset(cpu_objects.get()") == 1, "load-bearing zero fill was elided");
        require(count("std::memcpy(cpu_objects.get()") == 2, "kept wide/const memcpys differ");
        require(count("cpu_at<std::uint8_t>") == 0, "zero scalar or const array store leaked");
        require(count("grhsim_trunc_u64(UINT64_C(0),8));") == 0, "zero scalar store leaked");
        require(count("static const std::uint8_t data[]=") == 1, "mixed const array data differs");
        require(count("cpu_at<std::uint16_t>") == 2, "kept half-word store or fill loop differs");
        require(count("cpu_at<std::uint32_t>") == 1, "non-zero fill loop missing");
        std::cout << "Init zero-store elision checks passed\n";
    }

int main(int argc, char **argv)
{
    try
    {
        if (argc == 3 && std::string_view(argv[1]) == "--audit")
        {
            diag::Diagnostics diagnostics; auto model = loadGrhSimModel(argv[2], defaultDialectRegistry(), diagnostics);
            require(bool(model), "audit model load failed"); audit(*model); return 0;
        }
        const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto *testOutput = std::getenv("WOLVRIX_CPU_EMIT_TEST_OUTPUT");
        const auto directory = std::filesystem::path(testOutput ? testOutput : WOLVRIX_GRHSIM_TEST_ARTIFACT_DIR) / ("cpu_emit_" + std::to_string(serial));
        auto model = fixture(); map(model);
        const auto revision = model.semanticRevision();
        PassManager manager(defaultDialectRegistry()); std::string error; const auto output = directory.string();
        const std::array<std::string_view, 2> args{"--output", output};
        manager.addPass(defaultPassRegistry().create("cpu.st.emit-cpp", args, error));
        diag::Diagnostics diagnostics; const auto result = manager.run(model, diagnostics);
        for (const auto &message : diagnostics.messages()) std::cout << message.message << '\n';
        require(result.success && !result.changed && model.semanticRevision() == revision && !result.artifacts.empty(), "emit mutated IR or failed");
        require(emittedDirectStates(directory) == std::set<uint32_t>{1, 2, 3, 4, 5}, "private commit accepted multiple writers or missed a private register/latch");
        require(checkSamplingTasks(model, directory) == 3, "input/derived/mixed-edge sampling path coverage differs");
        require(checkActivityGuards(model, directory) != 0, "activity-driven task guard coverage differs");
        checkBufferLocals(directory);
        diag::Diagnostics repeated;
        require(!emitCpuCpp(model, directory, repeated).success, "emit overwrote nonempty directory");
        testShapeTwinFold();
        testBranchBlockFold(directory / "branch_block");
        testStartup(directory / "startup");
        testWideBitwise(directory / "bitwise");
        testWideActivity(directory / "wide_activity");
        testEmitShape(directory / "emit_shape");
        testInit(directory / "init");
        testInitZeroElide(directory / "init_zero_elide");
        testCalls(directory / "calls");
        testSamplingLimit(directory / "sampling_limit");
        testHistoryScan(directory / "history_scan");
        testStableHistorySkip(directory / "stable_history");
        testRandomHistorySharingFallback(directory / "random_history");
        testComputeHistorySharing(directory / "compute_history");
        testGateCompaction(directory / "gate_compaction");
        testHistoryCohorts(directory / "history_cohorts");
        testPrivateCommits(directory / "private_commits");
        testSharedCommitEdges(directory / "notifications", "128", "10000");
        testSharedCommitEdges(directory / "notifications_split", "2", "1");
        testScalarStaging(directory / "scalar_staging");
        testFallingEdgeElisionDeepCone(directory / "fp_cone");
        testIdentityAssigns(directory / "identity_assign", false);
        testIdentityAssigns(directory / "identity_assign_helpers", true);
        testSharedComputeClones(directory / "clones", false);
        testSharedComputeClones(directory / "clones_helpers", true);
        testHelperReadCaches(directory / "read_cache", false);
        testHelperReadCaches(directory / "read_cache_helpers", true);
        testBitwisePredicates(directory / "predicates", false);
        testBitwisePredicates(directory / "predicates_helpers", true);
        testBitwiseMuxes(directory / "bit_select", false);
        testBitwiseMuxes(directory / "bit_select_helpers", true);
        testMuxChainFold(directory / "mux_chain", false);
        testMuxChainFold(directory / "mux_chain_helpers", true);
        testCommitCompactWalk(directory / "commit_batch");
        testCommitMemWalk(directory / "commit_memwalk");
        testDynamicStats(directory / "dynamic_stats");
        testReplicateBroadcast(directory / "replicate_broadcast", false);
        testReplicateBroadcast(directory / "replicate_broadcast_helpers", true);
        testPackedBitRegisters(directory / "packed_bits", false);
        testPackedBitRegisters(directory / "packed_bits_helpers", true);
        testBitPackingDomains();
        testScalarConstants(directory / "constants", false);
        testScalarConstants(directory / "constants_helpers", true);
        testMemoryStaging(directory / "memory_stage");
        testStateSharing(directory / "state_share", false);
        testStateSharing(directory / "state_share_helpers", true);
        auto unsupported = fixture(); unsupported.addInput("four_state", unsupported.logicType(4, false, LogicDomain::FourState)); map(unsupported);
        diag::Diagnostics rejected; const auto rejectedPath = directory / "unsupported";
        require(!emitCpuCpp(unsupported, rejectedPath, rejected).success && !std::filesystem::exists(rejectedPath), "unsupported type produced artifacts");
        for (const char *name : {"cpu_flags", "cpu_task_1", "cpu_init_0", "cpu_at", "init", "cpu_bind_strings", "cpu_direct_again", "cpu_direct_state_changed", "cpu_direct_state_changed_one", "cpu_bitwise_words_changed", "cpu_arithmetic_words_changed", "cpu_shift_words_changed", "cpu_replicate_words_changed", "cpu_active_word", "cpu_write_scalar", "CpuRuntimeProfile", "cpu_runtime_profile", "cpu_profile_enabled", "cpu_profile_tick"})
        {
            auto collision = fixture(); collision.addInput(name, collision.logicType(1, false, LogicDomain::TwoState)); map(collision);
            diag::Diagnostics invalidName; const auto path = directory / (std::string("reserved_") + name);
            require(!emitCpuCpp(collision, path, invalidName).success && !std::filesystem::exists(path), "reserved member collision was accepted");
        }
        if (!std::filesystem::is_regular_file(WOLVRIX_TEST_VERILATOR)) { std::cerr << "Verilator unavailable\n"; return 77; }
        compileAndCompare(directory, "cpu_chain");
        const auto canonicalize = [](GrhSimModel &model) {
            PassManager manager(defaultDialectRegistry()); std::string error; diag::Diagnostics diagnostics;
            auto pass = defaultPassRegistry().create("grhsim.canonicalize-compute", {}, error);
            require(bool(pass), "compute canonicalization factory missing");
            manager.addPass(std::move(pass));
            require(manager.run(model, diagnostics).success, "compute canonicalization before differential test failed");
        };
        auto scalar = scalarFixture(); canonicalize(scalar); map(scalar);
        diag::Diagnostics scalarDiagnostics;
        require(emitCpuCpp(scalar, directory / "scalar", scalarDiagnostics).success, "scalar emit failed");
        {
            std::string generated;
            for (const auto &entry : std::filesystem::directory_iterator(directory / "scalar"))
                if (entry.path().extension() == ".cpp")
                {
                    std::ifstream stream(entry.path());
                    generated.append(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
                }
            require(generated.find("grhsim_mux_u64(") != std::string::npos,
                    "two-state scalar mux did not use the branchless mask helper");
        }
        compileAndCompare(directory / "scalar", "cpu_scalar");
        auto wide = wideFixture(); canonicalize(wide); map(wide);
        diag::Diagnostics wideDiagnostics;
        require(emitCpuCpp(wide, directory / "wide", wideDiagnostics).success, "wide emit failed");
        {
            std::string generated;
            for (const auto &entry : std::filesystem::directory_iterator(directory / "wide"))
                if (entry.path().extension() == ".cpp")
                {
                    std::ifstream stream(entry.path());
                    generated.append(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
                }
            require(generated.find("cpu_replicate_words_changed<") == std::string::npos &&
                    generated.find("cpu_rword=0-static_cast") != std::string::npos,
                    "wide 1-bit replication did not use the broadcast emission");
        }
        {
            // The caller-owned word helper keeps covering multi-bit sources directly.
            GrhSimModel model("cpu_replicate_wide_source"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto wide72 = model.logicType(72, false, LogicDomain::TwoState);
            const auto wide144 = model.logicType(144, false, LogicDomain::TwoState);
            const auto port = model.addInput("b", bit);
            const auto bv = model.addValue(bit);
            model.addOperation("core.input.read", {}, std::array{bv}, std::array{ObjectRef::input(port)});
            const auto in = model.addInput("w", wide72);
            const auto wv = model.addValue(wide72);
            model.addOperation("core.input.read", {}, std::array{wv}, std::array{ObjectRef::input(in)});
            const auto result = model.addValue(wide144);
            const std::array rep{Parameter{model.intern("rep"), int64_t(2)}};
            model.addOperation("core.compute.replicate", std::array{wv}, std::array{result}, {}, rep);
            const auto out = model.addOutput("o", wide144);
            model.addOperation("core.output.write", std::array{result}, {}, std::array{ObjectRef::output(out)});
            map(model);
            diag::Diagnostics helperDiagnostics;
            const auto helperPath = directory / "wide_replicate_helper";
            require(emitCpuCpp(model, helperPath, helperDiagnostics).success, "wide-source replicate emit failed");
            std::string generated;
            for (const auto &entry : std::filesystem::directory_iterator(helperPath))
                if (entry.path().extension() == ".cpp")
                {
                    std::ifstream stream(entry.path());
                    generated.append(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
                }
            require(generated.find("cpu_replicate_words_changed<") != std::string::npos,
                    "wide replication of a multi-bit source lost the caller-owned result helper");
        }
        compileAndCompare(directory / "wide", "cpu_wide");
        auto states = stateFixture(); canonicalize(states); map(states);
        diag::Diagnostics stateDiagnostics;
        require(emitCpuCpp(states, directory / "wide_state", stateDiagnostics).success, "wide state emit failed");
        compileAndCompare(directory / "wide_state", "cpu_wide_state");
        for (auto fixture : {cdcFixture, dualRamFixture})
        {
            auto multiclock = fixture();
            canonicalize(multiclock);
            map(multiclock); diag::Diagnostics multiclockDiagnostics;
            const auto top = std::string(multiclock.text(multiclock.name()));
            std::size_t domains = 0;
            for (const auto &partition : multiclock.cpuMapping()->partitionTree.partitions)
                domains += partition.attrs.eventGate.has_value();
            require(domains == (top == "cpu_cdc" ? 2u : 4u), "multiclock event domains were merged or lost");
            std::stringstream serialized;
            require(writeGrhSimJson(multiclock, serialized, defaultDialectRegistry(), multiclockDiagnostics), "multiclock JSON write failed");
            auto restored = readGrhSimJson(serialized, defaultDialectRegistry(), multiclockDiagnostics);
            require(bool(restored), "multiclock JSON fresh load failed");
            require(emitCpuCpp(*restored, directory / top, multiclockDiagnostics).success, "multiclock emit failed");
            if (top == "cpu_cdc")
            {
                bool sharingCovered = false;
                for (const auto &message : multiclockDiagnostics.messages())
                    sharingCovered |= message.message.find("history_shared_states=0 ") == std::string::npos &&
                        message.message.find("history_shared_states=") != std::string::npos;
                require(sharingCovered, "CDC fixture did not exercise shared event histories");
            }
            compileAndCompare(directory / top, top);
        }
        std::cout << "CPU emitted C++ tests passed: " << directory << '\n';
        return 0;
    }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
