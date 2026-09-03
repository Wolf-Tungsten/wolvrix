#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/demo_stats.hpp"
#include "transform/repcut.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace wolvrix::lib::transform;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[transform-tests] " << message << '\n';
        return 1;
    }

    struct PassRecord
    {
        bool ran = false;
        PassVerbosity verbosity = PassVerbosity::Error;
    };

    class RecordingPass : public Pass
    {
    public:
        RecordingPass(std::string id, PassRecord &record, std::vector<std::string> &order)
            : Pass(std::move(id), "recording"), record_(record), order_(order)
        {
        }

        PassResult run() override
        {
            record_.ran = true;
            record_.verbosity = verbosity();
            order_.push_back(id());
            if (emitDiagError)
            {
                diags().error(id(), "diagnostic failure");
            }
            return PassResult{changedOnRun, failOnRun, {}};
        }

        bool changedOnRun = false;
        bool failOnRun = false;
        bool emitDiagError = false;

    private:
        PassRecord &record_;
        std::vector<std::string> &order_;
    };

    class SessionCheckEmpty : public Pass
    {
    public:
        SessionCheckEmpty(std::string id, bool &reuseFlag)
            : Pass(std::move(id), "session-check"), reuseFlag_(reuseFlag)
        {
        }

        PassResult run() override
        {
            if (hasSessionValue("count"))
            {
                reuseFlag_ = true;
                diags().error(id(), "session value was not cleared between runs");
                return PassResult{false, true, {}};
            }
            return {};
        }

    private:
        bool &reuseFlag_;
    };

    class SessionWriter : public Pass
    {
    public:
        SessionWriter(std::string id, int value)
            : Pass(std::move(id), "session-writer"), value_(value)
        {
        }

        PassResult run() override
        {
            setSessionValue("count", value_);
            return {};
        }

    private:
        int value_;
    };

    class SessionReader : public Pass
    {
    public:
        SessionReader(std::string id, int expected)
            : Pass(std::move(id), "session-reader"), expected_(expected)
        {
        }

        PassResult run() override
        {
            const int *value = getSessionValue<int>("count");
            if (value == nullptr || *value != expected_)
            {
                diags().error(id(), "session value missing or mismatched");
                return PassResult{false, true, {}};
            }
            return {};
        }

    private:
        int expected_;
    };

    class VerbosityEmitter : public Pass
    {
    public:
        VerbosityEmitter() : Pass("verbosity-emitter", "verbosity-emitter") {}

        PassResult run() override
        {
            debug("debug message");
            info("info message");
            warning("warn message");
            return {};
        }
    };

} // namespace

