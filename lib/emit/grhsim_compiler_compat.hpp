#pragma once

#include <ostream>

namespace wolvrix::lib::emit
{
    // Preserve the compiler's optimization defaults. Keep compiler regressions
    // as reproducible failures instead of silently disabling an optimizer pass.
    inline void emitGrhSimCompilerCompatibilityMakefile(std::ostream &out)
    {
        out << "# Preserve compiler optimization defaults; no automatic workaround flags.\n";
        out << "GRHSIM_COMPILER_FLAGS :=\n";
    }
}
