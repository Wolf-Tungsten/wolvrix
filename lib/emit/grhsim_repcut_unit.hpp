#ifndef WOLVRIX_EMIT_GRHSIM_REPCUT_UNIT_HPP
#define WOLVRIX_EMIT_GRHSIM_REPCUT_UNIT_HPP

#include "core/emit.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace wolvrix::lib::emit::detail
{
    struct GrhSimRepCutPort
    {
        std::string graphName;
        std::string memberName;
        std::int64_t width = 0;
        bool input = false;
    };

    // clonedDesign belongs to the package emitter. Scheduling mutates only a
    // temporary unit clone, which is removed before this function returns.
    EmitResult emitGrhSimRepCutUnit(wolvrix::lib::grh::Design &clonedDesign,
                                    std::string_view graphName,
                                    std::string_view adapterName,
                                    std::span<const GrhSimRepCutPort> ports,
                                    const std::filesystem::path &packageDir,
                                    const EmitOptions &options,
                                    EmitDiagnostics *diagnostics);
}

#endif
