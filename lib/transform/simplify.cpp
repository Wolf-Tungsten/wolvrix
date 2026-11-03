#include "transform/simplify.hpp"

#include <chrono>
#include <utility>

#include "transform/const_fold.hpp"
#include "transform/dead_code_elim.hpp"
#include "transform/redundant_elim.hpp"
#include "ingest/node_attributes.hpp"

namespace wolvrix::lib::transform
{

    SimplifyPass::SimplifyPass()
        : Pass("simplify", "simplify", "Iteratively fold constants, remove redundancies, and eliminate dead code"),
          options_({})
    {
    }

    SimplifyPass::SimplifyPass(SimplifyOptions options)
        : Pass("simplify", "simplify", "Iteratively fold constants, remove redundancies, and eliminate dead code"),
          options_(options)
    {
    }

    PassResult SimplifyPass::run()
    {
        PassResult result;
        const std::size_t graphCount = design().graphs().size();
        logDebug("begin graphs=" + std::to_string(graphCount));

        // All child pass managers share a short-lived validation cache.  The
        // cache is revision keyed, so any graph mutation naturally invalidates
        // the entry while stable graphs can skip repeated node scans.
        SessionStore simplifySession;
        auto validationSlot = std::make_unique<SessionSlotValue<ingest::node::ValidationCache>>(
            ingest::node::ValidationCache{}, "firrtl-node-validation");
        auto *validationCache = &validationSlot->value;
        simplifySession.emplace("firrtl.node.validation", std::move(validationSlot));
        auto dirtySlot = std::make_unique<SessionSlotValue<DirtyWorklist>>(DirtyWorklist{}, "simplify-dirty-worklist");
        auto *dirty = &dirtySlot->value;
        simplifySession.emplace("simplify.dirty", std::move(dirtySlot));
        dirty->seed(design(), {"const-fold", "redundant-elim", "dead-code-elim"});

        PassManagerOptions pmOptions;
        pmOptions.stopOnError = true;
        pmOptions.emitTiming = false;
        pmOptions.verbosity = verbosity();
        pmOptions.logLevel = LogLevel::Warn;
        pmOptions.session = &simplifySession;
        pmOptions.keepDeclaredSymbols = keepDeclaredSymbols();
        pmOptions.logSink = [this](LogLevel level, std::string_view tag, std::string_view message) {
            this->log(level, tag, std::string(message));
        };

        ConstantFoldOptions foldOptions;
        foldOptions.semantics = options_.semantics;
        foldOptions.xFold = options_.xFold;
        if (options_.semantics == ConstantFoldOptions::Semantics::TwoState)
        {
            foldOptions.xFold = ConstantFoldOptions::XFoldMode::Strict;
        }
        PassManager fold(pmOptions), redundant(pmOptions), dead(pmOptions);
        auto foldPass = std::make_unique<ConstantFoldPass>(foldOptions);
        const auto *foldState = foldPass.get();
        fold.addPass(std::move(foldPass));
        auto redundantPass = std::make_unique<RedundantElimPass>();
        const auto *redundantState = redundantPass.get();
        redundant.addPass(std::move(redundantPass));
        auto deadPass = std::make_unique<DeadCodeElimPass>();
        const auto *deadState = deadPass.get();
        dead.addPass(std::move(deadPass));

        bool foldDirty = true;
        bool redundantDirty = true;
        bool deadDirty = true;
        for (int iter = 0; iter < options_.maxIterations; ++iter)
        {
            const auto iterStart = std::chrono::steady_clock::now();
            bool iterationChanged = false;
            std::string steps;
            const auto graphShape = [&]() {
                std::size_t operations = 0;
                std::size_t values = 0;
                for (const auto& entry : design().graphs()) {
                    operations += entry.second->operations().size();
                    values += entry.second->values().size();
                }
                return std::pair{operations, values};
            };
            auto runStep = [&](PassManager &pm, std::string_view name) {
                const auto beforeShape = graphShape();
                const auto start = std::chrono::steady_clock::now();
                const PassManagerResult step = pm.run(design(), diags());
                const auto afterShape = graphShape();
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - start).count();
                steps += " " + std::string(name) + "=" + std::to_string(step.changed)
                         + ":" + std::to_string(ms) + "ms"
                         + " ops=" + std::to_string(beforeShape.first) + "->" +
                         std::to_string(afterShape.first)
                         + " values=" + std::to_string(beforeShape.second) + "->" +
                         std::to_string(afterShape.second);
                result.changed = result.changed || step.changed;
                result.failed = result.failed || !step.success;
                iterationChanged = iterationChanged || step.changed;
                return step.changed;
            };
            if (std::exchange(foldDirty, false))
            {
                const bool changed = runStep(fold, "const-fold");
                foldDirty = foldState->needsRevisit();
                redundantDirty = redundantDirty || changed;
                deadDirty = deadDirty || changed;
                const auto& stats = foldState->stats();
                steps += " cf-constants=" + std::to_string(stats.constantsVisited) +
                         " cf-worklist=" + std::to_string(stats.worklistPopped) + "/" +
                         std::to_string(stats.worklistEnqueued) +
                         " cf-folded=" + std::to_string(stats.foldedOps) +
                         " cf-slices=" + std::to_string(stats.sliceCandidates) +
                         ":" + std::to_string(stats.simplifiedSlices) +
                         " cf-cmp=" + std::to_string(stats.comparisonCandidates) +
                         ":" + std::to_string(stats.unsignedComparisons) +
                         " cf-dead=" + std::to_string(stats.deadConstantCandidates) +
                         ":" + std::to_string(stats.deadConstants);
            }
            if (!result.failed && std::exchange(redundantDirty, false))
            {
                const bool changed = runStep(redundant, "redundant-elim");
                foldDirty = foldDirty || changed;
                redundantDirty = changed;
                deadDirty = deadDirty || changed;
                const auto& stats = redundantState->stats();
                steps += " re-ops=" + std::to_string(stats.operationsVisited) +
                         " re-removed=" + std::to_string(stats.opsRemoved) +
                         ":" + std::to_string(stats.valuesRemoved);
            }
            // Trim the initial graph, then batch dead-code cleanup while
            // rewrites are still progressing. DCE can reopen single-use rules,
            // so its changed result still wakes redundant-elim below.
            const bool cleanupNow = iter == 0 || iter + 1 == options_.maxIterations ||
                                    (!foldDirty && !redundantDirty);
            if (!result.failed && cleanupNow && std::exchange(deadDirty, false))
            {
                // DCE reaches its own liveness fixed point. Removing users can
                // enable single-use rewrites, but cannot create constant inputs.
                redundantDirty = runStep(dead, "dead-code-elim") || redundantDirty;
                const auto& stats = deadState->stats();
                steps += " dce-ops=" + std::to_string(stats.operationsVisited) +
                         " dce-removed=" + std::to_string(stats.opsRemoved) +
                         ":" + std::to_string(stats.valuesRemoved);
            }
            const auto iterMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - iterStart)
                                    .count();
            logInfo("simplify iteration=" + std::to_string(iter) + " changed="
                    + std::to_string(iterationChanged) + " " + std::to_string(iterMs) + "ms" + steps +
                    " node-validation=" + std::to_string(validationCache->hits) + "/" +
                    std::to_string(validationCache->misses));
            if (result.failed || (!foldDirty && !redundantDirty && !deadDirty))
            {
                break;
            }
        }

        if (!result.failed && (foldDirty || redundantDirty || deadDirty))
        {
            std::string pending;
            if (foldDirty) pending += " const-fold";
            if (redundantDirty) pending += " redundant-elim";
            if (deadDirty) pending += " dead-code-elim";
            logWarn("simplify reached max-iterations=" + std::to_string(options_.maxIterations) +
                    " before convergence; pending=" + pending);
        }

        return result;
    }

} // namespace wolvrix::lib::transform
