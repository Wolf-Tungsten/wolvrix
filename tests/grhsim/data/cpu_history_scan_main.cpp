#include "grhsim_cpu_history_scan.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string_view>

int main(int argc, char **argv)
{
    GrhSIM_cpu_history_scan model;
    const bool derived = argc > 1 && std::string_view(argv[1]) == "--derived";
    const bool randomHistory = argc > 1 && std::string_view(argv[1]) == "--random";
    std::mt19937 random(20260907);
    unsigned samples = 0;
    for (unsigned reset = 0; reset < 4; ++reset) {
        model.init();
        std::array<std::uint8_t, 32> expected{};
        std::array<bool, 32> historyA{}, historyB{};
        historyA.fill(true); historyA.back() = false; historyB.front() = true;
        const auto step = [&](bool clockA, bool clockB, bool enable, std::uint8_t data) {
            const bool eventB = derived ? !clockB : clockB;
            for (unsigned i = 0; i < expected.size(); ++i)
                if (enable && ((!historyA[i] && clockA) || (historyB[i] && !eventB))) expected[i] = data;
            historyA.fill(clockA); historyB.fill(eventB);
            model.clock_a = clockA; model.clock_b = clockB; model.enable = enable; model.data = data;
            model.eval(); ++samples;
            const std::array<std::uint8_t, 32> actual{
                model.q0, model.q1, model.q2, model.q3, model.q4, model.q5, model.q6, model.q7,
                model.q8, model.q9, model.q10, model.q11, model.q12, model.q13, model.q14, model.q15,
                model.q16, model.q17, model.q18, model.q19, model.q20, model.q21, model.q22, model.q23,
                model.q24, model.q25, model.q26, model.q27, model.q28, model.q29, model.q30, model.q31};
            if (actual != expected)
                throw std::runtime_error("history scan edge/sample mismatch at " + std::to_string(samples));
#ifndef CPU_HISTORY_SCAN_PRIVATE
            if (model.history_a != clockA || model.history_b != eventB)
                throw std::runtime_error("observed history mismatch at " + std::to_string(samples));
#endif
        };
        // Sample both events with writes disabled before checking random
        // histories; their independent initial bits need not be predicted.
        if (randomHistory) step(false, true, false, 0);
        step(true, !derived, true, 42);
        if (!randomHistory && (model.q0 != 0 || model.q30 != 0 || model.q31 != 42))
            throw std::runtime_error("history scan missed the final byte or discarded distinct initial histories");
        step(true, !derived, true, 77);
        step(false, !derived, true, 19);
        step(false, derived, true, 31);
        step(true, !derived, false, 91);
        step(true, !derived, true, 53);
        bool clockA = true, clockB = !derived;
        for (unsigned i = 0; i < 1024; ++i) {
            if (i % 3 == 0) clockA = !clockA;
            if (i % 5 == 0) clockB = !clockB;
            const auto data = std::uint8_t(random());
            const bool enable = i % 7 != 0;
            step(clockA, clockB, enable, data);
            if (i % 8 == 0) step(clockA, clockB, enable, data);
        }
    }
    std::cout << "history scan PASS samples=" << samples << " resets=4 derived=" << derived << '\n';
}
