#ifndef WOLVRIX_FIRRTL_EMIT_FIXTURE_HPP
#define WOLVRIX_FIRRTL_EMIT_FIXTURE_HPP

#include "firrtl_test_fixture.hpp"
#include "emit/grhsim/cpu-st-cpp.hpp"

namespace firrtl_test {
inline void pass(grh::Design& design, tx::SessionStore& session, std::string_view name,
                 std::initializer_list<std::string_view> arguments = {}) {
    tx::PassManagerOptions options; options.session = &session;
    tx::PassManager manager(options); std::string error;
    auto implementation = tx::makePass(name, std::span(arguments.begin(), arguments.size()), error); require(bool(implementation), error);
    manager.addPass(std::move(implementation)); manager.addPass(std::make_unique<tx::GrhVerifyPass>());
    tx::PassDiagnostics diagnostics;
    if (!manager.run(design, diagnostics).success) {
        std::string text; for (const auto& entry : diagnostics.messages()) text += entry.message + '\n';
        throw std::runtime_error(std::string(name) + ": " + text);
    }
}
inline void generate(grh::Design& design, const std::filesystem::path& path, std::string_view driverText,
                     std::initializer_list<std::string_view> partitionOptions = {}) {
    tx::SessionStore session;
    for (const auto name : {"grh-to-grhsim", "grhsim-simplify", "grhsim-node-cse", "grhsim-split-event-and-output", "grhsim-simplify", "grhsim-split-nonsink-and-sink", "grhsim-partition-nonsink", "grhsim-simplify", "grhsim-partition-sink"})
        pass(design, session, name, std::string_view(name) == "grhsim-partition-nonsink" ? partitionOptions : std::initializer_list<std::string_view>{});
    valid(design);
    std::filesystem::remove_all(path); std::filesystem::create_directories(path);
    namespace emit = wolvrix::lib::emit;
    emit::EmitDiagnostics diagnostics; emit::GrhSimCpuStCpp emitter(&diagnostics);
    emit::EmitOptions options; options.outputDir = (path / "model").string(); options.session = &session; options.attributes = {{"waveform", "none"}};
    if (!emitter.emit(design, options).success) {
        std::string text; for (const auto& entry : diagnostics.messages()) text += entry.message + '\n';
        throw std::runtime_error(text);
    }
    std::ofstream(path / "driver.cpp") << driverText;
    std::ofstream(path / "run.mk") << "CXX := clang++\nCXXFLAGS := -std=c++20 -O1 -fno-strict-aliasing -DWOLVRIX_GRHSIM_PERF=1\n"
        << "model/libgrhsim_top.a: model/Makefile\n\t$(MAKE) -C model CXX='$(CXX)' CXXFLAGS='$(CXXFLAGS)'\n"
        << "driver: driver.cpp model/libgrhsim_top.a\n\t$(CXX) $(CXXFLAGS) -I" << WOLVRIX_FIRRTL_FST_DIR << " $< model/libgrhsim_top.a -lz -o $@\n"
        << ".PHONY: run\nrun: driver\n\t./driver\n";
}
}
#endif
