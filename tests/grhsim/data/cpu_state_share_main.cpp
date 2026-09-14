#include "grhsim_cpu_state_share.hpp"

#include <iostream>
#include <random>

template<class T> void sample(const T &value) {
    std::cout.write(reinterpret_cast<const char *>(&value), sizeof(value));
}

int main() {
    GrhSIM_cpu_state_share model;
    std::mt19937_64 random(518231);
    for (unsigned reset = 0; reset < 4; ++reset) {
        model.clock = false; model.aux = false; model.enable = false;
        model.init();
        const auto eval = [&] {
            model.eval();
#define Q(L, S) sample(model.q##L##_##S##_0); sample(model.q##L##_##S##_1)
            Q(0, 0); Q(0, 1); Q(1, 0); Q(1, 1); Q(2, 0); Q(2, 1); Q(3, 0); Q(3, 1);
#undef Q
#define N(I) sample(model.negative##I##_0); sample(model.negative##I##_1)
            N(0); N(1); N(2); N(3); N(4); N(5); N(6); N(7); sample(model.observed_history);
#undef N
        };
        eval();
        for (unsigned step = 0; step < 8192; ++step) {
            if (step % 9 < 4) {
                model.enable = random() & 1;
                model.data0 = random() & 1; model.mask0 = random() & 1;
                model.data1 = static_cast<std::int8_t>(random()); model.mask1 = static_cast<std::int8_t>(random());
                model.data2 = random(); model.mask2 = random();
                for (unsigned word = 0; word < 3; ++word) { model.data3[word] = random(); model.mask3[word] = random(); }
                model.negative_data = random(); model.negative_mask = random();
#define EN(I) model.negative_enable##I = random() & 1
                EN(0); EN(1); EN(2); EN(3); EN(4); EN(5); EN(6); EN(7);
#undef EN
            }
            if (step % 8 == 0) { model.mask0 = false; model.mask1 = 0; model.mask2 = 0; model.mask3.fill(0); }
            if (step % 8 == 1) { model.mask0 = true; model.mask1 = -1; model.mask2 = UINT64_MAX; model.mask3.fill(UINT64_MAX); }
            if (step % 3 == 0) model.clock = !model.clock;
            if (step % 5 == 0) model.aux = !model.aux;
            eval();
        }
    }
}
