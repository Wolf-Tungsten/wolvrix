#pragma once

#include <ostream>

namespace wolvrix::lib::emit
{
    // LLVM 21's SSE2 lowering can merge discarded add lanes into live shuffle
    // lanes after SLP vectorization. SSSE3 uses a different, validated lowering.
    // Query target macros with the caller's flags, including cross-compile flags.
    inline void emitGrhSimCompilerCompatibilityMakefile(std::ostream &out)
    {
        out << "# Work around LLVM 21 x86/SSE2 vector shuffle wrong-code.\n";
        out << "GRHSIM_COMPILER_FLAGS := $(if $(filter "
               "WOLVRIX_GRHSIM_CLANG21_SSE2_WORKAROUND,$(shell printf '%s\\n' "
               "'#if defined(__clang__) && __clang_major__ == 21 && "
               "(defined(__i386__) || defined(__x86_64__)) && !defined(__SSSE3__)' "
               "'WOLVRIX_GRHSIM_CLANG21_SSE2_WORKAROUND' '#endif' | "
               "$(CXX) $(CPPFLAGS) $(CXXFLAGS) -E -P -x c++ -)),-fno-slp-vectorize)\n";
    }
}
