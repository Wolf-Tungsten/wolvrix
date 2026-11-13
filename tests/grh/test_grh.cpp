#include "core/store.hpp"
#include "core/grh.hpp"
#include "core/detail/block_executor.hpp"
#include "core/detail/shared_column.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace wolvrix::lib::grh;
using namespace wolvrix::lib::store;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[grh_tests] " << message << '\n';
        return 1;
    }

    template <typename Func>
    bool expectThrows(Func &&func)
    {
        try
        {
            func();
        }
        catch (const std::exception &)
        {
            return true;
        }
        return false;
    }

    bool sameSrcLoc(const std::optional<SrcLoc>& actual, const SrcLoc& expected)
    {
        return actual && actual->file == expected.file && actual->line == expected.line &&
            actual->column == expected.column && actual->endLine == expected.endLine &&
            actual->endColumn == expected.endColumn && actual->origin == expected.origin &&
            actual->pass == expected.pass && actual->note == expected.note;
    }

    int mutationCheckpoint()
    {
        Design design;
        auto& graph = design.createGraph("checkpoint");
        design.markAsTop("checkpoint");
        Graph* const originalGraph = &graph;
        const auto graphId = graph.id();
        const auto input = graph.createValue(graph.internSymbol("input"), 8, false);
        const auto holeValue = graph.createValue(graph.internSymbol("hole"), 8, false);
        const auto output = graph.createValue(graph.internSymbol("output"), 8, false);
        const auto holeOp = graph.createOperation(OperationKind::kConstant, graph.internSymbol("hole_op"));
        const auto operation = graph.createOperation(OperationKind::kNot, graph.internSymbol("not"));
        graph.addOperand(operation, input);
        graph.addResult(operation, output);
        if (!graph.eraseValue(holeValue) || !graph.eraseOp(holeOp))
            return fail("checkpoint fixture could not create ID holes");
        graph.bindInputPort("in", input);
        graph.bindOutputPort("out", output);
        graph.setMetadata("checkpoint", std::string("before"));
        graph.setAttr(operation, "strings", std::vector<std::string>{"original", "shared"});
        graph.setAttr(operation, "numbers", std::vector<int64_t>{1, 2, 3});
        graph.assignWriterOrder(operation, "state");
        const SrcLoc location{"checkpoint.fir", 17, 2, 17, 9, "firrtl", "ingest", "original"};
        graph.setOpAndValueSrcLocView(operation, output, SrcLocView(location));
        auto& tree = graph.getOrCreateSemanticTree("checkpoint.fir", "Checkpoint", "checkpoint");
        const auto path = tree.addPath(tree.root(), "child");
        const auto node = tree.addNode(path, "output");
        const std::array<SemanticId, 1> membership{node};
        graph.setOpSemanticIds(operation, membership);
        graph.setValueSemanticIds(input, membership);
        graph.addDeclaredSymbol(graph.valueSymbol(input));
        const auto scope = graph.internSymbol("generate");
        const auto group = graph.addGenerateGroup(scope, graph.valueSymbol(input));
        graph.addGenerateGroupSymbol(group, graph.valueSymbol(input));
        const auto firstInternalOp = graph.makeInternalOpSym();
        const auto firstInternalValue = graph.makeInternalValSym();
        graph.markLocalCyclesChecked();
        const auto* originalStore = &graph.payloadStore();
        const auto originalStats = graph.payloadStats();
        StoreDiagnostics diagnostics;
        StoreJson store(&diagnostics);
        const auto before = store.storeToString(design);
        if (!before || diagnostics.hasError()) return fail("checkpoint fixture did not serialize");
        {
            auto checkpoint = graph.checkpointMutation();
            if (&graph.payloadStore() == originalStore || graph.findValue("output") != output ||
                graph.findOperation("not") != operation || graph.lookupSymbol("_op_0") != firstInternalOp ||
                graph.lookupSymbol("_val_0") != firstInternalValue ||
                !expectThrows([&] { (void)graph.valueWidth(holeValue); }) ||
                !expectThrows([&] { (void)graph.opKind(holeOp); }))
                return fail("checkpoint work copy changed IDs, holes or symbol reservations");
            graph.semanticTree()->flatten();
            graph.setMetadata("checkpoint", std::string("partial"));
            graph.setAttr(operation, "numbers", std::vector<int64_t>{9, 10});
            graph.setAttr(operation, "strings", std::vector<std::string>{"partial-only"});
            graph.setOpSrcLoc(operation, SrcLoc{"partial-only.fir", 99});
            const auto partial = graph.createValue(graph.internSymbol("partial-only"), 8, false);
            graph.replaceOperand(operation, 0, partial);
            graph.clearDeclaredSymbols();
            graph.makeInternalOpSym();
            graph.makeInternalValSym();
            const auto writer = graph.createOperation(OperationKind::kNot, graph.internSymbol("partial_writer"));
            graph.assignWriterOrder(writer, "state");
            if (!graph.removeOutputPort("out") || !graph.eraseOpUnchecked(operation) ||
                !graph.eraseValueUnchecked(output))
                return fail("checkpoint work copy could not exercise erased semantic members");
        }
        const auto after = store.storeToString(design);
        const auto afterStats = graph.payloadStats();
        if (&graph != originalGraph || graph.id() != graphId || &graph.payloadStore() != originalStore ||
            !after || *before != *after || graph.findValue("output") != output ||
            graph.findOperation("not") != operation || graph.lookupSymbol("partial-only").valid() ||
            graph.lookupSymbol("partial-only.fir").valid() ||
            graph.semanticTree()->namesFlattened() || graph.semanticTree()->node(node).parent != path ||
            graph.semanticNodeOperations(node) != std::vector<OperationId>{operation} ||
            graph.semanticNodeAnchors(node) != std::vector<ValueId>{input} ||
            !graph.validateSemanticTree().empty() || !graph.localCyclesChecked() ||
            !sameSrcLoc(graph.opSrcLoc(operation), location) ||
            afterStats.strings != originalStats.strings || afterStats.attributes != originalStats.attributes ||
            afterStats.srcLocs != originalStats.srcLocs || afterStats.int64Payload != originalStats.int64Payload)
            return fail("checkpoint rollback did not restore the exact graph preimage");
        if (graph.symbolText(graph.makeInternalOpSym()) != "_op_1" ||
            graph.symbolText(graph.makeInternalValSym()) != "_val_1")
            return fail("checkpoint rollback did not restore internal name counters");
        const auto writer = graph.createOperation(OperationKind::kNot, graph.internSymbol("writer"));
        graph.assignWriterOrder(writer, "state");
        if (graph.getOperation(writer).attr("writerOrder") != std::optional<AttributeValue>{int64_t{1}})
            return fail("checkpoint rollback did not restore writer order counters");
        const auto* beforeException = &graph.payloadStore();
        try {
            auto checkpoint = graph.checkpointMutation();
            graph.setMetadata("checkpoint", std::string("exception"));
            throw std::runtime_error("late failure");
        } catch (const std::runtime_error&) {
        }
        if (&graph.payloadStore() != beforeException ||
            graph.metadata("checkpoint") != std::optional<AttributeValue>{std::string("before")})
            return fail("exception did not roll back the checkpoint");
        {
            auto checkpoint = graph.checkpointMutation(false);
            if (&graph.payloadStore() != beforeException) return fail("disabled checkpoint copied the graph");
            graph.setMetadata("disabled", true);
        }
        if (graph.metadata("disabled") != std::optional<AttributeValue>{true})
            return fail("disabled checkpoint reverted a mutation");
        {
            auto checkpoint = graph.checkpointMutation();
            graph.setMetadata("checkpoint", std::string("committed"));
            checkpoint.commit();
        }
        if (graph.metadata("checkpoint") != std::optional<AttributeValue>{std::string("committed")} ||
            graph.findValue("output") != output || graph.findOperation("not") != operation ||
            !expectThrows([&] { (void)graph.valueWidth(holeValue); }) ||
            !expectThrows([&] { (void)graph.opKind(holeOp); }))
            return fail("committed checkpoint lost graph mutations or original entity identities");
        return 0;
    }

    int cachedSourceLocationImports()
    {
        const SrcLoc shared{std::string(4096, 'f'), 17, 3, 19, 71,
            "source\norigin", std::string(1024, 'p'), std::string("note\0tail", 9)};
        const SrcLoc discarded{"discarded.fir", 1};
        const SrcLoc appended{shared.file, 23, 5, 27, 81, shared.origin, shared.pass, "appended"};
        const SrcLoc other{"other.fir", 91, 7, 92, 8, "other", "import", "distinct"};
        const SrcLoc seed{"target-only.fir", 2};
        const auto makeValue = [](Graph& graph, std::string_view name) {
            return graph.createValue(graph.internSymbol(name), 8, false);
        };
        const auto makeOp = [](Graph& graph, std::string_view name) {
            return graph.createOperation(OperationKind::kConstant, graph.internSymbol(name));
        };
        Design design;
        Graph& source = design.createGraph("import_source");
        Graph& target = design.createGraph("import_target");
        const auto deadSource = makeValue(source, "discarded");
        source.setValueSrcLoc(deadSource, discarded);
        auto sourceValue = makeValue(source, "value");
        auto sourceOp = makeOp(source, "op");
        source.setOpAndValueSrcLocView(sourceOp, sourceValue, SrcLocView(shared));
        auto missingValue = makeValue(source, "missing_value");
        auto missingOp = makeOp(source, "missing_op");
        const auto deadTarget = makeValue(target, "discarded");
        target.setValueSrcLoc(deadTarget, seed);
        const auto probe = makeValue(target, "probe");
        auto targetValue = makeValue(target, "value");
        auto targetOp = makeOp(target, "op");
        Graph::SrcLocImportContext context;
        target.copyValueSrcLoc(probe, source, deadSource, context);
        target.copyValueSrcLoc(targetValue, source, sourceValue, context);
        target.copyOpSrcLoc(targetOp, source, sourceOp, context);
        const auto initialRecords = target.payloadStats().srcLocs;
        for (int repeat = 0; repeat < 8; ++repeat) {
            target.copyOpSrcLoc(targetOp, source, sourceOp, context);
            target.copyValueSrcLoc(targetValue, source, sourceValue, context);
        }
        if (initialRecords != 3 || target.payloadStats().srcLocs != initialRecords ||
            !sameSrcLoc(target.valueSrcLoc(targetValue), shared) ||
            !sameSrcLoc(target.opSrcLoc(targetOp), shared))
            return fail("shared provenance import context lost value/op fields or duplicated records");

        // Appending source records keeps old mappings usable and grows the
        // cache for new IDs. The new record shares three strings with shared.
        auto appendedValue = makeValue(source, "appended_value");
        auto appendedOp = makeOp(source, "appended_op");
        source.setOpAndValueSrcLocView(appendedOp, appendedValue, SrcLocView(appended));
        target.copyOpSrcLoc(targetOp, source, appendedOp, context);
        target.copyValueSrcLoc(targetValue, source, appendedValue, context);
        if (!sameSrcLoc(target.valueSrcLoc(targetValue), appended) ||
            !sameSrcLoc(target.opSrcLoc(targetOp), appended) || target.payloadStats().srcLocs != initialRecords + 1)
            return fail("provenance import context did not accept appended source records");
        target.copyValueSrcLoc(targetValue, source, sourceValue, context);
        target.copyOpSrcLoc(targetOp, source, sourceOp, context);
        if (!sameSrcLoc(target.valueSrcLoc(targetValue), shared) ||
            !sameSrcLoc(target.opSrcLoc(targetOp), shared) || target.payloadStats().srcLocs != initialRecords + 1)
            return fail("source append invalidated an existing provenance mapping");

        // Removing the first record's only owner makes source compact reuse
        // that record number for different content, exposing stale caches.
        if (!source.eraseValue(deadSource)) return fail("cannot erase source provenance fixture");
        source.compact();
        sourceValue = source.findValue("value");
        sourceOp = source.findOperation("op");
        missingValue = source.findValue("missing_value");
        missingOp = source.findOperation("missing_op");
        target.copyOpSrcLoc(targetOp, source, sourceOp, context);
        target.copyValueSrcLoc(targetValue, source, sourceValue, context);
        if (!sameSrcLoc(target.valueSrcLoc(targetValue), shared) ||
            !sameSrcLoc(target.opSrcLoc(targetOp), shared) || target.payloadStats().srcLocs != initialRecords + 1)
            return fail("source compact reused stale provenance record or string mappings");

        // Target compaction removes every record except shared and renumbers
        // it, independently of the source's unchanged record numbering.
        if (!target.eraseValue(deadTarget) || !target.eraseValue(probe))
            return fail("cannot erase target provenance fixtures");
        target.compact();
        targetValue = target.findValue("value");
        targetOp = target.findOperation("op");
        target.copyValueSrcLoc(targetValue, source, sourceValue, context);
        target.copyOpSrcLoc(targetOp, source, sourceOp, context);
        if (target.payloadStats().srcLocs != 1 || !sameSrcLoc(target.valueSrcLoc(targetValue), shared) ||
            !sameSrcLoc(target.opSrcLoc(targetOp), shared))
            return fail("target compact reused a stale destination provenance mapping");

        Graph& otherSource = design.createGraph("other_source");
        const auto otherValue = makeValue(otherSource, "value");
        const auto otherOp = makeOp(otherSource, "op");
        otherSource.setOpAndValueSrcLocView(otherOp, otherValue, SrcLocView(other));
        Graph& otherTarget = design.createGraph("other_target");
        const auto otherTargetValue = makeValue(otherTarget, "value");
        const auto otherTargetOp = makeOp(otherTarget, "op");
        otherTarget.setOpAndValueSrcLocView(otherTargetOp, otherTargetValue, SrcLocView(seed));
        // Both sources have a record numbered 1; the targets' same-numbered
        // records also differ. Switching either endpoint must reset mappings.
        target.copyValueSrcLoc(targetValue, otherSource, otherValue, context);
        target.copyOpSrcLoc(targetOp, otherSource, otherOp, context);
        otherTarget.copyOpSrcLoc(otherTargetOp, otherSource, otherOp, context);
        otherTarget.copyValueSrcLoc(otherTargetValue, otherSource, otherValue, context);
        if (!sameSrcLoc(target.valueSrcLoc(targetValue), other) ||
            !sameSrcLoc(target.opSrcLoc(targetOp), other) ||
            !sameSrcLoc(otherTarget.valueSrcLoc(otherTargetValue), other) ||
            !sameSrcLoc(otherTarget.opSrcLoc(otherTargetOp), other))
            return fail("switching provenance import source or target reused another graph's mapping");
        otherTarget.copyValueSrcLoc(otherTargetValue, source, sourceValue, context);
        otherTarget.copyOpSrcLoc(otherTargetOp, source, sourceOp, context);
        target.copyOpSrcLoc(targetOp, source, sourceOp, context);
        target.copyValueSrcLoc(targetValue, source, sourceValue, context);
        if (!sameSrcLoc(otherTarget.valueSrcLoc(otherTargetValue), shared) ||
            !sameSrcLoc(otherTarget.opSrcLoc(otherTargetOp), shared) ||
            !sameSrcLoc(target.valueSrcLoc(targetValue), shared) || !sameSrcLoc(target.opSrcLoc(targetOp), shared))
            return fail("switching back to a prior provenance import pair lost source fields");

        target.copyValueSrcLoc(targetValue, source, missingValue, context);
        if (target.valueSrcLoc(targetValue) || !sameSrcLoc(target.opSrcLoc(targetOp), shared))
            return fail("cached missing value provenance did not clear only its destination");
        target.copyOpSrcLoc(targetOp, source, missingOp, context);
        if (target.opSrcLoc(targetOp) || target.valueSrcLoc(targetValue))
            return fail("cached missing operation provenance did not clear its destination");
        target.setValueSrcLoc(targetValue, seed);
        target.setOpSrcLoc(targetOp, other);
        const auto erasedValue = makeValue(source, "erased_value");
        const auto erasedOp = makeOp(source, "erased_op");
        const auto erasedTargetValue = makeValue(target, "erased_value");
        const auto erasedTargetOp = makeOp(target, "erased_op");
        if (!source.eraseValue(erasedValue) || !source.eraseOp(erasedOp) ||
            !target.eraseValue(erasedTargetValue) || !target.eraseOp(erasedTargetOp))
            return fail("cannot erase invalid provenance import fixtures");
        const auto unchanged = [&] {
            return sameSrcLoc(target.valueSrcLoc(targetValue), seed) &&
                sameSrcLoc(target.opSrcLoc(targetOp), other) &&
                sameSrcLoc(source.valueSrcLoc(sourceValue), shared) &&
                sameSrcLoc(source.opSrcLoc(sourceOp), shared) &&
                sameSrcLoc(otherSource.valueSrcLoc(otherValue), other) &&
                sameSrcLoc(otherSource.opSrcLoc(otherOp), other) &&
                sameSrcLoc(otherTarget.valueSrcLoc(otherTargetValue), shared) &&
                sameSrcLoc(otherTarget.opSrcLoc(otherTargetOp), shared);
        };
        const auto recordCount = target.payloadStats().srcLocs;
        for (const auto bad : {ValueId::invalid(), erasedValue, otherValue}) {
            if (!expectThrows([&] { target.copyValueSrcLoc(targetValue, source, bad, context); }) || !unchanged())
                return fail("invalid source value changed cached import destinations or source provenance");
        }
        for (const auto bad : {OperationId::invalid(), erasedOp, otherOp}) {
            if (!expectThrows([&] { target.copyOpSrcLoc(targetOp, source, bad, context); }) || !unchanged())
                return fail("invalid source operation changed cached import destinations or source provenance");
        }
        for (const auto bad : {ValueId::invalid(), erasedTargetValue, otherTargetValue}) {
            if (!expectThrows([&] { target.copyValueSrcLoc(bad, source, sourceValue, context); }) || !unchanged())
                return fail("invalid destination value changed another entity's cached provenance");
        }
        for (const auto bad : {OperationId::invalid(), erasedTargetOp, otherTargetOp}) {
            if (!expectThrows([&] { target.copyOpSrcLoc(bad, source, sourceOp, context); }) || !unchanged())
                return fail("invalid destination operation changed another entity's cached provenance");
        }
        if (target.payloadStats().srcLocs != recordCount)
            return fail("invalid cached import appended destination provenance records");
        target.copyValueSrcLoc(targetValue, source, sourceValue, context);
        target.copyOpSrcLoc(targetOp, source, sourceOp, context);
        if (!sameSrcLoc(target.valueSrcLoc(targetValue), shared) || !sameSrcLoc(target.opSrcLoc(targetOp), shared))
            return fail("rejected provenance imports damaged a previously valid context");

        if (!design.deleteGraph("import_source")) return fail("cannot destroy provenance import source");
        if (!sameSrcLoc(target.valueSrcLoc(targetValue), shared) || !sameSrcLoc(target.opSrcLoc(targetOp), shared))
            return fail("cached provenance import borrowed destroyed source strings");
        // A new Design deliberately reuses the old source's numeric GraphId
        // and record IDs; weak ownership must distinguish their lifetimes.
        Design replacementDesign;
        Graph& replacement = replacementDesign.createGraph("replacement_source");
        const auto replacementValue = makeValue(replacement, "value");
        const auto replacementOp = makeOp(replacement, "op");
        const SrcLoc replacementLoc{"replacement.fir", 37, 13, 41, 17, "new", "owner", "lifetime"};
        replacement.setOpAndValueSrcLocView(replacementOp, replacementValue, SrcLocView(replacementLoc));
        target.copyOpSrcLoc(targetOp, replacement, replacementOp, context);
        target.copyValueSrcLoc(targetValue, replacement, replacementValue, context);
        if (!sameSrcLoc(target.valueSrcLoc(targetValue), replacementLoc) ||
            !sameSrcLoc(target.opSrcLoc(targetOp), replacementLoc))
            return fail("context reused stale mappings after its source was destroyed");
        const auto finalRecords = target.payloadStats().srcLocs;
        target.copyValueSrcLoc(targetValue, target, targetValue, context);
        target.copyOpSrcLoc(targetOp, target, targetOp, context);
        if (target.payloadStats().srcLocs != finalRecords ||
            !sameSrcLoc(target.valueSrcLoc(targetValue), replacementLoc) ||
            !sameSrcLoc(target.opSrcLoc(targetOp), replacementLoc))
            return fail("same-graph cached provenance copy changed its record or fields");
        return 0;
    }

    int parallelGraphMutation() {
        Design design;
        auto pendingLeft = design.createUnpublishedGraph("left");
        auto pendingRight = design.createUnpublishedGraph("right");
        auto& left = *pendingLeft;
        auto& right = *pendingRight;
        const auto* leftAddress = &left;
        const auto* rightAddress = &right;
        const auto leftId = left.id(), rightId = right.id();
        if (!design.graphs().empty() || !design.graphOrder().empty() ||
            design.findGraph("left") || design.findGraph("right"))
            return fail("unpublished graphs entered the design catalog before publication");
        const auto before = design.revision();
        {
            auto scope = design.parallelGraphMutation();
            const auto during = design.revision();
            if (during <= before) return fail("parallel mutation entry did not invalidate prior revisions");
            if (!expectThrows([&] { auto nested = design.parallelGraphMutation(); }) ||
                design.revision() != during)
                return fail("nested parallel mutation scope was accepted or changed the revision");

            const auto populate = [](Graph& graph) {
                for (int index = 0; index < 128; ++index) {
                    const auto value = graph.createValue(graph.internSymbol("value" + std::to_string(index)), 8, false);
                    const auto op = graph.createOperation(OperationKind::kConstant);
                    graph.addResult(op, value);
                    graph.setAttr(op, "constValue", std::string("8'b00000000"));
                }
                graph.setMetadata("worker", std::string(graph.symbol()));
            };
            auto leftWorker = std::async(std::launch::async, [&] { populate(left); });
            auto rightWorker = std::async(std::launch::async, [&] { populate(right); });
            leftWorker.get();
            rightWorker.get();
            if (design.revision() != during)
                return fail("parallel graph mutations updated the shared design revision");
        }
        if (design.revision() <= before + 1 || left.operations().size() != 128 || right.values().size() != 128 ||
            left.id() != leftId || right.id() != rightId ||
            left.metadata("worker") != AttributeValue(std::string("left")) ||
            right.metadata("worker") != AttributeValue(std::string("right")))
            return fail("parallel mutation exit lost graph state, graph identity or revision invalidation");

        const auto leftValue = left.findValue("value127");
        const auto rightValue = right.findValue("value127");
        const auto beforeInvalidPublication = design.revision();
        Design foreign;
        if (!expectThrows([&] { design.publishGraph(nullptr); }) ||
            !expectThrows([&] { design.publishGraph(foreign.createUnpublishedGraph("foreign")); }) ||
            !expectThrows([&] { design.publishGraph(std::make_unique<Graph>(design, "left", rightId)); }) ||
            !design.graphs().empty() || !design.graphOrder().empty() ||
            design.revision() != beforeInvalidPublication)
            return fail("invalid publication was accepted or changed the design catalog");
        auto& publishedRight = design.publishGraph(std::move(pendingRight));
        auto& publishedLeft = design.publishGraph(std::move(pendingLeft));
        if (&publishedRight != rightAddress || &publishedLeft != leftAddress ||
            publishedLeft.id() != leftId || publishedRight.id() != rightId ||
            publishedLeft.findValue("value127") != leftValue || publishedRight.findValue("value127") != rightValue ||
            design.graphOrder() != std::vector<std::string>{"right", "left"})
            return fail("graph publication copied contents or changed graph/value identity");
        if (!expectThrows([&] { design.publishGraph(std::make_unique<Graph>(design, "left", leftId)); }))
            return fail("graph publication accepted a duplicate catalog name");

        try {
            auto scope = design.parallelGraphMutation();
            left.setMetadata("unwound", true);
            throw std::runtime_error("scope unwind");
        } catch (const std::runtime_error&) {}
        const auto afterScope = design.revision();
        left.setMetadata("serial", true);
        if (design.revision() <= afterScope)
            return fail("parallel mutation scope did not restore serial revision tracking after unwinding");
        return 0;
    }

    int unpublishedGraphPublication() {
        Design regular;
        Design deferred;
        regular.createGraph("wrapper");
        deferred.createGraph("wrapper");
        std::vector<std::unique_ptr<Graph>> pending;
        for (int index = 0; index < 48; ++index)
            pending.push_back(deferred.createUnpublishedGraph("module" + std::to_string(index)));
        if (deferred.graphs().size() != 1 || deferred.graphOrder() != std::vector<std::string>{"wrapper"})
            return fail("unpublished allocation changed existing catalog entries");

        const auto hashOrder = [](const Design& design) {
            std::vector<std::string> names;
            for (const auto& entry : design.graphs()) names.push_back(entry.first);
            return names;
        };
        for (std::size_t remaining = pending.size(); remaining > 0; --remaining) {
            const auto index = remaining - 1;
            regular.createGraph(pending[index]->symbol());
            deferred.publishGraph(std::move(pending[index]));
            if (index == 24) {
                regular.deleteGraph("wrapper");
                deferred.deleteGraph("wrapper");
            }
            if (regular.graphOrder() != deferred.graphOrder() || hashOrder(regular) != hashOrder(deferred))
                return fail("deferred publication changed catalog iteration after matching insertion/deletion events");
        }

        {
            auto discarded = deferred.createUnpublishedGraph("discarded");
        }
        if (deferred.findGraph("discarded") || !expectThrows([&] { deferred.createGraph("discarded"); }))
            return fail("discarded unpublished graph did not retain its documented name reservation");
        return 0;
    }

    int collidingStringPool()
    {
        struct ConstantHasher {
            std::size_t operator()(std::string_view) const noexcept { return 0; }
        };
        detail::BasicStringPool<ConstantHasher> pool;
        if (pool.intern("") != 0 || pool.lookup("missing") != 0 || pool.size() != 0)
            return fail("empty string or absent colliding text had an allocated StringId");

        const std::vector<std::string> texts{
            "alpha", "beta", std::string("left\0right", 10), std::string(8192, 'x')};
        std::vector<uint32_t> ids;
        std::vector<std::string_view> views;
        for (const auto& text : texts) {
            const auto id = pool.intern(text);
            if (id != ids.size() + 1 || pool.text(id) != text)
                return fail("different text with the same full hash shared a StringId");
            ids.push_back(id);
            views.push_back(pool.text(id));
        }
        for (int index = 0; index < 128; ++index)
            pool.intern("collision-" + std::to_string(index));
        pool.reserve(2048);
        pool.compact();
        for (std::size_t index = 0; index < texts.size(); ++index) {
            if (pool.lookup(texts[index]) != ids[index] || pool.intern(texts[index]) != ids[index] ||
                pool.text(ids[index]) != texts[index] || pool.text(ids[index]).data() != views[index].data())
                return fail("collision chain, StringId, or borrowed bytes changed after growth/compact");
        }
        if (pool.lookup("still-missing") != 0 || pool.intern("") != 0 ||
            pool.size() != texts.size() + 128)
            return fail("colliding lookup or repeated intern changed string pool contents");
        const auto added = pool.intern("post-compact");
        if (added != texts.size() + 129 || pool.lookup("post-compact") != added ||
            pool.lookup(texts.front()) != ids.front())
            return fail("adding a collision after compact lost an existing chain entry");
        return 0;
    }

    int sharedSymbolPayloadStrings()
    {
        Design design;
        Graph& graph = design.createGraph("shared_strings");
        const auto sharedSymbol = graph.internSymbol("shared");
        const auto reservedSymbol = graph.internSymbol("reserved");
        graph.createValue(sharedSymbol, 8, false);
        const auto op = graph.createOperation(OperationKind::kConstant, graph.internSymbol("constant"));
        graph.setAttr(op, "text", std::string("shared"));
        const auto text = graph.opAttrRange(op).find("text")->value().asString();
        if (text.data() != graph.symbolText(sharedSymbol).data())
            return fail("symbol and attribute text did not share interned bytes");
        graph.setAttr(op, "internal_name", std::string("_val_0"));
        const auto internal = graph.createValue(8, false);
        if (graph.symbolText(graph.valueSymbol(internal)) != "_val_0")
            return fail("payload text reserved or skipped an internal symbol name");
        graph.setAttr(op, "garbage", std::string("payload-only-old"));
        graph.setAttr(op, "garbage", std::string("retained"));
        const auto before = graph.payloadStats();
        graph.compact();
        const auto compactOp = graph.findOperation("constant");
        const auto compactText = graph.opAttrRange(compactOp).find("text")->value().asString();
        if (graph.lookupSymbol("shared") != sharedSymbol || graph.lookupSymbol("reserved") != reservedSymbol ||
            graph.symbolText(reservedSymbol) != "reserved" ||
            compactText.data() != graph.symbolText(sharedSymbol).data() ||
            graph.lookupSymbol("payload-only-old").valid() || graph.payloadStats().strings >= before.strings)
            return fail("compact lost symbol reservations, shared bytes, or payload garbage reclamation");
        return 0;
    }

    int symbolPayloadStateTransitions()
    {
        Design design;
        Graph& graph = design.createGraph("payload_state");
        const auto op = graph.createOperation(OperationKind::kConstant);
        const std::string promotedText = "payload-first-promoted";
        graph.setAttr(op, "payload", promotedText);
        const auto beforePromotion = graph.opAttrRange(op).find("payload");
        if (!beforePromotion || beforePromotion->value().asString() != promotedText ||
            graph.lookupSymbol(promotedText).valid())
            return fail("payload-first text unexpectedly became a symbol before promotion");
        const auto promoted = graph.internSymbol(promotedText);
        if (!promoted.valid() || graph.lookupSymbol(promotedText) != promoted ||
            graph.symbolText(promoted).data() != beforePromotion->value().asString().data())
            return fail("payload-first text did not promote to a symbol while retaining shared bytes");
        const auto promotedValue = graph.createValue(promoted, 8, false);
        if (!promotedValue.valid() || graph.internSymbol(promotedText).valid())
            return fail("promoted symbol was not bound or could be reserved twice");

        // A payload-only entry must not reserve the next generated operation
        // name.  This exercises tryInternFresh through the internal-name path.
        Graph& internal = design.createGraph("payload_internal");
        const auto firstOp = internal.createOperation(OperationKind::kConstant);
        internal.setAttr(firstOp, "payload", std::string("_op_1"));
        internal.internSymbol("_op_1");
        const auto secondOp = internal.createOperation(OperationKind::kConstant);
        if (internal.symbolText(internal.operationSymbol(secondOp)) != "_op_2")
            return fail("payload-only internal name was not skipped by fresh symbol generation");

        const auto reserved = graph.internSymbol("reserved-before-compact");
        const std::string retainedPayload = "payload-after-compact";
        const std::string garbagePayload = "payload-only-garbage";
        graph.setAttr(op, "retained", retainedPayload);
        graph.setAttr(op, "garbage", garbagePayload);
        graph.setAttr(op, "garbage", std::string("payload-only-replacement"));
        const auto beforeCompactStrings = graph.payloadStats().strings;
        graph.compact();
        if (graph.lookupSymbol("reserved-before-compact") != reserved ||
            graph.lookupSymbol(garbagePayload).valid() ||
            graph.payloadStats().strings + 1 != beforeCompactStrings ||
            graph.opAttrRange(graph.findOperation("_op_0")).find("retained")->value().asString() != retainedPayload)
            return fail("compact lost a symbol reservation, reclaimed payload-only text, or retained payload");
        const auto postCompact = graph.internSymbol(retainedPayload);
        if (!postCompact.valid() || graph.lookupSymbol(retainedPayload) != postCompact ||
            graph.symbolText(postCompact) != retainedPayload)
            return fail("payload text could not be promoted after compact");

        // Import copies the shared byte arena into the destination.  The
        // destination must be able to promote a payload-only entry and keep
        // it writable after the source design is destroyed.
        Design importedDesign;
        Graph* imported = nullptr;
        OperationId importedOp;
        {
            Design sourceDesign;
            Graph& source = sourceDesign.createGraph("source");
            const auto sourceOp = source.createOperation(OperationKind::kConstant);
            source.setAttr(sourceOp, "payload", std::string("import-payload"));
            imported = &importedDesign.importGraph(source, "imported");
            importedOp = imported->findOperation("_op_0");
            if (!importedOp.valid() || imported->lookupSymbol("import-payload").valid())
                return fail("imported payload unexpectedly occupied the destination symbol table");
            if (!imported->opAttrRange(importedOp).find("payload") ||
                imported->opAttrRange(importedOp).find("payload")->value().asString() != "import-payload")
                return fail("import lost payload-only text before promotion");
            const auto importedSymbol = imported->internSymbol("import-payload");
            if (!importedSymbol.valid() || imported->lookupSymbol("import-payload") != importedSymbol)
                return fail("imported payload could not be promoted independently");
        }
        imported->setAttr(importedOp, "payload", std::string("destination-only"));
        imported->compact();
        if (imported->opAttrRange(importedOp).find("payload")->value().asString() != "destination-only" ||
            imported->lookupSymbol("import-payload").valid() == false)
            return fail("destination payload/symbol state did not survive source destruction and compact");

        // StringPool reserves ID zero for empty text, while SymbolTable keeps
        // an independently valid empty-text symbol at SymbolId 1.
        GraphSymbolTable symbols;
        const auto empty = symbols.intern("");
        if (!empty.valid() || !symbols.contains("") || symbols.lookup("") != empty ||
            !symbols.text(empty).empty())
            return fail("empty SymbolTable text did not preserve its historical valid-symbol behavior");
        const auto copied = symbols;
        if (copied.lookup("") != empty || !copied.contains("") || copied.text(empty) != "")
            return fail("empty symbol was not preserved by SymbolTable copy");
        const auto nonEmpty = symbols.intern("after-empty");
        if (!nonEmpty.valid() || symbols.lookup("after-empty") != nonEmpty || nonEmpty == empty)
            return fail("nonempty symbol insertion after empty text changed symbol state");
        return 0;
    }

    int isolatedPayloadArenasAndReclamation()
    {
        Design leftDesign, rightDesign;
        Graph& left = leftDesign.createGraph("payload");
        Graph& right = rightDesign.createGraph("payload");
        const auto populate = [](Graph& graph, std::string_view prefix) {
            for (int index = 0; index < 64; ++index)
            {
                const auto op = graph.createOperation(OperationKind::kConstant,
                    graph.internSymbol("op" + std::to_string(index)));
                graph.setAttr(op, "strings", std::vector<std::string>{std::string(prefix), std::to_string(index)});
                graph.setOpSrcLoc(op, SrcLoc{std::string(prefix), 3});
            }
        };
        auto leftWorker = std::async(std::launch::async, [&] { populate(left, "left-source"); });
        auto rightWorker = std::async(std::launch::async, [&] { populate(right, "right-source"); });
        leftWorker.get(); rightWorker.get();
        if (&left.payloadStore() == &right.payloadStore()) return fail("independent graphs shared a payload arena");
        const auto leftOp = left.findOperation("op63");
        const auto rightOp = right.findOperation("op63");
        const auto leftStrings = left.opAttrRange(leftOp).find("strings")->value().asStringList();
        right.setAttr(rightOp, "strings", std::vector<std::string>{"right-updated"});
        if (leftStrings.size() != 2 || leftStrings[0] != "left-source" || leftStrings[1] != "63" ||
            right.opAttrRange(rightOp).find("strings")->value().asStringList()[0] != "right-updated" ||
            left.opSrcLoc(leftOp)->file != "left-source" || right.opSrcLoc(rightOp)->file != "right-source")
            return fail("parallel graph payload construction or mutation leaked across arenas");
        left.compact(); right.compact();
        if (left.opAttrRange(left.findOperation("op63")).find("strings")->value().asStringList()[0] != "left-source")
            return fail("compact mixed payload arenas");

        const auto leftSource = left.findOperation("op63");
        const auto copiedOp = left.createOperation(OperationKind::kConstant, left.internSymbol("copied"));
        left.setAttrView(copiedOp, "strings", left.opAttrRange(leftSource).find("strings")->value());
        left.setAttr(leftSource, "strings", std::vector<std::string>{"overwritten-source"});
        const auto sameGraphCopy = left.opAttrRange(copiedOp).find("strings")->value().asStringList();
        if (sameGraphCopy.size() != 2 || sameGraphCopy[0] != "left-source" || sameGraphCopy[1] != "63")
            return fail("same-graph attribute copy shared a mutable attribute record");
        Design importedDesign;
        Graph& imported = importedDesign.createGraph("imported");
        const auto importedOp = imported.createOperation(OperationKind::kConstant, imported.internSymbol("constant"));
        const std::string longPayload(8192, 'x');
        {
            Design temporaryDesign;
            Graph& temporary = temporaryDesign.createGraph("temporary");
            const auto source = temporary.createOperation(OperationKind::kConstant);
            temporary.setAttr(source, "strings", std::vector<std::string>{longPayload, "tail"});
            std::vector<bool> bits(65);
            for (std::size_t index = 0; index < bits.size(); ++index) bits[index] = index % 2 == 0;
            temporary.setAttr(source, "bits", bits);
            for (const auto attr : temporary.opAttrRange(source))
                imported.setAttrView(importedOp, attr.key(), attr.value());
        }
        imported.compact();
        const auto importedAttrs = imported.opAttrRange(imported.findOperation("constant"));
        const auto strings = importedAttrs.find("strings")->value().asStringList();
        const auto bits = importedAttrs.find("bits")->value().asBoolList();
        if (strings.size() != 2 || strings[0] != longPayload || strings[1] != "tail" ||
            bits.size() != 65 || !bits[0] || bits[1] || !bits[64])
            return fail("cross-graph attribute copy borrowed destroyed source payload storage");

        Design reclamationDesign;
        Graph& graph = reclamationDesign.createGraph("reclamation");
        const auto op = graph.createOperation(OperationKind::kConstant, graph.internSymbol("constant"));
        std::vector<std::string> discarded;
        for (int index = 0; index < 64; ++index) discarded.push_back("discarded" + std::to_string(index));
        graph.setAttr(op, "payload", discarded);
        graph.setAttr(op, "payload", int64_t(-27));
        graph.setAttr(op, "payload", std::vector<std::string>{"retained", "retained"});
        const auto before = graph.payloadStats();
        graph.compact();
        const auto after = graph.payloadStats();
        const auto payload = graph.opAttrRange(graph.findOperation("constant")).find("payload")->value().asStringList();
        if (payload.size() != 2 || payload[0] != "retained" || payload[1] != "retained" ||
            after.attributes != 1 || after.stringListPayload != 2 || after.strings >= before.strings ||
            after.stringListPayload >= before.stringListPayload)
            return fail("compact did not reclaim overwritten list/scalar payloads while preserving live records");
        return 0;
    }

    int shapeSubspanImport()
    {
        Design design;
        Graph& graph = design.createGraph("shape_subspan");
        const std::array<int64_t, 3> originalShape{2, 4, 8};
        const auto original = graph.createValue(graph.internSymbol("original"), 64, false,
                                                ValueType::Logic, originalShape);
        const auto imported = graph.createValue(graph.internSymbol("imported"), 64, false,
            ValueType::Logic, graph.valueShape(original).subspan(1));
        const auto backfilled = graph.createValue(graph.internSymbol("backfilled"), 64, false);
        graph.setValueShape(backfilled, graph.valueShape(original).subspan(2));
        const auto check = [&]() {
            const auto source = graph.valueShape(graph.findValue("original"));
            const auto copy = graph.valueShape(graph.findValue("imported"));
            const auto tail = graph.valueShape(graph.findValue("backfilled"));
            return source.size() == 3 && source[0] == 2 && source[1] == 4 && source[2] == 8 &&
                copy.size() == 2 && copy[0] == 4 && copy[1] == 8 && tail.size() == 1 && tail[0] == 8;
        };
        if (!check()) return fail("new shape import from a pooled subspan corrupted dimensions");
        graph.compact();
        if (!check()) return fail("compact changed shapes imported from pooled subspans");
        return 0;
    }

    int compactPackedRanges()
    {
        Design design;
        Graph& graph = design.createGraph("packed_ranges");
        const auto shared = graph.createValue(graph.internSymbol("shared"), 8, false);
        std::array<OperationId, 3> operations;
        for (std::size_t index = 0; index < operations.size(); ++index)
        {
            operations[index] = graph.createOperation(OperationKind::kConcat,
                graph.internSymbol("concat" + std::to_string(index)));
            const auto output = graph.createValue(graph.internSymbol("output" + std::to_string(index)), 24, false);
            graph.addResult(operations[index], output);
            graph.addOperand(operations[index], shared);
        }
        // Growing earlier slices after later operations have acquired storage
        // creates non-tail operand ranges and shared-value use-list growth.
        for (const auto op : operations)
        {
            graph.addOperand(op, shared);
            graph.addOperand(op, shared);
        }
        graph.compact();
        const auto compactShared = graph.findValue("shared");
        const auto stats = graph.payloadStats();
        if (stats.operands != 9 || stats.uses != 9 || stats.useSlots != stats.uses ||
            stats.operandIndexSlots != stats.operands || graph.valueUsers(compactShared).size() != 9)
            return fail("compact retained dead operand or use-list arena slots");
        for (const auto op : graph.operations())
        {
            const auto operands = graph.opOperands(op);
            if (operands.size() != 3 || operands[0] != compactShared ||
                operands[1] != compactShared || operands[2] != compactShared)
                return fail("packed compact ranges changed repeated shared operands");
        }
        return 0;
    }

    int pooledShapeAndSourceLocation()
    {
        Design design;
        Graph& graph = design.createGraph("pooled_shape_source");
        const std::array<int64_t, 2> shape{2, 4};
        const SrcLoc location{"pooled.fir", 11, 3, 11, 16, "FIRRTL", "ingest", "shared"};
        for (int index = 0; index < 32; ++index)
        {
            const auto input = graph.createValue(graph.internSymbol("input" + std::to_string(index)),
                                                 8, false, ValueType::Logic, shape);
            const auto output = graph.createValue(graph.internSymbol("output" + std::to_string(index)),
                                                  8, false, ValueType::Logic, shape);
            const auto op = graph.createOperation(OperationKind::kNot, graph.internSymbol("not" + std::to_string(index)));
            graph.addOperand(op, input);
            graph.addResult(op, output);
            graph.setOpSrcLoc(op, location);
            graph.setValueSrcLoc(input, location);
            graph.setValueSrcLoc(output, location);
        }
        const auto check = [&]() {
            const auto stats = graph.payloadStats();
            if (stats.srcLocs != 1 || stats.shapes != 1 || stats.shapeDims != 2) return false;
            for (const auto value : graph.values())
            {
                const auto dimensions = graph.valueShape(value);
                if (dimensions.size() != 2 || dimensions[0] != 2 || dimensions[1] != 4 ||
                    !sameSrcLoc(graph.valueSrcLoc(value), location)) return false;
            }
            for (const auto op : graph.operations())
                if (!sameSrcLoc(graph.opSrcLoc(op), location)) return false;
            return true;
        };
        if (!check()) return fail("equal shape/source-location payloads were not interned");
        graph.compact();
        if (!check()) return fail("compact changed pooled shape/source-location payloads");
        return 0;
    }

    int freezeOwnership()
    {
        const std::vector<std::string> payload{std::string(8192, 'a'), std::string(4096, 'b')};
        const SrcLoc original{"long/source/file.fir", 10, 2, 12, 7, "FIRRTL", "lower", std::string(4096, 'n')};
        Design design;
        auto& graph = design.createGraph("freeze_ownership");
        const auto deadValue = graph.createValue(graph.internSymbol("dead"), 1, false);
        const auto deadOp = graph.createOperation(OperationKind::kConstant, graph.internSymbol("dead_op"));
        graph.addResult(deadOp, deadValue);
        const auto liveInput = graph.createValue(graph.internSymbol("input"), 8, false,
                                                  ValueType::Logic, std::vector<int64_t>{2}, LogicDomain::TwoState);
        const auto liveOutput = graph.createValue(graph.internSymbol("output"), 8, false,
                                                   ValueType::Logic, std::vector<int64_t>{2}, LogicDomain::TwoState);
        const auto liveOp = graph.createOperation(OperationKind::kNot, graph.internSymbol("compute"));
        graph.addOperand(liveOp, liveInput); graph.addResult(liveOp, liveOutput);
        graph.setAttr(liveOp, "payload", payload); graph.setOpSrcLoc(liveOp, original);
        graph.setValueSrcLoc(liveOutput, original);
        graph.bindInputPort("z_input", liveInput); graph.bindOutputPort("a_output", liveOutput);
        if (!graph.eraseOp(deadOp) || !graph.eraseValue(deadValue)) return fail("could not create freeze ID gaps");
        graph.freeze();
        const auto frozenInput = graph.findValue("input"), frozenOutput = graph.findValue("output");
        const auto frozenOp = graph.findOperation("compute");
        if (!graph.frozen() || frozenInput.index != 1 || frozenOutput.index != 2 || frozenOp.index != 1 ||
            graph.getOperation(frozenOp).attr("payload") != AttributeValue(payload) ||
            !sameSrcLoc(graph.opSrcLoc(frozenOp), original) || !sameSrcLoc(graph.valueSrcLoc(frozenOutput), original) ||
            graph.opOperands(frozenOp)[0] != frozenInput || graph.opResults(frozenOp)[0] != frozenOutput ||
            graph.valueDef(frozenOutput) != frozenOp || graph.valueUsers(frozenInput).size() != 1 ||
            graph.valueUsers(frozenInput)[0].operation != frozenOp ||
            graph.inputPorts()[0].value != frozenInput || graph.outputPorts()[0].value != frozenOutput ||
            graph.valueShape(frozenOutput).size() != 1 || graph.valueShape(frozenOutput)[0] != 2 ||
            graph.valueDomain(frozenOutput) != LogicDomain::TwoState)
            return fail("Graph::freeze changed remapping, owned payload, SrcLoc, bindings, or uses");

        const auto invalidInput = graph.createValue(graph.internSymbol("erased_input"), 8, false);
        graph.replaceOperand(frozenOp, 0, invalidInput);
        if (!graph.eraseValueUnchecked(invalidInput)) return fail("could not create invalid freeze operand");
        std::string failure;
        try { graph.freeze(); }
        catch (const std::runtime_error& error) { failure = error.what(); }
        if (failure != "ValueId refers to erased value during freeze" || graph.frozen() ||
            graph.getOperation(frozenOp).attr("payload") != AttributeValue(payload) ||
            !sameSrcLoc(graph.opSrcLoc(frozenOp), original) || !sameSrcLoc(graph.valueSrcLoc(frozenOutput), original) ||
            graph.opOperands(frozenOp)[0] != invalidInput)
            return fail("failed Graph::freeze changed diagnostics or consumed mutable payload");
        graph.replaceOperand(frozenOp, 0, frozenInput);
        graph.freeze();
        if (graph.getOperation(graph.findOperation("compute")).attr("payload") != AttributeValue(payload))
            return fail("repaired graph lost owned payload after failed freeze");
        return 0;
    }

    int profileCountersRegression()
    {
        // Keep this check isolated from the rest of the suite: the counters
        // are process-wide so worker-thread graph construction can contribute
        // without taking a lock on an individual Design.
        setGraphProfileEnabled(true);
        resetGraphProfileStats();

        Design design;
        Graph& builder = design.createGraph("profile");
        const auto inputSymbol = builder.internSymbol("profile_input");
        const auto outputSymbol = builder.internSymbol("profile_output");
        const auto opSymbol = builder.internSymbol("profile_op");
        const auto input = builder.createValue(inputSymbol, 8, false);
        const auto output = builder.createValue(outputSymbol, 8, false);
        const auto op = builder.createOperation(OperationKind::kNot, opSymbol);
        builder.addOperand(op, input);
        builder.addResult(op, output);
        builder.setAttr(op, "profile.payload", std::string("payload"));
        const SrcLoc loc{"profile.fir", 3, 4, 3, 10, "profile", "test", "note"};
        builder.setOpSrcLoc(op, loc);
        builder.setValueSrcLoc(output, loc);

        builder.compact();
        builder.setAttr(op, "after_compact", true);
        const GraphProfileStats stats = graphProfileStats();
        setGraphProfileEnabled(false);

        if (stats.symbolInserts < 3 || stats.valueAllocations < 2 || stats.operationAllocations < 1 ||
            stats.attributeWrites < 1 || stats.attributeBytes < std::string_view("profile.payload").size() + 7 ||
            stats.srcLocWrites < 2 || stats.srcLocBytes < 2 * (std::string_view("profile.fir").size() +
                                                               std::string_view("profile").size() +
                                                               std::string_view("test").size() +
                                                               std::string_view("note").size()) ||
            stats.freezeCount != 1 || stats.thawCount != 0)
            return fail("graph profile counters missed one or more storage events");
        return 0;
    }

    int graphStoreFreezeOracle()
    {
        Design design;
        Graph& builder = design.createGraph("legacy_compact_golden");
        const auto sym = [&](std::string_view name) { return builder.internSymbol(name); };
        const auto dead = builder.createValue(sym("dead"), 8, false);
        const auto left = builder.createValue(sym("left"), 8, false);
        const auto right = builder.createValue(sym("right"), 8, false);
        const auto sum = builder.createValue(sym("sum"), 8, false);
        const auto output = builder.createValue(sym("output"), 8, false);
        const auto deadOp = builder.createOperation(OperationKind::kConstant, sym("dead_op"));
        builder.addResult(deadOp, dead);
        const auto add = builder.createOperation(OperationKind::kAdd, sym("add"));
        builder.addOperand(add, left);
        const auto assign = builder.createOperation(OperationKind::kAssign, sym("assign"));
        builder.addOperand(assign, sum);
        // Append to a non-tail operand range, then edit its positions.  The
        // unified store must preserve the legacy freeze use-list order.
        builder.addOperand(add, right);
        builder.insertOperand(add, 1, left);
        builder.replaceOperand(add, 1, right);
        if (!builder.eraseOperand(add, 2)) return fail("oracle operand fixture edit failed");
        builder.addResult(add, sum);
        builder.addResult(assign, output);
        builder.bindInputPort("z_right", right);
        builder.bindInputPort("a_left", left);
        builder.bindOutputPort("output", output);
        builder.setAttr(add, "z_payload", std::vector<std::string>{"same", "other", "same"});
        builder.setAttr(add, "a_payload", int64_t(-9));
        builder.setOpSrcLoc(add, SrcLoc{"oracle.fir", 7, 2, 7, 12, "FIRRTL", "lower", "note"});
        if (!builder.eraseOp(deadOp) || !builder.eraseValue(dead))
            return fail("oracle tombstone fixture edit failed");

        builder.compact();
        const auto compactLeft = builder.findValue("left");
        const auto compactRight = builder.findValue("right");
        const auto compactSum = builder.findValue("sum");
        const auto compactOutput = builder.findValue("output");
        const auto compactAdd = builder.findOperation("add");
        const auto compactAssign = builder.findOperation("assign");
        // These canonical IDs, ordering, and payloads were recorded from the
        // legacy freeze implementation before removing its storage backend.
        if (builder.values().size() != 4 || builder.operations().size() != 2 ||
            compactLeft.index != 1 || compactRight.index != 2 || compactSum.index != 3 ||
            compactOutput.index != 4 || compactAdd.index != 1 || compactAssign.index != 2 ||
            builder.inputPorts()[0].name != "a_left" || builder.inputPorts()[1].name != "z_right" ||
            builder.inputPorts()[0].value != compactLeft || builder.inputPorts()[1].value != compactRight ||
            builder.outputPorts()[0].value != compactOutput)
            return fail("compact changed legacy canonical IDs or port order");
        const auto operands = builder.opOperands(compactAdd);
        const auto results = builder.opResults(compactAdd);
        const auto assignOperands = builder.opOperands(compactAssign);
        const auto assignResults = builder.opResults(compactAssign);
        const auto attrs = builder.opAttrRange(compactAdd);
        if (attrs.size() != 2) return fail("compact changed legacy attribute count");
        auto attrIterator = attrs.begin();
        const auto firstAttr = *attrIterator++;
        const auto secondAttr = *attrIterator;
        if (builder.opKind(compactAdd) != OperationKind::kAdd ||
            builder.opKind(compactAssign) != OperationKind::kAssign ||
            operands.size() != 2 || operands[0] != compactLeft || operands[1] != compactRight ||
            results.size() != 1 || results[0] != compactSum ||
            assignOperands.size() != 1 || assignOperands[0] != compactSum ||
            assignResults.size() != 1 || assignResults[0] != compactOutput ||
            firstAttr.key() != "a_payload" || firstAttr.value().asInt() != -9 ||
            secondAttr.key() != "z_payload" ||
            secondAttr.value().toOwned() != AttributeValue(std::vector<std::string>{"same", "other", "same"}) ||
            !sameSrcLoc(builder.opSrcLoc(compactAdd), SrcLoc{"oracle.fir", 7, 2, 7, 12, "FIRRTL", "lower", "note"}))
            return fail("compact changed legacy operation, attribute, or source-location fields");
        const auto checkUsers = [&](ValueId value, OperationId operation, std::size_t operandIndex) {
            const auto users = builder.valueUsers(value);
            return users.size() == 1 && users[0].operation == operation && users[0].operandIndex == operandIndex;
        };
        if (builder.valueDef(compactLeft).valid() || builder.valueDef(compactRight).valid() ||
            builder.valueDef(compactSum) != compactAdd || builder.valueDef(compactOutput) != compactAssign ||
            !checkUsers(compactLeft, compactAdd, 0) || !checkUsers(compactRight, compactAdd, 1) ||
            !checkUsers(compactSum, compactAssign, 0) || !builder.valueUsers(compactOutput).empty())
            return fail("compact changed legacy definitions or use-list order");
        builder.compact();
        if (builder.findValue("left") != compactLeft || builder.findOperation("add") != compactAdd)
            return fail("repeated compact changed canonical identity");
        return 0;
    }

    int compactMutationJsonOracle()
    {
        Design uninterrupted;
        Design compacted;
        const auto prepare = [](Design& design) -> Graph& {
            Graph& graph = design.createGraph("compact_oracle");
            design.markAsTop("compact_oracle");
            const auto deadValue = graph.createValue(graph.internSymbol("dead"), 1, false);
            const auto deadOp = graph.createOperation(OperationKind::kConstant, graph.internSymbol("dead_op"));
            graph.addResult(deadOp, deadValue);
            graph.setAttr(deadOp, "constValue", std::string("1'b0"));
            const auto left = graph.createValue(graph.internSymbol("left"), 8, false,
                ValueType::Logic, std::array<int64_t, 1>{2}, LogicDomain::TwoState);
            const auto right = graph.createValue(graph.internSymbol("right"), 8, false,
                ValueType::Logic, std::array<int64_t, 1>{2}, LogicDomain::TwoState);
            const auto sum = graph.createValue(graph.internSymbol("sum"), 8, false,
                ValueType::Logic, std::array<int64_t, 1>{2}, LogicDomain::TwoState);
            const auto output = graph.createValue(graph.internSymbol("output"), 8, false,
                ValueType::Logic, std::array<int64_t, 1>{2}, LogicDomain::TwoState);
            const auto add = graph.createOperation(OperationKind::kAdd, graph.internSymbol("add"));
            graph.addOperand(add, left); graph.addOperand(add, right); graph.addResult(add, sum);
            const auto assign = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign"));
            graph.addOperand(assign, sum); graph.addResult(assign, output);
            graph.bindInputPort("z_right", right); graph.bindInputPort("a_left", left);
            graph.bindOutputPort("output", output);
            graph.setAttr(add, "a_bool", true);
            graph.setAttr(add, "b_int", int64_t(-13));
            graph.setAttr(add, "c_double", -2.5);
            graph.setAttr(add, "d_string", std::string(128, 's'));
            graph.setAttr(add, "e_bools", std::vector<bool>{true, false, true});
            graph.setAttr(add, "f_ints", std::vector<int64_t>{-1, 0, 2});
            graph.setAttr(add, "g_doubles", std::vector<double>{-1.5, 2.5});
            graph.setAttr(add, "h_strings", std::vector<std::string>{"first", "second", "first"});
            graph.setOpSrcLoc(add, SrcLoc{"compact.fir", 4, 1, 4, 20, "FIRRTL", "lower", "source"});
            graph.setValueSrcLoc(sum, SrcLoc{"compact.fir", 4, 1, 4, 20, "FIRRTL", "lower", "result"});
            if (!graph.eraseOp(deadOp) || !graph.eraseValue(deadValue))
                throw std::runtime_error("could not create compact JSON oracle tombstones");
            return graph;
        };
        Graph& left = prepare(uninterrupted);
        Graph& right = prepare(compacted);
        right.compact();
        if (!right.frozen()) return fail("compact JSON oracle did not enter canonical state");
        const auto mutate = [](Graph& graph) {
            const auto leftValue = graph.findValue("left");
            const auto rightValue = graph.findValue("right");
            const auto add = graph.findOperation("add");
            graph.insertOperand(add, 1, leftValue);
            graph.replaceOperand(add, 1, rightValue);
            if (!graph.eraseOperand(add, 2)) throw std::runtime_error("compact JSON oracle operand edit failed");
            graph.setAttr(add, "d_string", std::string("updated"));
            graph.eraseAttr(add, "b_int");
            graph.setAttr(add, "b_int", int64_t(19));
            const auto extra = graph.createValue(graph.internSymbol("extra"), 8, false,
                ValueType::Logic, std::array<int64_t, 1>{2}, LogicDomain::TwoState);
            const auto extraOp = graph.createOperation(OperationKind::kNot, graph.internSymbol("extra_op"));
            graph.addOperand(extraOp, leftValue); graph.addResult(extraOp, extra);
            graph.replaceAllUses(extra, rightValue);
            if (!graph.eraseOp(extraOp) || !graph.eraseValue(extra))
                throw std::runtime_error("compact JSON oracle appended deletion failed");
        };
        mutate(left); mutate(right);
        if (right.frozen()) return fail("post-compact mutation did not clear canonical state");
        left.compact(); right.compact();
        StoreDiagnostics diagnostics;
        StoreJson store(&diagnostics);
        const auto expected = store.storeToString(uninterrupted);
        const auto actual = store.storeToString(compacted);
        if (!expected || !actual || diagnostics.hasError() || *expected != *actual)
            return fail("compact boundary changed the JSON bytes of an identical mutation sequence");
        right.compact();
        const auto repeated = store.storeToString(compacted);
        if (!repeated || *repeated != *actual)
            return fail("repeated compact changed canonical JSON bytes");
        return 0;
    }

    int blockImportOwnership()
    {
        Design sourceDesign, expectedDesign, actualDesign;
        auto& source = sourceDesign.createGraph("source");
        const auto a = source.createValue(source.internSymbol("a"), 32, false, ValueType::Logic,
                                          std::array<int64_t, 2>{2, 2});
        const auto b = source.createValue(source.internSymbol("b"), 32, false, ValueType::Logic,
                                          std::array<int64_t, 2>{2, 2});
        const auto op = source.createOperation(OperationKind::kAdd, source.internSymbol("add"));
        source.addOperand(op, a); source.addOperand(op, a); source.addResult(op, b);
        source.setAttr(op, "a_bool", true);
        source.setAttr(op, "b_int", int64_t(-3));
        source.setAttr(op, "c_double", 2.5);
        source.setAttr(op, "d_string", std::string(2000, 's'));
        source.setAttr(op, "e_bits", std::vector<bool>(129, true));
        source.setAttr(op, "f_ints", std::vector<int64_t>{1, 2, 3});
        source.setAttr(op, "g_doubles", std::vector<double>{1.5, -2.5});
        source.setAttr(op, "h_strings", std::vector<std::string>{std::string(1000, 'x'), "", "same"});
        const SrcLoc loc{std::string(500, 'f'), 8, 2, 8, 9, "origin", "lower", "note"};
        source.setOpAndValueSrcLocView(op, b, SrcLocView(loc));
        source.compact();
        expectedDesign.importGraph(source, "target");
        auto& actual = actualDesign.createGraph("target");
        expectedDesign.markAsTop("target");
        actualDesign.markAsTop("target");
        Graph::BlockImportContext payload;
        Graph::SrcLocImportContext provenance;
        actual.prepareBlockImport(source, payload, provenance);
        const std::vector<ValueId> sourceValues(source.values().begin(), source.values().end());
        const std::vector<OperationId> sourceOps(source.operations().begin(), source.operations().end());
        std::vector<SymbolId> valueSymbols, opSymbols;
        for (auto value : sourceValues) valueSymbols.push_back(actual.internSymbol(source.symbolText(source.valueSymbol(value))));
        for (auto operation : sourceOps) opSymbols.push_back(actual.internSymbol(source.symbolText(source.operationSymbol(operation))));
        std::array<ValueId, 2> values;
        std::array<OperationId, 1> ops;
        actual.appendValueBlock(source, sourceValues, valueSymbols, values, provenance, 4);
        actual.appendOperationBlock(source, sourceOps, opSymbols, ops, provenance, 4);
        const std::array<uint32_t, 2> operandOffsets{0, 2}, resultOffsets{0, 1};
        const std::array<ValueId, 2> operands{values[0], values[0]};
        const std::array<ValueId, 1> results{values[1]};
        if (!expectThrows([&] {
            actual.setOperationBlockEdges(ops, resultOffsets, operands, resultOffsets, results);
        }) || actual.valueDef(values[1]).valid())
            return fail("invalid block offsets modified result definition");
        actual.setOperationBlockEdges(ops, operandOffsets, operands, resultOffsets, results);
        actual.appendOperationBlockAttributes(source, sourceOps, ops, payload);
        actual.rebuildUseLists();
        StoreDiagnostics diagnostics;
        StoreJson store(&diagnostics);
        const auto expected = store.storeToString(expectedDesign);
        const auto imported = store.storeToString(actualDesign);
        if (!expected || !imported || *expected != *imported) {
            std::cerr << "block import JSON present: expected=" << bool(expected)
                      << " actual=" << bool(imported) << '\n';
            for (const auto &message : diagnostics.messages()) std::cerr << "store: " << message.message << '\n';
            if (expected && imported) {
                std::size_t first = 0;
                while (first < expected->size() && first < imported->size() && (*expected)[first] == (*imported)[first]) ++first;
                const auto begin = first > 60 ? first - 60 : 0;
                std::cerr << "block import first JSON difference at byte " << first
                          << " expected-size=" << expected->size() << " actual-size=" << imported->size()
                          << "\nexpected: " << expected->substr(begin, 180)
                          << "\nactual: " << imported->substr(begin, 180) << '\n';
#ifdef GRH_STAGE1_JSON_PATH
                const auto directory = std::filesystem::path(GRH_STAGE1_JSON_PATH).parent_path();
                std::filesystem::create_directories(directory);
                std::ofstream(directory / "grh-block-import.expected.json") << *expected;
                std::ofstream(directory / "grh-block-import.actual.json") << *imported;
#endif
            }
            return fail("block import differs from ordinary graph import");
        }
        const auto extra = actual.createOperation(OperationKind::kConstant);
        const std::array<OperationId, 2> attrsOps{ops[0], extra};
        const AttributeValue sharedOrigins = std::vector<std::string>{"instance"};
        actual.setOperationBlockAttr(attrsOps, "originModules", AttrValueView(&sharedOrigins));
        actual.setAttr(ops[0], "originModules", std::vector<std::string>{"changed"});
        if (actual.opAttrRange(extra).find("originModules")->value().asStringList()[0] != "instance")
            return fail("batch attrs share mutable records between operations");
        const auto originalInts = actual.opAttrRange(ops[0]).find("f_ints")->value().asIntList();
        const auto sourceInts = source.opAttrRange(sourceOps[0]).find("f_ints")->value().asIntList();
        if (originalInts.data() != sourceInts.data()) return fail("block payload buffer was copied instead of retained");
        // Both graphs remain independently writable after buffer retention.
        source.setAttr(sourceOps[0], "f_ints", std::vector<int64_t>{9, 10});
        source.setAttr(sourceOps[0], "h_strings", std::vector<std::string>{"source-change"});
        actual.setAttr(ops[0], "target-extra", std::vector<int64_t>{11, 12});
        if (actual.opAttrRange(ops[0]).find("f_ints")->value().asIntList()[0] != 1 ||
            source.opAttrRange(sourceOps[0]).find("f_ints")->value().asIntList()[0] != 9)
            return fail("retained payload mutations leaked between graphs");
        if (!expectThrows([&] { actual.appendOperationBlockAttributes(source, sourceOps, ops, payload); }))
            return fail("mutated source did not invalidate block import context");
        sourceDesign.deleteGraph("source");
        if (!sameSrcLoc(actual.opSrcLoc(ops[0]), loc) ||
            actual.opAttrRange(ops[0]).find("d_string")->value().asString() != std::string(2000, 's') ||
            actual.opAttrRange(ops[0]).find("h_strings")->value().asStringList()[0] != std::string(1000, 'x') ||
            actual.opAttrRange(ops[0]).find("e_bits")->value().asBoolList().size() != 129)
            return fail("destroyed source invalidated retained block payload");
        actual.replaceOperand(ops[0], 0, values[1]);
        if (actual.valueUsers(values[0]).size() != 1 || actual.valueUsers(values[1]).size() != 1)
            return fail("rebuilt block backlinks do not support ordinary mutation");
        actual.compact();
        const auto compactOp = actual.findOperation("add");
        if (actual.opAttrRange(compactOp).find("f_ints")->value().asIntList()[2] != 3)
            return fail("compact did not repack retained payload segments");

        // Exercise real concurrent fixed-column writes beyond the grain size.
        Design largeSourceDesign, serialDesign, parallelDesign;
        auto& largeSource = largeSourceDesign.createGraph("source");
        constexpr std::size_t count = 32768;
        for (std::size_t i = 0; i < count; ++i) {
            const auto value = largeSource.createValue(1 + i % 31, false);
            const auto operation = largeSource.createOperation(OperationKind::kConstant);
            largeSource.addResult(operation, value);
        }
        const std::vector<ValueId> largeValues(largeSource.values().begin(), largeSource.values().end());
        const std::vector<OperationId> largeOps(largeSource.operations().begin(), largeSource.operations().end());
        const auto copy = [&](Design& design, std::size_t threads) {
            auto& graph = design.createGraph("copy");
            std::vector<SymbolId> vs, os;
            for (auto value : largeValues) vs.push_back(graph.internSymbol(largeSource.symbolText(largeSource.valueSymbol(value))));
            for (auto operation : largeOps) os.push_back(graph.internSymbol(largeSource.symbolText(largeSource.operationSymbol(operation))));
            std::vector<ValueId> outValues(count);
            std::vector<OperationId> outOps(count);
            Graph::SrcLocImportContext context;
            graph.appendValueBlock(largeSource, largeValues, vs, outValues, context, threads, true);
            graph.appendOperationBlock(largeSource, largeOps, os, outOps, context, threads, true);
            for (std::size_t i = 0; i < count; ++i)
                if (graph.valueWidth(outValues[i]) != largeSource.valueWidth(largeValues[i]) ||
                    graph.opKind(outOps[i]) != OperationKind::kConstant ||
                    outValues[i].index != i + 1 || outOps[i].index != i + 1)
                    throw std::runtime_error("parallel block columns mismatch");
            return graph.payloadStats();
        };
        const auto serial = copy(serialDesign, 1), parallel = copy(parallelDesign, 4);
        if (serial.values != parallel.values || serial.operations != parallel.operations)
            return fail("serial and parallel block sizes differ");
        auto &serialGraph = *serialDesign.findGraph("copy");
        auto &parallelGraph = *parallelDesign.findGraph("copy");
        const auto copiedValue = [&](Graph &graph, std::size_t index) {
            return graph.findValue(largeSource.symbolText(largeSource.valueSymbol(largeValues[index])));
        };
        const auto copiedOp = [&](Graph &graph, std::size_t index) {
            return graph.findOperation(largeSource.symbolText(largeSource.operationSymbol(largeOps[index])));
        };
        const auto sourceType = largeSource.valueLogicalType(largeValues[5]);
        const auto targetType = largeSource.valueLogicalType(largeValues[517]);
        const LogicalType sourceChange{ValueType::Logic, 19, true, LogicDomain::TwoState, {}};
        const LogicalType targetChange{ValueType::Logic, 23, true, LogicDomain::TwoState, {}};
        largeSource.setValueLogicalType(largeValues[5], sourceChange);
        largeSource.setOpKind(largeOps[6], OperationKind::kNot);
        if (serialGraph.valueLogicalType(copiedValue(serialGraph, 5)) != sourceType ||
            parallelGraph.valueLogicalType(copiedValue(parallelGraph, 5)) != sourceType ||
            serialGraph.opKind(copiedOp(serialGraph, 6)) != OperationKind::kConstant ||
            parallelGraph.opKind(copiedOp(parallelGraph, 6)) != OperationKind::kConstant)
            return fail("source Graph API mutation leaked into retained value/op column pages");
        serialGraph.setValueLogicalType(copiedValue(serialGraph, 517), targetChange);
        serialGraph.setOpKind(copiedOp(serialGraph, 518), OperationKind::kAdd);
        if (largeSource.valueLogicalType(largeValues[517]) != targetType ||
            parallelGraph.valueLogicalType(copiedValue(parallelGraph, 517)) != targetType ||
            largeSource.opKind(largeOps[518]) != OperationKind::kConstant ||
            parallelGraph.opKind(copiedOp(parallelGraph, 518)) != OperationKind::kConstant ||
            largeSource.valueLogicalType(largeValues[5]) != sourceChange ||
            serialGraph.valueLogicalType(copiedValue(serialGraph, 517)) != targetChange)
            return fail("target Graph API mutation leaked into another retained value/op column owner");
        Design aliasDesign;
        auto& aliases = aliasDesign.createGraph("aliases");
        const auto x = aliases.createValue(8, false), y = aliases.createValue(8, false);
        const auto z = aliases.createValue(8, false), w = aliases.createValue(8, false);
        const auto first = aliases.createOperation(OperationKind::kAdd);
        const auto second = aliases.createOperation(OperationKind::kAdd);
        aliases.addOperand(first, x); aliases.addOperand(first, y); aliases.addResult(first, z);
        const std::array<OperationId, 1> aliasOps{second};
        const std::array<ValueId, 1> aliasResults{w};
        aliases.setOperationBlockEdges(aliasOps, operandOffsets, aliases.opOperands(first), resultOffsets, aliasResults);
        aliases.rebuildUseLists();
        if (aliases.opOperands(second)[0] != x || aliases.opOperands(second)[1] != y ||
            aliases.valueUsers(x).size() != 2 || aliases.valueUsers(y).size() != 2)
            return fail("block edges did not preserve inputs borrowed from the destination pool");
        aliases.markLocalCyclesChecked();
        aliases.setOpSrcLoc(first, SrcLoc{"proof.fir", 1});
        const std::string firstName(aliases.symbolText(aliases.operationSymbol(first)));
        const std::string xName(aliases.symbolText(aliases.valueSymbol(x)));
        aliases.compact();
        if (!aliases.localCyclesChecked()) return fail("provenance or compact invalidated a local cycle proof");
        aliases.addOperand(aliases.findOperation(firstName), aliases.findValue(xName));
        if (aliases.localCyclesChecked()) return fail("topology mutation retained a stale local cycle proof");
        aliases.compactForFlatten();
        if (!aliases.consumePreparedFlattenFreeze() || aliases.consumePreparedFlattenFreeze())
            return fail("prepared source freeze receipt was lost or consumed twice");
        aliases.setOpSrcLoc(aliases.findOperation(firstName), SrcLoc{"proof.fir", 2});
        aliases.compactForFlatten();
        aliases.setOpSrcLoc(aliases.findOperation(firstName), SrcLoc{"proof.fir", 3});
        if (aliases.consumePreparedFlattenFreeze())
            return fail("mutation retained a stale prepared source freeze receipt");
        return 0;
    }

    int blockExecutorBatches()
    {
        std::promise<void> releaseFirst;
        const auto gate = releaseFirst.get_future().share();
        std::promise<void> secondFinished;
        auto completion = secondFinished.get_future();
        detail::BlockExecutor executor(2);
        executor.submit([gate] {
            gate.wait();
            throw std::runtime_error("first submitted failure");
        });
        executor.submit([] { throw std::runtime_error("second submitted failure"); });
        // The other worker can execute this only after recording the second
        // task's exception; the first task is still blocked on its gate.
        executor.submit([&] { secondFinished.set_value(); });
        completion.wait();
        releaseFirst.set_value();
        std::string exception;
        try { executor.wait(); }
        catch (const std::runtime_error& error) { exception = error.what(); }
        if (exception != "first submitted failure")
            return fail("block executor chose completion order instead of submission order");

        std::array<int, 64> reused{};
        for (std::size_t i = 0; i < reused.size(); ++i)
            executor.submit([&, i] { reused[i] = static_cast<int>(i + 1); });
        executor.wait();
        executor.wait();
        for (std::size_t i = 0; i < reused.size(); ++i)
            if (reused[i] != static_cast<int>(i + 1))
                return fail("block executor did not complete a batch after an exception");

        std::array<int, 64> drained{};
        {
            detail::BlockExecutor ending(3);
            for (std::size_t i = 0; i < drained.size(); ++i) ending.submit([&, i] {
                drained[i] = static_cast<int>(i + 1);
                if (i == 7) throw std::runtime_error("destructor must drain remaining tasks");
            });
        }
        for (std::size_t i = 0; i < drained.size(); ++i)
            if (drained[i] != static_cast<int>(i + 1))
                return fail("block executor destruction left a task incomplete");
        return 0;
    }

    int sharedColumnOwnership()
    {
        using Column = detail::PagedColumn<int>;
        const auto rows = [](std::size_t begin, std::size_t count) {
            std::vector<std::size_t> selected(count);
            for (std::size_t i = 0; i < count; ++i) selected[i] = begin + i;
            return selected;
        };
        Column source;
        source.reserve(4096);
        for (int i = 0; i < 1280; ++i) source.emplace_back(10000 + i);
        const auto* original = &std::as_const(source)[17];
        const auto selected = rows(17, 1024);
        Column target;
        target.appendSharedRowsFrom(source, selected);
        if (target.size() != selected.size() || &std::as_const(target)[0] != original ||
            target.stats().sharedRows != 1024 || target.stats().copiedRows != 0)
            return fail("column import copied a contiguous source allocation or miscounted shared rows");
        target.reserve(100000);
        target.shrink_to_fit();
        if (&std::as_const(target)[0] != original || target.stats().sharedRows != 1024 ||
            target.stats().copiedRows != 0)
            return fail("column reservation or shrink materialized retained pages");
        for (std::size_t i = 0; i < selected.size(); ++i)
            if (std::as_const(target)[i] != 10000 + static_cast<int>(selected[i]))
                return fail("column sharing changed an unaligned source row");

        // A range crossing two source pages can still share their common
        // allocation when the physical offsets are consecutive.
        Column second;
        const auto shifted = rows(5, 512);
        second.appendSharedRowsFrom(target, shifted);
        if (second.stats().sharedRows != 512 || second.stats().copiedRows != 0 ||
            &std::as_const(second)[0] != &std::as_const(target)[5])
            return fail("column import failed to share adjacent pages of the same owner");
        target.writableAt(6) = -6;
        if (std::as_const(source)[23] != 10023 || std::as_const(second)[1] != 10023 ||
            std::as_const(target)[6] != -6 ||
            &std::as_const(target)[300] != &std::as_const(source)[317])
            return fail("column write leaked to another owner or detached unrelated pages");
        second.writableAt(257) = -257;
        if (std::as_const(target)[262] != 10279 || std::as_const(second)[257] != -257)
            return fail("a second retained owner did not detach independently");

        Column mixedOwners;
        const auto crossing = rows(5, 256);
        mixedOwners.appendSharedRowsFrom(target, crossing);
        if (mixedOwners.stats().sharedRows != 0 || mixedOwners.stats().copiedRows != 256)
            return fail("a column page spanning different owners was treated as contiguous storage");
        for (std::size_t i = 0; i < crossing.size(); ++i)
            if (std::as_const(mixedOwners)[i] != std::as_const(target)[crossing[i]])
                return fail("copying a page across different owners lost data");

        source.writableAt(17) = -17;
        source.emplace_back(90001);
        source.reserve(10000);
        source.shrink_to_fit();
        if (source.size() != 1281 || std::as_const(source)[17] != -17 ||
            std::as_const(target)[0] != 10017 || std::as_const(second)[0] != 10022)
            return fail("source mutation or append invalidated retained destination pages");
        target.emplace_back(70000);
        target.resize(1022);
        target.resize(1030);
        for (std::size_t i = 1022; i < 1030; ++i)
            if (std::as_const(target)[i] != 0)
                return fail("paged resize exposed old source rows instead of default values");
        if (std::as_const(target)[6] != -6 || std::as_const(target)[1021] != 11038)
            return fail("paged resize changed retained live rows");

        Column surviving;
        {
            Column temporary;
            for (int i = 0; i < 768; ++i) temporary.emplace_back(20000 + i);
            surviving.appendSharedRowsFrom(temporary, rows(9, 512));
        }
        for (std::size_t i = 0; i < surviving.size(); ++i)
            if (std::as_const(surviving)[i] != 20009 + static_cast<int>(i))
                return fail("source destruction freed a retained column buffer");

        // Force an owned page rather than a directly borrowed source page,
        // then share it twice. Source writes must check storage ownership too.
        Column owned;
        auto permuted = rows(0, 256);
        std::swap(permuted[0], permuted[1]);
        owned.appendSharedRowsFrom(source, permuted);
        Column firstOwner, secondOwner;
        firstOwner.appendSharedRowsFrom(owned, rows(0, 256));
        secondOwner.appendSharedRowsFrom(owned, rows(0, 256));
        owned.writableAt(10) = -10;
        firstOwner.writableAt(11) = -11;
        if (std::as_const(firstOwner)[10] != 10010 || std::as_const(secondOwner)[10] != 10010 ||
            std::as_const(owned)[11] != 10011 || std::as_const(secondOwner)[11] != 10011)
            return fail("an owned page write leaked through multiple retained owners");

        using Small = detail::PagedColumn<int, 8>;
        Small smallSource, prefix;
        for (int i = 0; i < 80; ++i) smallSource.emplace_back(30000 + i);
        prefix.emplace_back(-1); prefix.emplace_back(-2); prefix.emplace_back(-3);
        prefix.appendSharedRowsFrom(smallSource, rows(0, 40));
        if (prefix.stats().sharedRows != 32 || prefix.stats().copiedRows != 8)
            return fail("partial destination pages did not isolate boundary copies");
        const auto* retained = &std::as_const(prefix)[8];
        prefix.reserve(1000);
        prefix.appendSharedRowsFrom(smallSource, rows(40, 23));
        if (prefix.stats().sharedRows != 48 || prefix.stats().copiedRows != 15 ||
            &std::as_const(prefix)[8] != retained)
            return fail("repeated append copied old pages or lost shared-row statistics");
        for (std::size_t i = 0; i < 63; ++i)
            if (std::as_const(prefix)[i + 3] != 30000 + static_cast<int>(i))
                return fail("appending through a partial tail changed prior data");
        if (!expectThrows([&] { prefix.appendSharedRowsFrom(prefix, rows(0, 1)); }) ||
            !expectThrows([&] { prefix.appendSharedRowsFrom(smallSource, rows(80, 1)); }) ||
            !expectThrows([&] { std::as_const(prefix).at(prefix.size()); }))
            return fail("column sharing accepted an invalid source or out-of-range row");
        prefix.resize(0);
        prefix.emplace_back(42);
        Small moved = std::move(prefix);
        if (!prefix.empty() || moved.size() != 1 || std::as_const(moved)[0] != 42)
            return fail("moving a column left invalid source metadata");
        prefix.emplace_back(99);
        if (prefix.size() != 1 || std::as_const(prefix)[0] != 99 || std::as_const(moved)[0] != 42)
            return fail("a moved-from column could not be reused independently");

        Small aliasedAppend;
        {
            Small lastSource;
            for (int i = 0; i < 8; ++i) lastSource.emplace_back(40000 + i);
            aliasedAppend.appendSharedRowsFrom(lastSource, rows(0, 8));
        }
        aliasedAppend.resize(7);
        const int& borrowed = std::as_const(aliasedAppend)[0];
        aliasedAppend.emplace_back(borrowed);
        if (aliasedAppend.size() != 8 || std::as_const(aliasedAppend)[7] != 40000)
            return fail("aliased column append read a detached and freed source page");
        return 0;
    }

    int consumedTopologyOwnership()
    {
        Design design;
        auto &source = design.createGraph("consumed_source");
        auto &target = design.createGraph("consuming_target");
        design.registerGraphAlias("consumed_alias", source);
        const auto deadInput = source.createValue(8, false);
        const auto deadOutput = source.createValue(8, false);
        const auto deadOp = source.createOperation(OperationKind::kAssign);
        source.addOperand(deadOp, deadInput);
        source.addResult(deadOp, deadOutput);
        if (!source.eraseOp(deadOp) || !source.eraseValue(deadInput) || !source.eraseValue(deadOutput))
            return fail("consume fixture could not create old topology garbage");
        const auto a = source.createValue(source.internSymbol("a"), 8, false);
        const auto b = source.createValue(source.internSymbol("b"), 8, false);
        const auto c = source.createValue(source.internSymbol("c"), 8, false);
        const auto d = source.createValue(source.internSymbol("d"), 8, false);
        const auto first = source.createOperation(OperationKind::kAdd, source.internSymbol("first"));
        const auto second = source.createOperation(OperationKind::kAssign, source.internSymbol("second"));
        const auto third = source.createOperation(OperationKind::kAssign, source.internSymbol("third"));
        source.addOperand(first, a);
        source.addOperand(first, a);
        source.addResult(first, b);
        source.addOperand(second, b);
        source.addResult(second, c);
        source.addOperand(third, c);
        source.addResult(third, d);
        // Relocate a live list after other lists have occupied the old tail.
        // Both this discarded slice and the erased op's slots remain unmapped.
        source.reserveOpOperandCapacity(first, 8);
        const auto *firstOperands = source.opOperands(first).data();
        const auto *secondOperands = source.opOperands(second).data();
        const auto *thirdOperands = source.opOperands(third).data();
        const auto *firstResults = source.opResults(first).data();
        const auto *secondResults = source.opResults(second).data();
        const auto *thirdResults = source.opResults(third).data();

        const auto seedValue = target.createValue(target.internSymbol("seed"), 8, false);
        const auto seedOp = target.createOperation(OperationKind::kConstant, target.internSymbol("seed_op"));
        target.addResult(seedOp, seedValue);
        std::vector<ValueId> mapped(static_cast<std::size_t>(d.index) + 1);
        for (const auto value : source.values()) {
            const auto name = "dst_" + std::string(source.symbolText(source.valueSymbol(value)));
            mapped[value.index] = target.createValue(target.internSymbol(name), 8, false);
        }
        const auto targetA = mapped[a.index], targetB = mapped[b.index];
        const auto targetC = mapped[c.index], targetD = mapped[d.index];
        const auto targetFirst = target.createOperation(OperationKind::kAdd, target.internSymbol("dst_first"));
        const auto targetSecond = target.createOperation(OperationKind::kAssign, target.internSymbol("dst_second"));
        const auto targetThird = target.createOperation(OperationKind::kAssign, target.internSymbol("dst_third"));
        const std::array<OperationId, 1> firstSource{first}, firstTarget{targetFirst};
        const std::array<OperationId, 2> tailSource{second, third}, tailTarget{targetSecond, targetThird};
        const std::array<uint32_t, 2> oneResult{0, 1};
        const std::array<uint32_t, 3> twoResults{0, 1, 2};
        const std::array<ValueId, 1> firstOutput{targetB};
        const std::array<ValueId, 2> tailOutput{targetC, targetD};
        target.setOperationBlockResults(firstTarget, oneResult, firstOutput);
        target.setOperationBlockResults(tailTarget, twoResults, tailOutput);
        const std::array<Graph::TopologyBlock, 2> blocks{{{firstSource, firstTarget}, {tailSource, tailTarget}}};
        const auto unchanged = [&] {
            return design.findGraph("consumed_source") == &source &&
                source.opOperands(first).data() == firstOperands &&
                target.opOperands(targetFirst).empty() && target.opOperands(targetSecond).empty() &&
                target.valueDef(targetB) == targetFirst && target.opResults(targetThird)[0] == targetD;
        };
        if (!expectThrows([&] { target.consumeBlockTopology(source, blocks, std::span<const ValueId>(mapped).first(1)); }) || !unchanged())
            return fail("short topology relocation modified or consumed its source");
        auto foreign = mapped;
        foreign[a.index] = a;
        if (!expectThrows([&] { target.consumeBlockTopology(source, blocks, foreign); }) || !unchanged())
            return fail("foreign topology operand target modified or consumed its source");
        const std::array<OperationId, 2> duplicateTarget{targetSecond, targetSecond};
        const std::array<Graph::TopologyBlock, 2> duplicate{{{firstSource, firstTarget}, {tailSource, duplicateTarget}}};
        if (!expectThrows([&] { target.consumeBlockTopology(source, duplicate, mapped); }) || !unchanged())
            return fail("duplicate topology destination modified or consumed its source");
        const std::array<OperationId, 1> deadSource{deadOp};
        const std::array<Graph::TopologyBlock, 1> erased{{{deadSource, firstTarget}}};
        if (!expectThrows([&] { target.consumeBlockTopology(source, erased, mapped); }) || !unchanged())
            return fail("erased topology source operation was consumed");

        // Results keep their already-published target identities even when
        // their source value is absent from operand relocation requirements.
        mapped[d.index] = seedValue;
        target.consumeBlockTopology(source, blocks, mapped);
        if (design.findGraph("consumed_source") || design.findGraph("consumed_alias"))
            return fail("topology consumption did not delete its source and aliases");
        if (target.opOperands(targetFirst).data() != firstOperands ||
            target.opOperands(targetSecond).data() != secondOperands ||
            target.opOperands(targetThird).data() != thirdOperands ||
            target.opResults(targetFirst).data() != firstResults ||
            target.opResults(targetSecond).data() != secondResults ||
            target.opResults(targetThird).data() != thirdResults)
            return fail("topology consumption copied source edge buffers instead of retaining their addresses");
        if (target.opOperands(targetFirst)[0] != targetA || target.opOperands(targetFirst)[1] != targetA ||
            target.opOperands(targetSecond)[0] != targetB || target.opOperands(targetThird)[0] != targetC ||
            target.opResults(targetThird)[0] != targetD || target.valueDef(targetD) != targetThird ||
            target.valueDef(seedValue) != seedOp)
            return fail("consumed topology changed relocated operands or published result definitions");
        target.rebuildUseLists();
        if (target.valueUsers(targetA).size() != 2 || target.valueUsers(targetB).size() != 1)
            return fail("consumed topology did not rebuild duplicate operand uses");
        {
            auto checkpoint = target.checkpointMutation();
            if (target.opOperands(targetFirst).data() == firstOperands)
                return fail("checkpoint retained mutable edge buffers from its preimage");
            target.replaceOperand(targetFirst, 1, seedValue);
            target.addOperand(targetSecond, targetA);
        }
        if (target.opOperands(targetFirst).data() != firstOperands ||
            target.opOperands(targetFirst)[1] != targetA || target.opOperands(targetSecond).size() != 1 ||
            target.valueUsers(targetA).size() != 2 || target.valueUsers(targetB).size() != 1)
            return fail("checkpoint did not restore consumed edge buffers and backlinks");
        target.replaceOperand(targetFirst, 1, seedValue);
        target.addOperand(targetSecond, targetA);
        if (!target.eraseOperand(targetSecond, 1) || target.valueUsers(targetA).size() != 1 ||
            target.valueUsers(seedValue).size() != 1)
            return fail("consumed topology did not support later independent edge mutations");
        target.rebuildUseLists();
        target.compact();
        const auto compactFirst = target.findOperation("dst_first");
        const auto compactSecond = target.findOperation("dst_second");
        const auto compactThird = target.findOperation("dst_third");
        if (target.opOperands(compactFirst)[0] != target.findValue("dst_a") ||
            target.opOperands(compactFirst)[1] != target.findValue("seed") ||
            target.opOperands(compactSecond)[0] != target.findValue("dst_b") ||
            target.opResults(compactThird)[0] != target.findValue("dst_d"))
            return fail("compact lost consumed topology or subsequent mutations");
        return 0;
    }

} // namespace

