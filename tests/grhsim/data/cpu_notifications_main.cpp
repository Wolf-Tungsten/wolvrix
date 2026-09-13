#include "grhsim_cpu_notifications.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>

int main()
{
    GrhSIM_cpu_notifications model;
    const std::array enables{&model.en0, &model.en1, &model.en2, &model.en3,
                             &model.en4, &model.en5, &model.en6, &model.en7};
    const std::array data{&model.d0, &model.d1, &model.d2, &model.d3,
                          &model.d4, &model.d5, &model.d6, &model.d7};
    std::mt19937 random(20260913);
    unsigned samples = 0;
    for (unsigned reset = 0; reset < 4; ++reset) {
        model.init();
        std::array<std::uint8_t, 41> expected{};
        bool previous = false;
        const auto step = [&] {
            if (model.clock && !previous)
                for (unsigned i = 0; i < expected.size(); ++i)
                    if (*enables[i % enables.size()])
                        expected[i] = (expected[i] & ~model.mask) | (*data[i % data.size()] & model.mask);
            previous = model.clock;
            model.eval();
            ++samples;
            std::uint8_t sum = 0;
            for (auto value : expected) sum += value + model.mask;
            if (model.sum != sum)
                throw std::runtime_error("shared commit edge lost a write or consumer activation");
        };
        model.clock = false;
        for (auto enable : enables) *enable = false;
        step();
        for (unsigned i = 0; i < 2048; ++i) {
            if (i % 4 == 0) {
                for (auto value : data) *value = random();
                model.mask = i % 3 == 0 ? 0 : i % 3 == 1 ? 255 : random();
            } else if (i % 4 == 1) {
                for (unsigned j = 0; j < enables.size(); ++j)
                    *enables[j] = i % 3 == 0 ? j == (i / 4) % 8 : (random() & 1) != 0;
            } else model.clock = !model.clock;
            step();
            if (i % 8 == 0) step();
        }
    }
    std::cout << "shared commit edge PASS samples=" << samples << " resets=4\n";
}
