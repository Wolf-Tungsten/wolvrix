#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wolvrix::lib::grhsim
{

    struct ShapeShareResult
    {
        // Per-task final file text: wrapper for folded tasks, original otherwise.
        std::vector<std::string> taskTexts;
        // Definitions of all shared shape functions (empty when nothing folded).
        std::string shapesCpp;
        // Extra member declarations for the model header.
        std::vector<std::string> decls;
        std::string summary;
        std::size_t groups = 0;
        std::size_t instances = 0;
        std::size_t varyingSlots = 0;
        std::size_t skippedHelpers = 0;
    };

    // Groups task translation units whose text is identical after normalizing
    // value-position numeric literals (and per-instance local identifier ids),
    // then folds each selected group into one noinline shared member function
    // plus per-task thin wrappers carrying the differing constants in static
    // param arrays. Compile-time literal positions (template arguments,
    // alignas, array declaration bounds, constexpr lines, string literals) are
    // never parameterized. Throws std::runtime_error on internal
    // verification failure.
    ShapeShareResult foldShapeTwins(const std::vector<std::string> &taskTexts,
                                    const std::vector<std::string> &taskFuncNames,
                                    const std::string &className,
                                    std::size_t minSourceBytes);

} // namespace wolvrix::lib::grhsim
