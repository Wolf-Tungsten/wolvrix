#include "mixed.hpp"

#include "ingest/firrtl.hpp"
#include "ingest/sv.hpp"
#include "firrtl/annotations.hpp"
#include "firrtl/semantic.hpp"
#include "transform/verify.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <queue>
#include <set>
#include <sstream>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace wolvrix::lib::ingest {
namespace {

// See firrtl.cpp.  The switch is sampled once per process so the default
// path pays only the branch in each timer constructor and no clock calls.
bool stageProfileEnabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("WOLVRIX_FIRRTL_INGEST_PROFILE");
        return value && value[0] && value[0] != '0';
    }();
    return enabled;
}

double processCpuSeconds() {
    timespec value{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value);
    return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_nsec) * 1e-9;
}

class StageTimer {
public:
    explicit StageTimer(const char* name) : enabled_(stageProfileEnabled()), name_(name) {
        if (enabled_) {
            wall_ = std::chrono::steady_clock::now();
            cpu_ = processCpuSeconds();
        }
    }
    ~StageTimer() {
        if (!enabled_) return;
        const auto wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_).count();
        std::cerr << "stage_profile name=" << name_ << " wall_s=" << wall
                  << " cpu_s=" << (processCpuSeconds() - cpu_) << '\n';
    }

private:
    bool enabled_ = false;
    const char* name_ = nullptr;
    std::chrono::steady_clock::time_point wall_{};
    double cpu_ = 0.0;
};

std::string quoteString(std::string_view text) {
    std::string result = "\"";
    for (const char c : text) {
        switch (c) {
        case 92: result.append(2, static_cast<char>(92)); break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += c; break;
        }
    }
    return result + '"';
}
std::string identifier(std::string_view name) {
    if (name.empty() || name.find_first_of(" \n\r\t\\") != std::string_view::npos)
        throw std::invalid_argument("invalid module or port identifier");
    return "\\" + std::string(name) + " ";
}
// Stable non-cryptographic identity. Full keys remain available for collision checks.
std::string identity(std::string_view text) {
    return sourceIdentity(text);
}
std::string metadata(const grh::Graph& graph, std::string_view name) {
    const auto value = graph.metadata(name); return value ? std::get<std::string>(*value) : std::string{};
}
std::string attribute(const grh::Graph& graph, grh::OperationId op, std::string_view name) {
    for (const auto& entry : graph.opAttrRange(op))
        if (entry.key() == name) return std::string(entry.value().asString());
    return {};
}
int32_t flatWidth(const PortLeafSignature& port) {
    int64_t width = port.elementWidth;
    if (width < 0) throw std::invalid_argument("unresolved boundary port width: " + port.name);
    for (const auto extent : port.shape) {
        if (extent < 0 || (extent && width > INT32_MAX / extent))
            throw std::invalid_argument("boundary array width exceeds GRH range: " + port.name);
        width *= extent;
    }
    return static_cast<int32_t>(width);
}
bool samePortSignature(const PortLeafSignature& left, const PortLeafSignature& right) {
    return left.name == right.name && left.fieldPath == right.fieldPath &&
        left.direction == right.direction && left.valueType == right.valueType &&
        left.elementWidth == right.elementWidth && left.isSigned == right.isSigned &&
        left.shape == right.shape && left.shapeFieldDepths == right.shapeFieldDepths &&
        left.sourceType == right.sourceType && left.isConst == right.isConst;
}
struct FirrtlEntry {
    std::shared_ptr<const firrtl::Source> source;
    std::size_t index = 0;
    ModuleSignature signature;
    unsigned state = 0;
    std::string graph;
};
struct PreloweredFirrtl {
    std::unique_ptr<grh::Graph> graph;
    ModuleSignature signature;
    std::exception_ptr failure;
    std::vector<std::string> dependencies;
    std::vector<SourcePosition> covers;
};

void summarizeBody(const firrtl::Source& source, const firrtl::ModuleBody& body,
                   const FirrtlOptions& options, PreloweredFirrtl& result) {
    const auto visit = [&](auto&& self, std::span<const firrtl::StmtId> statements,
                           std::string layer) -> void {
        for (const auto id : statements) {
            const auto& statement = body.arena.statements[id];
            if (statement.kind == firrtl::StmtKind::LayerBlock) {
                const auto path = layer.empty() ? std::string(statement.target) :
                    layer + "." + std::string(statement.target);
                if (std::any_of(options.enabledLayers.begin(), options.enabledLayers.end(),
                    [&](const auto& selected) { return selected == path || selected.starts_with(path + "."); }))
                    self(self, statement.body, path);
                continue;
            }
            if (statement.kind == firrtl::StmtKind::Instance)
                result.dependencies.emplace_back(statement.target);
            self(self, statement.body, layer);
            self(self, statement.otherwise, layer);
        }
    };
    visit(visit, body.statements, {});
    // Preserve the original arena traversal, including covers in disabled layers.
    for (const auto& statement : body.arena.statements)
        if (statement.kind == firrtl::StmtKind::Cover) result.covers.push_back(source.position(statement.range));
}

class FirrtlThreadPool {
public:
    explicit FirrtlThreadPool(std::size_t count) {
        workers_.reserve(count);
        try {
            for (std::size_t index = 0; index < count; ++index)
                workers_.emplace_back([this] { run(); });
        } catch (...) {
            {
                std::lock_guard lock(mutex_);
                stopping_ = true;
            }
            ready_.notify_all();
            for (auto& worker : workers_)
                if (worker.joinable()) worker.join();
            throw;
        }
    }
    FirrtlThreadPool(const FirrtlThreadPool&) = delete;
    FirrtlThreadPool& operator=(const FirrtlThreadPool&) = delete;
    ~FirrtlThreadPool() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_all();
        for (auto& worker : workers_)
            if (worker.joinable()) worker.join();
    }

    template <typename Function>
    auto submit(Function&& function) -> std::future<std::invoke_result_t<Function>> {
        using Result = std::invoke_result_t<Function>;
        auto task = std::make_shared<std::packaged_task<Result()>>(
            std::forward<Function>(function));
        auto result = task->get_future();
        {
            std::lock_guard lock(mutex_);
            if (stopping_) throw std::runtime_error("FIRRTL thread pool is stopped");
            tasks_.emplace([task = std::move(task)]() mutable { (*task)(); });
        }
        ready_.notify_one();
        return result;
    }

private:
    void run() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                if (stopping_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};
