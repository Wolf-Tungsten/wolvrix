#include "core/transform.hpp"

#include "transform/activity_schedule.hpp"
#include "transform/blackbox_guard.hpp"
#include "transform/comb_lane_pack.hpp"
#include "transform/comb_loop_elim.hpp"
#include "transform/dead_code_elim.hpp"
#include "transform/demo_stats.hpp"
#include "transform/hier_flatten.hpp"
#include "transform/instance_inline.hpp"
#include "transform/latch_transparent_read.hpp"
#include "transform/mem_to_reg.hpp"
#include "transform/memory_init_check.hpp"
#include "transform/memory_read_retime.hpp"
#include "transform/multidriven_guard.hpp"
#include "transform/reg_to_mem.hpp"
#include "transform/repcut.hpp"
#include "transform/simplify.hpp"
#include "transform/slice_index_const.hpp"
#include "transform/strip_debug.hpp"
#include "transform/xmr_resolve.hpp"

#include <chrono>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace wolvrix::lib::transform
{

    namespace
    {
        std::string formatContext(const wolvrix::lib::grh::Graph *graph, const wolvrix::lib::grh::Operation *op, const wolvrix::lib::grh::Value *value)
        {
            std::string ctx;
            if (graph)
            {
                ctx += graph->symbol();
            }
            if (op)
            {
                if (!ctx.empty())
                {
                    ctx += "::";
                }
                ctx.append(op->symbolText());
            }
            if (value)
            {
                if (!ctx.empty())
                {
                    ctx += "::";
                }
                ctx.append(value->symbolText());
            }
            return ctx;
        }

        constexpr int diagnosticLevel(PassDiagnosticKind kind)
        {
            switch (kind)
            {
            case PassDiagnosticKind::Debug:
                return 0;
            case PassDiagnosticKind::Info:
                return 1;
            case PassDiagnosticKind::Warning:
                return 2;
            case PassDiagnosticKind::Error:
            default:
                return 3;
            }
        }
    } // namespace

    void PassDiagnostics::error(std::string passName, std::string message, std::string context)
    {
        add(PassDiagnosticKind::Error, std::move(message), std::move(context), std::move(passName));
    }

    void PassDiagnostics::warning(std::string passName, std::string message, std::string context)
    {
        add(PassDiagnosticKind::Warning, std::move(message), std::move(context), std::move(passName));
    }

    void PassDiagnostics::info(std::string passName, std::string message, std::string context)
    {
        add(PassDiagnosticKind::Info, std::move(message), std::move(context), std::move(passName));
    }

    void PassDiagnostics::debug(std::string passName, std::string message, std::string context)
    {
        add(PassDiagnosticKind::Debug, std::move(message), std::move(context), std::move(passName));
    }

    Pass::Pass(std::string id, std::string name, std::string description)
        : id_(std::move(id)), name_(std::move(name)), description_(std::move(description))
    {
    }

    bool Pass::shouldEmit(PassDiagnosticKind kind) const noexcept
    {
        if (!context_)
        {
            return false;
        }
        return diagnosticLevel(kind) >= static_cast<int>(context_->verbosity);
    }

    bool Pass::shouldLog(LogLevel level) const noexcept
    {
        if (!context_ || !context_->logSink)
        {
            return false;
        }
        if (static_cast<int>(level) < static_cast<int>(context_->logLevel))
        {
            return false;
        }
        return true;
    }

    void Pass::log(LogLevel level, std::string message)
    {
        const std::string_view tag = name_.empty() ? std::string_view(id_) : std::string_view(name_);
        log(level, tag, std::move(message));
    }

    void Pass::log(LogLevel level, std::string_view tag, std::string message)
    {
        if (!shouldLog(level))
        {
            return;
        }
        context_->logSink(level, tag, message);
    }

    void Pass::error(std::string message, std::string context)
    {
        if (!shouldEmit(PassDiagnosticKind::Error))
        {
            return;
        }
        diags().error(name_.empty() ? id_ : name_, std::move(message), std::move(context));
    }

    void Pass::warning(std::string message, std::string context)
    {
        if (!shouldEmit(PassDiagnosticKind::Warning))
        {
            return;
        }
        diags().warning(name_.empty() ? id_ : name_, std::move(message), std::move(context));
    }

    void Pass::info(std::string message, std::string context)
    {
        if (!shouldEmit(PassDiagnosticKind::Info))
        {
            return;
        }
        diags().info(name_.empty() ? id_ : name_, std::move(message), std::move(context));
    }

    void Pass::debug(std::string message, std::string context)
    {
        if (!shouldEmit(PassDiagnosticKind::Debug))
        {
            return;
        }
        diags().debug(name_.empty() ? id_ : name_, std::move(message), std::move(context));
    }

    void Pass::error(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Operation &op, std::string message)
    {
        if (!shouldEmit(PassDiagnosticKind::Error))
        {
            return;
        }
        diags().error(name_.empty() ? id_ : name_, std::move(message), formatContext(&graph, &op, nullptr));
    }

    void Pass::warning(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Operation &op, std::string message)
    {
        if (!shouldEmit(PassDiagnosticKind::Warning))
        {
            return;
        }
        diags().warning(name_.empty() ? id_ : name_, std::move(message), formatContext(&graph, &op, nullptr));
    }

    void Pass::info(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Operation &op, std::string message)
    {
        if (!shouldEmit(PassDiagnosticKind::Info))
        {
            return;
        }
        diags().info(name_.empty() ? id_ : name_, std::move(message), formatContext(&graph, &op, nullptr));
    }

    void Pass::debug(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Operation &op, std::string message)
    {
        if (!shouldEmit(PassDiagnosticKind::Debug))
        {
            return;
        }
        diags().debug(name_.empty() ? id_ : name_, std::move(message), formatContext(&graph, &op, nullptr));
    }

    void Pass::error(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Value &value, std::string message)
    {
        if (!shouldEmit(PassDiagnosticKind::Error))
        {
            return;
        }
        diags().error(name_.empty() ? id_ : name_, std::move(message), formatContext(&graph, nullptr, &value));
    }

    void Pass::warning(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Value &value, std::string message)
    {
        if (!shouldEmit(PassDiagnosticKind::Warning))
        {
            return;
        }
        diags().warning(name_.empty() ? id_ : name_, std::move(message), formatContext(&graph, nullptr, &value));
    }

    void Pass::info(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Value &value, std::string message)
    {
        if (!shouldEmit(PassDiagnosticKind::Info))
        {
            return;
        }
        diags().info(name_.empty() ? id_ : name_, std::move(message), formatContext(&graph, nullptr, &value));
    }

    void Pass::debug(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Value &value, std::string message)
    {
        if (!shouldEmit(PassDiagnosticKind::Debug))
        {
            return;
        }
        diags().debug(name_.empty() ? id_ : name_, std::move(message), formatContext(&graph, nullptr, &value));
    }

    void Pass::error(const wolvrix::lib::grh::Graph &graph, std::string message)
    {
        if (!shouldEmit(PassDiagnosticKind::Error))
        {
            return;
        }
        diags().error(name_.empty() ? id_ : name_, std::move(message), formatContext(&graph, nullptr, nullptr));
    }

    void Pass::warning(const wolvrix::lib::grh::Graph &graph, std::string message)
    {
        if (!shouldEmit(PassDiagnosticKind::Warning))
        {
            return;
        }
        diags().warning(name_.empty() ? id_ : name_, std::move(message), formatContext(&graph, nullptr, nullptr));
    }

    void Pass::info(const wolvrix::lib::grh::Graph &graph, std::string message)
    {
        if (!shouldEmit(PassDiagnosticKind::Info))
        {
            return;
        }
        diags().info(name_.empty() ? id_ : name_, std::move(message), formatContext(&graph, nullptr, nullptr));
    }

    void Pass::debug(const wolvrix::lib::grh::Graph &graph, std::string message)
    {
        if (!shouldEmit(PassDiagnosticKind::Debug))
        {
            return;
        }
        diags().debug(name_.empty() ? id_ : name_, std::move(message), formatContext(&graph, nullptr, nullptr));
    }

    PassManager::PassManager(PassManagerOptions options)
        : options_(options)
    {
    }

    void PassManager::addPass(std::unique_ptr<Pass> pass, std::string instanceName)
    {
        PassEntry entry;
        if (pass)
        {
            if (!instanceName.empty())
            {
                pass->setName(std::move(instanceName));
            }
            else
            {
                pass->setName(pass->id());
            }
        }
        entry.instance = std::move(pass);
        pipeline_.push_back(std::move(entry));
    }

    void PassManager::clear()
    {
        pipeline_.clear();
    }

    PassManagerResult PassManager::run(wolvrix::lib::grh::Design &design, PassDiagnostics &diags)
    {
        PassManagerResult result;
        SessionStore localSession;
        SessionStore *session = options_.session ? options_.session : &localSession;
        PassContext context{design, diags, options_.verbosity, options_.logLevel, options_.logSink,
                            options_.keepDeclaredSymbols, session};
        bool encounteredFailure = false;
        auto emitLog = [&](LogLevel level, std::string_view tag, std::string_view message) {
            if (!options_.logSink)
            {
                return;
            }
            if (static_cast<int>(level) < static_cast<int>(options_.logLevel))
            {
                return;
            }
            options_.logSink(level, tag, message);
        };

        for (auto &entry : pipeline_)
        {
            if (options_.stopOnError && diags.hasError())
            {
                encounteredFailure = true;
                break;
            }

            Pass *pass = entry.instance.get();

            if (pass == nullptr)
            {
                diags.error("unknown", "Pass instance is null", "pipeline");
                encounteredFailure = true;
                if (options_.stopOnError)
                {
                    break;
                }
                continue;
            }

            pass->setContext(&context);
            auto startTime = std::chrono::steady_clock::now();
            PassResult passResult = pass->run();
            auto endTime = std::chrono::steady_clock::now();
            pass->clearContext();
            result.changed = result.changed || passResult.changed;

            if (options_.emitTiming)
            {
                auto durationMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime)
                        .count();
                std::string message;
                message.reserve(pass->id().size() + 32);
                message.append(pass->id());
                message.append(passResult.failed ? " failed in " : " done in ");
                message.append(std::to_string(durationMs));
                message.append("ms");
                if (passResult.changed)
                {
                    message.append(" (changed)");
                }
                emitLog(LogLevel::Info, "timing", message);
            }

            if (passResult.failed)
            {
                encounteredFailure = true;
                if (options_.stopOnError)
                {
                    break;
                }
            }
            else if (options_.stopOnError && diags.hasError())
            {
                encounteredFailure = true;
                break;
            }
        }

        result.success = !encounteredFailure && !diags.hasError();
        return result;
    }

    std::string normalizePassName(std::string_view name)
    {
        std::string normalized(name);
        for (char &ch : normalized)
        {
            if (ch == '_')
            {
                ch = '-';
            }
        }
        return normalized;
    }

    std::vector<std::string> availableTransformPasses()
    {
        return {
            "blackbox-guard",
            "comb-lane-pack",
            "comb-loop-elim",
            "dead-code-elim",
            "activity-schedule",
            "latch-transparent-read",
            "memory-read-retime",
            "slice-index-const",
            "multidriven-guard",
            "hier-flatten",
            "instance-inline",
            "xmr-resolve",
            "memory-init-check",
            "mem-to-reg",
            "reg-to-mem",
            "simplify",
            "stats",
            "strip-debug",
            "repcut",
        };
    }

    std::unique_ptr<Pass> makePass(std::string_view name,
                                   std::span<const std::string_view> args,
                                   std::string &error)
    {
        const std::string normalized = normalizePassName(name);
        if (normalized == "xmr-resolve")
        {
            if (!args.empty())
            {
                error = "xmr-resolve does not accept arguments";
                return nullptr;
            }
            return std::make_unique<XmrResolvePass>();
        }
        if (normalized == "multidriven-guard")
        {
            if (!args.empty())
            {
                error = "multidriven-guard does not accept arguments";
                return nullptr;
            }
            return std::make_unique<MultiDrivenGuardPass>();
        }
        if (normalized == "blackbox-guard")
        {
            if (!args.empty())
            {
                error = "blackbox-guard does not accept arguments";
                return nullptr;
            }
            return std::make_unique<BlackboxGuardPass>();
        }
        if (normalized == "comb-lane-pack")
        {
            CombLanePackOptions options;
            for (std::size_t i = 0; i < args.size(); ++i)
            {
                const std::string_view arg = args[i];
                auto parseSizeArg = [&](std::string_view name, std::size_t &out) -> bool {
                    if (i + 1 >= args.size())
                    {
                        error = std::string(name) + " expects a value";
                        return false;
                    }
                    try
                    {
                        out = static_cast<std::size_t>(std::stoull(std::string(args[++i])));
                    }
                    catch (const std::exception &)
                    {
                        error = std::string("invalid ") + std::string(name) + " value";
                        return false;
                    }
                    return true;
                };
                auto parseBoolArg = [&](std::string_view name, bool &out) -> bool {
                    if (i + 1 >= args.size())
                    {
                        error = std::string(name) + " expects a value";
                        return false;
                    }
                    const std::string_view text = args[++i];
                    if (text == "true" || text == "1" || text == "on")
                    {
                        out = true;
                        return true;
                    }
                    if (text == "false" || text == "0" || text == "off")
                    {
                        out = false;
                        return true;
                    }
                    error = std::string("invalid ") + std::string(name) + " value";
                    return false;
                };

                if (arg == "-min-group-size")
                {
                    if (!parseSizeArg("-min-group-size", options.minGroupSize))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-min-group-size="))
                {
                    try
                    {
                        options.minGroupSize = static_cast<std::size_t>(
                            std::stoull(std::string(arg.substr(std::string_view("-min-group-size=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -min-group-size value";
                        return nullptr;
                    }
                }
                else if (arg == "-max-group-size")
                {
                    if (!parseSizeArg("-max-group-size", options.maxGroupSize))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-max-group-size="))
                {
                    try
                    {
                        options.maxGroupSize = static_cast<std::size_t>(
                            std::stoull(std::string(arg.substr(std::string_view("-max-group-size=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -max-group-size value";
                        return nullptr;
                    }
                }
                else if (arg == "-min-packed-width")
                {
                    if (!parseSizeArg("-min-packed-width", options.minPackedWidth))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-min-packed-width="))
                {
                    try
                    {
                        options.minPackedWidth = static_cast<std::size_t>(
                            std::stoull(std::string(arg.substr(std::string_view("-min-packed-width=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -min-packed-width value";
                        return nullptr;
                    }
                }
                else if (arg == "-max-packed-width")
                {
                    if (!parseSizeArg("-max-packed-width", options.maxPackedWidth))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-max-packed-width="))
                {
                    try
                    {
                        options.maxPackedWidth = static_cast<std::size_t>(
                            std::stoull(std::string(arg.substr(std::string_view("-max-packed-width=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -max-packed-width value";
                        return nullptr;
                    }
                }
                else if (arg == "-max-tree-nodes")
                {
                    if (!parseSizeArg("-max-tree-nodes", options.maxTreeNodes))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-max-tree-nodes="))
                {
                    try
                    {
                        options.maxTreeNodes = static_cast<std::size_t>(
                            std::stoull(std::string(arg.substr(std::string_view("-max-tree-nodes=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -max-tree-nodes value";
                        return nullptr;
                    }
                }
                else if (arg == "-max-root-gap")
                {
                    if (!parseSizeArg("-max-root-gap", options.maxRootGap))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-max-root-gap="))
                {
                    try
                    {
                        options.maxRootGap = static_cast<std::size_t>(
                            std::stoull(std::string(arg.substr(std::string_view("-max-root-gap=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -max-root-gap value";
                        return nullptr;
                    }
                }
                else if (arg == "-require-declared-roots")
                {
                    if (!parseBoolArg("-require-declared-roots", options.requireDeclaredRoots))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-require-declared-roots="))
                {
                    const std::string_view text =
                        arg.substr(std::string_view("-require-declared-roots=").size());
                    if (text == "true" || text == "1" || text == "on")
                    {
                        options.requireDeclaredRoots = true;
                    }
                    else if (text == "false" || text == "0" || text == "off")
                    {
                        options.requireDeclaredRoots = false;
                    }
                    else
                    {
                        error = "invalid -require-declared-roots value";
                        return nullptr;
                    }
                }
                else if (arg == "-enable-declared-roots")
                {
                    if (!parseBoolArg("-enable-declared-roots", options.enableDeclaredRoots))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-enable-declared-roots="))
                {
                    const std::string_view text =
                        arg.substr(std::string_view("-enable-declared-roots=").size());
                    if (text == "true" || text == "1" || text == "on")
                    {
                        options.enableDeclaredRoots = true;
                    }
                    else if (text == "false" || text == "0" || text == "off")
                    {
                        options.enableDeclaredRoots = false;
                    }
                    else
                    {
                        error = "invalid -enable-declared-roots value";
                        return nullptr;
                    }
                }
                else if (arg == "-enable-storage-data-roots")
                {
                    if (!parseBoolArg("-enable-storage-data-roots", options.enableStorageDataRoots))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-enable-storage-data-roots="))
                {
                    const std::string_view text =
                        arg.substr(std::string_view("-enable-storage-data-roots=").size());
                    if (text == "true" || text == "1" || text == "on")
                    {
                        options.enableStorageDataRoots = true;
                    }
                    else if (text == "false" || text == "0" || text == "off")
                    {
                        options.enableStorageDataRoots = false;
                    }
                    else
                    {
                        error = "invalid -enable-storage-data-roots value";
                        return nullptr;
                    }
                }
                else if (arg == "-enable-mux")
                {
                    if (!parseBoolArg("-enable-mux", options.enableMux))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-enable-mux="))
                {
                    const std::string_view text = arg.substr(std::string_view("-enable-mux=").size());
                    if (text == "true" || text == "1" || text == "on")
                    {
                        options.enableMux = true;
                    }
                    else if (text == "false" || text == "0" || text == "off")
                    {
                        options.enableMux = false;
                    }
                    else
                    {
                        error = "invalid -enable-mux value";
                        return nullptr;
                    }
                }
                else if (arg == "-output-key")
                {
                    if (i + 1 >= args.size())
                    {
                        error = "-output-key expects a value";
                        return nullptr;
                    }
                    options.outputKey = std::string(args[++i]);
                }
                else if (arg.starts_with("-output-key="))
                {
                    options.outputKey = std::string(arg.substr(std::string_view("-output-key=").size()));
                }
                else
                {
                    error = "unknown comb-lane-pack option";
                    return nullptr;
                }
            }
            return std::make_unique<CombLanePackPass>(options);
        }
        if (normalized == "slice-index-const")
        {
            if (!args.empty())
            {
                error = "slice-index-const does not accept arguments";
                return nullptr;
            }
            return std::make_unique<SliceIndexConstPass>();
        }
        if (normalized == "hier-flatten")
        {
            HierFlattenOptions options;
            for (std::size_t i = 0; i < args.size(); ++i)
            {
                const std::string_view arg = args[i];
                if (arg == "-preserve-modules")
                {
                    options.preserveFlattenedModules = true;
                }
                else if (arg == "-sym-protect" || arg.starts_with("-sym-protect="))
                {
                    std::string_view value;
                    if (arg == "-sym-protect")
                    {
                        if (i + 1 >= args.size())
                        {
                            error = "-sym-protect expects a value";
                            return nullptr;
                        }
                        value = args[++i];
                    }
                    else
                    {
                        value = arg.substr(std::string_view("-sym-protect=").size());
                    }
                    if (value == "all")
                    {
                        options.symProtect = HierFlattenOptions::SymProtectMode::All;
                    }
                    else if (value == "hierarchy")
                    {
                        options.symProtect = HierFlattenOptions::SymProtectMode::Hierarchy;
                    }
                    else if (value == "stateful")
                    {
                        options.symProtect = HierFlattenOptions::SymProtectMode::Stateful;
                    }
                    else if (value == "none")
                    {
                        options.symProtect = HierFlattenOptions::SymProtectMode::None;
                    }
                    else
                    {
                        error = "unknown -sym-protect mode";
                        return nullptr;
                    }
                }
                else
                {
                    error = "unknown hier-flatten option";
                    return nullptr;
                }
            }
            return std::make_unique<HierFlattenPass>(options);
        }
        if (normalized == "instance-inline")
        {
            InstanceInlineOptions options;
            for (std::size_t i = 0; i < args.size(); ++i)
            {
                const std::string_view arg = args[i];
                if (arg == "-path")
                {
                    if (i + 1 >= args.size())
                    {
                        error = "-path expects a value";
                        return nullptr;
                    }
                    options.path = std::string(args[++i]);
                }
                else if (arg.starts_with("-path="))
                {
                    options.path = std::string(arg.substr(std::string_view("-path=").size()));
                }
                else
                {
                    error = "unknown instance-inline option";
                    return nullptr;
                }
            }
            if (options.path.empty())
            {
                error = "instance-inline requires -path";
                return nullptr;
            }
            return std::make_unique<InstanceInlinePass>(options);
        }
        if (normalized == "activity-schedule")
        {
            ActivityScheduleOptions options;
            for (std::size_t i = 0; i < args.size(); ++i)
            {
                const std::string_view arg = args[i];
                auto parseStringArg = [&](std::string_view name, std::string &out) -> bool {
                    if (i + 1 >= args.size())
                    {
                        error = std::string(name) + " expects a value";
                        return false;
                    }
                    out = std::string(args[++i]);
                    return true;
                };
                auto parseSizeArg = [&](std::string_view name, std::size_t &out) -> bool {
                    if (i + 1 >= args.size())
                    {
                        error = std::string(name) + " expects a value";
                        return false;
                    }
                    try
                    {
                        out = static_cast<std::size_t>(std::stoull(std::string(args[++i])));
                    }
                    catch (const std::exception &)
                    {
                        error = std::string("invalid ") + std::string(name) + " value";
                        return false;
                    }
                    return true;
                };
                auto parseBoolArg = [&](std::string_view name, bool &out) -> bool {
                    if (i + 1 >= args.size())
                    {
                        error = std::string(name) + " expects a value";
                        return false;
                    }
                    const std::string_view text = args[++i];
                    if (text == "true" || text == "1" || text == "on")
                    {
                        out = true;
                        return true;
                    }
                    if (text == "false" || text == "0" || text == "off")
                    {
                        out = false;
                        return true;
                    }
                    error = std::string("invalid ") + std::string(name) + " value";
                    return false;
                };

                if (arg == "-path")
                {
                    if (!parseStringArg("-path", options.path))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-path="))
                {
                    options.path = std::string(arg.substr(std::string_view("-path=").size()));
                }
                else if (arg == "-max-op-in-compute-supernode" ||
                         arg == "-max-compute-node-in-compute-supernode")
                {
                    if (!parseSizeArg(arg, options.maxOpInComputeSupernode))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-max-op-in-compute-supernode="))
                {
                    try
                    {
                        options.maxOpInComputeSupernode = static_cast<std::size_t>(std::stoull(std::string(
                            arg.substr(std::string_view("-max-op-in-compute-supernode=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -max-op-in-compute-supernode value";
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-max-compute-node-in-compute-supernode="))
                {
                    try
                    {
                        options.maxOpInComputeSupernode = static_cast<std::size_t>(std::stoull(std::string(
                            arg.substr(std::string_view("-max-compute-node-in-compute-supernode=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -max-compute-node-in-compute-supernode value";
                        return nullptr;
                    }
                }
                else if (arg == "-max-op-in-compute-node")
                {
                    if (!parseSizeArg("-max-op-in-compute-node", options.maxOpInComputeNode))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-max-op-in-compute-node="))
                {
                    try
                    {
                        options.maxOpInComputeNode = static_cast<std::size_t>(
                            std::stoull(std::string(arg.substr(std::string_view("-max-op-in-compute-node=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -max-op-in-compute-node value";
                        return nullptr;
                    }
                }
                else if (arg == "-max-op-in-commit-supernode")
                {
                    if (!parseSizeArg("-max-op-in-commit-supernode", options.maxOpInCommitSupernode))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-max-op-in-commit-supernode="))
                {
                    try
                    {
                        options.maxOpInCommitSupernode = static_cast<std::size_t>(std::stoull(std::string(
                            arg.substr(std::string_view("-max-op-in-commit-supernode=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -max-op-in-commit-supernode value";
                        return nullptr;
                    }
                }
                else if (arg == "-local-shared-compute-max-fanout")
                {
                    if (!parseSizeArg("-local-shared-compute-max-fanout",
                                      options.localSharedComputeMaxFanout))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-local-shared-compute-max-fanout="))
                {
                    try
                    {
                        options.localSharedComputeMaxFanout = static_cast<std::size_t>(std::stoull(std::string(
                            arg.substr(std::string_view("-local-shared-compute-max-fanout=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -local-shared-compute-max-fanout value";
                        return nullptr;
                    }
                }
                else if (arg == "-local-shared-compute-max-width")
                {
                    if (!parseSizeArg("-local-shared-compute-max-width",
                                      options.localSharedComputeMaxWidth))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-local-shared-compute-max-width="))
                {
                    try
                    {
                        options.localSharedComputeMaxWidth = static_cast<std::size_t>(std::stoull(std::string(
                            arg.substr(std::string_view("-local-shared-compute-max-width=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -local-shared-compute-max-width value";
                        return nullptr;
                    }
                }
                else if (arg == "-local-shared-compute-max-clones")
                {
                    if (!parseSizeArg("-local-shared-compute-max-clones",
                                      options.localSharedComputeMaxClones))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-local-shared-compute-max-clones="))
                {
                    try
                    {
                        options.localSharedComputeMaxClones = static_cast<std::size_t>(std::stoull(std::string(
                            arg.substr(std::string_view("-local-shared-compute-max-clones=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -local-shared-compute-max-clones value";
                        return nullptr;
                    }
                }
                else if (arg == "-local-shared-compute-max-cloned-op-ppm")
                {
                    if (!parseSizeArg("-local-shared-compute-max-cloned-op-ppm",
                                      options.localSharedComputeMaxClonedOpPpm))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-local-shared-compute-max-cloned-op-ppm="))
                {
                    try
                    {
                        options.localSharedComputeMaxClonedOpPpm = static_cast<std::size_t>(std::stoull(std::string(
                            arg.substr(std::string_view("-local-shared-compute-max-cloned-op-ppm=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -local-shared-compute-max-cloned-op-ppm value";
                        return nullptr;
                    }
                }
                else if (arg == "-local-shared-compute-common-owner-max-clones")
                {
                    if (!parseSizeArg("-local-shared-compute-common-owner-max-clones",
                                      options.localSharedComputeCommonOwnerMaxClones))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-local-shared-compute-common-owner-max-clones="))
                {
                    try
                    {
                        options.localSharedComputeCommonOwnerMaxClones =
                            static_cast<std::size_t>(std::stoull(std::string(arg.substr(
                                std::string_view("-local-shared-compute-common-owner-max-clones=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -local-shared-compute-common-owner-max-clones value";
                        return nullptr;
                    }
                }
                else if (arg == "-local-shared-compute-common-owner-max-cloned-op-ppm")
                {
                    if (!parseSizeArg("-local-shared-compute-common-owner-max-cloned-op-ppm",
                                      options.localSharedComputeCommonOwnerMaxClonedOpPpm))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-local-shared-compute-common-owner-max-cloned-op-ppm="))
                {
                    try
                    {
                        options.localSharedComputeCommonOwnerMaxClonedOpPpm =
                            static_cast<std::size_t>(std::stoull(std::string(arg.substr(
                                std::string_view("-local-shared-compute-common-owner-max-cloned-op-ppm=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -local-shared-compute-common-owner-max-cloned-op-ppm value";
                        return nullptr;
                    }
                }
                else if (arg == "-local-shared-compute-common-owner-policy")
                {
                    if (!parseStringArg("-local-shared-compute-common-owner-policy",
                                        options.localSharedComputeCommonOwnerPolicy))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-local-shared-compute-common-owner-policy="))
                {
                    options.localSharedComputeCommonOwnerPolicy = std::string(
                        arg.substr(std::string_view("-local-shared-compute-common-owner-policy=").size()));
                }
                else if (arg == "-enable-coarsen")
                {
                    if (!parseBoolArg("-enable-coarsen", options.enableCoarsen))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-enable-coarsen="))
                {
                    const std::string_view text = arg.substr(std::string_view("-enable-coarsen=").size());
                    if (text == "true" || text == "1" || text == "on")
                    {
                        options.enableCoarsen = true;
                    }
                    else if (text == "false" || text == "0" || text == "off")
                    {
                        options.enableCoarsen = false;
                    }
                    else
                    {
                        error = "invalid -enable-coarsen value";
                        return nullptr;
                    }
                }
                else if (arg == "-enable-chain-merge")
                {
                    if (!parseBoolArg("-enable-chain-merge", options.enableChainMerge))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-enable-chain-merge="))
                {
                    const std::string_view text = arg.substr(std::string_view("-enable-chain-merge=").size());
                    if (text == "true" || text == "1" || text == "on")
                    {
                        options.enableChainMerge = true;
                    }
                    else if (text == "false" || text == "0" || text == "off")
                    {
                        options.enableChainMerge = false;
                    }
                    else
                    {
                        error = "invalid -enable-chain-merge value";
                        return nullptr;
                    }
                }
                else if (arg == "-commit-guard-event-buckets")
                {
                    if (!parseBoolArg("-commit-guard-event-buckets", options.commitGuardEventBuckets))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-commit-guard-event-buckets="))
                {
                    const std::string_view text =
                        arg.substr(std::string_view("-commit-guard-event-buckets=").size());
                    if (text == "true" || text == "1" || text == "on")
                    {
                        options.commitGuardEventBuckets = true;
                    }
                    else if (text == "false" || text == "0" || text == "off")
                    {
                        options.commitGuardEventBuckets = false;
                    }
                    else
                    {
                        error = "invalid -commit-guard-event-buckets value";
                        return nullptr;
                    }
                }
                else if (arg == "-enable-local-shared-compute")
                {
                    if (!parseBoolArg("-enable-local-shared-compute", options.enableLocalSharedCompute))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-enable-local-shared-compute="))
                {
                    const std::string_view text = arg.substr(std::string_view("-enable-local-shared-compute=").size());
                    if (text == "true" || text == "1" || text == "on")
                    {
                        options.enableLocalSharedCompute = true;
                    }
                    else if (text == "false" || text == "0" || text == "off")
                    {
                        options.enableLocalSharedCompute = false;
                    }
                    else
                    {
                        error = "invalid -enable-local-shared-compute value";
                        return nullptr;
                    }
                }
                else if (arg == "-split-oversize-compute-nodes")
                {
                    if (!parseBoolArg("-split-oversize-compute-nodes", options.splitOversizeComputeNodes))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-split-oversize-compute-nodes="))
                {
                    const std::string_view text =
                        arg.substr(std::string_view("-split-oversize-compute-nodes=").size());
                    if (text == "true" || text == "1" || text == "on")
                    {
                        options.splitOversizeComputeNodes = true;
                    }
                    else if (text == "false" || text == "0" || text == "off")
                    {
                        options.splitOversizeComputeNodes = false;
                    }
                    else
                    {
                        error = "invalid -split-oversize-compute-nodes value";
                        return nullptr;
                    }
                }
                else if (arg == "-declared-value-compute-node-boundary")
                {
                    if (!parseBoolArg("-declared-value-compute-node-boundary",
                                      options.declaredValueComputeNodeBoundary))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-declared-value-compute-node-boundary="))
                {
                    const std::string_view text =
                        arg.substr(std::string_view("-declared-value-compute-node-boundary=").size());
                    if (text == "true" || text == "1" || text == "on")
                    {
                        options.declaredValueComputeNodeBoundary = true;
                    }
                    else if (text == "false" || text == "0" || text == "off")
                    {
                        options.declaredValueComputeNodeBoundary = false;
                    }
                    else
                    {
                        error = "invalid -declared-value-compute-node-boundary value";
                        return nullptr;
                    }
                }
                else if (arg == "-final-topo-policy")
                {
                    if (!parseStringArg("-final-topo-policy", options.finalTopoPolicy))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-final-topo-policy="))
                {
                    options.finalTopoPolicy =
                        std::string(arg.substr(std::string_view("-final-topo-policy=").size()));
                }
                else if (arg == "-kahn-level-pack-policy")
                {
                    if (!parseStringArg("-kahn-level-pack-policy", options.kahnLevelPackPolicy))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-kahn-level-pack-policy="))
                {
                    options.kahnLevelPackPolicy =
                        std::string(arg.substr(std::string_view("-kahn-level-pack-policy=").size()));
                }
                else if (arg == "-post-dp-refine-policy")
                {
                    if (!parseStringArg("-post-dp-refine-policy", options.postDpRefinePolicy))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-post-dp-refine-policy="))
                {
                    options.postDpRefinePolicy =
                        std::string(arg.substr(std::string_view("-post-dp-refine-policy=").size()));
                }
                else if (arg == "-post-dp-refine-max-rounds")
                {
                    if (!parseSizeArg("-post-dp-refine-max-rounds", options.postDpRefineMaxRounds))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-post-dp-refine-max-rounds="))
                {
                    try
                    {
                        options.postDpRefineMaxRounds = static_cast<std::size_t>(std::stoull(std::string(
                            arg.substr(std::string_view("-post-dp-refine-max-rounds=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -post-dp-refine-max-rounds value";
                        return nullptr;
                    }
                }
                else if (arg == "-post-dp-refine-max-moves")
                {
                    if (!parseSizeArg("-post-dp-refine-max-moves", options.postDpRefineMaxMoves))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-post-dp-refine-max-moves="))
                {
                    try
                    {
                        options.postDpRefineMaxMoves = static_cast<std::size_t>(std::stoull(std::string(
                            arg.substr(std::string_view("-post-dp-refine-max-moves=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -post-dp-refine-max-moves value";
                        return nullptr;
                    }
                }
                else if (arg == "-post-dp-refine-max-moved-op-ppm")
                {
                    if (!parseSizeArg("-post-dp-refine-max-moved-op-ppm",
                                      options.postDpRefineMaxMovedOpPpm))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-post-dp-refine-max-moved-op-ppm="))
                {
                    try
                    {
                        options.postDpRefineMaxMovedOpPpm = static_cast<std::size_t>(std::stoull(std::string(
                            arg.substr(std::string_view("-post-dp-refine-max-moved-op-ppm=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -post-dp-refine-max-moved-op-ppm value";
                        return nullptr;
                    }
                }
                else if (arg == "-post-dp-refine-max-regression-ppm")
                {
                    if (!parseSizeArg("-post-dp-refine-max-regression-ppm",
                                      options.postDpRefineMaxRegressionPpm))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-post-dp-refine-max-regression-ppm="))
                {
                    try
                    {
                        options.postDpRefineMaxRegressionPpm = static_cast<std::size_t>(std::stoull(std::string(
                            arg.substr(std::string_view("-post-dp-refine-max-regression-ppm=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -post-dp-refine-max-regression-ppm value";
                        return nullptr;
                    }
                }
                else if (arg == "-kahn-level-pack-max-moves")
                {
                    if (!parseSizeArg("-kahn-level-pack-max-moves", options.kahnLevelPackMaxMoves))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-kahn-level-pack-max-moves="))
                {
                    try
                    {
                        options.kahnLevelPackMaxMoves = static_cast<std::size_t>(std::stoull(std::string(
                            arg.substr(std::string_view("-kahn-level-pack-max-moves=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -kahn-level-pack-max-moves value";
                        return nullptr;
                    }
                }
                else if (arg == "-kahn-level-pack-max-moved-op-ppm")
                {
                    if (!parseSizeArg("-kahn-level-pack-max-moved-op-ppm",
                                      options.kahnLevelPackMaxMovedOpPpm))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-kahn-level-pack-max-moved-op-ppm="))
                {
                    try
                    {
                        options.kahnLevelPackMaxMovedOpPpm = static_cast<std::size_t>(std::stoull(std::string(
                            arg.substr(std::string_view("-kahn-level-pack-max-moved-op-ppm=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -kahn-level-pack-max-moved-op-ppm value";
                        return nullptr;
                    }
                }
                else if (arg == "-kahn-level-pack-max-regression-ppm")
                {
                    if (!parseSizeArg("-kahn-level-pack-max-regression-ppm",
                                      options.kahnLevelPackMaxRegressionPpm))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-kahn-level-pack-max-regression-ppm="))
                {
                    try
                    {
                        options.kahnLevelPackMaxRegressionPpm = static_cast<std::size_t>(std::stoull(std::string(
                            arg.substr(std::string_view("-kahn-level-pack-max-regression-ppm=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -kahn-level-pack-max-regression-ppm value";
                        return nullptr;
                    }
                }
                else if (arg == "-split-oversize-compute-node-max-ops")
                {
                    if (!parseSizeArg("-split-oversize-compute-node-max-ops",
                                      options.splitOversizeComputeNodeMaxOps))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-split-oversize-compute-node-max-ops="))
                {
                    try
                    {
                        options.splitOversizeComputeNodeMaxOps = static_cast<std::size_t>(std::stoull(std::string(
                            arg.substr(std::string_view("-split-oversize-compute-node-max-ops=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -split-oversize-compute-node-max-ops value";
                        return nullptr;
                    }
                }
                else if (arg == "-export-compute-dag")
                {
                    if (!parseStringArg("-export-compute-dag", options.exportComputeDagPath))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-export-compute-dag="))
                {
                    options.exportComputeDagPath =
                        std::string(arg.substr(std::string_view("-export-compute-dag=").size()));
                }
                else
                {
                    error = "unknown activity-schedule option";
                    return nullptr;
                }
            }
            return std::make_unique<ActivitySchedulePass>(options);
        }
        if (normalized == "simplify")
        {
            SimplifyOptions options;
            for (std::size_t i = 0; i < args.size(); ++i)
            {
                const std::string_view arg = args[i];
                if (arg == "-max-iter")
                {
                    if (i + 1 >= args.size())
                    {
                        error = "-max-iter expects a value";
                        return nullptr;
                    }
                    try
                    {
                        options.maxIterations = std::stoi(std::string(args[++i]));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -max-iter value";
                        return nullptr;
                    }
                }
                else if (arg == "-x-fold" || arg.starts_with("-x-fold="))
                {
                    std::string_view value;
                    if (arg == "-x-fold")
                    {
                        if (i + 1 >= args.size())
                        {
                            error = "-x-fold expects a value";
                            return nullptr;
                        }
                        value = args[++i];
                    }
                    else
                    {
                        value = arg.substr(std::string_view("-x-fold=").size());
                    }
                    if (value == "strict")
                    {
                        options.xFold = ConstantFoldOptions::XFoldMode::Strict;
                    }
                    else if (value == "known")
                    {
                        options.xFold = ConstantFoldOptions::XFoldMode::Known;
                    }
                    else if (value == "propagate")
                    {
                        options.xFold = ConstantFoldOptions::XFoldMode::Propagate;
                    }
                    else
                    {
                        error = "unknown -x-fold mode";
                        return nullptr;
                    }
                }
                else if (arg == "-semantics" || arg.starts_with("-semantics="))
                {
                    std::string_view value;
                    if (arg == "-semantics")
                    {
                        if (i + 1 >= args.size())
                        {
                            error = "-semantics expects a value";
                            return nullptr;
                        }
                        value = args[++i];
                    }
                    else
                    {
                        value = arg.substr(std::string_view("-semantics=").size());
                    }
                    if (value == "2state")
                    {
                        options.semantics = ConstantFoldOptions::Semantics::TwoState;
                    }
                    else if (value == "4state")
                    {
                        options.semantics = ConstantFoldOptions::Semantics::FourState;
                    }
                    else
                    {
                        error = "unknown -semantics mode";
                        return nullptr;
                    }
                }
                else
                {
                    error = "unknown simplify option";
                    return nullptr;
                }
            }
            return std::make_unique<SimplifyPass>(options);
        }
        if (normalized == "dead-code-elim")
        {
            DeadCodeElimOptions options;
            for (std::size_t i = 0; i < args.size(); ++i)
            {
                const std::string_view arg = args[i];
                if (arg == "-output-key")
                {
                    if (i + 1 >= args.size())
                    {
                        error = "-output-key expects a value";
                        return nullptr;
                    }
                    options.outputKey = std::string(args[++i]);
                }
                else if (arg.starts_with("-output-key="))
                {
                    options.outputKey = std::string(arg.substr(std::string_view("-output-key=").size()));
                }
                else if (arg == "-sample-limit")
                {
                    if (i + 1 >= args.size())
                    {
                        error = "-sample-limit expects a value";
                        return nullptr;
                    }
                    try
                    {
                        options.sampleLimit = static_cast<std::size_t>(std::stoull(std::string(args[++i])));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -sample-limit value";
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-sample-limit="))
                {
                    try
                    {
                        options.sampleLimit = static_cast<std::size_t>(
                            std::stoull(std::string(arg.substr(std::string_view("-sample-limit=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -sample-limit value";
                        return nullptr;
                    }
                }
                else
                {
                    error = "unknown dead-code-elim option";
                    return nullptr;
                }
            }
            return std::make_unique<DeadCodeElimPass>(options);
        }
        if (normalized == "comb-loop-elim")
        {
            CombLoopElimOptions options;
            for (std::size_t i = 0; i < args.size(); ++i)
            {
                const std::string_view arg = args[i];
                if (arg == "-max-analysis-nodes" || arg == "-max-nodes" || arg.starts_with("-max-analysis-nodes=") ||
                    arg.starts_with("-max-nodes="))
                {
                    std::string_view value;
                    if (arg == "-max-analysis-nodes" || arg == "-max-nodes")
                    {
                        if (i + 1 >= args.size())
                        {
                            error = "-max-analysis-nodes expects a value";
                            return nullptr;
                        }
                        value = args[++i];
                    }
                    else if (arg.starts_with("-max-analysis-nodes="))
                    {
                        value = arg.substr(std::string_view("-max-analysis-nodes=").size());
                    }
                    else
                    {
                        value = arg.substr(std::string_view("-max-nodes=").size());
                    }
                    try
                    {
                        options.maxAnalysisNodes = static_cast<std::size_t>(std::stoull(std::string(value)));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -max-analysis-nodes value";
                        return nullptr;
                    }
                }
                else if (arg == "-num-threads" || arg == "-threads" || arg.starts_with("-num-threads=") ||
                         arg.starts_with("-threads="))
                {
                    std::string_view value;
                    if (arg == "-num-threads" || arg == "-threads")
                    {
                        if (i + 1 >= args.size())
                        {
                            error = "-num-threads expects a value";
                            return nullptr;
                        }
                        value = args[++i];
                    }
                    else if (arg.starts_with("-num-threads="))
                    {
                        value = arg.substr(std::string_view("-num-threads=").size());
                    }
                    else
                    {
                        value = arg.substr(std::string_view("-threads=").size());
                    }
                    try
                    {
                        options.numThreads = static_cast<std::size_t>(std::stoull(std::string(value)));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -num-threads value";
                        return nullptr;
                    }
                }
                else if (arg == "-fix-false-loops" || arg == "-no-fix-false-loops" ||
                         arg.starts_with("-fix-false-loops="))
                {
                    if (arg == "-fix-false-loops")
                    {
                        options.fixFalseLoops = true;
                    }
                    else if (arg == "-no-fix-false-loops")
                    {
                        options.fixFalseLoops = false;
                    }
                    else
                    {
                        std::string_view value = arg.substr(std::string_view("-fix-false-loops=").size());
                        if (value == "1" || value == "true" || value == "yes")
                        {
                            options.fixFalseLoops = true;
                        }
                        else if (value == "0" || value == "false" || value == "no")
                        {
                            options.fixFalseLoops = false;
                        }
                        else
                        {
                            error = "invalid -fix-false-loops value";
                            return nullptr;
                        }
                    }
                }
                else if (arg == "-max-fix-iter" || arg == "-max-fix-iterations" || arg.starts_with("-max-fix-iter=") ||
                         arg.starts_with("-max-fix-iterations="))
                {
                    std::string_view value;
                    if (arg == "-max-fix-iter" || arg == "-max-fix-iterations")
                    {
                        if (i + 1 >= args.size())
                        {
                            error = "-max-fix-iter expects a value";
                            return nullptr;
                        }
                        value = args[++i];
                    }
                    else if (arg.starts_with("-max-fix-iter="))
                    {
                        value = arg.substr(std::string_view("-max-fix-iter=").size());
                    }
                    else
                    {
                        value = arg.substr(std::string_view("-max-fix-iterations=").size());
                    }
                    try
                    {
                        options.maxFixIterations = static_cast<std::size_t>(std::stoull(std::string(value)));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -max-fix-iter value";
                        return nullptr;
                    }
                }
                else if (arg == "-fail-on-true-loop")
                {
                    options.failOnTrueLoop = true;
                }
                else if (arg == "-output-key")
                {
                    if (i + 1 >= args.size())
                    {
                        error = "-output-key expects a value";
                        return nullptr;
                    }
                    options.outputKey = std::string(args[++i]);
                }
                else if (arg.starts_with("-output-key="))
                {
                    options.outputKey = std::string(arg.substr(std::string_view("-output-key=").size()));
                }
                else
                {
                    error = "unknown comb-loop-elim option";
                    return nullptr;
                }
            }
            return std::make_unique<CombLoopElimPass>(options);
        }
        if (normalized == "memory-init-check")
        {
            if (!args.empty())
            {
                error = "memory-init-check does not accept arguments";
                return nullptr;
            }
            return std::make_unique<MemoryInitCheckPass>();
        }
        if (normalized == "mem-to-reg")
        {
            MemToRegOptions options;
            for (std::size_t i = 0; i < args.size(); ++i)
            {
                const std::string_view arg = args[i];
                if (arg == "-row-limit")
                {
                    if (i + 1 >= args.size())
                    {
                        error = "-row-limit expects a value";
                        return nullptr;
                    }
                    try
                    {
                        options.rowLimit = std::stoll(std::string(args[++i]));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -row-limit value";
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-row-limit="))
                {
                    try
                    {
                        options.rowLimit = std::stoll(std::string(arg.substr(std::string_view("-row-limit=").size())));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -row-limit value";
                        return nullptr;
                    }
                }
                else if (arg == "-strict-init")
                {
                    options.strictInit = true;
                }
                else if (arg == "-no-strict-init")
                {
                    options.strictInit = false;
                }
                else
                {
                    error = "unknown mem-to-reg option";
                    return nullptr;
                }
            }
            if (options.rowLimit <= 0)
            {
                error = "-row-limit must be > 0";
                return nullptr;
            }
            return std::make_unique<MemToRegPass>(options);
        }
        if (normalized == "memory-read-retime")
        {
            if (!args.empty())
            {
                error = "memory-read-retime does not accept arguments";
                return nullptr;
            }
            return std::make_unique<MemoryReadRetimePass>();
        }
        if (normalized == "reg-to-mem")
        {
            RegToMemOptions options;
            for (std::size_t i = 0; i < args.size(); ++i)
            {
                const std::string_view arg = args[i];
                if (arg == "-no-intent")
                {
                    options.enableIntent = false;
                }
                else if (arg == "-intent")
                {
                    options.enableIntent = true;
                }
                else if (arg == "-true-merge")
                {
                    options.enableTrueMerge = true;
                }
                else if (arg == "-no-true-merge")
                {
                    options.enableTrueMerge = false;
                }
                else if (arg == "-ordered-writes")
                {
                    options.enableOrderedWrites = true;
                }
                else if (arg == "-no-ordered-writes")
                {
                    options.enableOrderedWrites = false;
                }
                else if (arg == "-decoded-write-storage")
                {
                    options.enableDecodedWriteStorage = true;
                }
                else if (arg == "-no-decoded-write-storage")
                {
                    options.enableDecodedWriteStorage = false;
                }
                else if (arg == "-min-element-count")
                {
                    if (i + 1 >= args.size())
                    {
                        error = "-min-element-count expects a value";
                        return nullptr;
                    }
                    try
                    {
                        options.minElementCount = static_cast<std::size_t>(std::stoull(std::string(args[++i])));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -min-element-count value";
                        return nullptr;
                    }
                }
                else
                {
                    error = "unknown reg-to-mem option";
                    return nullptr;
                }
            }
            if (options.minElementCount < 2)
            {
                error = "-min-element-count must be >= 2";
                return nullptr;
            }
            return std::make_unique<RegToMemPass>(options);
        }
        if (normalized == "latch-transparent-read")
        {
            if (!args.empty())
            {
                error = "latch-transparent-read does not accept arguments";
                return nullptr;
            }
            return std::make_unique<LatchTransparentReadPass>();
        }
        if (normalized == "strip-debug")
        {
            StripDebugOptions options;
            for (std::size_t i = 0; i < args.size(); ++i)
            {
                const std::string_view arg = args[i];
                if (arg == "-path")
                {
                    if (i + 1 >= args.size())
                    {
                        error = "-path expects a value";
                        return nullptr;
                    }
                    options.path = std::string(args[++i]);
                }
                else if (arg.starts_with("-path="))
                {
                    options.path = std::string(arg.substr(std::string_view("-path=").size()));
                }
                else
                {
                    error = "unknown strip-debug option";
                    return nullptr;
                }
            }
            return std::make_unique<StripDebugPass>(options);
        }
        if (normalized == "stats")
        {
            StatsOptions options;
            for (std::size_t i = 0; i < args.size(); ++i)
            {
                const std::string_view arg = args[i];
                if (arg == "-output-key")
                {
                    if (i + 1 >= args.size())
                    {
                        error = "-output-key expects a value";
                        return nullptr;
                    }
                    options.outputKey = std::string(args[++i]);
                }
                else if (arg.starts_with("-output-key="))
                {
                    options.outputKey = std::string(arg.substr(std::string_view("-output-key=").size()));
                }
                else
                {
                    error = "unknown stats option";
                    return nullptr;
                }
            }
            return std::make_unique<StatsPass>(options);
        }
        if (normalized == "hrbcut")
        {
            error = "hrbcut pass has been removed";
            return nullptr;
        }
        if (normalized == "repcut")
        {
            RepcutOptions options;
            for (std::size_t i = 0; i < args.size(); ++i)
            {
                const std::string_view arg = args[i];
                auto parseStringArg = [&](std::string_view name, std::string &out) -> bool {
                    if (i + 1 >= args.size())
                    {
                        error = std::string(name) + " expects a value";
                        return false;
                    }
                    out = std::string(args[++i]);
                    return true;
                };
                auto parseSizeArg = [&](std::string_view name, std::size_t &out) -> bool {
                    if (i + 1 >= args.size())
                    {
                        error = std::string(name) + " expects a value";
                        return false;
                    }
                    try
                    {
                        out = static_cast<std::size_t>(std::stoull(std::string(args[++i])));
                    }
                    catch (const std::exception &)
                    {
                        error = std::string("invalid ") + std::string(name) + " value";
                        return false;
                    }
                    return true;
                };
                auto parseDoubleArg = [&](std::string_view name, double &out) -> bool {
                    if (i + 1 >= args.size())
                    {
                        error = std::string(name) + " expects a value";
                        return false;
                    }
                    try
                    {
                        out = std::stod(std::string(args[++i]));
                    }
                    catch (const std::exception &)
                    {
                        error = std::string("invalid ") + std::string(name) + " value";
                        return false;
                    }
                    return true;
                };

                if (arg == "-path")
                {
                    if (!parseStringArg("-path", options.path))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-path="))
                {
                    options.path = std::string(arg.substr(std::string_view("-path=").size()));
                }
                else if (arg == "-partition-count")
                {
                    if (!parseSizeArg("-partition-count", options.partitionCount))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-partition-count="))
                {
                    try
                    {
                        options.partitionCount = static_cast<std::size_t>(
                            std::stoull(std::string(arg.substr(std::string_view("-partition-count=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -partition-count value";
                        return nullptr;
                    }
                }
                else if (arg == "-imbalance-factor")
                {
                    if (!parseDoubleArg("-imbalance-factor", options.imbalanceFactor))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-imbalance-factor="))
                {
                    try
                    {
                        options.imbalanceFactor = std::stod(
                            std::string(arg.substr(std::string_view("-imbalance-factor=").size())));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -imbalance-factor value";
                        return nullptr;
                    }
                }
                else if (arg == "-work-dir")
                {
                    if (!parseStringArg("-work-dir", options.workDir))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-work-dir="))
                {
                    options.workDir = std::string(arg.substr(std::string_view("-work-dir=").size()));
                }
                else if (arg == "-partitioner")
                {
                    if (!parseStringArg("-partitioner", options.partitioner))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-partitioner="))
                {
                    options.partitioner = std::string(arg.substr(std::string_view("-partitioner=").size()));
                }
                else if (arg == "-mtkahypar-preset")
                {
                    if (!parseStringArg("-mtkahypar-preset", options.mtKaHyParPreset))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-mtkahypar-preset="))
                {
                    options.mtKaHyParPreset = std::string(arg.substr(std::string_view("-mtkahypar-preset=").size()));
                }
                else if (arg == "-mtkahypar-threads")
                {
                    if (!parseSizeArg("-mtkahypar-threads", options.mtKaHyParThreads))
                    {
                        return nullptr;
                    }
                }
                else if (arg.starts_with("-mtkahypar-threads="))
                {
                    try
                    {
                        options.mtKaHyParThreads = static_cast<std::size_t>(
                            std::stoull(std::string(arg.substr(std::string_view("-mtkahypar-threads=").size()))));
                    }
                    catch (const std::exception &)
                    {
                        error = "invalid -mtkahypar-threads value";
                        return nullptr;
                    }
                }
                else if (arg == "-kahypar-path" || arg.starts_with("-kahypar-path="))
                {
                    error = "-kahypar-path has been removed; use -partitioner=mt-kahypar instead";
                    return nullptr;
                }
                else if (arg == "-keep-intermediate-files")
                {
                    options.keepIntermediateFiles = true;
                }
                else
                {
                    error = "unknown repcut option";
                    return nullptr;
                }
            }
            return std::make_unique<RepcutPass>(options);
        }
        error = "unknown pass: " + normalized;
        return nullptr;
    }

} // namespace wolvrix::lib::transform
