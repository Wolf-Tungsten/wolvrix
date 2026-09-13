#include "grhsim/backend/cpu_emit.hpp"
#include "emit/readmem.hpp"
#include "grhsim/dialect/registry.hpp"
#include "grhsim/io/json.hpp"
#include "grhsim/ir/model.hpp"

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
                    source.find("cpu_stage_cell(") != std::string::npos,
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
        require(count("cpu_write_scalar<bool>(") == 7, "compute history samples were not collapsed to unit representatives");
        require(count("cpu_write_scalar<bool>(" + std::to_string(observed.index) + ",") == 2,
                "history referenced by two calls lost an unconditional sample");
        // Every unit holds a same-key call pair, so each pair's repeated event guard collapses to one local.
        bool hoisted = false;
        for (const auto &message : diagnostics.messages())
            hoisted |= message.message.find("compute_guard_snapshots=6 compute_guard_snapshot_uses=12 ") != std::string::npos;
        require(hoisted, "compute guard hoisting missed same-unit repeated event guards");
        require(count("const bool cpu_cevent_") == 6, "compute guard locals were not emitted once per unit key");
        require(count("if(cpu_cevent_") == 12, "guarded system tasks did not consume the hoisted event guard");
        require(count("(false ||") == 6, "repeated event guard expressions were not collapsed");
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
        diag::Diagnostics repeated;
        require(!emitCpuCpp(model, directory, repeated).success, "emit overwrote nonempty directory");
        testStartup(directory / "startup");
        testWideBitwise(directory / "bitwise");
        testWideActivity(directory / "wide_activity");
        testEmitShape(directory / "emit_shape");
        testInit(directory / "init");
        testCalls(directory / "calls");
        testSamplingLimit(directory / "sampling_limit");
        testHistoryScan(directory / "history_scan");
        testStableHistorySkip(directory / "stable_history");
        testRandomHistorySharingFallback(directory / "random_history");
        testComputeHistorySharing(directory / "compute_history");
        testHistoryCohorts(directory / "history_cohorts");
        testPrivateCommits(directory / "private_commits");
        testSharedCommitEdges(directory / "notifications", "128", "10000");
        testSharedCommitEdges(directory / "notifications_split", "2", "1");
        testScalarStaging(directory / "scalar_staging");
        auto unsupported = fixture(); unsupported.addInput("four_state", unsupported.logicType(4, false, LogicDomain::FourState)); map(unsupported);
        diag::Diagnostics rejected; const auto rejectedPath = directory / "unsupported";
        require(!emitCpuCpp(unsupported, rejectedPath, rejected).success && !std::filesystem::exists(rejectedPath), "unsupported type produced artifacts");
        for (const char *name : {"cpu_flags", "cpu_task_1", "cpu_init_0", "cpu_at", "init", "cpu_bind_strings", "cpu_direct_again", "cpu_direct_state_changed", "cpu_bitwise_words_changed", "cpu_arithmetic_words_changed", "cpu_shift_words_changed", "cpu_active_word", "cpu_write_scalar", "CpuRuntimeProfile", "cpu_runtime_profile", "cpu_profile_enabled", "cpu_profile_tick"})
        {
            auto collision = fixture(); collision.addInput(name, collision.logicType(1, false, LogicDomain::TwoState)); map(collision);
            diag::Diagnostics invalidName; const auto path = directory / (std::string("reserved_") + name);
            require(!emitCpuCpp(collision, path, invalidName).success && !std::filesystem::exists(path), "reserved member collision was accepted");
        }
        if (!std::filesystem::is_regular_file(WOLVRIX_TEST_VERILATOR)) { std::cerr << "Verilator unavailable\n"; return 77; }
        compileAndCompare(directory, "cpu_chain");
        auto scalar = scalarFixture(); map(scalar);
        diag::Diagnostics scalarDiagnostics;
        require(emitCpuCpp(scalar, directory / "scalar", scalarDiagnostics).success, "scalar emit failed");
        compileAndCompare(directory / "scalar", "cpu_scalar");
        auto wide = wideFixture(); map(wide);
        diag::Diagnostics wideDiagnostics;
        require(emitCpuCpp(wide, directory / "wide", wideDiagnostics).success, "wide emit failed");
        compileAndCompare(directory / "wide", "cpu_wide");
        auto states = stateFixture(); map(states);
        diag::Diagnostics stateDiagnostics;
        require(emitCpuCpp(states, directory / "wide_state", stateDiagnostics).success, "wide state emit failed");
        compileAndCompare(directory / "wide_state", "cpu_wide_state");
        for (auto fixture : {cdcFixture, dualRamFixture})
        {
            auto multiclock = fixture();
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
