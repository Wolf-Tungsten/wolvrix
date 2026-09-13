#include "grhsim_cpu_memory_stage.hpp"
#include "memory_stage_slots.hpp"

#include <array>
#include <iostream>
#include <random>
#include <stdexcept>

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

template<unsigned Width, class T>
void testLane(GrhSIM_cpu_memory_stage &model, unsigned lane,
              T &data0, T &data1, T &mask0, T &mask1, const T &output) {
    model.init();
    const auto [key, offset] = memory_stage_slots[lane];
    const auto write = [&](unsigned row, std::uint64_t data, std::uint64_t mask = UINT64_MAX) {
        model.cpu_write_cell<T, Width>(key, offset, row, 0, 0, true, data, mask);
    };
    const auto visible = [&](unsigned row) { return cpu_at<T>(model.cpu_objects.get(), offset + row * sizeof(T)); };
    write(0, 0); write(1, UINT64_MAX, 0);
    require(model.cpu_pending.empty() && !model.cpu_dirty[key] && !model.cpu_dirty[key + 1],
            "unchanged memory write allocated pending work");
    write(0, 1); write(0, 1); write(1, 1);
    require(model.cpu_pending.size() == 2 && visible(0) == 0 && visible(1) == 0,
            "memory staging duplicated a row or published early");
    write(0, 0); write(1, 0);
    require(model.cpu_pending.size() == 2 && !model.cpu_publish() && visible(0) == 0,
            "memory cancellation ignored the current shadow");
    require(model.cpu_pending.empty() && !model.cpu_dirty[key] && !model.cpu_dirty[key + 1],
            "memory publication retained pending/dirty rows");
    cpu_at<T>(model.cpu_stage_cell(key, offset, sizeof(T), 2, 0, 0, true), 0) = T{1};
    write(2, 0);
    require(!model.cpu_publish() && visible(2) == 0, "cell write ignored a legacy staged value");
    write(3, 1);
    require(model.cpu_publish() && visible(3) == 1, "changed cell failed to publish");
    write(3, 1);
    require(model.cpu_pending.empty(), "nonzero no-op created pending work");

    const std::uint64_t bits = UINT64_MAX >> (64 - Width);
    std::mt19937_64 random(7393 + lane);
    for (unsigned reset = 0; reset < 3; ++reset) {
        model.init(); model.enable = true; model.clock = false; model.read_address = 0;
        model.eval();
        std::array<std::uint64_t, 4> expected{};
        for (unsigned sample = 0; sample < 1024; ++sample) {
            model.clock = false; model.eval();
            model.address_a = random() & 7;
            model.address_b = sample % 2 ? model.address_a : random() & 7;
            data0 = static_cast<T>(random()); data1 = static_cast<T>(random());
            mask0 = static_cast<T>(random()); mask1 = static_cast<T>(random());
            switch (sample % 8) {
            case 0: mask0 = T{0}; mask1 = T{0}; break;
            case 1: model.address_b = model.address_a;
                data0 = data1 = static_cast<T>(expected[model.address_a & 3]); break;
            case 2: model.address_b = model.address_a;
                mask0 = mask1 = static_cast<T>(bits);
                data1 = static_cast<T>(expected[model.address_a & 3]); break;
            case 3: model.address_b = model.address_a;
                mask0 = static_cast<T>(bits & UINT64_C(0x5555555555555555));
                mask1 = static_cast<T>(bits & UINT64_C(0xaaaaaaaaaaaaaaaa)); break;
            default: break;
            }
            model.enable = sample % 11 != 0;
            if (model.enable) {
                unsigned port = 0;
                for (auto pair : {std::pair{data0, mask0}, std::pair{data1, mask1}}) {
                    const unsigned row = port++ ? model.address_b : model.address_a;
                    if (row >= expected.size()) continue;
                    const auto mask = static_cast<std::uint64_t>(pair.second) & bits;
                    expected[row] = ((expected[row] & ~mask) | (static_cast<std::uint64_t>(pair.first) & mask)) & bits;
                }
            }
            model.clock = true;
            for (unsigned row = 0; row < 4; ++row) {
                model.read_address = row; model.eval();
                require((static_cast<std::uint64_t>(output) & bits) == expected[row],
                        "ordered masked memory write or addressed-reader activation mismatch");
                if constexpr (std::is_signed_v<T>) {
                    const auto extended = expected[row] & (UINT64_C(1) << (Width - 1)) ? expected[row] | ~bits : expected[row];
                    require(static_cast<std::uint64_t>(output) == extended, "memory result is not sign-extended");
                }
            }
        }
    }
}

int main() {
    GrhSIM_cpu_memory_stage model;
#define LANE(N, W) testLane<W>(model, N, model.data##N##_0, model.data##N##_1, model.mask##N##_0, model.mask##N##_1, model.q##N)
    LANE(0, 1); LANE(1, 5); LANE(2, 5); LANE(3, 8); LANE(4, 13);
    LANE(5, 13); LANE(6, 32); LANE(7, 32); LANE(8, 64); LANE(9, 64);
#undef LANE
    std::cout << "Memory staging: 10 widths/signs, no-op pending elision, ordered ports, cancellation, legacy shadow and 153630 evals passed\n";
}
