#pragma once

#include <ostream>

namespace wolvrix::lib::emit
{
    // LLVM 21's SSE2 lowering can merge discarded add lanes into live shuffle
    // lanes after SLP vectorization. SSSE3 avoids this particular shuffle bug.
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
        // LLVM 21.1.5 and 23.1.2 can move a scalar bool store ahead of a vector load
        // that supplies the old value to a change reduction. This drops an
        // activation even though the corresponding output value changes.
        // The native x86 reproducer passes with SLP disabled; keep this
        // workaround independent of graph layout and generated identifiers.
        out << "# Work around LLVM 21/23 x86 SLP bool change-reduction wrong-code.\n";
        out << "GRHSIM_COMPILER_FLAGS += $(if $(filter "
               "WOLVRIX_GRHSIM_CLANG_SLP_CHANGE_WORKAROUND,$(shell printf '%s\\n' "
               "'#if defined(__clang__) && (__clang_major__ == 21 || __clang_major__ == 23) && "
               "(defined(__i386__) || defined(__x86_64__))' "
               "'WOLVRIX_GRHSIM_CLANG_SLP_CHANGE_WORKAROUND' '#endif' | "
               "$(CXX) $(CPPFLAGS) $(CXXFLAGS) -E -P -x c++ -)),-fno-slp-vectorize)\n";
    }
}
