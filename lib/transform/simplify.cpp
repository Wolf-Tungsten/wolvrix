#include "transform/simplify.hpp"

#include <chrono>
#include <utility>

#include "transform/const_fold.hpp"
#include "transform/dead_code_elim.hpp"
#include "transform/redundant_elim.hpp"

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

        PassManagerOptions pmOptions;
        pmOptions.stopOnError = true;
        pmOptions.emitTiming = false;
        pmOptions.verbosity = verbosity();
        pmOptions.logLevel = LogLevel::Warn;
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
        redundant.addPass(std::make_unique<RedundantElimPass>());
        dead.addPass(std::make_unique<DeadCodeElimPass>());

        bool foldDirty = true;
        bool redundantDirty = true;
        bool deadDirty = true;
        for (int iter = 0; iter < options_.maxIterations; ++iter)
        {
            const auto iterStart = std::chrono::steady_clock::now();
            bool iterationChanged = false;
            std::string steps;
            auto runStep = [&](PassManager &pm, std::string_view name) {
                const auto start = std::chrono::steady_clock::now();
                const PassManagerResult step = pm.run(design(), diags());
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - start).count();
                steps += " " + std::string(name) + "=" + std::to_string(step.changed)
                         + ":" + std::to_string(ms) + "ms";
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
            }
            if (!result.failed && std::exchange(redundantDirty, false))
            {
                const bool changed = runStep(redundant, "redundant-elim");
                foldDirty = foldDirty || changed;
                redundantDirty = changed;
                deadDirty = deadDirty || changed;
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
            }
            const auto iterMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - iterStart)
                                    .count();
            logInfo("simplify iteration=" + std::to_string(iter) + " changed="
                    + std::to_string(iterationChanged) + " " + std::to_string(iterMs) + "ms" + steps);
            if (result.failed || (!foldDirty && !redundantDirty && !deadDirty))
            {
                break;
            }
        }

        return result;
    }

} // namespace wolvrix::lib::transform
