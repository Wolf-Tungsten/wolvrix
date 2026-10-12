#include "../../lib/emit/grhsim_compiler_compat.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
std::string quote(const std::string &text) {
    std::string result = "'";
    for (const char c : text) result += c == '\'' ? "'\\''" : std::string(1, c);
    return result + "'";
}
}

int main() {
    const std::filesystem::path root =
        std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_compiler_compat";
    std::filesystem::create_directories(root);
    const auto makefile = root / "Makefile";
    {
        std::ofstream out(makefile);
        out << "CXX ?= c++\nCXXFLAGS ?= -std=c++20 -O3\n";
        wolvrix::lib::emit::emitGrhSimCompilerCompatibilityMakefile(out);
        out << "SOURCE := " << WOLF_CLANG_REDUCTION_FIXTURE << "\n"
            << ".PHONY: all\nall:\n"
            << "\t$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(GRHSIM_COMPILER_FLAGS) -c $(SOURCE) -o model.o\n"
            << "\t$(CXX) -std=c++20 -O0 -DGRHSIM_REDUCTION_MAIN $(SOURCE) model.o -o probe\n"
            << "\t./probe\n";
    }
    // CI can select an affected Clang release through PATH.
    for (const std::string compiler : {std::string(WOLF_TEST_CXX_COMPILER), std::string("clang++")}) {
        if (compiler == "clang++" &&
            std::system("command -v clang++ >/dev/null 2>&1") != 0) continue;
        for (const std::string includes : {std::string(), std::string("-include cstdint")}) {
            const std::string command = "make -C " + quote(root.string()) +
                " CXX=" + quote(compiler) + " CPPFLAGS=" + quote(includes);
            if (std::system(command.c_str()) != 0) {
                std::cerr << "GrhSIM compiler compatibility regression failed: " << compiler << '\n';
                return 1;
            }
        }
    }
    return 0;
}
