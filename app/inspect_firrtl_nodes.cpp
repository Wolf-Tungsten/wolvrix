#include "ingest/firrtl.hpp"
#include "ingest/node_attributes.hpp"
#include "transform/grhsim/node-cse.hpp"
#include "../lib/ingest/firrtl/semantic.hpp"
#include "core/store.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <set>

namespace in = wolvrix::lib::ingest;
namespace fi = in::firrtl;
namespace tx = wolvrix::lib::transform;
namespace grh = wolvrix::lib::grh;

int main(int argc, char** argv) {
    try {
        if (argc != 5 && argc != 9) throw std::invalid_argument("usage: inspect-firrtl-nodes INPUT MODULE off|gsim REPORT [off|prefer|auto legacy|bytes MAX_NODES CLONE]");
        if (std::string_view(argv[3]) != "off" && std::string_view(argv[3]) != "gsim")
            throw std::invalid_argument("node CSE mode must be off or gsim");
        const auto started = std::chrono::steady_clock::now();
        tx::SessionStore session;
        auto indexed = fi::indexFile(argv[1], "frozen-xs");
        if (!indexed.success()) throw std::invalid_argument("FIRRTL indexing failed");
        const auto index = indexed.source->findModule(argv[2]);
        if (!index) throw std::invalid_argument("missing selected FIRRTL module");
        grh::Design design; in::FirrtlOptions options;
        options.invalidPolicy = in::FirrtlInvalidPolicy::Zero;
        options.initPolicy = in::FirrtlInitPolicy::Zero;
        std::map<std::string, in::ModuleSignature> interfaces;
        std::set<std::string> active;
        const auto lower = [&](auto&& self, std::size_t index) -> void {
            const auto& module = indexed.source->modules()[index];
            const std::string name(module.name);
            if (design.findGraph(name)) return;
            if (!active.insert(name).second) throw std::invalid_argument("recursive module in node inspection");
            if (module.kind != fi::ModuleKind::Module) throw std::invalid_argument("node inspection requires a self-contained FIRRTL hierarchy");
            auto parsed = fi::parseModule(indexed.source, index);
            if (!parsed.success()) throw std::invalid_argument("selected FIRRTL module parsing failed: " + name);
            for (const auto& statement : parsed.body->arena.statements) if (statement.kind == fi::StmtKind::Instance) {
                const auto dependency = indexed.source->findModule(statement.target);
                if (!dependency) throw std::invalid_argument("missing dependency");
                self(self, *dependency);
            }
            interfaces[name] = fi::detail::lowerModule(*indexed.source, *parsed.body, design, options, &interfaces);
            active.erase(name);
        };
        lower(lower, *index);
        design.markAsTop(argv[2]);
        auto& graph = *design.findGraph(argv[2]);
        std::ofstream report(argv[4]);
        if (!report) throw std::invalid_argument("cannot write report");
        const auto snapshot = [&](std::string_view stage) {
            in::node::recordRetention(graph);
            report << "stage=" << stage << " operations=" << graph.operations().size() << '\n';
            const auto retention = graph.metadata("firrtl.nodeRetention");
            for (const auto& value : std::get<std::vector<std::string>>(*retention)) report << value << '\n';
            report.flush();
        };
        snapshot("ingested");
        const auto execute = [&](std::string_view name, std::initializer_list<std::string_view> args = {}) {
            const auto passStarted = std::chrono::steady_clock::now();
            std::string error; auto pass = tx::makePass(name, std::span(args.begin(), args.size()), error);
            if (!pass) throw std::invalid_argument(error);
            tx::PassManagerOptions managerOptions; managerOptions.session = &session;
            tx::PassManager manager(managerOptions); manager.addPass(std::move(pass));
            tx::PassDiagnostics diagnostics;
            if (!manager.run(design, diagnostics).success) {
                for (const auto& entry : diagnostics.messages()) std::cerr << entry.message << '\n';
                throw std::invalid_argument("node inspection pass failed: " + std::string(name));
            }
            report << "pass-us=" << name << ':' << std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - passStarted).count() << '\n';
            for (const auto& entry : diagnostics.messages())
                if (entry.message.find("hint-analysis-us=") != std::string::npos) report << entry.message << '\n';
            snapshot(name);
        };
        execute("hier-flatten");
        execute("simplify", {"-semantics=2state"});
        execute("grh-to-grhsim");
        execute("grhsim-simplify");
        execute("grhsim-pack-bit-registers");
        execute("grhsim-simplify");
        execute("grhsim-recover-masked-or");
        execute("grhsim-simplify");
        execute("grhsim-node-cse", {std::string_view(argv[3]) == "off" ? "-firrtl-node-cse=off" : "-firrtl-node-cse=gsim"});
        const auto stats = graph.metadata("firrtl.nodeCseStats");
        for (const auto& value : std::get<std::vector<std::string>>(*stats)) report << value << '\n';
        if (argc == 9) {
            execute("grhsim-split-event-and-output");
            execute("grhsim-split-nonsink-and-sink");
            const auto split = wolvrix::lib::store::StoreJson{}.storeToString(design);
            if (!split) throw std::invalid_argument("split checkpoint store failed");
            report << "split-checkpoint-bytes=" << split->size() << '\n';
            std::ofstream splitFile(std::string(argv[4]) + ".split.json");
            splitFile << *split;
            if (!splitFile) throw std::invalid_argument("split checkpoint write failed");
            const std::string hint = "-firrtl-nodes=" + std::string(argv[5]);
            const std::string cost = "-cost-model=" + std::string(argv[6]);
            const std::string capacity = "-max-supernode-nodes=" + std::string(argv[7]);
            const std::string clone = "-clone-shared=" + std::string(argv[8]);
            execute("grhsim-partition-nonsink", {hint, cost, capacity, clone});
            const auto census = graph.metadata("grhsim.nonsinkHintStats");
            for (const auto& value : std::get<std::vector<std::string>>(*census)) report << value << '\n';
            report << "partition-report-stage=nonsink-partitioned-before-simplify\n";
            execute("grhsim-simplify");
            execute("grhsim-partition-sink");
            // Save the actual schedule/IR checkpoint for review and CPU reemit.
            const auto final = wolvrix::lib::store::StoreJson{}.storeToString(design);
            if (!final) throw std::invalid_argument("partition checkpoint store failed");
            std::ofstream checkpoint(std::string(argv[4]) + ".json");
            checkpoint << *final;
            if (!checkpoint) throw std::invalid_argument("partition checkpoint write failed");
        }
        const auto json = wolvrix::lib::store::StoreJson{}.storeToString(design);
        if (!json) throw std::invalid_argument("node checkpoint store failed");
        auto restored = grh::Design::fromJsonString(*json);
        in::node::recordRetention(*restored.findGraph(argv[2]));
        report << "checkpoint-bytes=" << json->size() << '\n';
        // Measure serialization overhead against the same executable graph.
        // This stripped copy is for the census only, never a resumable checkpoint.
        for (const auto& [name, current] : restored.graphs()) {
            for (const auto op : current->operations()) in::node::write(*current, op, {});
            std::vector<std::string> metadata;
            for (const auto& entry : current->metadata())
                if (entry.key.starts_with("firrtl.node")) metadata.emplace_back(entry.key);
            for (const auto& key : metadata) current->eraseMetadata(key);
        }
        const auto stripped = wolvrix::lib::store::StoreJson{}.storeToString(restored);
        if (!stripped) throw std::invalid_argument("node-free census serialization failed");
        report << "checkpoint-without-node-attrs-bytes=" << stripped->size() << '\n';
        report << "node-serialization-bytes=" << json->size() - stripped->size() << '\n';
        report << "elapsed-ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() << '\n';
        const auto reportBytes = report.tellp(); report << "report-bytes-before-footer=" << reportBytes << '\n';
        std::cout << "node-retention=passed module=" << argv[2] << " report=" << argv[4] << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
