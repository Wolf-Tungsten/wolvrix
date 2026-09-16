#include "grhsim_cpu_dynamic_stats.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

int main() {
    GrhSIM_cpu_dynamic_stats model;
    model.set_runtime_profile_enabled(true);
    unsigned checks = 0;
    for (unsigned reset = 0; reset < 2; ++reset) {
        model.init();
        std::uint8_t q = 0;
        bool previousClock = false;
        for (unsigned step = 0; step < 1024; ++step) {
            model.a = step & 255;
            model.b = (step * 7) & 255;
            model.enable = (step >> 2) & 1;
            model.clock = step & 1;
            const std::uint8_t sum = model.a + model.b;
            const std::uint8_t sel = model.enable ? sum : model.a;
            const bool en = model.enable && model.b;
            const std::uint8_t dat = sel ^ model.a;
            const std::uint8_t msk = model.a | model.b;
            if (!previousClock && model.clock && en) q = (q & ~msk) | (dat & msk);
            previousClock = model.clock;
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                model.eval();
                if (model.out_sum != sum || model.out_sel != sel || model.out_dat != dat || model.out_q != q)
                    throw std::runtime_error("dynamic stats changed model behavior");
                ++checks;
            }
        }
    }
    if (std::freopen("dynamic_stats.log", "w", stderr) == nullptr)
        throw std::runtime_error("stderr redirect failed");
    model.dump_runtime_profile();
    std::fclose(stderr);
    std::ifstream in("dynamic_stats.log");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto need = [&](const char *marker) {
        if (text.find(marker) == std::string::npos)
            throw std::runtime_error(std::string("missing counter dump: ") + marker);
    };
    need("[grhsim-cpu-phase] evals=");
    need("[grhsim-dyn] kind core.compute.add wr=");
    need("[grhsim-dyn] kind core.compute.mux wr=");
    need("[grhsim-dyn] sn ");
    need("[grhsim-dyn] commit ");
    need("[grhsim-dyn] totals grp_pub=");
    need("port_eval=");
    need("pub_calls=");
    for (std::size_t pos = 0;;) {
        pos = text.find("[grhsim-dyn] kind ", pos);
        if (pos == std::string::npos) break;
        const auto line = text.substr(pos, text.find('\n', pos) - pos);
        unsigned long long wr = 0, ch = 0;
        if (std::sscanf(line.c_str(), "[grhsim-dyn] kind %*s wr=%llu ch=%llu", &wr, &ch) != 2 || wr == 0 || ch > wr)
            throw std::runtime_error("counter line violates 0 < ch <= wr: " + line);
        ++pos;
    }
    std::cout << "dynamic stats checks=" << checks << "\n";
}
