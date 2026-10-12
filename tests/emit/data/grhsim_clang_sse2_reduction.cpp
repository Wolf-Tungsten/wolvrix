#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

// Standalone reduction of LLVM 21.1.5 x86/SSE2 wrong-code. Preserve this
// layout: changing load alignment changes SLP's shuffle selection.
struct alignas(64) ReductionModel {
    unsigned char pad0[4744]{};
    std::array<std::uint64_t, 10> bits{};
    unsigned char pad1[16104]{};
    std::array<std::uint8_t, 1024> active{};
    unsigned char pad2[6182]{};
    std::array<bool, 18877> modes{};
    std::array<std::uint8_t, 9455> values{};
    void evaluate();
};
static_assert(offsetof(ReductionModel, bits) == 4744);
static_assert(offsetof(ReductionModel, modes) == 28134);
static_assert(offsetof(ReductionModel, values) == 47011);

#ifndef GRHSIM_REDUCTION_MAIN
void ReductionModel::evaluate() {
    const bool bit0 = (bits[7] >> 2) & 1;
    const bool bit1 = (bits[7] >> 1) & 1;
    const std::uint8_t a = ((values[7515] & 7) +
        (((values[7502] & 3) + ((values[7493] + bit0) & 3)) & 7)) & 15;
    const std::uint8_t b = ((values[7516] & 7) +
        (((values[7503] & 3) + ((values[7494] + bit1) & 3)) & 7)) & 15;
    const auto term = [](bool mode, std::uint8_t v) {
        return mode ? v > 2 : (v & 15) != 0;
    };
    const bool result =
        term(modes[13086], a) |
        term(modes[13087], b) |
        term(modes[13074], values[6238]) |
        term(modes[13088], values[7506]) |
        term(modes[13075], values[7507]) |
        term(modes[13076], values[7508]) |
        term(modes[13089], values[7509]) |
        term(modes[13077], values[7510]) |
        term(modes[13090], values[7511]) |
        term(modes[13091], values[7512]) |
        term(modes[13092], values[7513]) |
        term(modes[13093], values[3363]) |
        term(modes[13078], values[3364]) |
        term(modes[13094], values[3365]) |
        term(modes[13095], values[3366]) |
        term(modes[13079], values[3367]) |
        term(modes[13080], values[7514]) |
        term(modes[13081], values[3368]) |
        term(modes[13082], values[3369]);
    const bool changed = modes[17734] != !result;
    active[120] |= static_cast<std::uint8_t>(-static_cast<std::uint8_t>(changed)) & 128;
    active[150] |= static_cast<std::uint8_t>(-static_cast<std::uint8_t>(changed)) & 128;
    modes[17734] = !result;
}
#else
int main() {
    ReductionModel model{};
    model.modes.fill(true);
    model.modes[17734] = false;
    model.evaluate();
    if (!model.modes[17734] || model.active[120] != 128 || model.active[150] != 128) {
        std::fprintf(stderr, "zero inputs must reduce to false before inversion\n");
        return 1;
    }
    model.values[7508] = 3;
    model.evaluate();
    if (model.modes[17734]) return 2;
    model.values[7508] = 1;
    model.modes[13076] = false;
    model.evaluate();
    return model.modes[17734] ? 3 : 0;
}
#endif