using ParsedModuleKey = std::pair<const firrtl::Source*, std::size_t>;
std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::invalid_argument("cannot read blackbox source or annotation file: " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class Transaction {
public:
    explicit Transaction(const IngestRequest& request) : request_(request), sv_(request) { sv_.sources.clear(); sv_.tops.clear(); }
    IngestResult run(const ModuleSpecialization* specialization = nullptr);
private:
    const IngestRequest& request_;
    IngestRequest sv_;
    IngestResult result_;
    grh::Design design_;
    std::map<std::string, FirrtlEntry> firrtl_;
    std::map<ParsedModuleKey, std::shared_ptr<const firrtl::ModuleBody>> parsedModules_;
    std::map<std::string, ModuleSignature> interfaces_;
    std::map<std::string, PreloweredFirrtl> prelowered_;
    std::map<const firrtl::Source*, firrtl::detail::AnnotationIndex> annotations_;
    std::map<std::string, std::string> svGraphs_;
    std::map<std::string, std::string> specializations_;
    std::unordered_set<std::string> declarations_;
    std::set<std::string> collectedBlackboxes_;
    std::map<std::string, std::string> inlineNames_;
    std::set<std::string> inlineContents_, blackboxPaths_;
    std::filesystem::path directory_;
    unsigned file_ = 0;
    void error(std::string message, const ModuleSignature* signature = nullptr);
    std::filesystem::path write(std::string_view contents, std::string_view extension);
    void collectAnnotations();
    void collectBlackboxes(const std::string& module);
    void resolveFirrtlInterfaces(const std::vector<std::string>& tops);
    void prepareFirrtlLowering(const std::vector<std::string>& roots);
    std::shared_ptr<const firrtl::ModuleBody> parseFirrtlModule(const FirrtlEntry& entry,
                                                                std::string_view name);
    std::string ensureImplementation(const ModuleSpecialization& specialization);
    std::string ensureFirrtl(const std::string& name);
    std::string ensureSv(const ModuleSpecialization& specialization);
    std::string importSv(IngestResult parsed, const std::string& top);
    std::string adapter(const ModuleSignature& source, const std::string& target,
                        const std::vector<PortBinding>& bindings);
    void bindDeclarations(grh::Graph& graph);
    void canonicalizeGeneratedLocations();
    void checkHierarchy();
    std::string prototypes(const std::vector<std::string>& names);
};
void Transaction::error(std::string message, const ModuleSignature* signature) {
    result_.diagnostics.push_back({IngestSeverity::Error, std::move(message),
        signature ? signature->name : std::string{}, "ingest-link",
        signature ? signature->name : std::string{}, signature ? signature->location : std::nullopt});
}
std::filesystem::path Transaction::write(std::string_view contents, std::string_view extension) {
    if (directory_.empty()) {
        static std::atomic<uint64_t> sequence{0};
        auto root = std::filesystem::current_path();
        for (auto ancestor = root; !ancestor.empty(); ancestor = ancestor.parent_path()) {
            if (std::filesystem::is_regular_file(ancestor / "AGENTS.md") && std::filesystem::is_directory(ancestor / "wolvrix")) {
                root = ancestor; break;
            }
            if (ancestor == ancestor.root_path()) break;
        }
        directory_ = root / "ptmp/firrtl-ingest/transactions" /
            (std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(sequence++));
        std::filesystem::create_directories(directory_);
    }
    const auto path = directory_ / (std::to_string(file_++) + std::string(extension));
    std::ofstream output(path, std::ios::binary); output << contents;
    if (!output) throw std::runtime_error("cannot write ingest transaction source: " + path.string());
    return path;
}
void Transaction::collectAnnotations() {
    for (const auto& name : request_.firrtl.annotationFiles) {
        const auto path = std::filesystem::path(name).is_absolute() ? std::filesystem::path(name) : request_.inputRoot / name;
        const auto text = readText(path);
        result_.dependencies.push_back("annotation:" + path.generic_string() + ":fnv1a64:" + identity(text));
        try {
            auto sidecar = firrtl::detail::parseAnnotations(text);
            for (auto& record : sidecar.records) {
                const auto circuit = record.target.substr(1, record.target.find('|') - 1);
                const firrtl::Source* selected = nullptr;
                for (const auto& [source, annotations] : annotations_) {
                    if (!source->findModule(record.module) || (!circuit.empty() && source->circuit() != circuit)) continue;
                    if (selected) throw std::invalid_argument("ambiguous sidecar annotation target in " + path.string() + ": " + record.target);
                    selected = source;
                }
                if (!selected) throw std::invalid_argument("unknown sidecar annotation target in " + path.string() + ": " + record.target);
                firrtl::detail::AnnotationIndex one; one.records.push_back(record);
                firrtl::detail::validateAnnotations(*selected, one);
                auto& destination = annotations_.at(selected);
                ++destination.counts[record.kind];
                destination.byModule[record.module].push_back(destination.records.size());
                destination.records.push_back(std::move(record));
            }
        } catch (const firrtl::detail::Error& failure) {
            const auto offset = std::min<std::size_t>(failure.range.begin, text.size());
            const auto previous = offset ? text.rfind('\n', offset - 1) : std::string::npos;
            const auto begin = previous == std::string::npos ? 0 : previous + 1;
            const auto end = text.find('\n', offset);
            result_.diagnostics.push_back({IngestSeverity::Error, failure.what(), path.string(), "ingest-firrtl", {},
                SourcePosition{{}, path, failure.range.line, failure.range.column, text.substr(begin, end - begin), {}}});
            throw std::invalid_argument("invalid sidecar annotation: " + path.string());
        }
    }
}
void Transaction::collectBlackboxes(const std::string& module) {
    if (!collectedBlackboxes_.insert(module).second) return;
    for (const auto& [source, annotations] : annotations_) for (const auto& record : annotations.records) {
        if (record.module != module) continue;
        if (record.kind == "firrtl.transforms.BlackBoxInlineAnno") {
            const auto& name = record.fields.at("name"); const auto& text = record.fields.at("text");
            const auto [previous, inserted] = inlineNames_.emplace(name, text);
            if (!inserted && previous->second != text) throw std::invalid_argument("conflicting inline blackbox filename: " + name);
            result_.dependencies.push_back("inline:" + record.target + ":" + name + ":fnv1a64:" + identity(text));
            if (!inserted) continue;
            const auto relative = std::filesystem::path(name);
            if (relative.empty() || relative.is_absolute() || std::any_of(relative.begin(), relative.end(), [](const auto& part) { return part == ".."; }))
                throw std::invalid_argument("unsafe inline blackbox filename: " + name);
            // Keep names so relative include directives resolve to sibling inline files.
            (void)write({}, ".anchor");
            const auto path = directory_ / "inline" / relative;
            std::filesystem::create_directories(path.parent_path());
            std::ofstream output(path, std::ios::binary); output << text;
            if (!output) throw std::runtime_error("cannot write inline blackbox: " + path.string());
            if (inlineContents_.insert(text).second)
                sv_.sources.push_back({SourceLanguage::SystemVerilog, path, record.target});
        } else if (record.kind == "firrtl.transforms.BlackBoxPathAnno" || record.kind == "firrtl.transforms.BlackBoxResourceAnno") {
            std::filesystem::path path;
            if (record.kind == "firrtl.transforms.BlackBoxPathAnno") {
                path = record.fields.at("path");
                if (path.is_relative()) path = (request_.inputRoot.empty() ? source->path().parent_path() : request_.inputRoot) / path;
            } else {
                auto resource = std::filesystem::path(record.fields.at("resourceId")).relative_path();
                if (std::any_of(resource.begin(), resource.end(), [](const auto& part) { return part == ".."; }))
                    throw std::invalid_argument("invalid blackbox resource path");
                for (const auto& name : request_.firrtl.resourceRoots) {
                    auto root = std::filesystem::path(name); if (root.is_relative()) root = request_.inputRoot / root;
                    const auto candidate = root / resource;
                    if (!std::filesystem::is_regular_file(candidate)) continue;
                    if (!path.empty()) throw std::invalid_argument("ambiguous blackbox resource: " + resource.string());
                    path = candidate;
                }
                if (path.empty()) throw std::invalid_argument("missing blackbox resource: " + resource.string());
            }
            const auto text = readText(path); path = std::filesystem::weakly_canonical(path);
            result_.dependencies.push_back("blackbox:" + record.target + ":" + path.generic_string() + ":fnv1a64:" + identity(text));
            if (blackboxPaths_.insert(path.generic_string()).second) sv_.sources.push_back({SourceLanguage::SystemVerilog, path, record.target});
        }
    }
}

std::shared_ptr<const firrtl::ModuleBody> Transaction::parseFirrtlModule(const FirrtlEntry& entry,
                                                                          std::string_view name) {
    const ParsedModuleKey key{entry.source.get(), entry.index};
    if (const auto found = parsedModules_.find(key); found != parsedModules_.end()) {
        return found->second;
    }

    auto parsed = firrtl::detail::parseSharedModule(entry.source, entry.index);
    if (!parsed.success()) throw std::invalid_argument("cannot parse FIRRTL module: " + std::string(name));
    auto body = parsed.sharedBody;
    const auto [found, inserted] = parsedModules_.emplace(key, std::move(body));
    return found->second;
}

void Transaction::resolveFirrtlInterfaces(const std::vector<std::string>& tops) {
    StageTimer stageTimer("firrtl_interface_fixed_point");
    // Abstract Reset belongs to the connected hierarchy. Defaulting an isolated
    // child port to UInt before seeing its parent loses asynchronous resets.
    using Endpoint = std::pair<std::string, std::vector<std::string>>;
    struct Reset { std::size_t parent; unsigned flags = 0; };
    std::map<Endpoint, std::size_t> endpoints;
    std::vector<Reset> resets;
    const auto endpoint = [&](const Endpoint& key) {
        const auto [found, inserted] = endpoints.emplace(key, resets.size());
        if (inserted) resets.push_back({found->second});
        return found->second;
    };
    const auto root = [&](std::size_t id) {
        while (resets[id].parent != id) { resets[id].parent = resets[resets[id].parent].parent; id = resets[id].parent; }
        return id;
    };
    const auto merge = [&](std::size_t left, std::size_t right, const firrtl::Range& range) {
        left = root(left); right = root(right);
        resets[right].parent = left; resets[left].flags |= resets[right].flags;
        if (resets[left].flags == 3)
            throw firrtl::detail::Error(range, "Reset is connected to both synchronous and asynchronous resets across the hierarchy");
    };
    std::map<std::string, unsigned> states;
    std::map<std::string, std::set<std::string>> reverseParents;
    std::set<std::string> dirtyModules;
    std::set<std::string> changedModules;
    std::set<std::string> resolvedModules;
    bool firstRound = true;
    bool changed = false;
    std::map<Endpoint, std::size_t> portIndices;
    std::set<Endpoint> inferredWidths;
    for (const auto& [name, entry] : firrtl_) {
        for (std::size_t i = 0; i < entry.signature.ports.size(); ++i) {
            const auto& port = entry.signature.ports[i];
            const Endpoint key{name, port.fieldPath};
            portIndices.emplace(key, i);
            if (port.elementWidth < 0) inferredWidths.insert(key);
        }
    }
    std::map<Endpoint, int32_t> drivenWidths;
    std::map<ParsedModuleKey, std::shared_ptr<const firrtl::detail::InterfaceSummary>> summaries;
    const auto visit = [&](auto&& self, const std::string& name) -> void {
        const auto found = firrtl_.find(name);
        if (found == firrtl_.end() || found->second.signature.external) return;
        if (states[name] == 2) return;
        if (states[name] == 1) throw std::invalid_argument("recursive module dependency: " + name);
        states[name] = 1;
        const auto& entry = found->second;

        try {
            bool childChanged = false;
            const ParsedModuleKey parsedKey{entry.source.get(), entry.index};
            auto summary = summaries.find(parsedKey);
            if (summary == summaries.end())
                summary = summaries.emplace(parsedKey,
                    firrtl::detail::sharedInterfaceSummary(entry.source, entry.index, request_.firrtl)).first;
            for (const auto& target : summary->second->dependencies) {
                if (firrtl_.contains(target)) reverseParents[target].insert(name);
                self(self, target);
                childChanged |= changedModules.contains(target) || resolvedModules.contains(target);
            }
            if (!firstRound && !dirtyModules.contains(name) && !childChanged) {
                states[name] = 2;
                return;
            }

            const auto evaluation = summary->second->evaluate(interfaces_);

            auto signature = entry.signature; signature.ports.clear();
            std::map<uint32_t, std::size_t> groups;
            for (const auto& leaf : summary->second->leaves) {
                const auto& path = leaf.port.fieldPath;
                const auto bits = evaluation.widths[leaf.width];
                if (!leaf.target.empty() && bits >= 0) {
                    const Endpoint target{leaf.target, leaf.fields};
                    if (inferredWidths.contains(target)) {
                        const auto [width, inserted] = drivenWidths.emplace(target, bits);
                        if (!inserted) width->second = std::max(width->second, static_cast<int32_t>(bits));
                    }
                }
                if (leaf.kind == firrtl::TypeKind::Reset) {
                    const auto id = endpoint({name, path});
                    resets[root(id)].flags |= evaluation.resets[leaf.resetGroup];
                    const auto [group, inserted] = groups.emplace(leaf.resetGroup, id);
                    if (!inserted) merge(id, group->second, leaf.range);
                    if (!leaf.target.empty()) merge(id, endpoint({leaf.target, leaf.fields}), leaf.range);
                }
                if (!leaf.target.empty()) continue;
                auto port = leaf.port;
                port.elementWidth = static_cast<int32_t>(bits);
                port.requiresTypeResolution = port.elementWidth <= 0 ||
                    std::find(port.shape.begin(), port.shape.end(), 0) != port.shape.end() ||
                    port.sourceType.starts_with("Probe<") || port.sourceType.starts_with("RWProbe<");
                signature.ports.push_back(std::move(port));
            }
            const auto& previous = interfaces_.at(name);
            bool signatureChanged = signature.ports.size() != previous.ports.size();
            for (std::size_t i = 0; i < signature.ports.size(); ++i) {
                const auto& port = signature.ports[i];
                const Endpoint key{name, port.fieldPath};
                const auto prior = portIndices.find(key);
                const bool portChanged = prior == portIndices.end() || prior->second >= previous.ports.size() ||
                    !samePortSignature(port, previous.ports[prior->second]);
                signatureChanged |= portChanged;
            }
            changed |= signatureChanged;
            if (signatureChanged) resolvedModules.insert(name);
            for (std::size_t i = 0; i < signature.ports.size(); ++i) portIndices[{name, signature.ports[i].fieldPath}] = i;
            interfaces_[name] = std::move(signature); states[name] = 2;
        } catch (const firrtl::detail::Error& failure) {
            result_.diagnostics.push_back(firrtl::detail::diagnostic(*entry.source, failure, name)); throw;
        }
    };
    std::size_t round = 0;
    do {
        changed = false; states.clear(); resolvedModules.clear();
        for (const auto& top : tops) visit(visit, top);
        for (const auto& [key, bits] : drivenWidths) {
            const auto module = interfaces_.find(key.first);
            const auto portIndex = portIndices.find(key);
            if (module == interfaces_.end() || portIndex == portIndices.end() ||
                portIndex->second >= module->second.ports.size())
                throw std::invalid_argument("missing FIRRTL inferred width port: " + key.first);
            auto& port = module->second.ports[portIndex->second];
            if (bits > port.elementWidth) {
                port.elementWidth = bits;
                changed = true;
                resolvedModules.insert(key.first);
            }
        }
        if (++round > firrtl_.size() * 2 + 1)
            throw std::invalid_argument("hierarchical FIRRTL width inference did not converge");
        if (request_.logSink && request_.logLevel <= LogLevel::Info)
            request_.logSink({LogLevel::Info, "ingest-firrtl", "hierarchical interface inference round=" + std::to_string(round) +
                " modules=" + std::to_string(states.size()) + " changed=" + (changed ? "1" : "0")});
        firstRound = false;
        dirtyModules.clear();
        if (changed) {
            for (const auto& module : resolvedModules) {
                // A width update is applied to the module's interface after
                // its visit; reevaluate its summary before propagating to
                // parents so its derived output widths become visible.
                dirtyModules.insert(module);
                const auto parents = reverseParents.find(module);
                if (parents == reverseParents.end()) continue;
                dirtyModules.insert(parents->second.begin(), parents->second.end());
            }
        }
        changedModules.swap(resolvedModules);
    } while (changed);
    for (const auto& [key, id] : endpoints) {
        const auto found = portIndices.find(key);
        if (found == portIndices.end()) continue;
        auto& port = interfaces_.at(key.first).ports[found->second];
        if (port.sourceType == "Reset") {
            port.sourceType = resets[root(id)].flags == 2 ? "AsyncReset" : "UInt";
            port.requiresTypeResolution = !port.elementWidth || std::find(port.shape.begin(), port.shape.end(), 0) != port.shape.end();
        }
    }
}

void Transaction::prepareFirrtlLowering(const std::vector<std::string>& roots) {
    const std::size_t requested = request_.firrtl.lowerThreads;
    if (requested <= 1 || roots.empty()) return;
    StageTimer stageTimer("firrtl_strict_analyze_lower");
    const auto preparationStarted = std::chrono::steady_clock::now();

    // Discover reachable modules before workers start. This mirrors
    // ensureFirrtl's layer selection and preserves lazy error behavior.
    std::set<std::string> reachable;
    std::set<std::string> visiting;
    const auto discover = [&](auto&& self, const std::string& name) -> void {
        const auto found = firrtl_.find(name);
        if (found == firrtl_.end() || found->second.signature.external ||
            found->second.state == 2 || prelowered_.contains(name) ||
            reachable.contains(name) || !visiting.insert(name).second) return;
        const auto summary = firrtl::detail::sharedInterfaceSummary(found->second.source,
            found->second.index, request_.firrtl);
        for (const auto& dependency : summary->dependencies) self(self, dependency);
        visiting.erase(name);
        reachable.insert(name);
    };
    for (const auto& root : roots) discover(discover, root);
    if (reachable.empty()) return;

    const std::size_t workers = std::min(requested, reachable.size());
    // Interface signatures are immutable during worker execution. Share one
    // snapshot so each module task does not copy the entire module map.
    const auto interfaces = std::make_shared<const std::map<std::string, ModuleSignature>>(interfaces_);
    const auto options = request_.firrtl;
    // Allocate graphs with their final owner and IDs on the coordinator.
    // Defer catalog insertion until serial publication; hash table traversal
    // order also affects verification diagnostics.
    std::vector<std::pair<std::string, std::unique_ptr<grh::Graph>>> graphs;
    graphs.reserve(reachable.size());
    for (const auto& name : reachable) {
        graphs.emplace_back(name, design_.createUnpublishedGraph(name));
    }
    // Keep graph allocation/publication order stable, but feed the largest
    // module bodies first so workers do not leave one long tail of work.
    std::vector<std::size_t> schedule(graphs.size());
    std::iota(schedule.begin(), schedule.end(), 0);
    std::sort(schedule.begin(), schedule.end(), [&](std::size_t left, std::size_t right) {
        const auto& lhs = firrtl_.at(graphs[left].first).source->modules()[firrtl_.at(graphs[left].first).index];
        const auto& rhs = firrtl_.at(graphs[right].first).source->modules()[firrtl_.at(graphs[right].first).index];
        const auto lhsSize = lhs.range.end - lhs.range.begin;
        const auto rhsSize = rhs.range.end - rhs.range.begin;
        return lhsSize != rhsSize ? lhsSize > rhsSize : graphs[left].first < graphs[right].first;
    });
    const auto loweringStarted = std::chrono::steady_clock::now();
    {
        // The pool is destroyed (and joins) before the mutation scope, also
        // on exceptions. Coalesce owner revision updates at this boundary.
        auto mutation = design_.parallelGraphMutation();
        FirrtlThreadPool pool(workers);
        std::vector<std::pair<std::string, std::future<PreloweredFirrtl>>> pending;
        pending.reserve(graphs.size());
        for (const auto graphIndex : schedule) {
            auto& [name, graph] = graphs[graphIndex];
            const auto& entry = firrtl_.at(name);
            auto body = parseFirrtlModule(entry, name);
            parsedModules_.erase(ParsedModuleKey{entry.source.get(), entry.index});
            pending.emplace_back(name, pool.submit(
                [source = entry.source, module = entry.index, body = std::move(body), graph = std::move(graph), interfaces, options,
                 annotations = &annotations_.at(entry.source.get())]() mutable {
                    PreloweredFirrtl result;
                    result.graph = std::move(graph);
                    try {
                        summarizeBody(*source, *body, options, result);
                        // Boundary signatures are immutable. The same worker
                        // owns the only strict analysis and immediately lowers
                        // it; no full semantic state waits on the coordinator.
                        firrtl::detail::Semantics semantics(*source, *body, options, interfaces.get());
                        semantics.analyze();
                        result.signature = firrtl::detail::lowerModule(semantics, *result.graph, options);
                        firrtl::detail::applyAnnotations(*source, source->modules()[module], *annotations, *result.graph);
                    } catch (...) {
                        // Re-throw in ensureFirrtl's deterministic dependency
                        // order through the normal source-aware diagnostics.
                        result.failure = std::current_exception();
                    }
                    firrtl::detail::releaseParsedModule(*source, module);
                    body.reset();
                    return result;
                }));
        }
        for (auto& [name, future] : pending)
            prelowered_.insert_or_assign(name, future.get());
    }
    if (request_.logSink && request_.logLevel <= LogLevel::Info)
        request_.logSink({LogLevel::Info, "ingest-firrtl",
            "parallel FIRRTL lowering threads=" + std::to_string(workers) +
            " modules=" + std::to_string(prelowered_.size()) +
            " prepare_s=" + std::to_string(std::chrono::duration<double>(
                loweringStarted - preparationStarted).count()) +
            " lower_s=" + std::to_string(std::chrono::duration<double>(
                std::chrono::steady_clock::now() - loweringStarted).count())});
}

std::string Transaction::prototypes(const std::vector<std::string>& names) {
    std::string text;
    for (const auto& name : names) {
        const auto& entry = firrtl_.at(name);
        if (entry.signature.external) continue;
        text += "(* wolvrix_link_declaration = 1 *) module " + identifier(name) + "(\n";
        bool first = true;
        for (const auto& port : interfaces_.at(name).ports) {
            if (!flatWidth(port)) continue;
            if (port.requiresTypeResolution) throw std::invalid_argument("unresolved FIRRTL boundary interface: " + name + "." + port.name);
            if (!first) text += ",\n"; first = false;
            text += port.direction == LeafDirection::Input ? "input " : "output ";
            text += "logic "; if (port.isSigned) text += "signed ";
            text += "[" + std::to_string(port.elementWidth - 1) + ":0] " + identifier(port.name);
            for (const auto extent : port.shape) text += "[0:" + std::to_string(extent - 1) + "]";
        }
        text += "\n); endmodule\n";
    }
    return text;
}

std::string Transaction::adapter(const ModuleSignature& source, const std::string& target,
                                 const std::vector<PortBinding>& bindings) {
    const auto* child = design_.findGraph(target);
    if (!child) throw std::runtime_error("missing implementation graph: " + target);
    const auto name = "__firrtl_abi_" + identity(source.sourceUnit + "/" + source.name + "/" + target);
    if (design_.findGraph(name)) return name;
    auto& graph = design_.createGraph(name);
    const auto instance = graph.createOperation(grh::OperationKind::kInstance);
    graph.setAttr(instance, "moduleName", target);
    graph.setAttr(instance, "instanceName", std::string("implementation"));
    graph.setMetadata("ingest.link.kind", std::string("adapter"));
    std::vector<std::string> inputs, outputs;
    std::set<std::string> used;
    std::set<std::string> bound;
    for (const auto& binding : bindings)
        if (!bound.insert(binding.sourcePort).second) throw std::invalid_argument("duplicate source port binding: " + binding.sourcePort);
    const auto hasPort = [&](const std::string& name) {
        return child->inputPortValue(name) || child->outputPortValue(name) ||
            std::any_of(child->inoutPorts().begin(), child->inoutPorts().end(), [&](const auto& port) { return port.name == name; });
    };
    const auto connect = [&](const PortLeafSignature& port, const std::string& targetPort,
                             grh::ValueId value, bool scalarized = false) {
        const auto expected = port.direction == LeafDirection::Input ? child->inputPortValue(targetPort) : child->outputPortValue(targetPort);
        if (!expected || !used.insert(targetPort).second) throw std::invalid_argument("missing, duplicate or wrong-direction implementation port: " + source.name + "." + targetPort);
        const auto targetType = child->valueLogicalType(expected);
        const auto sourceType = graph.valueLogicalType(value);
        if (sourceType.width != targetType.width || sourceType.kind != targetType.kind || sourceType.domain != targetType.domain ||
            (scalarized && !targetType.shape.empty()) ||
            (!sourceType.shape.empty() && !targetType.shape.empty() && sourceType.shape != targetType.shape))
            throw std::invalid_argument("incompatible boundary ABI width, kind, domain or shape: " + source.name + "." + port.name);
        const auto bridge = sourceType == targetType ? value : graph.createTemporaryValue(targetType);
        if (port.direction == LeafDirection::Input) {
            inputs.push_back(targetPort); graph.addOperand(instance, bridge);
        } else {
            outputs.push_back(targetPort); graph.addResult(instance, bridge);
        }
        if (bridge != value) {
            const auto assign = graph.createTemporaryOperation(grh::OperationKind::kAssign);
            graph.setAttr(assign, "ingest.abi.view", true);
            graph.addOperand(assign, port.direction == LeafDirection::Input ? value : bridge);
            graph.addResult(assign, port.direction == LeafDirection::Input ? bridge : value);
        }
    };
    for (const auto& port : source.ports) {
        if (!flatWidth(port)) continue;
        if (port.requiresTypeResolution)
            throw std::invalid_argument("unresolved FIRRTL external boundary interface: " + source.name + "." + port.name);
        if (port.direction == LeafDirection::Inout) throw std::invalid_argument("unsupported FIRRTL inout boundary: " + source.name + "." + port.name);
        auto targetPort = port.name;
        const auto binding = std::find_if(bindings.begin(), bindings.end(), [&](const auto& value) { return value.sourcePort == port.name; });
        if (binding != bindings.end()) targetPort = binding->implementationPort;
        else if (!hasPort(targetPort)) {
            // FIRRTL's emitted flat ABI joins Bundle paths with '_'.
            targetPort.clear();
            for (const auto& field : port.fieldPath) {
                if (!targetPort.empty()) targetPort += '_';
                targetPort += field;
            }
        }
        const grh::LogicalType sourceType{port.valueType, flatWidth(port), port.isSigned, grh::LogicDomain::FourState, port.shape};
        const auto value = graph.createValue(graph.internSymbol(port.name), sourceType);
        if (port.direction == LeafDirection::Input) graph.bindInputPort(port.name, value);
        else graph.bindOutputPort(port.name, value);
        if (binding != bindings.end() || hasPort(targetPort) || port.shape.empty()) {
            connect(port, targetPort, value);
            continue;
        }
        // Chisel blackboxes may expose r[0].data as scalar SV r_0_data.
        // Preserve the FIRRTL array on this side and unpack only at the ABI.
        if (port.shapeFieldDepths.size() != port.shape.size() ||
            !std::is_sorted(port.shapeFieldDepths.begin(), port.shapeFieldDepths.end()) ||
            std::any_of(port.shapeFieldDepths.begin(), port.shapeFieldDepths.end(),
                [&](std::size_t depth) { return !depth || depth > port.fieldPath.size(); }))
            throw std::invalid_argument("missing FIRRTL array ABI path: " + source.name + "." + port.name);
        std::vector<int64_t> indices;
        const auto scalarName = [&]() {
            std::string name;
            std::size_t dimension = 0;
            for (std::size_t field = 0; field < port.fieldPath.size(); ++field) {
                if (!name.empty()) name += '_';
                name += port.fieldPath[field];
                while (dimension < indices.size() && port.shapeFieldDepths[dimension] == field + 1)
                    name += '_' + std::to_string(indices[dimension++]);
            }
            return name;
        };
        const auto split = [&](auto&& self, std::size_t dimension, grh::ValueId aggregate) -> void {
            if (dimension == port.shape.size()) {
                connect(port, scalarName(), aggregate, true);
                return;
            }
            auto elementType = graph.valueLogicalType(aggregate);
            elementType.width /= elementType.shape.front();
            elementType.shape.erase(elementType.shape.begin());
            std::vector<grh::ValueId> elements;
            for (int64_t index = 0; index < port.shape[dimension]; ++index) {
                const auto element = graph.createTemporaryValue(elementType);
                if (port.direction == LeafDirection::Input) {
                    const auto offset = graph.createTemporaryValue(
                        grh::LogicalType{grh::ValueType::Logic, 32, false, grh::LogicDomain::FourState, {}});
                    const auto constant = graph.createTemporaryOperation(grh::OperationKind::kConstant);
                    graph.setAttr(constant, "constValue", "32'd" + std::to_string(index)); graph.addResult(constant, offset);
                    const auto slice = graph.createTemporaryOperation(grh::OperationKind::kSliceArray);
                    graph.addOperand(slice, aggregate); graph.addOperand(slice, offset); graph.addResult(slice, element);
                }
                indices.push_back(index); self(self, dimension + 1, element); indices.pop_back();
                elements.push_back(element);
            }
            if (port.direction == LeafDirection::Output) {
                const auto create = graph.createTemporaryOperation(grh::OperationKind::kArrayCreate);
                for (const auto element : elements) graph.addOperand(create, element);
                graph.addResult(create, aggregate);
            }
        };
        split(split, 0, value);
    }
    if (used.size() != child->inputPorts().size() + child->outputPorts().size() || !child->inoutPorts().empty())
        throw std::invalid_argument("implementation port count differs from FIRRTL interface: " + source.name);
    for (const auto& binding : bindings)
        if (std::none_of(source.ports.begin(), source.ports.end(), [&](const auto& port) { return port.name == binding.sourcePort; }))
            throw std::invalid_argument("binding names an unknown source port: " + binding.sourcePort);
    graph.setAttr(instance, "inputPortName", std::move(inputs)); graph.setAttr(instance, "outputPortName", std::move(outputs));
    graph.setAttr(instance, "inoutPortName", std::vector<std::string>{});
    return name;
}

std::string Transaction::ensureFirrtl(const std::string& name) {
    auto found = firrtl_.find(name);
    if (found == firrtl_.end()) throw std::invalid_argument("unresolved FIRRTL module: " + name);
    auto& entry = found->second;
    if (entry.state == 2) return entry.graph;
    if (entry.state == 1) throw std::invalid_argument("recursive module dependency: " + name);
    entry.state = 1;
    try {
        if (entry.signature.external) {
            collectBlackboxes(name);
            ModuleSpecialization implementation{SourceLanguage::SystemVerilog, {},
                entry.signature.implementationName.empty() ? name : entry.signature.implementationName, entry.signature.parameters};
            std::vector<PortBinding> ports;
            for (const auto& binding : request_.externalBindings) {
                if (binding.moduleName != name || (!binding.sourceUnit.empty() && binding.sourceUnit != entry.signature.sourceUnit)) continue;
                implementation = binding.implementation; ports = binding.ports;
            }
            std::string target;
            if (implementation.language == SourceLanguage::Firrtl) target = ensureImplementation(implementation);
            else {
                if (sv_.sources.empty() && sv_.slangArgs.empty())
                    throw std::invalid_argument("missing external implementation binding for '" + name + "' (" + implementation.implementationName + ")");
                target = ensureSv(implementation);
            }
            entry.graph = adapter(interfaces_.at(name), target, ports);
        } else {
            auto prelowered = prelowered_.find(name);
            std::shared_ptr<const firrtl::ModuleBody> parsed;
            PreloweredFirrtl summary;
            if (prelowered == prelowered_.end()) {
                parsed = parseFirrtlModule(entry, name);
                summarizeBody(*entry.source, *parsed, request_.firrtl, summary);
            }
            const auto& bodySummary = prelowered == prelowered_.end() ? summary : prelowered->second;
            std::map<std::string, std::string> targets;
            for (const auto& target : bodySummary.dependencies) targets.emplace(target, ensureFirrtl(target));
            const auto covers = bodySummary.covers;
            if (prelowered == prelowered_.end() && request_.firrtl.lowerThreads > 1) {
                // SV can expose FIRRTL declarations beyond the initial walk.
                // Link their dependencies first, as in the serial endpoint,
                // then submit strict analysis/lowering with the resolved ABI.
                prepareFirrtlLowering({name});
                prelowered = prelowered_.find(name);
                parsed.reset();
            }
            if (prelowered != prelowered_.end()) {
                if (prelowered->second.failure) std::rethrow_exception(prelowered->second.failure);
                if (!prelowered->second.graph)
                    throw std::runtime_error("parallel FIRRTL lowering produced no graph: " + name);
                design_.publishGraph(std::move(prelowered->second.graph));
                entry.signature = std::move(prelowered->second.signature);
                prelowered_.erase(prelowered);
            } else {
                firrtl::detail::Semantics semantics(*entry.source, *parsed, request_.firrtl, &interfaces_);
                semantics.analyze();
                auto& graph = design_.createGraph(name);
                entry.signature = firrtl::detail::lowerModule(semantics, graph, request_.firrtl);
                firrtl::detail::applyAnnotations(*entry.source, entry.source->modules()[entry.index],
                    annotations_.at(entry.source.get()), graph);
            }
            interfaces_[name] = entry.signature; entry.graph = name;
            auto& graph = *design_.findGraph(name);
            const bool cyclesChecked = graph.localCyclesChecked();

            if (const auto warnings = graph.metadata("firrtl.readUnderWriteWarnings")) {
                for (const auto& message : std::get<std::vector<std::string>>(*warnings))
                    result_.diagnostics.push_back({IngestSeverity::Warning, message, name, "ingest-firrtl", name, {}});
            }
            for (const auto& position : covers)
                result_.diagnostics.push_back({IngestSeverity::Warning, "FIRRTL cover uses a simulation hit trace; no formal coverage model is generated",
                    name, "ingest-firrtl", name, position});
            for (const auto op : graph.operations()) if (graph.opKind(op) == grh::OperationKind::kInstance)
                graph.setAttr(op, "moduleName", targets.at(attribute(graph, op, "moduleName")));
            // Binding module names and importing annotations above only change
            // attributes/provenance, so the lowerer's edge proof still holds.
            if (cyclesChecked) graph.markLocalCyclesChecked();
            parsedModules_.erase(ParsedModuleKey{entry.source.get(), entry.index});
            firrtl::detail::releaseParsedModule(*entry.source, entry.index);
        }
        entry.state = 2; return entry.graph;
    } catch (const firrtl::detail::Error& failure) {
        result_.diagnostics.push_back(firrtl::detail::diagnostic(*entry.source, failure, name)); throw;
    } catch (const std::exception& failure) {
        error(failure.what(), &entry.signature); throw;
    }
}
std::string Transaction::ensureImplementation(const ModuleSpecialization& specialization) {
    if (specialization.language == SourceLanguage::SystemVerilog) return ensureSv(specialization);
    const auto entry = firrtl_.find(specialization.implementationName);
    if (entry == firrtl_.end()) throw std::invalid_argument("unresolved FIRRTL implementation: " + specialization.implementationName);
    if (!specialization.sourceUnit.empty() && specialization.sourceUnit != entry->second.signature.sourceUnit)
        throw std::invalid_argument("FIRRTL implementation belongs to another source unit: " + specialization.implementationName);
    if (!specialization.parameters.empty())
        throw std::invalid_argument("FIRRTL module implementations do not accept specialization parameters");
    return ensureFirrtl(specialization.implementationName);
}
void Transaction::bindDeclarations(grh::Graph& graph) {
    for (const auto op : graph.operations()) {
        if (attribute(graph, op, "ingest.link.kind") != "declaration") continue;
        const auto target = ensureFirrtl(attribute(graph, op, "moduleName"));
        graph.setOpKind(op, grh::OperationKind::kInstance); graph.setAttr(op, "moduleName", target);
        graph.eraseAttr(op, "ingest.link.kind");
    }
}
std::string Transaction::importSv(IngestResult parsed, const std::string& top) {
    result_.sourceOwners.insert(result_.sourceOwners.end(), parsed.sourceOwners.begin(), parsed.sourceOwners.end());
    result_.diagnostics.insert(result_.diagnostics.end(), parsed.diagnostics.begin(), parsed.diagnostics.end());
    result_.dependencies.insert(result_.dependencies.end(), parsed.dependencies.begin(), parsed.dependencies.end());
    if (!parsed.success()) throw std::invalid_argument("SystemVerilog elaboration failed for '" + top + "'");
    std::map<std::string, std::string> names;
    std::vector<std::string> added;
    for (const auto& name : parsed.design->graphOrder()) {
        const auto& graph = *parsed.design->findGraph(name);
        const auto key = metadata(graph, "ingest.sourceModule") + "/" + metadata(graph, "ingest.parameterSignature");
        const auto [found, inserted] = svGraphs_.emplace(key, "__sv_" + identity(key));
        names.emplace(name, found->second);
        if (inserted) {
            if (design_.findGraph(found->second)) throw std::runtime_error("SV specialization identity collision");
            design_.importGraph(graph, found->second); added.push_back(found->second);
        }
    }
    for (const auto& name : added) {
        auto& graph = *design_.findGraph(name);
        for (const auto op : graph.operations()) {
            if (graph.opKind(op) != grh::OperationKind::kInstance) continue;
            const auto module = attribute(graph, op, "moduleName");
            if (!names.contains(module)) throw std::invalid_argument("unresolved SV implementation: " + module);
            graph.setAttr(op, "moduleName", names.at(module));
        }
    }
    for (const auto& name : added) bindDeclarations(*design_.findGraph(name));
    for (auto& signature : parsed.modules) {
        if (signature.implementationName.starts_with("__wolvrix_specialize_")) continue;
        signature.name = names.at(signature.name);
        if (std::none_of(result_.modules.begin(), result_.modules.end(), [&](const auto& existing) { return existing.name == signature.name; }))
            result_.modules.push_back(std::move(signature));
    }
    if (parsed.design->topGraphs().size() != 1) throw std::invalid_argument("specialization requires exactly one SV top");
    return names.at(parsed.design->topGraphs().front());
}
std::string Transaction::ensureSv(const ModuleSpecialization& specialization) {
    if (specialization.implementationName.empty()) throw std::invalid_argument("missing implementation module name");
    if (sv_.sources.empty() && sv_.slangArgs.empty()) throw std::invalid_argument("missing top or implementation module: " + specialization.implementationName);
    std::string key = specialization.sourceUnit + "/" + specialization.implementationName;
    auto parameters = specialization.parameters;
    std::sort(parameters.begin(), parameters.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    for (std::size_t i = 1; i < parameters.size(); ++i)
        if (parameters[i - 1].name == parameters[i].name) throw std::invalid_argument("duplicate specialization parameter: " + parameters[i].name);
    for (const auto& parameter : parameters) key += "/" + parameter.name + "=" + std::to_string(parameter.value.index()) + ":" + parameterLiteral(parameter.value);
    if (const auto found = specializations_.find(key); found != specializations_.end()) return found->second;
    auto request = sv_;
    const auto wrapper = "__wolvrix_specialize_" + identity(key);
    std::string wrapperText = "module " + wrapper + ";\n" + identifier(specialization.implementationName);
    if (!parameters.empty()) {
        wrapperText += " #("; bool first = true;
        for (const auto& parameter : parameters) {
            if (!first) wrapperText += ","; first = false;
            wrapperText += "." + identifier(parameter.name) + "(" + parameterLiteral(parameter.value) + ")";
        }
        wrapperText += ")";
    }
    wrapperText += " implementation();\nendmodule\n";
    request.sources.push_back({SourceLanguage::SystemVerilog, write(wrapperText, ".sv"), "internal.specialization"});
    request.tops = {wrapper};
    const auto wrapperGraph = importSv(readSystemVerilog(request, declarations_, [&](const auto& required) {
        for (const auto& name : required)
            if (std::any_of(interfaces_.at(name).ports.begin(), interfaces_.at(name).ports.end(),
                            [](const auto& port) { return port.requiresTypeResolution; })) ensureFirrtl(name);
        const auto declarationText = prototypes(required);
        return declarationText.empty() ? std::filesystem::path{} : write(declarationText, ".sv");
    }), wrapper);
    std::string target;
    const auto& graph = *design_.findGraph(wrapperGraph);
    for (const auto op : graph.operations()) if (graph.opKind(op) == grh::OperationKind::kInstance) {
        if (!target.empty()) throw std::runtime_error("specialization wrapper has multiple implementations");
        target = attribute(graph, op, "moduleName");
    }
    if (target.empty()) throw std::invalid_argument("missing real specialization implementation: " + specialization.implementationName);
    if (!specialization.sourceUnit.empty()) {
        const auto unit = std::find_if(sv_.sources.begin(), sv_.sources.end(), [&](const auto& source) { return source.unit == specialization.sourceUnit; });
        if (unit == sv_.sources.end()) throw std::invalid_argument("unknown SV implementation source unit: " + specialization.sourceUnit);
        auto path = unit->path; if (path.is_relative()) path = request_.inputRoot / path;
        if (std::filesystem::weakly_canonical(path) != std::filesystem::weakly_canonical(metadata(*design_.findGraph(target), "ingest.sourceFile")))
            throw std::invalid_argument("SV implementation belongs to another source unit: " + specialization.implementationName);
    }
    design_.deleteGraph(wrapperGraph);
    result_.dependencies.push_back("specialization:" + key); specializations_.emplace(key, target);
    return target;
}
void Transaction::checkHierarchy() {
    std::map<std::string, unsigned> colors;
    std::function<void(const std::string&)> visit = [&](const std::string& name) {
        if (colors[name] == 1) throw std::invalid_argument("recursive linked module hierarchy: " + name);
        if (colors[name] == 2) return; colors[name] = 1;
        const auto* graph = design_.findGraph(name); if (!graph) throw std::invalid_argument("unresolved linked module: " + name);
        for (const auto op : graph->operations()) {
            if (attribute(*graph, op, "ingest.link.kind") == "declaration") throw std::invalid_argument("unbound interface declaration");
            if (graph->opKind(op) == grh::OperationKind::kBlackbox) throw std::invalid_argument("unresolved external module: " + attribute(*graph, op, "moduleName"));
            if (graph->opKind(op) == grh::OperationKind::kInstance) visit(attribute(*graph, op, "moduleName"));
        }
        colors[name] = 2;
    };
    for (const auto& top : design_.topGraphs()) visit(top);
}
void Transaction::canonicalizeGeneratedLocations() {
    if (directory_.empty()) return;
    const auto prefix = directory_.generic_string() + "/";
    std::map<std::string, std::string> identities;
    std::unordered_map<std::string, std::string> canonicalCache;
    const auto canonical = [&](const std::string& file) -> std::string {
        if (file.empty()) return file;
        if (const auto cached = canonicalCache.find(file); cached != canonicalCache.end()) return cached->second;
        const auto path = std::filesystem::absolute(file).lexically_normal().generic_string();
        if (!path.starts_with(prefix)) {
            canonicalCache.emplace(file, file);
            return file;
        }
        const auto found = identities.find(path);
        if (found != identities.end()) {
            canonicalCache.emplace(file, found->second);
            return found->second;
        }
        const auto name = "generated-sv:fnv1a64:" + identity(readText(path)) + "/" + path.substr(prefix.size());
        identities.emplace(path, name);
        canonicalCache.emplace(file, name);
        return name;
    };
    // Source-unit binding checks use physical paths until linking is complete.
    // Published generated-source locations use content identity, as dependencies do.
    for (const auto& [key, name] : svGraphs_) {
        auto* graph = design_.findGraph(name);
        if (!graph) continue; // Specialization wrappers have already been removed.
        const auto file = metadata(*graph, "ingest.sourceFile");
        if (const auto normalized = canonical(file); normalized != file)
            graph->setMetadata("ingest.sourceFile", normalized);
        for (const auto value : graph->values()) if (auto loc = graph->valueSrcLoc(value)) {
            const auto normalized = canonical(loc->file);
            if (normalized == loc->file) continue;
            loc->file = normalized;
            graph->setValueSrcLoc(value, std::move(*loc));
        }
        for (const auto op : graph->operations()) if (auto loc = graph->opSrcLoc(op)) {
            const auto normalized = canonical(loc->file);
            if (normalized == loc->file) continue;
            loc->file = normalized;
            graph->setOpSrcLoc(op, std::move(*loc));
        }
    }
}
IngestResult Transaction::run(const ModuleSpecialization* specialization) {
    auto firrtlRequest = request_; firrtlRequest.sources.clear();
    for (const auto& input : request_.sources) {
        if (input.language == SourceLanguage::Firrtl) firrtlRequest.sources.push_back(input);
        else sv_.sources.push_back(input);
    }
    InterfaceScanResult scanned; scanned.success = true;
    if (!firrtlRequest.sources.empty()) scanned = scanFirrtlInterfaces(firrtlRequest);
    result_.sourceOwners = std::move(scanned.sourceOwners); result_.modules = std::move(scanned.modules);
    result_.diagnostics = std::move(scanned.diagnostics); if (!scanned.success) return std::move(result_);
    try {
        StageTimer annotationTimer("firrtl_annotation_and_interface_publish");
        for (const auto& owner : result_.sourceOwners) {
            const auto source = std::dynamic_pointer_cast<const firrtl::Source>(owner); if (!source) continue;
            result_.dependencies.push_back("firrtl:" + std::string(source->unit()) + ":fnv1a64:" + identity(source->text()));
            try { annotations_.emplace(source.get(), firrtl::detail::readAnnotations(*source)); }
            catch (const firrtl::detail::Error& failure) {
                result_.diagnostics.push_back(firrtl::detail::diagnostic(*source, failure));
                return std::move(result_);
            }
            for (std::size_t i = 0; i < source->modules().size(); ++i) {
                auto signature = describeFirrtlModule(*source, source->modules()[i]);
                interfaces_.emplace(signature.name, signature);
                if (!signature.external) declarations_.insert(signature.name);
                if (!firrtl_.emplace(signature.name, FirrtlEntry{source, i, signature}).second)
                    throw std::invalid_argument("ambiguous or duplicate FIRRTL module: " + signature.name);
            }
        }
        collectAnnotations();
        auto tops = request_.tops;
        std::set<std::string> bound;
        for (const auto& binding : request_.externalBindings) {
            const auto entry = firrtl_.find(binding.moduleName);
            if (entry == firrtl_.end() || !entry->second.signature.external ||
                (!binding.sourceUnit.empty() && binding.sourceUnit != entry->second.signature.sourceUnit))
                throw std::invalid_argument("binding names an unknown external module/source unit: " + binding.moduleName);
            if (!bound.insert(binding.moduleName).second) throw std::invalid_argument("duplicate external binding: " + binding.moduleName);
        }
        if (specialization) {
            if (specialization->language == SourceLanguage::Firrtl) {
                resolveFirrtlInterfaces({specialization->implementationName});
                prepareFirrtlLowering({specialization->implementationName});
            }
            const auto graph = ensureImplementation(*specialization);
            design_.markAsTop(graph); tops.clear();
        }
        if (tops.empty() && !specialization) for (const auto& owner : result_.sourceOwners)
            if (const auto source = std::dynamic_pointer_cast<const firrtl::Source>(owner)) tops.emplace_back(source->circuit());
        if (!specialization) {
            resolveFirrtlInterfaces(tops);
            prepareFirrtlLowering(tops);
        }
        StageTimer publishTimer("firrtl_graph_publish_and_hierarchy");
        std::set<std::string> selected;
        for (const auto& top : tops) {
            if (!selected.insert(top).second) throw std::invalid_argument("duplicate top module: " + top);
            const auto graph = firrtl_.contains(top) ? ensureFirrtl(top) : ensureSv({SourceLanguage::SystemVerilog, {}, top, {}});
            design_.markAsTop(graph);
        }
        checkHierarchy();
        {
            StageTimer verifyTimer("firrtl_linked_verify_total");
            if (!verifyLinkedDesign(design_, result_.diagnostics, request_.firrtl.lowerThreads)) return std::move(result_);
        }
        for (auto& signature : result_.modules) if (interfaces_.contains(signature.name)) signature = interfaces_.at(signature.name);
        // Generated sources use content identity instead of transaction paths.
        const auto transactionPrefix = directory_.empty() ? std::string{} : "sv:" + directory_.generic_string() + "/";
        for (auto& dependency : result_.dependencies) if (!transactionPrefix.empty() && dependency.starts_with(transactionPrefix)) {
            const auto hash = dependency.rfind(":fnv1a64:");
            dependency = "generated-sv" + dependency.substr(hash);
        }
        std::sort(result_.dependencies.begin(), result_.dependencies.end());
        result_.dependencies.erase(std::unique(result_.dependencies.begin(), result_.dependencies.end()), result_.dependencies.end());
        std::vector<std::string> boundAnnotations, pendingAnnotations;
        for (const auto& [source, annotations] : annotations_) for (const auto& record : annotations.records)
            if (record.kind.starts_with("firrtl.transforms.BlackBox"))
                (collectedBlackboxes_.contains(record.module) ? boundAnnotations : pendingAnnotations).push_back(record.raw);
        for (const auto& name : design_.graphOrder()) {
            auto& graph = *design_.findGraph(name);
            graph.setMetadata("ingest.dependencies", result_.dependencies);
            graph.setMetadata("firrtl.boundExternalAnnotations", boundAnnotations);
            graph.setMetadata("firrtl.pendingExternalAnnotations", pendingAnnotations);
        }
        canonicalizeGeneratedLocations();
        if (!prelowered_.empty())
            throw std::runtime_error("unpublished FIRRTL modules remain at transaction commit");
        for (const auto& owner : result_.sourceOwners) {
            if (const auto source = std::dynamic_pointer_cast<const firrtl::Source>(owner))
                firrtl::detail::clearParsedModules(*source);
        }
        result_.design.emplace(std::move(design_));
    } catch (const std::exception& failure) { error(failure.what()); }
    return std::move(result_);
}
} // namespace

std::string sourceIdentity(std::string_view text) {
    uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char c : text) { hash ^= c; hash *= 1099511628211ULL; }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

bool verifyLinkedDesign(grh::Design& design, std::vector<IngestDiagnostic>& diagnostics, std::size_t threads) {
    try {
        std::map<std::string, unsigned> states;
        std::function<void(const std::string&)> visit = [&](const std::string& name) {
            if (states[name] == 1) throw std::invalid_argument("recursive linked module hierarchy: " + name);
            if (states[name] == 2) return;
            states[name] = 1;
            const auto* graph = design.findGraph(name);
            if (!graph) throw std::invalid_argument("unresolved linked module: " + name);
            for (const auto op : graph->operations()) {
                if (attribute(*graph, op, "ingest.link.kind") == "declaration")
                    throw std::invalid_argument("unbound interface declaration in " + name);
                if (graph->opKind(op) == grh::OperationKind::kBlackbox)
                    throw std::invalid_argument("unresolved external module in " + name);
                if (graph->opKind(op) != grh::OperationKind::kInstance) continue;
                const auto target = attribute(*graph, op, "moduleName");
                const auto* child = design.findGraph(target);
                if (!child) throw std::invalid_argument("unresolved linked instance: " + target);
                const auto attrs = graph->opAttrRange(op);
                const auto check = [&](std::string_view key, const auto& ports, auto values) {
                    const auto attr = attrs.find(key);
                    const auto names = attr ? attr->value().get<std::vector<std::string>>() : std::nullopt;
                    if (!names || names->size() != ports.size() || values.size() != ports.size())
                        throw std::invalid_argument("linked instance port count differs: " + target + "." + std::string(key));
                    std::unordered_map<std::string_view, grh::ValueId> portValues;
                    portValues.reserve(ports.size());
                    for (const auto& port : ports) portValues.emplace(port.name, port.value);
                    std::set<std::string> used;
                    for (std::size_t i = 0; i < names->size(); ++i) {
                        const auto port = portValues.find(std::string_view((*names)[i]));
                        if (port == portValues.end() || !used.emplace((*names)[i]).second)
                            throw std::invalid_argument("unknown or duplicate linked instance port: " + target + "." + std::string((*names)[i]));
                        if (graph->valueLogicalType(values[i]) != child->valueLogicalType(port->second))
                            throw std::invalid_argument("incompatible linked instance port ABI: " + target + "." + std::string((*names)[i]));
                    }
                };
                if (child->inoutPorts().empty()) {
                    check("inputPortName", child->inputPorts(), graph->opOperands(op));
                    check("outputPortName", child->outputPorts(), graph->opResults(op));
                }
                visit(target);
            }
            states[name] = 2;
        };
        for (const auto& top : design.topGraphs()) visit(top);
    } catch (const std::exception& failure) {
        diagnostics.push_back({IngestSeverity::Error, failure.what(), {}, "ingest-link"});
        return false;
    }
    transform::PassManagerOptions managerOptions;
    managerOptions.graphThreads = std::max(std::size_t{1}, threads);
    transform::PassManager manager(managerOptions);
    manager.addPass(std::make_unique<transform::GrhVerifyPass>());
    transform::PassDiagnostics verification;
    StageTimer verifyPassesTimer("firrtl_linked_verify_passes");
    const auto verifyResult = manager.run(design, verification);
    if (stageProfileEnabled()) {
        for (const auto& stage : verifyResult.stages)
            std::cerr << "stage_profile name=firrtl_linked_verify_pass pass=" << stage.pass
                      << " wall_s=" << stage.wallSeconds
                      << " worker_wall_s=" << stage.workerSeconds
                      << " overlapped=" << (stage.overlapped ? 1 : 0) << '\n';
    }
    const bool success = verifyResult.success;
    for (const auto& entry : verification.messages()) {
        std::optional<SourcePosition> position;
        if (entry.sourcePosition) position = SourcePosition{{}, entry.sourcePosition->file, entry.sourcePosition->line, entry.sourcePosition->column, {}, {}};
        diagnostics.push_back({entry.kind == diag::DiagnosticKind::Error ? IngestSeverity::Error : IngestSeverity::Warning,
            entry.message, entry.context, entry.passName, entry.originSymbol, std::move(position)});
    }
    return success;
}

std::string parameterLiteral(const ParameterValue& value) {
    if (const auto integer = std::get_if<IntegerParameter>(&value)) {
        if (!integer->width || integer->bits.size() != integer->width || integer->bits.find_first_not_of("01xz") != std::string::npos)
            throw std::invalid_argument("invalid typed integer parameter");
        return std::to_string(integer->width) + (integer->isSigned ? "'sb" : "'b") + integer->bits;
    }
    if (const auto string = std::get_if<std::string>(&value)) return quoteString(*string);
    if (const auto raw = std::get_if<RawStringParameter>(&value)) return raw->text;
    const auto real = std::get<double>(value);
    if (!std::isfinite(real)) throw std::invalid_argument("non-finite module parameter");
    std::ostringstream out; out << std::setprecision(std::numeric_limits<double>::max_digits10) << real;
    auto text = out.str(); if (text.find_first_of(".eE") == std::string::npos) text += ".0"; return text;
}
IngestResult readMixedSources(const IngestRequest& request) {
    return Transaction(request).run();
}
IngestResult readSpecializedSources(const IngestRequest& request, const ModuleSpecialization& specialization) {
    return Transaction(request).run(&specialization);
}
} // namespace wolvrix::lib::ingest