int main()
{
    wolvrix::lib::grh::Design design;
    design.createGraph("top");

    // Case 1: pipeline order and aggregated changed flag
    {
        PassManager manager;
        manager.options().verbosity = PassVerbosity::Debug;
        PassDiagnostics diags;
        std::vector<std::string> order;

        PassRecord firstRecord;
        PassRecord secondRecord;

        auto firstPass = std::make_unique<RecordingPass>("first", firstRecord, order);
        firstPass->changedOnRun = true;
        manager.addPass(std::move(firstPass));

        auto secondPass = std::make_unique<RecordingPass>("second", secondRecord, order);
        manager.addPass(std::move(secondPass));

        PassManagerResult result = manager.run(design, diags);
        if (!result.success)
        {
            return fail("Expected transform pipeline to succeed");
        }
        if (!result.changed)
        {
            return fail("Expected pipeline to report aggregated changes");
        }
        if (order != std::vector<std::string>{"first", "second"})
        {
            return fail("Unexpected pass execution order");
        }
        if (!firstRecord.ran || !secondRecord.ran)
        {
            return fail("Expected both passes to run");
        }
        if (firstRecord.verbosity != PassVerbosity::Debug || secondRecord.verbosity != PassVerbosity::Debug)
        {
            return fail("Expected verbosity level to propagate through context");
        }
        if (!diags.empty())
        {
            return fail("Did not expect diagnostics for successful pipeline");
        }
    }

    // Case 2: failure short-circuits subsequent passes when stopOnError is true
    {
        PassManager manager;
        std::vector<std::string> order;
        PassRecord failingRecord;
        PassRecord tailRecord;

        auto failing = std::make_unique<RecordingPass>("fail", failingRecord, order);
        failing->failOnRun = true;
        manager.addPass(std::move(failing));

        auto tail = std::make_unique<RecordingPass>("tail", tailRecord, order);
        manager.addPass(std::move(tail));

        PassDiagnostics diags;
        PassManagerResult result = manager.run(design, diags);
        if (result.success)
        {
            return fail("Expected transform pipeline to fail when a pass reports failure");
        }
        if (order != std::vector<std::string>{"fail"})
        {
            return fail("stopOnError should prevent downstream passes after failure");
        }
        if (tailRecord.ran)
        {
            return fail("Trailing pass should not have executed after failure");
        }
    }

    // Case 3: diagnostics errors respect stopOnError option
    {
        PassManager manager;
        manager.options().stopOnError = false;
        std::vector<std::string> order;
        PassRecord diagRecord;
        PassRecord tailRecord;

        auto diagPass = std::make_unique<RecordingPass>("diag", diagRecord, order);
        diagPass->emitDiagError = true;
        manager.addPass(std::move(diagPass));

        auto tail = std::make_unique<RecordingPass>("tail", tailRecord, order);
        tail->changedOnRun = true;
        manager.addPass(std::move(tail));

        PassDiagnostics diags;
        PassManagerResult result = manager.run(design, diags);
        if (order != std::vector<std::string>{"diag", "tail"})
        {
            return fail("stopOnError disabled should allow pipeline to continue after diagnostics error");
        }
        if (!diags.hasError())
        {
            return fail("Diagnostics should record errors emitted by passes");
        }
        if (result.success)
        {
            return fail("Pipeline should report failure when diagnostics contain errors");
        }
        if (!result.changed)
        {
            return fail("Changes should still be aggregated even when diagnostics contain errors");
        }
    }

    // Case 4: session allows cross-pass data and resets per run
    {
        PassManager manager;
        bool sessionReused = false;

        manager.addPass(std::make_unique<SessionCheckEmpty>("check", sessionReused));
        manager.addPass(std::make_unique<SessionWriter>("write", 7));
        manager.addPass(std::make_unique<SessionReader>("read", 7));

        PassDiagnostics diags;
        PassManagerResult result = manager.run(design, diags);
        if (!result.success || diags.hasError())
        {
            return fail("Expected session pipeline to succeed on first run");
        }
        diags.clear();
        result = manager.run(design, diags);
        if (!result.success || diags.hasError())
        {
            return fail("Expected session pipeline to succeed on second run");
        }
        if (sessionReused)
        {
            return fail("Session values should be cleared between PassManager runs");
        }
    }

    // Case 5: verbosity filters diagnostics below threshold
    {
        PassManager manager;
        manager.options().verbosity = PassVerbosity::Warning;

        PassDiagnostics diags;
        manager.addPass(std::make_unique<VerbosityEmitter>());
        PassManagerResult result = manager.run(design, diags);
        if (!result.success)
        {
            return fail("Verbosity filtering should not fail the pipeline without errors");
        }
        std::size_t debugCount = 0;
        std::size_t infoCount = 0;
        std::size_t warnCount = 0;
        for (const auto &msg : diags.messages())
        {
            if (msg.kind == PassDiagnosticKind::Debug)
            {
                ++debugCount;
            }
            else if (msg.kind == PassDiagnosticKind::Info)
            {
                ++infoCount;
            }
            else if (msg.kind == PassDiagnosticKind::Warning)
            {
                ++warnCount;
            }
        }
        if (warnCount != 1)
        {
            return fail("Warning diagnostics should survive filtering");
        }
        if (infoCount != 0 || debugCount != 0)
        {
            return fail("Diagnostics below verbosity threshold should be filtered out");
        }
    }

    // Case 6: built-in stats pass reports counts
    {
        wolvrix::lib::grh::Design designStats;
        wolvrix::lib::grh::Graph &graph = designStats.createGraph("g");
        graph.createValue(graph.internSymbol("v0"), 1, false);
        graph.createValue(graph.internSymbol("v1"), 1, false);
        graph.createOperation(wolvrix::lib::grh::OperationKind::kAssign, graph.internSymbol("op0"));

        PassManager manager;
        manager.options().verbosity = PassVerbosity::Info;
        manager.addPass(std::make_unique<StatsPass>());

        PassDiagnostics diags;
        PassManagerResult result = manager.run(designStats, diags);
        if (!result.success)
        {
            return fail("Expected stats pass to succeed");
        }
        if (diags.hasError())
        {
            return fail("Stats pass should not record errors");
        }
        if (diags.messages().empty())
        {
            return fail("Stats pass should emit a diagnostic with counts");
        }
        const auto &message = diags.messages().front();
        if (message.passName != "stats" || message.kind != PassDiagnosticKind::Info)
        {
            return fail("Stats pass should emit an info diagnostic");
        }
        if (message.message.find("\"graph_count\":1") == std::string::npos ||
            message.message.find("\"operation_count\":1") == std::string::npos ||
            message.message.find("\"value_count\":2") == std::string::npos ||
            message.message.find("\"value_bitwidth_total\":2") == std::string::npos ||
            message.message.find("\"value_widths\":") == std::string::npos ||
            message.message.find("\"1\":2") == std::string::npos ||
            message.message.find("\"operation_kinds\":") == std::string::npos ||
            message.message.find("\"kAssign\":1") == std::string::npos ||
            message.message.find("\"register_widths\":") == std::string::npos ||
            message.message.find("\"latch_widths\":") == std::string::npos ||
            message.message.find("\"memory_widths\":") == std::string::npos ||
            message.message.find("\"memory_capacity_bits\":") == std::string::npos ||
            message.message.find("\"writeport_cone_depths\":") == std::string::npos ||
            message.message.find("\"writeport_cone_sizes\":") == std::string::npos ||
            message.message.find("\"writeport_cone_fanins\":") == std::string::npos ||
            message.message.find("\"comb_result_user_counts\":") == std::string::npos ||
            message.message.find("\"comb_op_fanout_sinks\":") == std::string::npos ||
            message.message.find("\"readport_fanout_sinks\":") == std::string::npos)
        {
            return fail("Stats pass diagnostic did not contain expected counts");
        }
    }

    // Case 7: stats pass reports direct user distribution for combinational results
    {
        wolvrix::lib::grh::Design designStats;
        wolvrix::lib::grh::Graph &graph = designStats.createGraph("g");

        const auto inA = graph.createValue(graph.internSymbol("a"), 1, false);
        const auto inB = graph.createValue(graph.internSymbol("b"), 1, false);
        graph.bindInputPort("a", inA);
        graph.bindInputPort("b", inB);

        const auto sum = graph.createValue(graph.internSymbol("sum"), 1, false);
        const auto add = graph.createOperation(wolvrix::lib::grh::OperationKind::kAdd,
                                               graph.internSymbol("add0"));
        graph.addOperand(add, inA);
        graph.addOperand(add, inB);
        graph.addResult(add, sum);

        const auto neg = graph.createValue(graph.internSymbol("neg"), 1, false);
        const auto notOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kNot,
                                                 graph.internSymbol("not0"));
        graph.addOperand(notOp, sum);
        graph.addResult(notOp, neg);
        graph.bindOutputPort("y", neg);

        const auto sysTask =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kSystemTask,
                                  graph.internSymbol("display0"));
        graph.addOperand(sysTask, sum);
        graph.setAttr(sysTask, "name", std::string("$display"));

        PassManager manager;
        manager.options().verbosity = PassVerbosity::Info;
        manager.addPass(std::make_unique<StatsPass>());

        PassDiagnostics diags;
        PassManagerResult result = manager.run(designStats, diags);
        if (!result.success || diags.hasError())
        {
            return fail("Expected stats pass direct-user case to succeed");
        }
        if (diags.messages().empty())
        {
            return fail("Stats pass should emit a diagnostic for direct-user case");
        }

        const auto &message = diags.messages().front().message;
        if (message.find("\"comb_result_user_counts\":{\"0\":1,\"2\":1}") ==
                std::string::npos &&
            message.find("\"comb_result_user_counts\":{\"2\":1,\"0\":1}") ==
                std::string::npos)
        {
            return fail("Stats pass did not report expected combinational result user distribution");
        }
        if (message.find("\"comb_result_user_counts\":{\"max\":2,\"symbols\":[\"g::sum\"]}") ==
                std::string::npos &&
            message.find("\"comb_result_user_counts\":{\"max\":2,\"symbols\":[\"g::sum\",\"g::neg\"]}") ==
                std::string::npos)
        {
            return fail("Stats pass did not report expected max symbol for combinational result users");
        }
    }

    // Case 8: repcut weight mode parsing is discrete and strict
    {
        if (RepcutOptions{}.weightMode != RepcutWeightMode::kBaseline)
        {
            return fail("Expected repcut weight mode to default to baseline");
        }

        const auto expectAccepted = [&](std::vector<std::string_view> args,
                                        std::string_view description) -> bool {
            std::string parseError;
            if (makePass("repcut", args, parseError) == nullptr)
            {
                std::cerr << "[transform-tests] Expected " << description
                          << " to be accepted, error: " << parseError << '\n';
                return false;
            }
            return true;
        };
        if (!expectAccepted({}, "omitted repcut weight mode") ||
            !expectAccepted({"-weight-mode", "baseline"}, "separated baseline weight mode") ||
            !expectAccepted({"-weight-mode=closure-aware"}, "equals closure-aware weight mode"))
        {
            return 1;
        }

        for (const std::string_view invalid : {
                 std::string_view("candidate"),
                 std::string_view("closure_aware"),
                 std::string_view("BASELINE"),
                 std::string_view(""),
             })
        {
            std::string parseError;
            const std::vector<std::string_view> args = {"-weight-mode", invalid};
            if (makePass("repcut", args, parseError) != nullptr ||
                parseError.find("expected baseline or closure-aware") == std::string::npos)
            {
                return fail("Expected invalid repcut weight mode to be rejected: " +
                            std::string(invalid));
            }
        }

        std::string parseError;
        const std::vector<std::string_view> missingValueArgs = {"-weight-mode"};
        if (makePass("repcut", missingValueArgs, parseError) != nullptr ||
            parseError != "-weight-mode expects a value")
        {
            return fail("Expected missing repcut weight mode value to be rejected");
        }
    }

    return 0;
}
