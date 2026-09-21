#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace wolvrix::lib::grhsim
{

    struct BlockShareResult
    {
        // Per-task final file text with folded branch blocks replaced by calls.
        std::vector<std::string> taskTexts;
        // Helper definition translation units (boiler + a share of helpers).
        std::vector<std::string> blocksChunks;
        // Extra member declarations for the model header.
        std::vector<std::string> decls;
        std::string summary;
        std::size_t groups = 0;
        std::size_t instances = 0;
        std::size_t varyingSlots = 0;
        std::size_t skippedBlocks = 0;
        std::size_t foldedSourceBytes = 0;
        std::size_t excludedGroups = 0;
        double estimatedGrowth = 0.0;
    };

    struct BlockShareOptions
    {
        std::size_t minSourceBytes = 1500;
        // Optional execution hotness (perf sample percent) per task function
        // name, e.g. "cpu_task_3974" -> 0.99. When set together with a
        // non-negative growthBudget, candidate groups are excluded greedily
        // (highest estimated-growth per saved source byte first) until the
        // remaining estimated parameter-load growth fits the budget. The
        // estimate is in model units of sum(hotness% x loadBloat); on the
        // XiangShan coremark workload 1.0 model unit ~= +2.3% host
        // instruction growth (calibrated against perf stat, 2026-09).
        const std::unordered_map<std::string, double> *taskHotness = nullptr;
        double growthBudget = -1.0;
    };

    // Loads a hotness TSV of "funcName<TAB>samplePercent" lines (as produced
    // from `perf report --stdio --no-children`). Malformed lines are skipped.
    // Throws std::runtime_error when the file cannot be opened.
    std::unordered_map<std::string, double> loadTaskHotnessFile(const std::filesystem::path &path);

    // Groups `if(cpu_active_word&N){...}` branch bodies whose text is identical
    // after normalizing value-position numeric literals (and per-instance local
    // identifier ids), then folds each selected group into one noinline shared
    // member function plus per-instance block-scope static param arrays holding
    // the differing constants. Bodies referencing cpu_active_word receive it
    // through a by-reference parameter. Bodies containing return/goto/
    // break/continue, cpu_shape_p slot references, undeclared cpu_ identifiers,
    // or lexically enclosed by a loop are never folded. Compile-time literal
    // positions (template arguments, alignas, array declaration bounds,
    // constexpr lines, string literals) are never parameterized. Throws
    // std::runtime_error on internal verification failure.
    BlockShareResult foldBranchBlocks(const std::vector<std::string> &taskTexts,
                                      const std::vector<std::string> &taskFuncNames,
                                      const std::string &className,
                                      const BlockShareOptions &options = {});

} // namespace wolvrix::lib::grhsim
