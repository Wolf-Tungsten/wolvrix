#include "transform/repcut_assignment_model.hpp"
#include "transform/repcut_boundary_bundle.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>

using namespace wolvrix::lib::transform;

namespace
{
    void check(bool condition, const char *message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    template <class Exception = std::invalid_argument, class F> void rejects(F action)
    {
        try { action(); }
        catch (const Exception &) { return; }
        throw std::runtime_error("invalid model or arithmetic was accepted");
    }

    RepcutAssignmentModel diamond()
    {
        return {"fixture-v1", 3, {"domain_a.rise", "domain_b.fall", "unknown"},
                {{{0, 1, 1}, {10, 0, 4}}, {{0}, {3, 2, 0}}, {{1}, {5, 7, 1}},
                 {{2}, {2, 9, 3}}, {{}, {1000, 1000, 1000}}},
                {{11, 0, 8, {0, 1, 1, 2}}, {12, 0, 9, {1, 2}}, {13, 2, 65, {0}}}};
    }

    void checkBundleOracle(const RepcutAssignmentEvaluator &evaluator,
                           const std::vector<uint32_t> &assignment,
                           const RepcutAssignmentReport &report)
    {
        std::vector<detail::RepcutBoundaryValueDesc> values;
        for (const auto &v : evaluator.model().values)
        {
            std::set<uint32_t> readers;
            const auto owner = assignment[v.writerAtom];
            for (const auto atom : v.readerAtoms)
                if (assignment[atom] != owner) readers.insert(assignment[atom]);
            values.push_back({v.key, owner, static_cast<int64_t>(v.width),
                              {readers.begin(), readers.end()}});
        }
        const auto expected = detail::buildRepcutBoundaryBundles(values);
        std::size_t count = 0;
        for (const auto &group : report.transfers)
        {
            if (group.members.size() == 1) continue;
            ++count;
            for (const auto &member : group.members)
            {
                const auto &position = expected.members.at(member.key);
                const auto &bundle = expected.groups.at(position.groupIndex);
                check(bundle.ownerPart == group.ownerPart && bundle.dstParts == group.readerParts &&
                      static_cast<uint64_t>(bundle.totalWidth) == group.width &&
                      static_cast<uint64_t>(position.sliceStart) == member.lowBit &&
                      static_cast<uint64_t>(position.sliceEnd + 1) == member.lowBit + member.width,
                      "assignment bundle must match production boundary helper");
            }
        }
        check(count == expected.groups.size(), "production bundle count differs");
    }

    void widths()
    {
        const std::vector<uint64_t> widths{1, 8, 9, 16, 17, 32, 33, 64, 65, 448, 449, 512, 513};
        const std::vector<uint64_t> bytes {1, 1, 2,  2,  4,  4,  8,  8, 16,  56,  64,  64,  72};
        for (std::size_t i = 0; i < widths.size(); ++i)
        {
            const auto f = grhsimTransferFeaturesV1(widths[i]);
            check(f.calls == 1 && f.storageBytes == bytes[i], "GrhSIM native storage rounding");
            check(f.scalarCalls == (widths[i] <= 64) && f.wideCalls == (widths[i] > 64),
                  "GrhSIM scalar/wide boundary");
            check(f.full64ByteBlocks == (widths[i] <= 64 ? 0 : bytes[i] / 64), "full copy groups");
            check(f.tailBytes == (widths[i] <= 64 ? 0 : bytes[i] % 64), "partial copy groups");
        }
        rejects([&] { grhsimTransferFeaturesV1(0); });
    }

    void unionAndMoves()
    {
        RepcutAssignmentEvaluator evaluator(diamond());
        RepcutAssignmentState state(evaluator, {0, 0, 1}, 3);
        check(state.report().parts[0].features == std::vector<uint64_t>({18, 9, 5}), "diamond deduplication");
        check(state.report().uniqueFeatures == std::vector<uint64_t>({20, 18, 8}), "ignore unreferenced work");
        check(state.report().totalFeatures == state.report().uniqueFeatures, "initial work has no replication");
        check(state.report().transfers.size() == 2 && state.report().parts[0].sent.storageBytes == 4 &&
              state.report().parts[1].sent.storageBytes == 16, "bundle rounding and local reader suppression");
        checkBundleOracle(evaluator, state.assignment(), state.report());
        state.moveAtom(1, 1);
        check(state.report().totalFeatures == std::vector<uint64_t>({30, 18, 12}), "first replica has full work");
        state.moveAtom(0, 1);
        check(state.report().totalFeatures == state.report().uniqueFeatures && state.report().transfers.empty(),
              "last source reference removes work and all-local transfer");
        state.moveAtom(2, 2);
        check(state.report() == evaluator.evaluate(state.assignment(), 3), "incremental move equals full rebuild");
        const auto before = state.report();
        state.moveAtom(2, 2);
        check(state.report() == before, "same-part move is a no-op");
        rejects([&] { state.moveAtom(3, 0); });
        rejects([&] { state.moveAtom(0, 3); });
        check(state.report() == before, "failed moves preserve state");
    }

    void senderTail()
    {
        RepcutAssignmentModel model{"fixture-v1", 4, {"work"}, {},
                                    {{1, 0, 512, {2}}, {2, 1, 512, {3}}}};
        RepcutAssignmentEvaluator evaluator(model);
        const auto a = evaluator.evaluate(std::vector<uint32_t>{0, 0, 2, 3}, 4);
        const auto b = evaluator.evaluate(std::vector<uint32_t>{0, 1, 2, 3}, 4);
        check(a.totalTransfers == b.totalTransfers, "total communication is identical");
        check(a.parts[0].sent.storageBytes == 128 && b.parts[0].sent.storageBytes == 64 &&
              b.parts[1].sent.storageBytes == 64, "sender bottleneck differs with equal total volume");
    }

    void backendAdapter()
    {
        RepcutAssignmentModel model{"fixture-v1", 2, {"work"}, {{{0, 1}, {3}}}, {{1, 0, 1, {1}}}};
        const auto assignment = std::vector<uint32_t>{0, 1};
        RepcutAssignmentEvaluator ordinary(model);
        RepcutAssignmentEvaluator alternate(model, {"fixture-eight-byte-slot-v1", [](uint64_t width) {
            auto f = grhsimTransferFeaturesV1(width);
            if (width <= 64) f.storageBytes = 8;
            return f;
        }});
        const auto a = ordinary.evaluate(assignment, 2), b = alternate.evaluate(assignment, 2);
        check(a.totalFeatures == b.totalFeatures && a.totalTransfers.storageBytes == 1 &&
              b.totalTransfers.storageBytes == 8, "transfer adapter must be independent of compute features");
        rejects([&] { RepcutAssignmentEvaluator bad(model, {"missing-function", {}}); });
    }

    void inferredOwner()
    {
        RepcutAssignmentModel model{"fixture-read-only-v1", 4, {"work"}, {},
            {{1, kRepcutNoAtom, 32, {0, 1}, 0}, {2, kRepcutNoAtom, 1, {2, 3}, 0}},
            {{{{0, 1, 1}, {2}, {2}}}}};
        RepcutAssignmentEvaluator e(model);
        RepcutAssignmentState state(e, {0, 0, 1, 2}, 3);
        for (const auto &t : state.report().transfers)
            check(t.ownerPart == 1, "read-only owner counts users, not replicated atoms");
        state.moveAtom(2, 0);
        for (const auto &t : state.report().transfers)
            check(t.ownerPart == 0, "read-only owner must follow whole-storage user votes after a move");
        check(state.report() == e.evaluate(state.assignment(), 3), "inferred ownership incremental parity");
        model.inferredOwners[0].userAtoms = {{0}, {1}};
        const auto tied = RepcutAssignmentEvaluator(model).evaluate(std::vector<uint32_t>{2, 1, 0, 0}, 3);
        for (const auto &t : tied.transfers) check(t.ownerPart == 1, "read-only owner tie break");
        model.values[0].writerAtom = 0;
        rejects([&] { RepcutAssignmentEvaluator bad(model); });
    }

    void directImpact()
    {
        // Moving an otherwise local reader splits the sender's packed output
        // and changes a third partition's input layout. An unrelated link stays out.
        RepcutAssignmentEvaluator split({"impact-v1", 5, {"work"}, {},
            {{11, 0, 8, {1, 2}}, {12, 0, 8, {2}}, {13, 4, 8, {3}}}});
        const std::vector<uint32_t> before{0, 0, 2, 3, 4}, after{0, 1, 2, 3, 4};
        const auto old = split.evaluate(before, 5), next = split.evaluate(after, 5);
        check(repcutAssignmentDirectImpact(before, old, after, next) == std::vector<uint32_t>({0, 1, 2}),
              "split bundle impact must include an unmoved consumer, exclude unrelated links");
        auto reordered = old; std::reverse(reordered.transfers.begin(), reordered.transfers.end());
        check(repcutAssignmentDirectImpact(before, old, before, reordered).empty(),
              "unchanged assignment or transfer order must have no direct impact");

        // Same peers, byte counts and call counts, but different payload members.
        RepcutAssignmentEvaluator swap({"impact-v1", 3, {"work"}, {},
            {{21, 0, 8, {2}}, {22, 1, 8, {2}}}});
        const std::vector<uint32_t> a{0, 1, 2}, b{1, 0, 2};
        const auto ar = swap.evaluate(a, 3), br = swap.evaluate(b, 3);
        check(ar.parts == br.parts && ar.totalTransfers == br.totalTransfers, "equal-total impact fixture");
        check(repcutAssignmentDirectImpact(a, ar, b, br) == std::vector<uint32_t>({0, 1, 2}),
              "equal-byte payload replacement still invalidates the receiver");
        RepcutAssignmentEvaluator local({"impact-v1", 2, {"work"}, {}, {}});
        const std::vector<uint32_t> x{0, 0}, y{0, 1};
        check(repcutAssignmentDirectImpact(x, local.evaluate(x, 3), y, local.evaluate(y, 3)) ==
                  std::vector<uint32_t>({0, 1}), "local-only ASC moves still rebuild both endpoints");
        auto bad = old; bad.parts.pop_back();
        rejects([&] { repcutAssignmentDirectImpact(before, old, after, bad); });
        bad = old; bad.transfers.front().readerParts = {5};
        rejects([&] { repcutAssignmentDirectImpact(before, old, after, bad); });
        bad = old; bad.transfers.push_back(bad.transfers.front());
        rejects([&] { repcutAssignmentDirectImpact(before, old, after, bad); });
        rejects([&] { repcutAssignmentDirectImpact(std::vector<uint32_t>{0}, old, after, next); });
    }

    void randomized()
    {
        std::mt19937 rng(907);
        RepcutAssignmentModel model{"random-fixture-v1", 40, {"a.rise", "a.fall", "b.rise", "unknown"}, {}, {}};
        for (unsigned i = 0; i < 120; ++i)
        {
            RepcutWorkPiece p;
            for (unsigned j = 0, n = rng() % 12; j < n; ++j) p.atoms.push_back(rng() % model.atomCount);
            for (unsigned j = 0; j < 4; ++j) p.features.push_back(rng() % 300);
            model.pieces.push_back(std::move(p));
        }
        for (unsigned i = 0; i < 100; ++i)
        {
            RepcutDirectedValue v{i, static_cast<uint32_t>(rng() % model.atomCount), 1 + rng() % 2048, {}};
            for (unsigned j = 0, n = rng() % 10; j < n; ++j) v.readerAtoms.push_back(rng() % model.atomCount);
            model.values.push_back(std::move(v));
        }
        RepcutAssignmentEvaluator evaluator(model);
        std::vector<uint32_t> assignment(model.atomCount);
        for (auto &part : assignment) part = rng() % 7;
        RepcutAssignmentState state(evaluator, assignment, 7);
        for (unsigned trial = 0; trial < 256; ++trial)
        {
            state.moveAtom(rng() % model.atomCount, rng() % 7);
            const auto rebuilt = evaluator.evaluate(state.assignment(), 7);
            check(state.report() == rebuilt, "random incremental/full report mismatch");
            checkBundleOracle(evaluator, state.assignment(), rebuilt);
            // A separate per-part set-union oracle, without using evaluator occupancy.
            for (uint32_t part = 0; part < 7; ++part)
            {
                std::vector<uint64_t> features(4);
                for (const auto &p : model.pieces)
                    if (std::any_of(p.atoms.begin(), p.atoms.end(), [&](auto atom) {
                            return state.assignment()[atom] == part; }))
                        for (unsigned f = 0; f < 4; ++f) features[f] += p.features[f];
                check(features == rebuilt.parts[part].features, "set-union reference mismatch");
            }
        }
        std::reverse(model.values.begin(), model.values.end());
        for (auto &v : model.values) std::reverse(v.readerAtoms.begin(), v.readerAtoms.end());
        check(RepcutAssignmentEvaluator(model).evaluate(state.assignment(), 7) == state.report(),
              "input ordering changes deterministic report");
    }

    void invalidAndOverflow()
    {
        auto m = diamond();
        m.pieces[0].atoms.push_back(m.atomCount);
        rejects([&] { RepcutAssignmentEvaluator e(m); });
        m = diamond(); m.values[0].readerAtoms.push_back(m.atomCount);
        rejects([&] { RepcutAssignmentEvaluator e(m); });
        m = diamond(); m.values[0].writerAtom = m.atomCount;
        rejects([&] { RepcutAssignmentEvaluator e(m); });
        m = diamond(); m.values[0].width = 0;
        rejects([&] { RepcutAssignmentEvaluator e(m); });
        m = diamond(); m.values.push_back(m.values[0]);
        rejects([&] { RepcutAssignmentEvaluator e(m); });
        m = diamond(); m.pieces[0].features.pop_back();
        rejects([&] { RepcutAssignmentEvaluator e(m); });
        RepcutAssignmentEvaluator e(diamond());
        rejects([&] { e.evaluate(std::vector<uint32_t>{0, 1}, 2); });
        rejects([&] { e.evaluate(std::vector<uint32_t>{0, 1, 2}, 2); });
        rejects([&] { e.evaluate(std::vector<uint32_t>{0, 0, 0}, 0); });

        const auto large = std::numeric_limits<uint64_t>::max();
        RepcutAssignmentEvaluator overflow({"overflow-v1", 2, {"work"}, {{{0, 1}, {large}}}, {}});
        RepcutAssignmentState state(overflow, {0, 0}, 2);
        const auto before = state.report();
        rejects<std::overflow_error>([&] { state.moveAtom(1, 1); });
        check(state.report() == before && state.assignment() == std::vector<uint32_t>({0, 0}),
              "overflow must leave both report and occupancy unchanged");
        state.moveAtom(0, 0);
        RepcutAssignmentEvaluator huge({"overflow-v1", 2, {"work"}, {},
            {{1, 0, large / 2, {1}}, {2, 0, 1, {1}}}});
        rejects<std::overflow_error>([&] { huge.evaluate(std::vector<uint32_t>{0, 1}, 2); });
    }
}

int main()
{
    try
    {
        widths(); unionAndMoves(); senderTail(); backendAdapter(); inferredOwner(); directImpact(); randomized(); invalidAndOverflow();
        std::cout << "PASS: union occupancy, 256 moves, directed bundles, sender tails, widths, overflow\n";
    }
    catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
