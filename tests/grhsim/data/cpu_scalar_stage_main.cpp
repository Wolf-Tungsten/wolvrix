#include "grhsim_cpu_scalar_stage.hpp"
#include "scalar_stage_slots.hpp"

#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class T>
void testLane(GrhSIM_cpu_scalar_stage &model, unsigned lane, unsigned width,
              T &data0, T &data1, T &mask0, T &mask1, const T &output, const bool &history)
{
    model.init();
    const auto [state, offset] = scalar_stage_slots[lane];
    const auto write = [&](T value) { model.cpu_write_scalar<T>(model.cpu_objects.get(), model.cpu_shadow.get(), state, offset, 0, 0, true, value); };
    const auto visible = [&] { return cpu_at<T>(model.cpu_objects.get(), offset); };
    write(T{0});
    require(model.cpu_pending.empty() && !model.cpu_dirty[state], "clean no-op created a pending write");
    write(T{1}); write(T{1});
    require(model.cpu_pending.size() == 1 && model.cpu_dirty[state] && visible() == T{0},
            "dirty no-op duplicated a record or published early");
    write(T{0});
    require(model.cpu_pending.size() == 1 && !model.cpu_publish() && visible() == T{0},
            "write back to visible value lost cancellation semantics");
    require(model.cpu_pending.empty() && !model.cpu_dirty[state], "publication left dirty state");
    // Interoperate with a previously staged value, as required for shared shadow storage.
    model.cpu_stage<T>(model.cpu_objects.get(), model.cpu_shadow.get(), state, offset, 0, 0, true) = T{1}; write(T{0});
    require(!model.cpu_publish() && visible() == T{0}, "full write ignored an existing shadow");
    write(T{1});
    require(model.cpu_publish() && visible() == T{1}, "changed scalar failed to publish projection");
    write(T{1});
    require(model.cpu_pending.empty(), "unchanged nonzero visible value created pending work");

    model.init(); model.clock = false; model.enable = true;
    model.eval();
    const std::uint64_t bits = width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
    std::mt19937_64 random(90173 + lane);
    std::uint64_t expected = 0;
    for (unsigned sample = 0; sample < 1024; ++sample) {
        model.clock = false; model.eval();
        require(!history, "falling edge failed to sample observed history");
        data0 = static_cast<T>(random()); data1 = static_cast<T>(random());
        mask0 = static_cast<T>(random()); mask1 = static_cast<T>(random());
        switch (sample % 8) {
        case 0: mask0 = T{0}; mask1 = T{0}; break;
        case 1: data0 = static_cast<T>(expected); data1 = data0; break;
        case 2: // Full first write followed by restoration of the pre-edge value.
            mask0 = static_cast<T>(bits); mask1 = mask0; data1 = static_cast<T>(expected); break;
        case 3: // Disjoint masks must merge using the first writer's shadow.
            mask0 = static_cast<T>(bits & UINT64_C(0x5555555555555555));
            mask1 = static_cast<T>(bits & UINT64_C(0xaaaaaaaaaaaaaaaa)); break;
        default: break;
        }
        model.enable = sample % 11 != 0;
        if (model.enable) {
            for (auto pair : {std::pair{data0, mask0}, std::pair{data1, mask1}}) {
                const auto mask = static_cast<std::uint64_t>(pair.second) & bits;
                expected = ((expected & ~mask) | (static_cast<std::uint64_t>(pair.first) & mask)) & bits;
            }
        }
        model.clock = true; model.eval();
        require((static_cast<std::uint64_t>(output) & bits) == expected && history,
                "ordered scalar masked write or edge sampling mismatch");
        // Changing data at a stable high clock must not create a second edge.
        data0 = static_cast<T>(random()); data1 = static_cast<T>(random()); model.eval();
        require((static_cast<std::uint64_t>(output) & bits) == expected && history,
                "stable clock repeated a write");
    }
}

