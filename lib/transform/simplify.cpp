#include "transform/simplify.hpp"

#include <chrono>

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

        bool anyChanged = false;
        bool failed = false;

        for (int iter = 0; iter < options_.maxIterations; ++iter)
        {
            const auto iterStart = std::chrono::steady_clock::now();
            PassManagerOptions pmOptions;
            pmOptions.stopOnError = true;
            pmOptions.emitTiming = false;
            pmOptions.verbosity = verbosity();
            pmOptions.logLevel = LogLevel::Warn;
            pmOptions.keepDeclaredSymbols = keepDeclaredSymbols();
            pmOptions.logSink = [this](LogLevel level, std::string_view tag, std::string_view message) {
                this->log(level, tag, std::string(message));
            };

            PassManager pm(pmOptions);
            ConstantFoldOptions foldOptions;
            foldOptions.semantics = options_.semantics;
            foldOptions.xFold = options_.xFold;
            if (options_.semantics == ConstantFoldOptions::Semantics::TwoState)
            {
                foldOptions.xFold = ConstantFoldOptions::XFoldMode::Strict;
            }

            pm.addPass(std::make_unique<ConstantFoldPass>(foldOptions));
            pm.addPass(std::make_unique<RedundantElimPass>());
            pm.addPass(std::make_unique<DeadCodeElimPass>());

            PassManagerResult pmResult = pm.run(design(), diags());
            const auto iterMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - iterStart)
                                    .count();
            logInfo("simplify iteration=" + std::to_string(iter) + " changed="
                    + std::to_string(pmResult.changed) + " " + std::to_string(iterMs) + "ms");
            if (!pmResult.success)
            {
                failed = true;
                break;
            }
            if (!pmResult.changed)
            {
                break;
            }
            anyChanged = true;
        }

        result.changed = anyChanged;
        result.failed = failed;
        return result;
    }

} // namespace wolvrix::lib::transform
