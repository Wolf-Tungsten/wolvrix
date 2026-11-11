#ifndef WOLVRIX_TRANSFORM_HPP
#define WOLVRIX_TRANSFORM_HPP

#include "core/diagnostics.hpp"
#include "core/logging.hpp"
#include "core/grh.hpp"

#include <algorithm>
#include <any>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace wolvrix::lib::transform
{

    enum class PassVerbosity
    {
        Debug = 0,
        Info = 1,
        Warning = 2,
        Error = 3
    };

    using PassDiagnosticKind = wolvrix::lib::diag::DiagnosticKind;
    using PassDiagnostic = wolvrix::lib::diag::Diagnostic;

    class PassDiagnostics : public wolvrix::lib::diag::Diagnostics
    {
    public:
        using wolvrix::lib::diag::Diagnostics::Diagnostics;
        void append(const PassDiagnostic& entry) {
            add(entry.kind, entry.message, entry.context, entry.passName, entry.originSymbol,
                entry.location, entry.sourcePosition);
        }

        void error(std::string passName, std::string message, std::string context = {},
                   std::optional<diag::SourcePosition> position = {});
        void warning(std::string passName, std::string message, std::string context = {},
                     std::optional<diag::SourcePosition> position = {});
        void info(std::string passName, std::string message, std::string context = {},
                  std::optional<diag::SourcePosition> position = {});
        void debug(std::string passName, std::string message, std::string context = {},
                   std::optional<diag::SourcePosition> position = {});
    };

    using TransformDiagnostics = PassDiagnostics;

    struct SessionSlot
    {
        virtual ~SessionSlot() = default;
        virtual std::unique_ptr<SessionSlot> clone() const = 0;
        virtual std::string_view kind() const noexcept { return "opaque"; }
    };

    template <typename T>
    struct SessionSlotValue : SessionSlot
    {
        template <typename U>
        explicit SessionSlotValue(U &&v, std::string kind = {})
            : value(std::forward<U>(v)), kindName(std::move(kind)) {}

        T value;
        std::string kindName;

        std::unique_ptr<SessionSlot> clone() const override
        {
            return std::make_unique<SessionSlotValue<T>>(value, kindName);
        }

        std::string_view kind() const noexcept override
        {
            return kindName.empty() ? std::string_view("opaque") : std::string_view(kindName);
        }
    };

    using SessionStore = std::unordered_map<std::string, std::unique_ptr<SessionSlot>>;

    template <typename T>
    const T& requireSessionValue(const SessionStore* store, std::string_view key)
    {
        if (!store) throw std::runtime_error("A session is required for " + std::string(key));
        const auto it = store->find(std::string(key));
        if (it == store->end() || !it->second)
            throw std::runtime_error("Missing session artifact: " + std::string(key));
        const auto* slot = dynamic_cast<const SessionSlotValue<T>*>(it->second.get());
        if (!slot)
            throw std::runtime_error("Wrong session artifact type: " + std::string(key) +
                                     " (stored " + std::string(it->second->kind()) + ")");
        return slot->value;
    }

    struct DesignStamp
    {
        uint64_t instance = 0;
        uint64_t revision = 0;
        wolvrix::lib::grh::GraphId top;

        static DesignStamp capture(const wolvrix::lib::grh::Graph& graph)
        {
            return {graph.owner().instanceId(), graph.owner().revision(), graph.id()};
        }
        bool matches(const wolvrix::lib::grh::Graph& graph) const noexcept
        {
            return instance == graph.owner().instanceId() &&
                   revision == graph.owner().revision() && top == graph.id();
        }
    };

    struct PassContext
    {
        wolvrix::lib::grh::Design &design;
        PassDiagnostics &diags;
        PassVerbosity verbosity = PassVerbosity::Info;
        LogLevel logLevel = LogLevel::Warn;
        std::function<void(LogLevel, std::string_view, std::string_view)> logSink;
        // Explicit retention override. An empty value selects the pass-specific
        // default: elimination passes do not retain declarations; others do.
        std::optional<bool> keepDeclaredSymbols;
        SessionStore *session = nullptr;
        // Non-owning selection for an explicitly graph-local kernel. The
        // design catalog remains available for read-only ABI checks.
        wolvrix::lib::grh::Graph *graph = nullptr;
    };

    struct PassResult
    {
        bool changed = false;
        bool failed = false;
        std::vector<std::string> artifacts;
        std::any graphData;
    };

    // A deterministic graph-level worklist shared by the passes that form a
    // fixed-point pipeline.  The first invocation seeds every graph; later
    // invocations consume only graphs that a preceding pass actually changed.
    // Passes still retain their complete-graph behavior when no worklist is
    // installed (standalone callers and legacy pipelines).
    struct DirtyWorklist
    {
        using GraphNames = std::unordered_set<std::string>;
        std::unordered_map<std::string, GraphNames> pending;
        bool active = false;

        void seed(const wolvrix::lib::grh::Design& design,
                  std::initializer_list<std::string_view> passIds)
        {
            active = true;
            GraphNames graphs;
            graphs.reserve(design.graphs().size());
            for (const auto& entry : design.graphs()) graphs.insert(entry.first);
            for (const auto pass : passIds) pending[std::string(pass)] = graphs;
        }

        std::optional<GraphNames> take(std::string_view passId)
        {
            if (!active) return std::nullopt;
            const auto found = pending.find(std::string(passId));
            if (found == pending.end()) return std::nullopt;
            GraphNames result = std::move(found->second);
            pending.erase(found);
            return result;
        }

        void enqueue(std::string_view passId, std::string_view graph)
        {
            pending[std::string(passId)].insert(std::string(graph));
        }
    };

    inline wolvrix::lib::grh::SrcLoc makeTransformSrcLoc(std::string_view passId,
                                               std::string_view note = {})
    {
        wolvrix::lib::grh::SrcLoc loc{};
        loc.origin = "transform";
        loc.pass = std::string(passId);
        loc.note = std::string(note);
        return loc;
    }

    // `originModules` records the `$`-joined source module instance paths of
    // each op's defining logic (def-site only, never data-dependency
    // propagation). Merges/shares take the ordered-dedup union; clones carry
    // it for free with the other attrs. Stamped by hier-flatten; pass-synth
    // ops inherit it from their primary source op(s) via inheritOrigin.
    inline constexpr std::string_view kOriginModulesKey = "originModules";

    inline std::optional<std::vector<std::string>> getOriginModules(
        const wolvrix::lib::grh::Graph &graph,
        wolvrix::lib::grh::OperationId op)
    {
        // Graph has no lean single-attr accessor; the span scan is cheap
        // (attribute lists are short) and keeps callers off fat Operation.
        for (const auto kv : graph.opAttrRange(op))
        {
            if (kv.key() != kOriginModulesKey)
            {
                continue;
            }
            if (const auto values = kv.value().get<std::vector<std::string>>())
            {
                return std::vector<std::string>(values->begin(), values->end());
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    inline void appendOriginModule(std::vector<std::string> &origins, std::string_view module)
    {
        if (module.empty())
        {
            return;
        }
        if (std::find(origins.begin(), origins.end(), module) == origins.end())
        {
            origins.emplace_back(module);
        }
    }

    inline void appendOriginModules(std::vector<std::string> &origins,
                                    const std::vector<std::string> &extra)
    {
        for (const std::string &module : extra)
        {
            appendOriginModule(origins, module);
        }
    }

    inline void prependOriginModule(std::vector<std::string> &origins, std::string_view module)
    {
        if (module.empty())
        {
            return;
        }
        if (std::find(origins.begin(), origins.end(), module) != origins.end())
        {
            return;
        }
        origins.insert(origins.begin(), std::string(module));
    }

    // Ordered-dedup union of `extra` into op's originModules; sets the attr
    // when absent and removes it when the union is empty. Returns true when
    // the attribute changed.
    inline bool mergeOriginModules(wolvrix::lib::grh::Graph &graph,
                                   wolvrix::lib::grh::OperationId op,
                                   const std::vector<std::string> &extra)
    {
        if (extra.empty()) return false;
        std::optional<wolvrix::lib::grh::StringListView> current;
        for (const auto attribute : graph.opAttrRange(op))
            if (attribute.key() == kOriginModulesKey) {
                current = attribute.value().get<std::vector<std::string>>();
                break;
            }
        // Shared constants/expressions can have thousands of origin paths.
        // Most reuses add no path: inspect borrowed data before copying it.
        const auto missing = [&](const auto& module) {
            return !module.empty() && (!current || std::find(current->begin(), current->end(), module) == current->end());
        };
        if (std::none_of(extra.begin(), extra.end(), missing)) return false;
        std::vector<std::string> merged;
        if (current) merged.assign(current->begin(), current->end());
        const std::size_t before = merged.size();
        appendOriginModules(merged, extra);
        if (merged.empty())
        {
            if (before == 0)
            {
                return false;
            }
            return graph.eraseAttr(op, kOriginModulesKey);
        }
        if (merged.size() == before)
        {
            return false;
        }
        graph.setAttr(op, kOriginModulesKey, std::move(merged));
        return true;
    }

    // Union of all source ops' originModules onto newOp; a fully sourceless
    // synthesis (fresh guard/const) keeps the attr absent.
    inline void inheritOrigin(wolvrix::lib::grh::Graph &graph,
                              wolvrix::lib::grh::OperationId newOp,
                              std::initializer_list<wolvrix::lib::grh::OperationId> srcOps)
    {
        std::vector<std::string> merged;
        for (const wolvrix::lib::grh::OperationId src : srcOps)
        {
            if (!src.valid())
            {
                continue;
            }
            if (const auto origins = getOriginModules(graph, src))
            {
                appendOriginModules(merged, *origins);
            }
            // Keep operation-level provenance with the synthesized operation.
            // Value-level SemanticTree identities are shape-dependent and are
            // transferred explicitly by each rewrite (for example
            // mergeOrigins/makeOperation); copying every source result here
            // would incorrectly keep an aggregate declaration alive when a
            // split view selects an unrelated scalar element.
            graph.mergeOpSemanticIds(newOp, src);
        }
        if (!merged.empty())
        {
            graph.setAttr(newOp, kOriginModulesKey, std::move(merged));
        }
    }

    class Pass
    {
    public:
        Pass(std::string id, std::string name, std::string description = {});
        virtual ~Pass() = default;

        virtual PassResult run() = 0;
        // A fresh instance owns its context and diagnostics on one worker.
        // Cross-graph/catalog-mutating passes deliberately return nullptr.
        virtual std::unique_ptr<Pass> cloneGraphKernel() const { return {}; }
        virtual void finishGraphKernels(std::span<const PassResult>) {}
        virtual bool usesSourceGraphOrder() const noexcept { return false; }
        // Fusing across pass boundaries requires a stable dialect set.
        virtual bool preservesDialects() const noexcept { return false; }
        virtual bool supportsDialect(std::string_view dialect) const noexcept { return dialect == "grh"; }

        const std::string &id() const noexcept { return id_; }
        const std::string &name() const noexcept { return name_; }
        const std::string &description() const noexcept { return description_; }
        void setName(std::string name) { name_ = std::move(name); }

    protected:
        std::vector<wolvrix::lib::grh::Graph*> graphs(bool sourceOrder = false) const;
        bool graphKernel() const noexcept { return context_ && context_->graph; }
        void log(LogLevel level, std::string message);
        void log(LogLevel level, std::string_view tag, std::string message);
        void logInfo(std::string message) { log(LogLevel::Info, std::move(message)); }
        void logWarn(std::string message) { log(LogLevel::Warn, std::move(message)); }
        void logError(std::string message) { log(LogLevel::Error, std::move(message)); }
        void logDebug(std::string message) { log(LogLevel::Debug, std::move(message)); }
        wolvrix::lib::grh::Design &design() { return context_->design; }
        PassDiagnostics &diags() { return context_->diags; }
        PassVerbosity verbosity() const noexcept { return context_ ? context_->verbosity : PassVerbosity::Error; }
        bool hasSessionValue(std::string_view key) const noexcept
        {
            if (!context_ || !context_->session)
            {
                return false;
            }
            return context_->session->find(std::string(key)) != context_->session->end();
        }
        SessionSlot *getSessionSlot(std::string_view key) noexcept
        {
            if (!context_ || !context_->session)
            {
                return nullptr;
            }
            auto it = context_->session->find(std::string(key));
            return it == context_->session->end() ? nullptr : it->second.get();
        }
        const SessionSlot *getSessionSlot(std::string_view key) const noexcept
        {
            if (!context_ || !context_->session)
            {
                return nullptr;
            }
            auto it = context_->session->find(std::string(key));
            return it == context_->session->end() ? nullptr : it->second.get();
        }
        template <typename T>
        T *getSessionValue(std::string_view key) noexcept
        {
            if (auto *slot = getSessionSlot(key))
            {
                if (auto *typed = dynamic_cast<SessionSlotValue<T> *>(slot))
                {
                    return &typed->value;
                }
            }
            return nullptr;
        }
        template <typename T>
        const T *getSessionValue(std::string_view key) const noexcept
        {
            if (const auto *slot = getSessionSlot(key))
            {
                if (const auto *typed = dynamic_cast<const SessionSlotValue<T> *>(slot))
                {
                    return &typed->value;
                }
            }
            return nullptr;
        }
        template <typename T>
        void setSessionValue(std::string key, T &&value)
        {
            setSessionValue(std::move(key), std::forward<T>(value), {});
        }
        template <typename T>
        void setSessionValue(std::string key, T &&value, std::string kind)
        {
            if (!context_ || !context_->session)
            {
                return;
            }
            context_->session->insert_or_assign(
                std::move(key),
                std::make_unique<SessionSlotValue<std::decay_t<T>>>(std::forward<T>(value),
                                                                       std::move(kind)));
        }
        void eraseSessionValue(std::string_view key)
        {
            if (!context_ || !context_->session)
            {
                return;
            }
            context_->session->erase(std::string(key));
        }
        void debug(std::string message, std::string context = {});
        void error(std::string message, std::string context = {});
        void warning(std::string message, std::string context = {});
        void info(std::string message, std::string context = {});
        void error(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Operation &op, std::string message);
        void warning(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Operation &op, std::string message);
        void info(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Operation &op, std::string message);
        void debug(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Operation &op, std::string message);
        void error(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Value &value, std::string message);
        void warning(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Value &value, std::string message);
        void info(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Value &value, std::string message);
        void debug(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Value &value, std::string message);
        void error(const wolvrix::lib::grh::Graph &graph, std::string message);
        void warning(const wolvrix::lib::grh::Graph &graph, std::string message);
        void info(const wolvrix::lib::grh::Graph &graph, std::string message);
        void debug(const wolvrix::lib::grh::Graph &graph, std::string message);
        bool keepDeclaredSymbols() const noexcept
        {
            if (context_ && context_->keepDeclaredSymbols)
            {
                return *context_->keepDeclaredSymbols;
            }
            return id_ != "dead-code-elim" && id_ != "split-static-array" &&
                id_ != "expr-opt" && id_ != "used-bits" && id_ != "split-nodes" &&
                id_ != "constant-analysis" && id_ != "alias-analysis" &&
                id_ != "pattern-detect" && id_ != "common-expr";
        }
        SessionStore* session() noexcept { return context_ ? context_->session : nullptr; }

    private:
        friend class PassManager;

        bool shouldEmit(PassDiagnosticKind kind) const noexcept;
        bool shouldLog(LogLevel level) const noexcept;
        void setContext(PassContext *ctx) { context_ = ctx; }
        void clearContext() { context_ = nullptr; }

        std::string id_;
        std::string name_;
        std::string description_;
        PassContext *context_ = nullptr;
    };

    struct PassManagerOptions
    {
        bool stopOnError = true;
        bool emitTiming = false;
        PassVerbosity verbosity = PassVerbosity::Info;
        LogLevel logLevel = LogLevel::Warn;
        std::function<void(LogLevel, std::string_view, std::string_view)> logSink;
        // Forwarded to every PassContext. Set a boolean to override the
        // pass-specific declaration-retention default for the whole pipeline.
        std::optional<bool> keepDeclaredSymbols;
        SessionStore *session = nullptr;
        std::size_t graphThreads = 1;
        // Optional eligibility gate for final compaction (e.g. positional ABI).
        std::function<bool(const wolvrix::lib::grh::Graph&)> compactGraph;
        bool prepareFlattenSources = false;
    };

    struct PassStageResult
    {
        std::string pass;
        std::vector<PassDiagnostic> diagnostics;
        double wallSeconds = 0;
        double workerSeconds = 0;
        bool overlapped = false;
    };

    struct PassManagerResult
    {
        bool success = true;
        bool changed = false;
        std::vector<PassStageResult> stages;
    };

    class PassManager
    {
    public:
        explicit PassManager(PassManagerOptions options = PassManagerOptions());

        void addPass(std::unique_ptr<Pass> pass, std::string instanceName = {});
        void clear();

        PassManagerResult run(wolvrix::lib::grh::Design &design, PassDiagnostics &diags);
        PassManagerResult runGraph(wolvrix::lib::grh::Graph& graph, PassDiagnostics& diags);
        // All entries must offer graph kernels. One worker completes the
        // ordered local chain before taking another graph. Results/diagnostics
        // are published by stage, then the original graph traversal order.
        // Continue-on-error and kernels without a dialect-preservation contract
        // retain serial stage barriers. A failed fused chain may have performed
        // later speculative mutations; transactional callers discard its design.
        PassManagerResult runGraphPipeline(wolvrix::lib::grh::Design& design,
                                           PassDiagnostics& diags, bool compactAtEnd = false);

        PassManagerOptions &options() noexcept { return options_; }
        const PassManagerOptions &options() const noexcept { return options_; }

    private:
        PassManagerResult runImpl(wolvrix::lib::grh::Design& design, PassDiagnostics& diags,
                                  wolvrix::lib::grh::Graph* graph, bool allowGraphThreads = true);
        struct PassEntry
        {
            std::unique_ptr<Pass> instance;
        };

        std::vector<PassEntry> pipeline_;
        PassManagerOptions options_;
    };

    std::string normalizePassName(std::string_view name);

    std::vector<std::string> availableTransformPasses();

    std::unique_ptr<Pass> makePass(std::string_view name,
                                   std::span<const std::string_view> args,
                                   std::string &error);

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_HPP