void testPhaseProfile()
{
    GrhSIM_cpu_scalar_stage plain, profiled;
    plain.init(); profiled.init();
    profiled.set_runtime_profile_enabled(true);
    std::mt19937 random(83115);
    for (unsigned sample = 0; sample < 2048; ++sample) {
        const bool clock = (random() & 1) != 0, enable = (random() & 3) != 0;
        plain.clock = profiled.clock = clock;
        plain.enable = profiled.enable = enable;
        plain.data3_0 = profiled.data3_0 = random();
        plain.data3_1 = profiled.data3_1 = random();
        plain.mask3_0 = profiled.mask3_0 = random();
        plain.mask3_1 = profiled.mask3_1 = random();
        plain.eval(); profiled.eval();
#define SAME_LANE(N) require(plain.q##N == profiled.q##N && plain.history##N == profiled.history##N, \
                            "phase profiling changed model behavior")
        SAME_LANE(0); SAME_LANE(1); SAME_LANE(2); SAME_LANE(3); SAME_LANE(4);
        SAME_LANE(5); SAME_LANE(6); SAME_LANE(7); SAME_LANE(8); SAME_LANE(9);
#undef SAME_LANE
    }
    const auto measured = profiled.cpu_runtime_profile();
    require(measured.evals == 2048 && measured.rounds >= measured.evals,
            "phase profiling counted evals or convergence rounds incorrectly");
    require(measured.compute_ns > 0 && measured.commit_ns > 0 && measured.publish_ns > 0,
            "phase profiling omitted a nonempty phase");
    require(measured.eval_ns >= measured.compute_ns + measured.commit_ns + measured.publish_ns,
            "phase profiling overlaps timing intervals");
    const auto disabled = plain.cpu_runtime_profile();
    require(disabled.evals == 0 && disabled.rounds == 0 && disabled.eval_ns == 0 &&
            disabled.compute_ns == 0 && disabled.commit_ns == 0 && disabled.publish_ns == 0,
            "disabled profiling changed counters");
    profiled.dump_runtime_profile();
    profiled.set_runtime_profile_enabled(false);
    for (unsigned i = 0; i < 16; ++i) { profiled.clock = !profiled.clock; profiled.eval(); }
    const auto paused = profiled.cpu_runtime_profile();
    require(paused.evals == measured.evals && paused.rounds == measured.rounds &&
            paused.eval_ns == measured.eval_ns && paused.compute_ns == measured.compute_ns &&
            paused.commit_ns == measured.commit_ns && paused.publish_ns == measured.publish_ns,
            "paused profiling did not preserve counters");
    profiled.set_runtime_profile_enabled(true);
    require(profiled.cpu_runtime_profile().evals == 0 && profiled.cpu_runtime_profile().eval_ns == 0,
            "enabling profiling did not start a fresh measurement");
    profiled.eval();
    require(profiled.cpu_runtime_profile().evals == 1, "re-enabled profiling did not record eval");
    profiled.init();
    const auto reset = profiled.cpu_runtime_profile();
    require(reset.evals == 0 && reset.rounds == 0 && reset.eval_ns == 0 &&
            reset.compute_ns == 0 && reset.commit_ns == 0 && reset.publish_ns == 0,
            "model init did not clear profiling counters");
    profiled.eval();
    require(profiled.cpu_runtime_profile().evals == 1, "model init lost profiling enable state");
    std::cout << "phase profile PASS replay_evals=2048 pause_evals=16\n";
}

int main()
{
    GrhSIM_cpu_scalar_stage model;
#define CHECK_LANE(N, W) testLane(model, N, W, model.data##N##_0, model.data##N##_1, \
                                model.mask##N##_0, model.mask##N##_1, model.q##N, model.history##N)
    CHECK_LANE(0, 1); CHECK_LANE(1, 5); CHECK_LANE(2, 5); CHECK_LANE(3, 8);
    CHECK_LANE(4, 13); CHECK_LANE(5, 13); CHECK_LANE(6, 32); CHECK_LANE(7, 32);
    CHECK_LANE(8, 64); CHECK_LANE(9, 64);
#undef CHECK_LANE
    std::cout << "scalar staging PASS lanes=10 edges=10240\n";
    testPhaseProfile();
}