int main()
{
    try
    {
        if (profileCountersRegression() != 0) return 1;
        if (graphStoreFreezeOracle() != 0) return 1;
        if (compactMutationJsonOracle() != 0) return 1;
        if (freezeOwnership() != 0) return 1;
        if (pooledShapeAndSourceLocation() != 0) return 1;
        if (isolatedPayloadArenasAndReclamation() != 0) return 1;
        if (collidingStringPool() != 0) return 1;
        if (sharedSymbolPayloadStrings() != 0) return 1;
        if (symbolPayloadStateTransitions() != 0) return 1;
        if (cachedSourceLocationImports() != 0) return 1;
        if (blockImportOwnership() != 0) return 1;
        if (compactPackedRanges() != 0) return 1;
        if (shapeSubspanImport() != 0) return 1;
        if (parallelGraphMutation() != 0) return 1;
        if (unpublishedGraphPublication() != 0) return 1;
        if (blockExecutorBatches() != 0) return 1;
        if (sharedColumnOwnership() != 0) return 1;
        if (consumedTopologyOwnership() != 0) return 1;
        if (mutationCheckpoint() != 0) return 1;
        {
            Design ordered;
            auto& last = ordered.createGraph("z");
            const auto id = last.id();
            ordered.createGraph("a");
            ordered.markAsTop("z");
            ordered.registerGraphAlias("alias", last);
            ordered.sortGraphOrder();
            if (ordered.graphOrder() != std::vector<std::string>{"a", "z"} ||
                ordered.findGraph("alias") != &last || last.id() != id ||
                ordered.topGraphs() != std::vector<std::string>{"z"})
                return fail("sorting graph traversal changed graph identity, aliases or tops");
        }
        Design design;
        {
            Graph &generated = design.createGraph("internal_symbols");
            const auto occupied = generated.internSymbol("_op_0");
            const auto reserved = generated.internSymbol("_op_1");
            generated.createOperation(OperationKind::kConstant, occupied);
            const auto op = generated.createOperation(OperationKind::kConstant);
            const auto valReserved = generated.internSymbol("_val_0");
            const auto value = generated.createValue(1, false);
            if (generated.symbolText(generated.getOperation(op).symbol()) != "_op_2" ||
                generated.symbolText(generated.valueSymbol(value)) != "_val_1" ||
                generated.lookupSymbol("_op_1") != reserved ||
                generated.lookupSymbol("_val_0") != valReserved)
                return fail("internal symbol generation changed collision or reservation order");
            design.deleteGraph("internal_symbols");
        }
        Graph &graph = design.createGraph("demo");

        SymbolId symA = graph.internSymbol("a");
        SymbolId symB = graph.internSymbol("b");
        SymbolId symSum = graph.internSymbol("sum");
        SymbolId symSumCopy = graph.internSymbol("sum_copy");
        SymbolId symLane1 = graph.internSymbol("lane1");
        SymbolId symLane0 = graph.internSymbol("lane0");
        SymbolId symPacked = graph.internSymbol("packed_lanes");
        SymbolId symPackedIndex = graph.internSymbol("packed_index");
        SymbolId symPackedOut = graph.internSymbol("packed_out");
        ValueId a = graph.createValue(symA, 8, false);
        ValueId b = graph.createValue(symB, 8, false);
        ValueId sum = graph.createValue(symSum, 8, false);
        ValueId sumCopy = graph.createValue(symSumCopy, 8, false);
        ValueId lane1 = graph.createValue(symLane1, 3, false);
        ValueId lane0 = graph.createValue(symLane0, 3, false);
        const std::vector<int64_t> packedShape{2};
        ValueId packed = graph.createValue(symPacked, 6, false, ValueType::Logic, packedShape);
        ValueId packedIndex = graph.createValue(symPackedIndex, 1, false);
        ValueId packedOut = graph.createValue(symPackedOut, 3, false);

        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);
        graph.bindInputPort("lane1", lane1);
        graph.bindInputPort("lane0", lane0);
        graph.bindInputPort("packed_index", packedIndex);
        graph.bindOutputPort("sum", sum);
        graph.bindOutputPort("sum_copy", sumCopy);
        graph.bindOutputPort("packed_out", packedOut);
        graph.addDeclaredSymbol(symA);
        graph.addDeclaredSymbol(symSum);
        design.addDeclaredSymbol(design.internSymbol("demo"));

        OperationId op = graph.createOperation(OperationKind::kAdd, graph.internSymbol("add0"));
        graph.addOperand(op, a);
        graph.addOperand(op, b);
        graph.addResult(op, sum);

        bool threwOnNaNAttribute = expectThrows([&]
                                                { graph.setAttr(op, "invalid_nan",
                                                                AttributeValue(std::numeric_limits<double>::quiet_NaN())); });
        if (!threwOnNaNAttribute)
        {
            return fail("Expected NaN attribute to throw");
        }

        bool threwOnDoubleArrayInfinity = expectThrows([&]
                                                       { graph.setAttr(op, "invalid_array",
                                                                       AttributeValue(std::vector<double>{0.25, std::numeric_limits<double>::infinity()})); });
        if (!threwOnDoubleArrayInfinity)
        {
            return fail("Expected double array with infinity to throw");
        }

        OperationId assignOp = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign0"));
        graph.addOperand(assignOp, sum);
        graph.addResult(assignOp, sumCopy);
        if (graph.getValue(sumCopy).definingOp() != assignOp)
        {
            return fail("Assign result defining operation not set");
        }

        OperationId packedConcat = graph.createOperation(OperationKind::kConcat, graph.internSymbol("packed_concat"));
        graph.addOperand(packedConcat, lane1);
        graph.addOperand(packedConcat, lane0);
        graph.addResult(packedConcat, packed);

        OperationId packedSlice = graph.createOperation(OperationKind::kSliceArray, graph.internSymbol("packed_slice"));
        graph.addOperand(packedSlice, packed);
        graph.addOperand(packedSlice, packedIndex);
        graph.addResult(packedSlice, packedOut);

        const Value aValue = graph.getValue(a);
        const auto aUsers = aValue.users();
        if (aUsers.size() != 1 || aUsers[0].operation != op)
        {
            return fail("Operand usage tracking failed");
        }
        if (graph.getValue(sum).definingOp() != op)
        {
            return fail("Result defining operation not set");
        }

        ValueId c = graph.createValue(graph.internSymbol("c"), 8, false);
        graph.bindInputPort("c", c);

        graph.replaceOperand(op, 1, c);
        if (graph.getOperation(op).operands()[1] != c)
        {
            return fail("Operand replacement did not update slot");
        }
        if (!graph.getValue(b).users().empty())
        {
            return fail("Old operand still listed as user after replacement");
        }
        const Value cValue = graph.getValue(c);
        const auto cUsers = cValue.users();
        if (cUsers.size() != 1 || cUsers[0].operation != op || cUsers[0].operandIndex != 1)
        {
            return fail("New operand usage tracking incorrect after replacement");
        }

        Graph &auxGraph = design.createGraph("aux");
        ValueId foreignValue = auxGraph.createValue(auxGraph.internSymbol("foreign"), 8, false);
        bool threwOnForeignOperand = expectThrows([&]
                                                  { graph.replaceOperand(op, 0, foreignValue); });
        if (!threwOnForeignOperand)
        {
            return fail("Expected replacing operand with foreign value to throw");
        }

        OperationId passThrough = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign1"));
        graph.addOperand(passThrough, c);
        ValueId passResult = graph.createValue(graph.internSymbol("passthrough"), 8, false);
        graph.addResult(passThrough, passResult);

        ValueId passResultAlt = graph.createValue(graph.internSymbol("passthrough_alt"), 8, false);
        graph.replaceResult(passThrough, 0, passResultAlt);
        if (graph.getValue(passResult).definingOp().valid())
        {
            return fail("Old result still records defining operation after replacement");
        }
        if (graph.getValue(passResultAlt).definingOp() != passThrough)
        {
            return fail("New result defining operation incorrect after replacement");
        }
        if (graph.getOperation(passThrough).results()[0] != passResultAlt)
        {
            return fail("Result replacement did not update slot");
        }

        bool threwOnForeignResult = expectThrows([&]
                                                 { graph.replaceResult(passThrough, 0, foreignValue); });
        if (!threwOnForeignResult)
        {
            return fail("Expected replacing result with foreign value to throw");
        }

        ValueId existingResult = graph.createValue(graph.internSymbol("existing_result"), 8, false);
        OperationId existingProducer = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_existing"));
        graph.addOperand(existingProducer, c);
        graph.addResult(existingProducer, existingResult);

        bool threwOnExistingResult = expectThrows([&]
                                                  { graph.replaceResult(passThrough, 0, existingResult); });
        if (!threwOnExistingResult)
        {
            return fail("Expected replacing result with already-defined value to throw");
        }

        bool threwOnDuplicateValue = expectThrows([&]
                                                  { graph.createValue(graph.internSymbol("a"), 1, false); });
        if (!threwOnDuplicateValue)
        {
            return fail("Expected duplicate value symbol to throw");
        }

        bool threwOnInvalidWidth = expectThrows([&]
                                                { graph.createValue(graph.internSymbol("invalid"), 0, false); });
        if (!threwOnInvalidWidth)
        {
            return fail("Expected zero width to throw");
        }

        design.markAsTop("demo");

        // Generate group annotation: two per-round copies of one declaration.
        const SymbolId symGenScope = graph.internSymbol("gen_loop");
        const SymbolId symGenName = graph.internSymbol("sig");
        const SymbolId symGen0 = graph.internSymbol("gen_loop$0$sig");
        const SymbolId symGen1 = graph.internSymbol("gen_loop$1$sig");
        graph.createValue(symGen0, 8, false);
        graph.createValue(symGen1, 8, false);
        graph.addDeclaredSymbol(symGen0);
        graph.addDeclaredSymbol(symGen1);
        const std::size_t genGroup = graph.addGenerateGroup(symGenScope, symGenName);
        graph.addGenerateGroupSymbol(genGroup, symGen0);
        graph.addGenerateGroupSymbol(genGroup, symGen1);
        // Post-flatten shape: instance prefix folded into the scope, the bare
        // declaration name symbol shared, members renamed hierarchically.
        const SymbolId symHierScope = graph.internSymbol("u_child$gen_loop");
        const SymbolId symHierGen0 = graph.internSymbol("u_child$gen_loop$0$sig");
        const SymbolId symHierGen1 = graph.internSymbol("u_child$gen_loop$1$sig");
        graph.createValue(symHierGen0, 8, false);
        graph.createValue(symHierGen1, 8, false);
        graph.addDeclaredSymbol(symHierGen0);
        graph.addDeclaredSymbol(symHierGen1);
        const std::size_t hierGroup = graph.addGenerateGroup(symHierScope, symGenName);
        graph.addGenerateGroupSymbol(hierGroup, symHierGen0);
        graph.addGenerateGroupSymbol(hierGroup, symHierGen1);
        if (graph.generateGroups().size() != 2 ||
            graph.generateGroups().front().symbols.size() != 2 ||
            graph.generateGroups().back().symbols.size() != 2)
        {
            return fail("Generate group registration failed");
        }

        // eraseOp/eraseValue must drop the erased entity's symbol from every
        // generate group while keeping the (possibly empty) group itself.
        Graph &eraseGraph = design.createGraph("erase_case");
        const SymbolId eraseScope = eraseGraph.internSymbol("gen");
        const SymbolId eraseName = eraseGraph.internSymbol("w");
        const SymbolId eraseSymVal = eraseGraph.internSymbol("gen$0$w");
        const SymbolId eraseSymOp = eraseGraph.internSymbol("gen$1$w");
        const SymbolId eraseSymKeep = eraseGraph.internSymbol("gen$2$w");
        const ValueId eraseVal = eraseGraph.createValue(eraseSymVal, 8, false);
        eraseGraph.createValue(eraseSymKeep, 8, false);
        const OperationId eraseOp =
            eraseGraph.createOperation(OperationKind::kConstant, eraseSymOp);
        eraseGraph.setAttr(eraseOp, "constValue", AttributeValue(std::string("8'h0")));
        eraseGraph.addDeclaredSymbol(eraseSymVal);
        eraseGraph.addDeclaredSymbol(eraseSymOp);
        eraseGraph.addDeclaredSymbol(eraseSymKeep);
        const std::size_t eraseGroup =
            eraseGraph.addGenerateGroup(eraseScope, eraseName);
        eraseGraph.addGenerateGroupSymbol(eraseGroup, eraseSymVal);
        eraseGraph.addGenerateGroupSymbol(eraseGroup, eraseSymOp);
        eraseGraph.addGenerateGroupSymbol(eraseGroup, eraseSymKeep);
        if (!eraseGraph.eraseValue(eraseVal))
        {
            return fail("eraseValue failed for generate group member");
        }
        if (!eraseGraph.eraseOp(eraseOp))
        {
            return fail("eraseOp failed for generate group member");
        }
        if (eraseGraph.isDeclaredSymbol(eraseSymVal) || eraseGraph.isDeclaredSymbol(eraseSymOp))
        {
            return fail("Erased symbols still marked declared");
        }
        if (eraseGraph.generateGroups().size() != 1)
        {
            return fail("Erase removed the generate group itself");
        }
        const auto &eraseGroupSymbols = eraseGraph.generateGroups().front().symbols;
        if (eraseGroupSymbols.size() != 1 || eraseGroupSymbols.front() != eraseSymKeep)
        {
            return fail("Generate group membership not cleaned on erase");
        }
        if (eraseGraph.lookupSymbol("gen$0$w").valid() &&
            eraseGraph.findValue("gen$0$w").valid())
        {
            return fail("Erased value still resolvable");
        }
        if (!eraseGraph.validateDeclaredSymbols().empty())
        {
            return fail("validateDeclaredSymbols reports issues on a consistent graph");
        }

        // Renaming a declared value leaves its old symbol dangling in the
        // annotations; validateDeclaredSymbols must flag every entry.
        {
            Design danglingDesign;
            Graph &danglingGraph = danglingDesign.createGraph("dangling_case");
            const SymbolId danglingSym = danglingGraph.internSymbol("sig");
            const ValueId danglingVal = danglingGraph.createValue(danglingSym, 8, false);
            danglingGraph.addDeclaredSymbol(danglingSym);
            const std::size_t danglingGroup = danglingGraph.addGenerateGroup(
                danglingGraph.internSymbol("gen"), danglingGraph.internSymbol("w"));
            danglingGraph.addGenerateGroupSymbol(danglingGroup, danglingSym);
            if (!danglingGraph.validateDeclaredSymbols().empty())
            {
                return fail("validateDeclaredSymbols false positive before rename");
            }
            danglingGraph.setValueSymbol(danglingVal, danglingGraph.internSymbol("renamed"));
            if (danglingGraph.validateDeclaredSymbols().size() != 2)
            {
                return fail("validateDeclaredSymbols missed dangling entries");
            }
        }

        StoreDiagnostics emitDiagnostics;
        StoreJson emitter(&emitDiagnostics);
        StoreOptions emitOptions;
        auto jsonOpt = emitter.storeToString(design, emitOptions);
        if (!jsonOpt || emitDiagnostics.hasError())
        {
            return fail("Failed to emit JSON for design");
        }
        std::string json = *jsonOpt;

#ifdef GRH_STAGE1_JSON_PATH
        try
        {
            namespace fs = std::filesystem;
            const fs::path artifactPath = fs::path(GRH_STAGE1_JSON_PATH);
            const fs::path artifactDir = artifactPath.parent_path();

            if (!artifactDir.empty())
            {
                std::error_code ec;
                fs::create_directories(artifactDir, ec);
                if (ec)
                {
                    return fail(std::string("Failed to create artifact directory: ") + ec.message());
                }
            }

            std::ofstream outFile(artifactPath);
            if (!outFile)
            {
                return fail("Failed to open artifact file for writing");
            }
            outFile << json;
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Failed to persist GRH stage1 JSON: ") + ex.what());
        }
#endif

        Design parsed = Design::fromJsonString(json);
        if (parsed.topGraphs().size() != 1 || parsed.topGraphs()[0] != "demo")
        {
            return fail("Top graph round-trip failed");
        }

        const Graph *parsedGraph = parsed.findGraph("demo");
        if (!parsedGraph)
        {
            return fail("Parsed graph missing");
        }
        SymbolId parsedSymA = parsedGraph->lookupSymbol("a");
        if (!parsedSymA.valid() || !parsedGraph->isDeclaredSymbol(parsedSymA))
        {
            return fail("Declared symbol not preserved in graph");
        }
        SymbolId parsedModuleSym = parsed.lookupSymbol("demo");
        if (!parsedModuleSym.valid() || !parsed.isDeclaredSymbol(parsedModuleSym))
        {
            return fail("Declared symbol not preserved in design");
        }

        if (parsedGraph->generateGroups().size() != 2)
        {
            return fail("Generate group not preserved in graph");
        }
        const auto &parsedGroup = parsedGraph->generateGroups().front();
        if (parsedGraph->symbolText(parsedGroup.scope) != "gen_loop" ||
            parsedGraph->symbolText(parsedGroup.name) != "sig" ||
            parsedGroup.symbols.size() != 2 ||
            parsedGraph->symbolText(parsedGroup.symbols[0]) != "gen_loop$0$sig" ||
            parsedGraph->symbolText(parsedGroup.symbols[1]) != "gen_loop$1$sig")
        {
            return fail("Generate group content mismatch after round-trip");
        }
        const auto &parsedHierGroup = parsedGraph->generateGroups().back();
        if (parsedGraph->symbolText(parsedHierGroup.scope) != "u_child$gen_loop" ||
            parsedGraph->symbolText(parsedHierGroup.name) != "sig" ||
            parsedHierGroup.symbols.size() != 2 ||
            parsedGraph->symbolText(parsedHierGroup.symbols[0]) != "u_child$gen_loop$0$sig" ||
            parsedGraph->symbolText(parsedHierGroup.symbols[1]) != "u_child$gen_loop$1$sig")
        {
            return fail("Hierarchical generate group content mismatch after round-trip");
        }
        const Graph *parsedEraseGraph = parsed.findGraph("erase_case");
        if (!parsedEraseGraph || parsedEraseGraph->generateGroups().size() != 1 ||
            parsedEraseGraph->generateGroups().front().symbols.size() != 1 ||
            parsedEraseGraph->symbolText(parsedEraseGraph->generateGroups().front().symbols[0]) !=
                "gen$2$w")
        {
            return fail("Generate group erase cleanup not preserved after round-trip");
        }
        if (json.find("\"generateGroups\"") == std::string::npos)
        {
            return fail("Emitted JSON missing generateGroups field");
        }

        // Compact mode writes through Graph::writeJson; groups must survive too.
        emitDiagnostics.clear();
        StoreOptions compactOptions;
        compactOptions.jsonMode = JsonPrintMode::Compact;
        auto compactJsonOpt = emitter.storeToString(design, compactOptions);
        if (!compactJsonOpt || emitDiagnostics.hasError())
        {
            return fail("Failed to emit compact JSON for design");
        }
        if (compactJsonOpt->find("\"generateGroups\"") == std::string::npos)
        {
            return fail("Compact JSON missing generateGroups field");
        }
        Design compactParsed = Design::fromJsonString(*compactJsonOpt);
        const Graph *compactGraph = compactParsed.findGraph("demo");
        if (!compactGraph || compactGraph->generateGroups().size() != 2 ||
            compactGraph->generateGroups().front().symbols.size() != 2 ||
            compactGraph->generateGroups().back().symbols.size() != 2)
        {
            return fail("Generate group not preserved in compact round-trip");
        }

        // generateGroups is optional in v5.
        const std::string legacyJson = R"({
  "formatVersion": 5,
  "graphs": [
    {
      "symbol": "legacy",
      "dialects": {"grh": 2},
      "metadata": {},
      "declaredSymbols": [],
      "vals": [],
      "ports": {
        "in": [],
        "out": [],
        "inout": []
      },
      "ops": []
    }
  ],
  "declaredSymbols": [],
  "tops": ["legacy"]
})";
        Design legacy = Design::fromJsonString(legacyJson);
        const Graph *legacyGraph = legacy.findGraph("legacy");
        if (!legacyGraph)
        {
            return fail("Legacy JSON without generateGroups failed to load");
        }
        if (!legacyGraph->generateGroups().empty())
        {
            return fail("Legacy JSON produced unexpected generate groups");
        }

        const OperationId parsedOpId = parsedGraph->findOperation("add0");
        if (!parsedOpId.valid())
        {
            return fail("Parsed operation missing");
        }
        const Operation parsedOp = parsedGraph->getOperation(parsedOpId);
        if (parsedOp.operands().size() != 2 || parsedOp.results().size() != 1)
        {
            return fail("Parsed operation connectivity mismatch");
        }

        const OperationId parsedAssignId = parsedGraph->findOperation("assign0");
        if (!parsedAssignId.valid())
        {
            return fail("Parsed assign operation missing");
        }
        const Operation parsedAssign = parsedGraph->getOperation(parsedAssignId);
        if (parsedAssign.kind() != OperationKind::kAssign)
        {
            return fail("Assign operation kind mismatch");
        }
        if (parsedAssign.operands().size() != 1 || parsedAssign.results().size() != 1)
        {
            return fail("Assign operation connectivity mismatch");
        }

        const OperationId parsedPackedConcatId = parsedGraph->findOperation("packed_concat");
        if (!parsedPackedConcatId.valid())
        {
            return fail("Parsed packed concat operation missing");
        }
        const Operation parsedPackedConcat = parsedGraph->getOperation(parsedPackedConcatId);
        const OperationId parsedPackedSliceId = parsedGraph->findOperation("packed_slice");
        if (!parsedPackedSliceId.valid())
        {
            return fail("Parsed packed slice operation missing");
        }
        const Operation parsedPackedSlice = parsedGraph->getOperation(parsedPackedSliceId);
        const wolvrix::lib::grh::ValueId parsedPackedBase = parsedPackedSlice.operands().front();
        const auto parsedBaseShape = parsedGraph->valueShape(parsedPackedBase);
        if (parsedPackedSlice.kind() != OperationKind::kSliceArray ||
            parsedPackedSlice.operands().size() != 2 ||
            parsedPackedSlice.results().size() != 1 ||
            parsedBaseShape.size() != 1 || parsedBaseShape.front() != 2)
        {
            return fail("Packed kSliceArray did not round-trip");
        }

        auto parsedAssignKind = parseOperationKind("kAssign");
        if (!parsedAssignKind || *parsedAssignKind != OperationKind::kAssign)
        {
            return fail("Failed to parse kAssign operation kind");
        }

        emitDiagnostics.clear();
        auto jsonAgainOpt = emitter.storeToString(parsed, emitOptions);
        if (!jsonAgainOpt || emitDiagnostics.hasError())
        {
            return fail("Failed to re-emit JSON after parse");
        }
        std::string jsonAgain = *jsonAgainOpt;
        if (json != jsonAgain)
        {
            return fail("JSON serialization not stable");
        }

        const std::string nestedArrayJson = R"({
  "formatVersion": 5,
  "graphs": [
    {
      "symbol": "illegal_nested_array",
      "dialects": {"grh": 2},
      "metadata": {},
      "declaredSymbols": [],
      "vals": [],
      "ports": {
        "in": [],
        "out": [],
        "inout": []
      },
      "ops": [
        {
          "id": 1,
          "sym": "bad",
          "kind": "kAdd",
          "in": [],
          "out": [],
          "attrs": {
            "illegal": {
              "t": "int[]",
              "vs": [
                1,
                [2, 3]
              ]
            }
          }
        }
      ]
    }
  ],
  "declaredSymbols": [],
  "tops": []
})";
        bool threwOnNestedArrayAttribute = expectThrows([&]
                                                        { Design::fromJsonString(nestedArrayJson); });
        if (!threwOnNestedArrayAttribute)
        {
            return fail("Expected nested array attribute to throw during parse");
        }

        const std::string objectAttributeJson = R"({
  "formatVersion": 5,
  "graphs": [
    {
      "symbol": "illegal_object_attr",
      "dialects": {"grh": 2},
      "metadata": {},
      "declaredSymbols": [],
      "vals": [],
      "ports": {
        "in": [],
        "out": [],
        "inout": []
      },
      "ops": [
        {
          "id": 1,
          "sym": "bad_obj",
          "kind": "kAdd",
          "in": [],
          "out": [],
          "attrs": {
            "illegal": {
              "t": "string",
              "v": {
                "unexpected": "object"
              }
            }
          }
        }
      ]
    }
  ],
  "declaredSymbols": [],
  "tops": []
})";
        bool threwOnObjectAttribute = expectThrows([&]
                                                   { Design::fromJsonString(objectAttributeJson); });
        if (!threwOnObjectAttribute)
        {
            return fail("Expected object attribute to throw during parse");
        }

        {
            namespace grh_ir = wolvrix::lib::grh;

            grh_ir::GraphSymbolTable arenaSymbols;
            const auto shortSym = arenaSymbols.intern("short");
            const std::string longName(10000, 's');
            const auto longSym = arenaSymbols.intern(longName);
            const auto shortText = arenaSymbols.text(shortSym);
            const auto longText = arenaSymbols.text(longSym);
            const auto* shortAddress = shortText.data();
            const auto* longAddress = longText.data();
            for (int index = 0; index < 4096; ++index)
                arenaSymbols.intern("arena-growth-" + std::to_string(index));
            if (shortText != "short" || longText != longName ||
                arenaSymbols.text(shortSym).data() != shortAddress ||
                arenaSymbols.text(longSym).data() != longAddress ||
                arenaSymbols.lookup("short") != shortSym || arenaSymbols.lookup(longName) != longSym)
                return fail("SymbolTable arena growth invalidated borrowed symbol text");
            auto copiedSymbols = arenaSymbols;
            arenaSymbols = GraphSymbolTable{};
            if (copiedSymbols.lookup("short") != shortSym || copiedSymbols.text(longSym) != longName)
                return fail("SymbolTable copied indexes borrowed another table's arena");
            GraphSymbolTable assignedSymbols;
            assignedSymbols = copiedSymbols;
            auto movedSymbols = std::move(copiedSymbols);
            if (assignedSymbols.text(longSym) != longName || movedSymbols.lookup(longName) != longSym)
                return fail("SymbolTable copy/move changed symbol IDs or text");

            grh_ir::DesignSymbolTable designSymbols;
            auto demoSym = designSymbols.intern("demo");
            if (!designSymbols.valid(demoSym))
            {
                return fail("DesignSymbolTable did not mark interned symbol as valid");
            }
            if (!designSymbols.contains("demo"))
            {
                return fail("DesignSymbolTable contains() failed for interned symbol");
            }
            auto demoLookup = designSymbols.lookup("demo");
            if (!demoLookup.valid() || demoLookup != demoSym)
            {
                return fail("DesignSymbolTable lookup failed for interned symbol");
            }
            if (designSymbols.text(demoSym) != "demo")
            {
                return fail("DesignSymbolTable text() mismatch");
            }
            if (designSymbols.lookup("missing").valid())
            {
                return fail("DesignSymbolTable lookup should miss unknown symbol");
            }
            auto dupSym = designSymbols.intern("demo");
            if (!dupSym.valid() || dupSym != demoSym)
            {
                return fail("Expected duplicate intern to return existing SymbolId");
            }
            if (designSymbols.valid(grh_ir::SymbolId::invalid()))
            {
                return fail("Invalid SymbolId reported as valid");
            }
            bool threwOnInvalidText = expectThrows([&]
                                                   { designSymbols.text(grh_ir::SymbolId::invalid()); });
            if (!threwOnInvalidText)
            {
                return fail("Expected invalid SymbolId text() to throw");
            }

            grh_ir::GraphSymbolTable graphSymbols;
            auto valueSym = graphSymbols.intern("value0");
            if (!graphSymbols.contains("value0") || graphSymbols.text(valueSym) != "value0")
            {
                return fail("GraphSymbolTable did not roundtrip symbol");
            }

            grh_ir::GraphId graphA{1, 0};
            grh_ir::GraphId graphB{2, 0};
            if (!graphA.valid() || grh_ir::GraphId::invalid().valid())
            {
                return fail("GraphId valid/invalid check failed");
            }

            grh_ir::ValueId valueId{1, 0, graphA};
            if (!valueId.valid())
            {
                return fail("ValueId valid check failed");
            }
            bool threwOnCrossGraphValue = expectThrows([&]
                                                       { valueId.assertGraph(graphB); });
            if (!threwOnCrossGraphValue)
            {
                return fail("Expected ValueId cross-graph assertion to throw");
            }
            bool threwOnInvalidValue = expectThrows([&]
                                                    { grh_ir::ValueId::invalid().assertGraph(graphA); });
            if (!threwOnInvalidValue)
            {
                return fail("Expected invalid ValueId to throw on assertGraph");
            }

            grh_ir::OperationId opId{1, 0, graphA};
            bool threwOnCrossGraphOp = expectThrows([&]
                                                    { opId.assertGraph(graphB); });
            if (!threwOnCrossGraphOp)
            {
                return fail("Expected OperationId cross-graph assertion to throw");
            }
        }

        {
            namespace grh_ir = wolvrix::lib::grh;

            grh_ir::Design fixtureDesign;
            grh_ir::Graph& builder = fixtureDesign.createGraph("fixture");

            auto symA = builder.internSymbol("a");
            auto symB = builder.internSymbol("b");
            auto symSum = builder.internSymbol("sum");
            auto symOut = builder.internSymbol("out");
            auto symAdd = builder.internSymbol("add0");
            auto symAssign = builder.internSymbol("assign0");

            auto vA = builder.createValue(symA, 1, false);
            auto vB = builder.createValue(symB, 1, false);
            auto vSum = builder.createValue(symSum, 1, false);
            auto vOut = builder.createValue(symOut, 1, false);

            auto opAdd = builder.createOperation(OperationKind::kAdd, symAdd);
            builder.addOperand(opAdd, vA);
            builder.addOperand(opAdd, vB);
            builder.addResult(opAdd, vSum);

            auto opAssign = builder.createOperation(OperationKind::kAssign, symAssign);
            builder.addOperand(opAssign, vSum);
            builder.addResult(opAssign, vOut);

            builder.compact();
            const grh_ir::Graph& view = builder;

            auto values = view.values();
            if (values.size() != 4 || values[0] != vA || values[1] != vB || values[2] != vSum || values[3] != vOut)
            {
                return fail("Graph value order mismatch");
            }

            auto ops = view.operations();
            if (ops.size() != 2 || ops[0] != opAdd || ops[1] != opAssign)
            {
                return fail("Graph operation order mismatch");
            }

            auto addOperands = view.opOperands(opAdd);
            if (addOperands.size() != 2 || addOperands[0] != vA || addOperands[1] != vB)
            {
                return fail("Graph operand range for add op mismatch");
            }
            auto addResults = view.opResults(opAdd);
            if (addResults.size() != 1 || addResults[0] != vSum)
            {
                return fail("Graph result range for add op mismatch");
            }

            auto assignOperands = view.opOperands(opAssign);
            if (assignOperands.size() != 1 || assignOperands[0] != vSum)
            {
                return fail("Graph operand range for assign op mismatch");
            }
            auto assignResults = view.opResults(opAssign);
            if (assignResults.size() != 1 || assignResults[0] != vOut)
            {
                return fail("Graph result range for assign op mismatch");
            }

            if (view.valueDef(vSum) != opAdd)
            {
                return fail("Graph valueDef mismatch for sum");
            }
            if (view.valueDef(vA).valid() || view.valueDef(vB).valid())
            {
                return fail("Graph valueDef should be invalid for non-results");
            }
            if (view.valueDef(vOut) != opAssign)
            {
                return fail("Graph valueDef mismatch for output value");
            }
            if (view.valueWidth(vSum) != 1 || view.valueWidth(vOut) != 1)
            {
                return fail("Graph valueWidth mismatch");
            }

            auto usersA = view.valueUsers(vA);
            if (usersA.size() != 1 || usersA[0].operation != opAdd || usersA[0].operandIndex != 0)
            {
                return fail("Graph useList mismatch for value a");
            }
            auto usersB = view.valueUsers(vB);
            if (usersB.size() != 1 || usersB[0].operation != opAdd || usersB[0].operandIndex != 1)
            {
                return fail("Graph useList mismatch for value b");
            }
            auto usersSum = view.valueUsers(vSum);
            if (usersSum.size() != 1 || usersSum[0].operation != opAssign || usersSum[0].operandIndex != 0)
            {
                return fail("Graph useList mismatch for value sum");
            }
            if (!view.valueUsers(vOut).empty())
            {
                return fail("Graph useList should be empty for output value");
            }
        }

        {
            namespace grh_ir = wolvrix::lib::grh;

            grh_ir::Design fixtureDesign;
            grh_ir::Graph& builder = fixtureDesign.createGraph("fixture");

            auto symPortA = std::string("a");
            auto symPortOut = std::string("out");
            auto symVa = builder.internSymbol("va");
            auto symVb = builder.internSymbol("vb");
            auto symSum = builder.internSymbol("sum");
            auto symOutVal = builder.internSymbol("out_val");
            auto symOutTmp = builder.internSymbol("_val_test_out_tmp");
            auto symAdd = builder.internSymbol("add0");
            auto symAssign = builder.internSymbol("assign0");
            auto symAssignTmp = builder.internSymbol("_op_test_assign_tmp");
            auto vA = builder.createValue(symVa, 1, false);
            auto vB = builder.createValue(symVb, 1, false);
            auto vSum = builder.createValue(symSum, 1, false);
            auto vOut = builder.createValue(symOutTmp, 1, false);

            auto opAdd = builder.createOperation(OperationKind::kAdd, symAdd);
            builder.addOperand(opAdd, vA);
            builder.addOperand(opAdd, vB);
            builder.addResult(opAdd, vSum);

            auto opAssign = builder.createOperation(OperationKind::kAssign, symAssignTmp);
            builder.setOpSymbol(opAssign, symAssign);
            builder.addOperand(opAssign, vSum);
            builder.addResult(opAssign, vOut);

            builder.setValueSymbol(vOut, symOutVal);

            builder.bindInputPort("b", vB);
            builder.bindInputPort(symPortA, vA);
            builder.bindOutputPort(symPortOut, vOut);

            builder.setAttr(opAdd, "delay", AttributeValue(int64_t(5)));
            builder.setAttr(opAdd, "flag", AttributeValue(true));
            builder.setAttr(opAdd, "ratio", AttributeValue(1.25));
            builder.setAttr(opAdd, "label", AttributeValue(std::string("node")));
            builder.setAttr(opAdd, "flags", AttributeValue(std::vector<bool>{true, false}));
            builder.setAttr(opAdd, "counts", AttributeValue(std::vector<int64_t>{2, 3}));
            builder.setAttr(opAdd, "ratios", AttributeValue(std::vector<double>{2.5, 3.5}));
            builder.setAttr(opAdd, "labels", AttributeValue(std::vector<std::string>{"a", "b"}));

            SrcLoc opLoc;
            opLoc.file = "demo.sv";
            opLoc.line = 10;
            builder.setOpSrcLoc(opAdd, opLoc);

            SrcLoc valueLoc;
            valueLoc.file = "demo.sv";
            valueLoc.line = 12;
            builder.setValueSrcLoc(vA, valueLoc);

            builder.compact();
            const grh_ir::Graph& view = builder;

            auto inPorts = view.inputPorts();
            if (inPorts.size() != 2 || inPorts[0].name != "a" || inPorts[1].name != "b")
            {
                return fail("Graph input port ordering mismatch");
            }
            auto outPorts = view.outputPorts();
            if (outPorts.size() != 1 || outPorts[0].name != "out")
            {
                return fail("Graph output port ordering mismatch");
            }

            if (view.operationSymbol(opAdd) != symAdd || view.operationSymbol(opAssign) != symAssign)
            {
                return fail("Graph opSymbol mismatch");
            }
            if (view.valueSymbol(vOut) != symOutVal)
            {
                return fail("Graph valueSymbol mismatch");
            }

            auto attr = view.getOperation(opAdd).attr("delay");
            if (!attr || std::get<int64_t>(*attr) != 5)
            {
                return fail("Graph opAttr lookup mismatch");
            }
            if (view.opAttrRange(opAdd).size() != 8)
            {
                return fail("Graph opAttrs size mismatch");
            }
            const auto viewAttrs = view.opAttrRange(opAdd);
            const auto delayRef = viewAttrs.find("delay");
            if (!delayRef || delayRef->key() != "delay" || delayRef->value().kind() != AttrValueKind::Int ||
                delayRef->value().asInt() != 5)
            {
                return fail("Graph AttrRange lookup mismatch");
            }
            const auto checkAttrKind = [&](std::string_view key, AttrValueKind kind) {
                const auto found = viewAttrs.find(key);
                return found && found->value().kind() == kind;
            };
            if (!checkAttrKind("flag", AttrValueKind::Bool) ||
                !checkAttrKind("ratio", AttrValueKind::Double) ||
                !checkAttrKind("label", AttrValueKind::String) ||
                !checkAttrKind("flags", AttrValueKind::BoolList) ||
                !checkAttrKind("counts", AttrValueKind::IntList) ||
                !checkAttrKind("ratios", AttrValueKind::DoubleList) ||
                !checkAttrKind("labels", AttrValueKind::StringList) ||
                viewAttrs.find("missing"))
            {
                return fail("Graph AttrRange did not expose every AttributeValue branch");
            }
            if (!viewAttrs.find("flag")->value().asBool() ||
                viewAttrs.find("ratio")->value().asDouble() != 1.25 ||
                viewAttrs.find("label")->value().asString() != "node" ||
                viewAttrs.find("flags")->value().asBoolList().size() != 2 ||
                viewAttrs.find("counts")->value().asIntList().size() != 2 ||
                viewAttrs.find("ratios")->value().asDoubleList().size() != 2 ||
                viewAttrs.find("labels")->value().asStringList().size() != 2)
            {
                return fail("Graph AttrRange value access mismatch");
            }

            auto opSrcLoc = view.opSrcLoc(opAdd);
            if (!opSrcLoc || opSrcLoc->line != 10)
            {
                return fail("Graph opSrcLoc mismatch");
            }
            auto valSrcLoc = view.valueSrcLoc(vA);
            if (!valSrcLoc || valSrcLoc->line != 12)
            {
                return fail("Graph valueSrcLoc mismatch");
            }

            if (!view.valueIsInput(vA) || view.valueIsInput(vSum) || !view.valueIsOutput(vOut))
            {
                return fail("Graph port flags mismatch");
            }
        }

        {
            namespace grh_ir = wolvrix::lib::grh;

            grh_ir::Design fixtureDesign;
            grh_ir::Graph& builder = fixtureDesign.createGraph("fixture");

            auto symX = builder.internSymbol("x");
            auto symY = builder.internSymbol("y");
            auto symTmp = builder.internSymbol("tmp");
            auto symOut = builder.internSymbol("out");
            auto symAlt = builder.internSymbol("alt");
            auto symAdd = builder.internSymbol("add0");
            auto symAssign = builder.internSymbol("assign0");

            auto vX = builder.createValue(symX, 1, false);
            auto vY = builder.createValue(symY, 1, false);
            auto vTmp = builder.createValue(symTmp, 1, false);
            auto vOut = builder.createValue(symOut, 1, false);
            auto vAlt = builder.createValue(symAlt, 1, false);

            auto opAdd = builder.createOperation(OperationKind::kAdd, symAdd);
            builder.addOperand(opAdd, vX);
            builder.addOperand(opAdd, vY);
            builder.addResult(opAdd, vTmp);

            auto opAssign = builder.createOperation(OperationKind::kAssign, symAssign);
            builder.addOperand(opAssign, vTmp);
            builder.addResult(opAssign, vOut);

            builder.replaceOperand(opAdd, 1, vX);
            builder.replaceAllUses(vTmp, vX);
            builder.replaceResult(opAssign, 0, vAlt);

            builder.compact();
            const grh_ir::Graph& view = builder;
            auto addOperands = view.opOperands(opAdd);
            if (addOperands.size() != 2 || addOperands[0] != vX || addOperands[1] != vX)
            {
                return fail("Graph replaceOperand mismatch");
            }
            auto assignOperands = view.opOperands(opAssign);
            if (assignOperands.size() != 1 || assignOperands[0] != vX)
            {
                return fail("Graph replaceAllUses mismatch");
            }
            auto assignResults = view.opResults(opAssign);
            if (assignResults.size() != 1 || assignResults[0] != vAlt)
            {
                return fail("Graph replaceResult mismatch");
            }
            if (view.valueDef(vOut).valid())
            {
                return fail("Graph replaceResult did not clear old definition");
            }
        }

        {
            namespace grh_ir = wolvrix::lib::grh;

            grh_ir::Design fixtureDesign;
            grh_ir::Graph& builder = fixtureDesign.createGraph("fixture");

            auto symA = builder.internSymbol("a");
            auto symB = builder.internSymbol("b");
            auto symSum = builder.internSymbol("sum");
            auto symOut = builder.internSymbol("out");
            auto symDead = builder.internSymbol("dead");
            auto symAdd = builder.internSymbol("add0");
            auto symAssign = builder.internSymbol("assign0");
            auto symDeadOp = builder.internSymbol("dead_op");

            auto vA = builder.createValue(symA, 1, false);
            auto vB = builder.createValue(symB, 1, false);
            auto vSum = builder.createValue(symSum, 1, false);
            auto vOut = builder.createValue(symOut, 1, false);
            auto vDead = builder.createValue(symDead, 1, false);

            auto opAdd = builder.createOperation(OperationKind::kAdd, symAdd);
            builder.addOperand(opAdd, vA);
            builder.addOperand(opAdd, vB);
            builder.addResult(opAdd, vSum);

            auto opAssign = builder.createOperation(OperationKind::kAssign, symAssign);
            builder.addOperand(opAssign, vSum);
            builder.addResult(opAssign, vOut);

            auto opDead = builder.createOperation(OperationKind::kAssign, symDeadOp);
            builder.addOperand(opDead, vA);
            builder.addResult(opDead, vDead);

            if (!builder.eraseOperand(opAdd, 1))
            {
                return fail("Graph eraseOperand failed");
            }

            if (!builder.eraseResult(opDead, 0))
            {
                return fail("Graph eraseResult failed");
            }
            if (!builder.eraseValue(vDead))
            {
                return fail("Graph eraseValue failed");
            }

            std::array<grh_ir::ValueId, 1> replacements = {vA};
            if (!builder.eraseOp(opAdd, replacements))
            {
                return fail("Graph eraseOp with replacement failed");
            }

            builder.compact();
            const grh_ir::Graph& view = builder;
            auto assignOperands = view.opOperands(view.findOperation("assign0"));
            if (assignOperands.size() != 1 || assignOperands[0] != view.findValue("a"))
            {
                return fail("Graph eraseOp replacement did not update uses");
            }
        }

        {
            Graph &symbolGraph = design.createGraph("symbol_checks");
            SymbolId sym = symbolGraph.internSymbol("dup_symbol");
            ValueId val = symbolGraph.createValue(sym, 1, false);
            (void)val;

            if (symbolGraph.internSymbol("dup_symbol").valid())
            {
                return fail("internSymbol should return invalid for bound symbol");
            }
            if (!symbolGraph.lookupSymbol("dup_symbol").valid())
            {
                return fail("lookupSymbol should resolve existing symbol");
            }

            bool threwInvalid = expectThrows([&]
                                             { symbolGraph.createValue(SymbolId::invalid(), 1, false); });
            if (!threwInvalid)
            {
                return fail("Expected createValue to throw on invalid SymbolId");
            }

            bool threwConflict = expectThrows([&]
                                              { symbolGraph.createOperation(OperationKind::kAdd, sym); });
            if (!threwConflict)
            {
                return fail("Expected createOperation to throw on bound SymbolId");
            }

            ValueId portValue = symbolGraph.createValue(symbolGraph.internSymbol("port_value"), 1, false);
            symbolGraph.bindInputPort("port_only", portValue);
            if (symbolGraph.lookupSymbol("port_only").valid())
            {
                return fail("Port name should not be interned into symbol table");
            }
            if (symbolGraph.findValue("port_only").valid())
            {
                return fail("Port name should not resolve as a value symbol");
            }
        }

        {
            // Temporary entities carry topology and type information without
            // reserving a semantic symbol.  Their display names are only a
            // serialization aid and must not become SymbolTable entries.
            Design temporaryDesign;
            Graph &temporaryGraph = temporaryDesign.createGraph("temporary");
            temporaryDesign.markAsTop("temporary");
            const auto semanticValue = temporaryGraph.createValue(
                temporaryGraph.internSymbol("semantic"), 1, false);
            const auto temporaryValue = temporaryGraph.createTemporaryValue(8, false);
            const auto temporaryResult = temporaryGraph.createTemporaryValue(8, false);
            const auto firstTemporaryOp = temporaryGraph.createTemporaryOperation(OperationKind::kAdd);
            const auto secondTemporaryOp = temporaryGraph.createTemporaryOperation(OperationKind::kAssign);
            temporaryGraph.addOperand(secondTemporaryOp, temporaryValue);
            temporaryGraph.addResult(secondTemporaryOp, temporaryResult);
            temporaryGraph.addOperand(firstTemporaryOp, temporaryValue);
            temporaryGraph.addOperand(firstTemporaryOp, semanticValue);

            const auto temporaryValueName = temporaryGraph.valueDisplayName(temporaryValue);
            const auto temporaryOpName = temporaryGraph.operationDisplayName(firstTemporaryOp);
            if (temporaryValueName.empty() || temporaryOpName.empty() ||
                temporaryGraph.valueSymbol(temporaryValue).valid() ||
                temporaryGraph.operationSymbol(firstTemporaryOp).valid() ||
                temporaryGraph.lookupSymbol(temporaryValueName).valid() ||
                temporaryGraph.lookupSymbol(temporaryOpName).valid() ||
                temporaryGraph.findValue(temporaryValueName).valid() ||
                temporaryGraph.findOperation(temporaryOpName).valid())
            {
                return fail("Temporary entity unexpectedly entered the symbol table");
            }
            if (!temporaryGraph.valueSymbol(semanticValue).valid() ||
                temporaryGraph.symbolText(temporaryGraph.valueSymbol(semanticValue)) != "semantic")
            {
                return fail("Semantic value lost its symbol beside temporary entities");
            }

            StoreJson temporaryStore;
            StoreOptions temporaryOptions;
            temporaryOptions.jsonMode = JsonPrintMode::PrettyCompact;
            temporaryOptions.emitDefUsers = true;
            const auto temporaryJson = temporaryStore.storeToString(temporaryDesign, temporaryOptions);
            if (!temporaryJson || temporaryJson->find("\"formatVersion\": 5") == std::string::npos ||
                temporaryJson->find("\"tmp\"") != std::string::npos ||
                temporaryJson->find("tmp_val_") != std::string::npos ||
                temporaryJson->find("tmp_op_") != std::string::npos)
            {
                return fail("Temporary entities acquired a JSON symbol or legacy temporary marker");
            }
            const auto usersStart = temporaryJson->find("\"users\"");
            const auto firstUser = usersStart == std::string::npos ? std::string::npos :
                temporaryJson->find("\"op\": 1", usersStart);
            const auto secondUser = usersStart == std::string::npos ? std::string::npos :
                temporaryJson->find("\"op\": 2", usersStart);
            if (usersStart == std::string::npos || firstUser == std::string::npos ||
                secondUser == std::string::npos || firstUser > secondUser)
            {
                return fail("JSON use-list was not sorted by operation ID and operand index");
            }

            Design temporaryRoundTrip = Design::fromJsonString(*temporaryJson);
            const Graph *roundTripGraph = temporaryRoundTrip.findGraph("temporary");
            if (!roundTripGraph)
            {
                return fail("Temporary graph missing after JSON load");
            }
            const auto roundTripValue = roundTripGraph->values()[1];
            const auto roundTripOp = roundTripGraph->operations()[0];
            if (roundTripGraph->valueSymbol(roundTripValue).valid() ||
                roundTripGraph->operationSymbol(roundTripOp).valid())
            {
                return fail("JSON load materialized temporary symbols");
            }
            const auto temporaryJsonAgain = temporaryStore.storeToString(temporaryRoundTrip, temporaryOptions);
            if (!temporaryJsonAgain || *temporaryJsonAgain != *temporaryJson)
            {
                return fail("Temporary JSON store/load/store was not deterministic");
            }
            if (temporaryRoundTrip.findGraph("temporary")->lookupSymbol("semantic").valid() == false ||
                temporaryRoundTrip.findGraph("temporary")->lookupSymbol("tmp_val_2").valid() ||
                temporaryRoundTrip.findGraph("temporary")->lookupSymbol("tmp_op_3").valid())
            {
                return fail("Temporary JSON round-trip changed semantic symbol table");
            }

            temporaryGraph.materializeInternalSymbols();
            if (!temporaryGraph.valueSymbol(temporaryValue).valid() ||
                !temporaryGraph.operationSymbol(firstTemporaryOp).valid() ||
                temporaryGraph.findValue(temporaryValueName) != temporaryValue ||
                temporaryGraph.findOperation(temporaryOpName) != firstTemporaryOp)
            {
                return fail("Temporary symbol materialization failed at downstream boundary");
            }
        }

        {
            GraphPayload store;
            const auto intRecord = store.internAttribute("int", int64_t(-3));
            const auto boolRecord = store.internAttribute("bool", true);
            const auto doubleRecord = store.internAttribute("double", 1.5);
            const auto stringRecord = store.internAttribute("string", std::string("payload"));
            const auto boolListRecord = store.internAttribute("bools", std::vector<bool>{true, false, true});
            const auto intListRecord = store.internAttribute("ints", std::vector<int64_t>{4, 5});
            const auto doubleListRecord = store.internAttribute("doubles", std::vector<double>{6.0, 7.0});
            const auto stringListRecord = store.internAttribute("strings", std::vector<std::string>{"x", "y"});
            const auto srcRecord = store.internSrcLoc(SrcLoc{"payload.fir", 4, 2, 4, 8, "origin", "pass", "note"});
            if (store.attribute(intRecord).kind != AttrValueKind::Int ||
                store.attribute(boolRecord).payload != 1 ||
                store.attribute(doubleRecord).kind != AttrValueKind::Double ||
                store.string(store.attribute(stringRecord).payload) != "payload" ||
                store.bitPayload({store.attribute(boolListRecord).slice.offset, 3}).size() != 1 ||
                store.int64Payload(store.attribute(intListRecord).slice).size() != 2 ||
                store.doublePayload(store.attribute(doubleListRecord).slice).size() != 2 ||
                store.stringListPayload(store.attribute(stringListRecord).slice).size() != 2 ||
                store.string(store.srcLoc(srcRecord).file) != "payload.fir")
            {
                return fail("GraphPayload record mismatch");
            }
            const auto beforeCompact = store.stats();
            store.compact();
            if (store.stats().strings != beforeCompact.strings ||
                store.stats().attributes != beforeCompact.attributes ||
                store.stats().srcLocs != beforeCompact.srcLocs)
            {
                return fail("GraphPayload compact changed stable record counts");
            }
            GraphPayload independent;
            independent.internString("private");
            if (store.string(1) == "private") return fail("GraphPayload arenas are not independent");
        }

        {
            // Profiling is opt-in and must report the logical graph events
            // without changing construction semantics when disabled.
            setGraphProfileEnabled(false);
            resetGraphProfileStats();
            Design unprofiledDesign;
            Graph& unprofiled = unprofiledDesign.createGraph("unprofiled");
            const auto unprofiledSym = unprofiled.internSymbol("unprofiled");
            const auto unprofiledValue = unprofiled.createValue(unprofiledSym, 1, false);
            const auto unprofiledOp = unprofiled.createOperation(OperationKind::kConstant, unprofiled.internSymbol("unprofiled-op"));
            unprofiled.addResult(unprofiledOp, unprofiledValue);
            unprofiled.setAttr(unprofiledOp, "disabled", true);
            if (graphProfileStats().symbolInserts != 0 || graphProfileStats().attributeWrites != 0 ||
                graphProfileStats().valueAllocations != 0 || graphProfileStats().operationAllocations != 0)
            {
                return fail("disabled graph profiling recorded events");
            }

            setGraphProfileEnabled(true);
            resetGraphProfileStats();
            Design profiledDesign;
            Graph& profiled = profiledDesign.createGraph("profiled");
            const auto valueSym = profiled.internSymbol("profiled");
            const auto value = profiled.createValue(valueSym, 8, false);
            const auto op = profiled.createOperation(OperationKind::kAdd, profiled.internSymbol("profiled-op"));
            profiled.addOperand(op, value);
            profiled.addResult(op, value);
            profiled.setAttr(op, "bool", true);
            profiled.setAttr(op, "int", int64_t(7));
            profiled.setAttr(op, "double", 2.5);
            profiled.setAttr(op, "string", std::string("text"));
            profiled.setAttr(op, "bool-list", std::vector<bool>{true, false});
            profiled.setAttr(op, "int-list", std::vector<int64_t>{1, 2});
            profiled.setAttr(op, "double-list", std::vector<double>{1.0, 2.0});
            profiled.setAttr(op, "string-list", std::vector<std::string>{"a", "b"});
            profiled.setOpSrcLoc(op, SrcLoc{"profile.fir", 3, 2, 3, 4, "", "", "note"});
            const auto stats = graphProfileStats();
            if (stats.symbolInserts == 0 || stats.valueAllocations != 1 || stats.operationAllocations != 1 ||
                stats.attributeWrites != 8 || stats.attributeBytes == 0 || stats.srcLocWrites != 1 ||
                stats.srcLocBytes == 0)
            {
                return fail("enabled graph profiling counters were incomplete");
            }

            // Compact and later edits use the same storage without thawing.
            Design profileDesign;
            auto &profileGraph = profileDesign.createGraph("profile_graph");
            const auto profileValue = profileGraph.createValue(1, false);
            const auto profileOp = profileGraph.createOperation(OperationKind::kConstant);
            profileGraph.addResult(profileOp, profileValue);
            profileGraph.setAttr(profileOp, "payload", std::string("graph-store"));
            profileGraph.setOpSrcLoc(profileOp, SrcLoc{"profile-graph.fir", 1});
            profileGraph.compact();
            const auto payloadStats = profileGraph.payloadStats();
            if (!profileGraph.payloadStats().operations || !profileGraph.payloadStats().values ||
                !payloadStats.attributes || !payloadStats.srcLocs)
                return fail("Graph did not retain payload arena records across compact");
            profileGraph.createValue(1, false);
            const auto compactStats = graphProfileStats();
            if (compactStats.freezeCount != 1 || compactStats.thawCount != 0)
            {
                return fail("compact/mutation profiling counters were incomplete");
            }
            setGraphProfileEnabled(false);
        }

    }
    catch (const std::exception &ex)
    {
        return fail(std::string("Unhandled exception: ") + ex.what());
    }
    return 0;
}
