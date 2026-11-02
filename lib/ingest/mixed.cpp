#include "mixed.hpp"

#include "ingest/firrtl.hpp"
#include "ingest/sv.hpp"
#include "firrtl/annotations.hpp"
#include "firrtl/semantic.hpp"
#include "transform/comb_loop_elim.hpp"
#include "transform/verify.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_map>

namespace wolvrix::lib::ingest {
namespace {
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
    for (const auto& entry : graph.opAttrs(op))
        if (entry.key == name) return std::get<std::string>(entry.value);
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

    auto parsed = firrtl::parseModule(entry.source, entry.index);
    if (!parsed.success()) throw std::invalid_argument("cannot parse FIRRTL module: " + std::string(name));
    auto body = std::make_shared<firrtl::ModuleBody>(std::move(*parsed.body));
    const auto [found, inserted] = parsedModules_.emplace(key, std::move(body));
    return found->second;
}

void Transaction::resolveFirrtlInterfaces(const std::vector<std::string>& tops) {
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
    const auto visit = [&](auto&& self, const std::string& name) -> void {
        const auto found = firrtl_.find(name);
        if (found == firrtl_.end() || found->second.signature.external) return;
        if (states[name] == 2) return;
        if (states[name] == 1) throw std::invalid_argument("recursive module dependency: " + name);
        states[name] = 1;
        const auto& entry = found->second;

        const auto parsed = parseFirrtlModule(entry, name);
        try {
            bool childChanged = false;
            const auto dependencies = [&](auto&& walk, std::span<const firrtl::StmtId> statements, std::string layer) -> void {
                for (const auto id : statements) {
                    const auto& statement = parsed->arena.statements[id];
                    if (statement.kind == firrtl::StmtKind::LayerBlock) {
                        const auto path = layer.empty() ? std::string(statement.target) : layer + "." + std::string(statement.target);
                        if (std::any_of(request_.firrtl.enabledLayers.begin(), request_.firrtl.enabledLayers.end(),
                            [&](const auto& enabled) { return enabled == path || enabled.starts_with(path + "."); }))
                            walk(walk, statement.body, path);
                        continue;
                    }
                    if (statement.kind == firrtl::StmtKind::Instance) {
                        const std::string target(statement.target);
                        if (firrtl_.contains(target)) reverseParents[target].insert(name);
                        self(self, target);
                        childChanged |= changedModules.contains(target) || resolvedModules.contains(target);
                    }
                    walk(walk, statement.body, layer); walk(walk, statement.otherwise, layer);
                }
            };
            dependencies(dependencies, parsed->statements, {});
            if (!firstRound && !dirtyModules.contains(name) && !childChanged) {
                states[name] = 2;
                return;
            }

            firrtl::detail::Semantics semantics(*entry.source, *parsed, request_.firrtl, &interfaces_);
            semantics.analyze(true);

            auto signature = entry.signature; signature.ports.clear();
            std::map<uint32_t, std::size_t> groups;
            std::map<std::string, std::string> instances;
            for (const auto& instance : semantics.instances) instances.emplace(instance.name, instance.target);
            for (const auto& declarationName : semantics.declarationOrder) {
                const auto& declaration = semantics.declarations.at(declarationName);
                if (!declaration.port && !instances.contains(declarationName)) continue;
                for (const auto& leaf : semantics.leavesCached(declaration.type, declaration.flow)) {
                    auto path = leaf.fields; path.insert(path.begin(), declarationName);
                    if (instances.contains(declarationName) && semantics.width(leaf.type) >= 0) {
                        const Endpoint target{instances.at(declarationName), leaf.fields};
                        if (inferredWidths.contains(target)) {
                            const auto [width, inserted] = drivenWidths.emplace(target, semantics.width(leaf.type));
                            if (!inserted) width->second = std::max(width->second, static_cast<int32_t>(semantics.width(leaf.type)));
                        }
                    }
                    if (leaf.type->kind == firrtl::TypeKind::Reset) {
                        const auto id = endpoint({name, path});
                        resets[root(id)].flags |= semantics.resetConstraint(leaf.type);
                        const auto [group, inserted] = groups.emplace(semantics.resetGroup(leaf.type), id);
                        if (!inserted) merge(id, group->second, declaration.range);
                        if (instances.contains(declarationName))
                            merge(id, endpoint({instances.at(declarationName), leaf.fields}), declaration.range);
                    }
                    if (!declaration.port) continue;
                    PortLeafSignature port; port.name = declarationName; port.fieldPath = path;
                    for (const auto& field : leaf.fields) port.name += "__DOT__" + field;
                    port.direction = leaf.flow == 1 ? LeafDirection::Input : LeafDirection::Output;
                    port.elementWidth = static_cast<int32_t>(semantics.width(leaf.type)); port.shape = leaf.shape;
                    port.shapeFieldDepths = leaf.shapeFieldDepths;
                    port.isSigned = leaf.type->kind == firrtl::TypeKind::SInt; port.isConst = leaf.type->constant;
                    port.requiresTypeResolution = port.elementWidth <= 0 ||
                        std::find(port.shape.begin(), port.shape.end(), 0) != port.shape.end() || leaf.probe != firrtl::TypeKind::Unknown;
                    const auto kind = leaf.type->kind;
                    port.sourceType = kind == firrtl::TypeKind::Reset ? "Reset" : kind == firrtl::TypeKind::AsyncReset ? "AsyncReset" :
                        kind == firrtl::TypeKind::Clock ? "Clock" : kind == firrtl::TypeKind::SInt ? "SInt" : "UInt";
                    if (leaf.probe != firrtl::TypeKind::Unknown)
                        port.sourceType = std::string(leaf.probe == firrtl::TypeKind::Probe ? "Probe<" : "RWProbe<") + port.sourceType + ">";
                    signature.ports.push_back(std::move(port));
                }
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
                // its visit; re-analyze that module before propagating to
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
        const auto bridge = sourceType == targetType ? value : graph.createValue(targetType);
        if (port.direction == LeafDirection::Input) {
            inputs.push_back(targetPort); graph.addOperand(instance, bridge);
        } else {
            outputs.push_back(targetPort); graph.addResult(instance, bridge);
        }
        if (bridge != value) {
            const auto assign = graph.createOperation(grh::OperationKind::kAssign);
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
                const auto element = graph.createValue(elementType);
                if (port.direction == LeafDirection::Input) {
                    const auto offset = graph.createValue(grh::LogicalType{grh::ValueType::Logic, 32, false, grh::LogicDomain::FourState, {}});
                    const auto constant = graph.createOperation(grh::OperationKind::kConstant);
                    graph.setAttr(constant, "constValue", "32'd" + std::to_string(index)); graph.addResult(constant, offset);
                    const auto slice = graph.createOperation(grh::OperationKind::kSliceArray);
                    graph.addOperand(slice, aggregate); graph.addOperand(slice, offset); graph.addResult(slice, element);
                }
                indices.push_back(index); self(self, dimension + 1, element); indices.pop_back();
                elements.push_back(element);
            }
            if (port.direction == LeafDirection::Output) {
                const auto create = graph.createOperation(grh::OperationKind::kArrayCreate);
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
            const auto parsed = parseFirrtlModule(entry, name);
            std::map<std::string, std::string> targets;
            const auto dependencies = [&](auto&& self, std::span<const firrtl::StmtId> statements, std::string layer) -> void {
                for (const auto id : statements) {
                    const auto& statement = parsed->arena.statements[id];
                    if (statement.kind == firrtl::StmtKind::LayerBlock) {
                        const auto path = layer.empty() ? std::string(statement.target) : layer + "." + std::string(statement.target);
                        const bool enabled = std::any_of(request_.firrtl.enabledLayers.begin(), request_.firrtl.enabledLayers.end(),
                            [&](const auto& selected) { return selected == path || selected.starts_with(path + "."); });
                        if (enabled) self(self, statement.body, path);
                        continue;
                    }
                    if (statement.kind == firrtl::StmtKind::Instance) {
                        const std::string target(statement.target); targets.emplace(target, ensureFirrtl(target));
                    }
                    self(self, statement.body, layer); self(self, statement.otherwise, layer);
                }
            };
            dependencies(dependencies, parsed->statements, {});
            entry.signature = firrtl::detail::lowerModule(*entry.source, *parsed, design_, request_.firrtl, &interfaces_);
            interfaces_[name] = entry.signature; entry.graph = name;
            auto& graph = *design_.findGraph(name);

            for (const auto& statement : parsed->arena.statements) if (statement.kind == firrtl::StmtKind::Cover)
                result_.diagnostics.push_back({IngestSeverity::Warning, "FIRRTL cover uses a simulation hit trace; no formal coverage model is generated",
                    name, "ingest-firrtl", name, entry.source->position(statement.range)});
            for (const auto op : graph.operations()) if (graph.opKind(op) == grh::OperationKind::kInstance)
                graph.setAttr(op, "moduleName", targets.at(attribute(graph, op, "moduleName")));
            firrtl::detail::applyAnnotations(*entry.source, entry.source->modules()[entry.index], annotations_.at(entry.source.get()), graph);
            parsedModules_.erase(ParsedModuleKey{entry.source.get(), entry.index});
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
    const auto canonical = [&](const std::string& file) -> std::string {
        if (file.empty()) return file;
        const auto path = std::filesystem::absolute(file).lexically_normal().generic_string();
        if (!path.starts_with(prefix)) return file;
        const auto found = identities.find(path);
        if (found != identities.end()) return found->second;
        const auto name = "generated-sv:fnv1a64:" + identity(readText(path)) + "/" + path.substr(prefix.size());
        identities.emplace(path, name);
        return name;
    };
    // Source-unit binding checks use physical paths until linking is complete.
    // Published generated-source locations use content identity, as dependencies do.
    for (const auto& [key, name] : svGraphs_) {
        auto* graph = design_.findGraph(name);
        if (!graph) continue; // Specialization wrappers have already been removed.
        const auto file = metadata(*graph, "ingest.sourceFile");
        if (!file.empty()) graph->setMetadata("ingest.sourceFile", canonical(file));
        for (const auto value : graph->values()) if (auto loc = graph->valueSrcLoc(value)) {
            loc->file = canonical(loc->file);
            graph->setValueSrcLoc(value, std::move(*loc));
        }
        for (const auto op : graph->operations()) if (auto loc = graph->opSrcLoc(op)) {
            loc->file = canonical(loc->file);
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
            if (specialization->language == SourceLanguage::Firrtl)
                resolveFirrtlInterfaces({specialization->implementationName});
            const auto graph = ensureImplementation(*specialization);
            design_.markAsTop(graph); tops.clear();
        }
        if (tops.empty() && !specialization) for (const auto& owner : result_.sourceOwners)
            if (const auto source = std::dynamic_pointer_cast<const firrtl::Source>(owner)) tops.emplace_back(source->circuit());
        if (!specialization) resolveFirrtlInterfaces(tops);
        std::set<std::string> selected;
        for (const auto& top : tops) {
            if (!selected.insert(top).second) throw std::invalid_argument("duplicate top module: " + top);
            const auto graph = firrtl_.contains(top) ? ensureFirrtl(top) : ensureSv({SourceLanguage::SystemVerilog, {}, top, {}});
            design_.markAsTop(graph);
        }
        checkHierarchy();
        if (!verifyLinkedDesign(design_, result_.diagnostics)) return std::move(result_);
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

bool verifyLinkedDesign(grh::Design& design, std::vector<IngestDiagnostic>& diagnostics) {
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
                const auto operation = graph->getOperation(op);
                const auto check = [&](std::string_view key, const auto& ports, auto values) {
                    const auto attr = operation.attr(key);
                    const auto* names = attr ? std::get_if<std::vector<std::string>>(&*attr) : nullptr;
                    if (!names || names->size() != ports.size() || values.size() != ports.size())
                        throw std::invalid_argument("linked instance port count differs: " + target + "." + std::string(key));
                    std::unordered_map<std::string_view, grh::ValueId> portValues;
                    portValues.reserve(ports.size());
                    for (const auto& port : ports) portValues.emplace(port.name, port.value);
                    std::set<std::string> used;
                    for (std::size_t i = 0; i < names->size(); ++i) {
                        const auto port = portValues.find(std::string_view((*names)[i]));
                        if (port == portValues.end() || !used.insert((*names)[i]).second)
                            throw std::invalid_argument("unknown or duplicate linked instance port: " + target + "." + (*names)[i]);
                        if (graph->valueLogicalType(values[i]) != child->valueLogicalType(port->second))
                            throw std::invalid_argument("incompatible linked instance port ABI: " + target + "." + (*names)[i]);
                    }
                };
                if (child->inoutPorts().empty()) {
                    check("inputPortName", child->inputPorts(), operation.operands());
                    check("outputPortName", child->outputPorts(), operation.results());
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
    transform::PassManager manager;
    const bool pendingCycles = std::any_of(design.graphs().begin(), design.graphs().end(), [](const auto& entry) {
        return entry.second->metadata("firrtl.pendingCombLoopCheck").has_value();
    });
    if (pendingCycles) {
        transform::CombLoopElimOptions options;
        options.failOnTrueLoop = true;
        // Check module-local array dependencies here. Instance input/output
        // dependencies become precise after the ordinary hier-flatten pass.
        options.treatInstancesAsBoundary = true;
        manager.addPass(std::make_unique<transform::CombLoopElimPass>(options));
    }
    manager.addPass(std::make_unique<transform::GrhVerifyPass>());
    transform::PassDiagnostics verification;
    const bool success = manager.run(design, verification).success;
    for (const auto& entry : verification.messages()) {
        std::optional<SourcePosition> position;
        if (entry.sourcePosition) position = SourcePosition{{}, entry.sourcePosition->file, entry.sourcePosition->line, entry.sourcePosition->column, {}, {}};
        diagnostics.push_back({entry.kind == diag::DiagnosticKind::Error ? IngestSeverity::Error : IngestSeverity::Warning,
            entry.message, entry.context, entry.passName, entry.originSymbol, std::move(position)});
    }
    if (pendingCycles && !success) {
        const auto message = std::find_if(verification.messages().begin(), verification.messages().end(), [](const auto& entry) {
            return entry.passName == "comb-loop-elim";
        });
        std::optional<SourcePosition> position;
        if (message != verification.messages().end() && message->sourcePosition) {
            const auto& source = *message->sourcePosition;
            position = SourcePosition{{}, source.file, source.line, source.column, {}, {}};
        }
        diagnostics.push_back({IngestSeverity::Error, "combinational cycle in FIRRTL connections remains after range analysis", {}, "ingest-link", {}, std::move(position)});
    }
    if (success && pendingCycles)
        for (const auto& entry : design.graphs()) entry.second->eraseMetadata("firrtl.pendingCombLoopCheck");
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
