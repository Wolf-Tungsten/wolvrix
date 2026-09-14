#include "grhsim_cpu_wide.hpp"
#include "Vcpu_wide.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>

template<std::size_t N, class Reference>
void check(const std::array<std::uint64_t, N> &actual, const Reference &expected, std::size_t width)
{
    for (std::size_t bit = 0; bit < width; ++bit)
        if (((actual[bit / 64] >> (bit % 64)) & 1u) != ((expected[bit / 32] >> (bit % 32)) & 1u))
            throw std::runtime_error("wide CPU/Verilator mismatch at bit " + std::to_string(bit));
    if (width % 64 && actual.back() >> (width % 64))
        throw std::runtime_error("nonzero padding bits");
}

int main()
{
    GrhSIM_cpu_wide model; Vcpu_wide reference; model.init();
    const std::array<std::uint64_t, 2> padded{1, ~UINT64_C(1)}, clean{1, 0};
    if (grhsim_compare_extended_words(padded.data(), 2, 65, clean.data(), 2, 65, false) != 0)
        throw std::runtime_error("comparison included padding bits");
    const std::array<std::uint64_t, 1> replicateSource{{1}};
    std::array<std::uint64_t, 3> replicateResult{};
    if (!cpu_replicate_words_changed<3>(replicateSource, 1, 137, 137, replicateResult) ||
        replicateResult != std::array<std::uint64_t, 3>{UINT64_MAX, UINT64_MAX, 0x1ff})
        throw std::runtime_error("caller-owned replication produced the wrong first result");
    if (cpu_replicate_words_changed<3>(replicateSource, 1, 137, 137, replicateResult))
        throw std::runtime_error("caller-owned replication reported a stable result as changed");
    std::mt19937_64 random(197);
    const std::array<std::uint64_t, 12> shifts{0, 1, 27, 28, 63, 64, 65, 127, 447, 448, 449, UINT64_MAX};
    for (unsigned sample = 0; sample < 256; ++sample)
    {
        model.narrow = reference.narrow = random() & 0xfffffff;
        model.bit_in = reference.bit_in = sample & 1;
        model.wide = {random(), random(), random() & 1};
        if (sample < 4) model.wide = {sample == 0 ? 0u : sample == 1 ? 254u : sample == 2 ? 255u : 256u, 0, 0};
        model.other = sample % 3 ? std::array<std::uint64_t, 3>{random(), random(), random() & 1} : model.wide;
        model.signed_short = {random(), random() & 7};
        model.signed_wide = model.wide;
        if (sample % 3 == 0)
        {
            const bool negative = (model.signed_short[1] & 4) != 0;
            model.signed_wide = {model.signed_short[0], model.signed_short[1] | (negative ? ~UINT64_C(7) : 0), negative ? 1u : 0u};
        }
        model.signed_byte = static_cast<std::int8_t>(random());
        if (sample < 4) model.signed_byte = -1;
        reference.signed_byte = static_cast<std::uint8_t>(model.signed_byte);
        for (unsigned i = 0; i < 5; ++i)
        {
            reference.wide[i] = static_cast<std::uint32_t>(model.wide[i / 2] >> (32 * (i % 2)));
            reference.other[i] = static_cast<std::uint32_t>(model.other[i / 2] >> (32 * (i % 2)));
            reference.signed_wide[i] = static_cast<std::uint32_t>(model.signed_wide[i / 2] >> (32 * (i % 2)));
        }
        for (unsigned i = 0; i < 3; ++i)
            reference.signed_short[i] = static_cast<std::uint32_t>(model.signed_short[i / 2] >> (32 * (i % 2)));
        for (const auto shift : shifts)
        {
            model.shift = reference.shift = shift;
            model.wide_shift.fill(0); model.wide_shift[0] = shift;
            if (sample % 3 == 1) model.wide_shift[1] = 1;
            if (sample % 3 == 2) model.wide_shift[7] = UINT64_C(1) << 63;
            for (unsigned i = 0; i < 16; ++i)
                reference.wide_shift[i] = static_cast<std::uint32_t>(model.wide_shift[i / 2] >> (32 * (i % 2)));
            model.eval(); reference.eval();
            check(model.deep, reference.deep, 1024);
            check(model.mixed, reference.mixed, 185);
            check(model.shifted, reference.shifted, 448);
            check(model.sum, reference.sum, 129);
            check(model.repeated, reference.repeated, 137);
            if (model.parity != reference.parity) throw std::runtime_error("wide parity mismatch");
            if (model.mixed_parity != reference.mixed_parity) throw std::runtime_error("concat fanout mismatch");
            check(model.wide_shl, reference.wide_shl, 129);
            check(model.wide_lshr, reference.wide_lshr, 129);
            check(model.wide_ashr, reference.wide_ashr, 129);
#define CHECK(name) if (model.name != reference.name) throw std::runtime_error(#name " mismatch");
            CHECK(cmp_eq) CHECK(cmp_ne) CHECK(cmp_lt) CHECK(cmp_le) CHECK(cmp_gt) CHECK(cmp_ge)
            CHECK(scmp_eq) CHECK(scmp_ne) CHECK(scmp_lt) CHECK(scmp_le) CHECK(scmp_gt) CHECK(scmp_ge)
            CHECK(mixed_lt) CHECK(scalar_signed_lt) CHECK(scalar_shl)
#undef CHECK
            model.eval();
            check(model.mixed, reference.mixed, 185);
        }
    }
    std::cout << "wide emitted CPU / Verilator PASS samples=3072\n";
}
