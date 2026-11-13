#include "transform/node_evolution.hpp"

#include "constant_index.hpp"
#include "core/state.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace wolvrix::lib::transform
{
    namespace
    {
        using Ref = GraphEvolutionTrace::Ref;

        struct RefHash
        {
            std::size_t operator()(const Ref &ref) const noexcept
            {
                const uint64_t identifier = (uint64_t{ref.generation} << 32) | ref.index;
                return std::hash<uint64_t>{}(identifier) ^
                    (ref.operation ? std::size_t{0x9e3779b9} : std::size_t{0}) ^
                    (ref.graph ? std::size_t{0x85ebca6b} : std::size_t{0});
            }
        };

        void quoted(std::ostream &out, std::string_view value)
        {
            constexpr char hex[] = "0123456789abcdef";
            out.put('"');
            for (const unsigned char ch : value)
            {
                switch (ch)
                {
                case '"': out << "\\\""; break;
                case '\\': out << "\\\\"; break;
                case '\n': out << "\\n"; break;
                case '\r': out << "\\r"; break;
                case '\t': out << "\\t"; break;
                default:
                    if (ch < 0x20) out << "\\u00" << hex[ch >> 4] << hex[ch & 15];
                    else out.put(static_cast<char>(ch));
                }
            }
            out.put('"');
        }

        void reference(std::ostream &out, const Ref &ref)
        {
            out << "{\"kind\":\"" << (ref.graph ? "grh-graph" : ref.operation ? "grh-operation" : "grh-value")
                << "\",\"id\":" << ref.index << ",\"generation\":" << ref.generation << '}';
        }

        template<typename Range>
        void references(std::ostream &out, const Range &refs)
        {
            out.put('[');
            bool first = true;
            for (const auto &ref : refs)
            {
                if (!first) out.put(',');
                first = false;
                reference(out, ref);
            }
            out.put(']');
        }

        template<typename Range>
        void integers(std::ostream &out, const Range &values)
        {
            out.put('[');
            bool first = true;
            for (const auto value : values)
            {
                if (!first) out.put(',');
                first = false;
                out << value;
            }
            out.put(']');
        }

        void attribute(std::ostream &out, grh::AttrValueView value)
        {
            out << "{\"type\":" << static_cast<unsigned>(value.kind()) << ",\"value\":";
            const auto number = [&](double numberValue) {
                if (!std::isfinite(numberValue))
                    throw std::runtime_error("node evolution attribute is not a finite JSON number");
                out << std::setprecision(std::numeric_limits<double>::max_digits10) << numberValue;
            };
            const auto list = [&](const auto &values, const auto &write) {
                out.put('[');
                bool first = true;
                for (const auto item : values)
                {
                    if (!first) out.put(',');
                    first = false;
                    write(item);
                }
                out.put(']');
            };
            switch (value.kind())
            {
            case grh::AttrValueKind::Bool: out << (value.asBool() ? "true" : "false"); break;
            case grh::AttrValueKind::Int: out << value.asInt(); break;
            case grh::AttrValueKind::Double: number(value.asDouble()); break;
            case grh::AttrValueKind::String: quoted(out, value.asString()); break;
            case grh::AttrValueKind::BoolList:
                list(value.asBoolList(), [&](bool item) { out << (item ? "true" : "false"); });
                break;
            case grh::AttrValueKind::IntList: integers(out, value.asIntList()); break;
            case grh::AttrValueKind::DoubleList: list(value.asDoubleList(), number); break;
            case grh::AttrValueKind::StringList:
                list(value.asStringList(), [&](std::string_view item) { quoted(out, item); });
                break;
            }
            out.put('}');
        }

        // Split only the top-level object emitted by this writer. Values remain
        // exact JSON slices; nested arrays and objects are never normalized.
        std::map<std::string, std::string_view> fields(std::string_view body)
        {
            std::map<std::string, std::string_view> result;
            std::size_t position = 1;
            while (position + 1 < body.size())
            {
                if (body[position++] != '"') throw std::logic_error("invalid native snapshot field");
                const auto nameStart = position;
                while (position < body.size() && body[position] != '"') ++position;
                const std::string name(body.substr(nameStart, position - nameStart));
                if (position + 1 >= body.size() || body[position + 1] != ':')
                    throw std::logic_error("invalid native snapshot separator");
                position += 2;
                const auto valueStart = position;
                int depth = 0;
                bool string = false, escaped = false;
                for (; position < body.size(); ++position)
                {
                    const char ch = body[position];
                    if (string)
                    {
                        if (escaped) escaped = false;
                        else if (ch == '\\') escaped = true;
                        else if (ch == '"') string = false;
                    }
                    else if (ch == '"') string = true;
                    else if (ch == '[' || ch == '{') ++depth;
                    else if (ch == ']' || ch == '}')
                    {
                        if (depth == 0) break;
                        --depth;
                    }
                    else if (ch == ',' && depth == 0) break;
                }
                result.emplace(name, body.substr(valueStart, position - valueStart));
                if (position < body.size() && body[position] == ',') ++position;
            }
            return result;
        }

        void location(std::ostream &out, const std::optional<grh::SrcLoc> &loc)
        {
            if (!loc) { out << "null"; return; }
            out << "{\"file\":";
            quoted(out, loc->file);
            out << ",\"line\":" << loc->line << ",\"column\":" << loc->column
                << ",\"endLine\":" << loc->endLine << ",\"endColumn\":" << loc->endColumn;
            out << ",\"origin\":"; quoted(out, loc->origin);
            out << ",\"pass\":"; quoted(out, loc->pass);
            out << ",\"note\":"; quoted(out, loc->note);
            out.put('}');
        }

        struct Generations
        {
            std::vector<uint32_t> generations;
            std::vector<uint8_t> present;

            void add(Ref ref)
            {
                if (ref.index >= present.size())
                {
                    present.resize(static_cast<std::size_t>(ref.index) + 1);
                    generations.resize(static_cast<std::size_t>(ref.index) + 1);
                }
                generations[ref.index] = ref.generation;
                present[ref.index] = 1;
            }

            bool contains(Ref ref) const
            {
                return ref.index < present.size() && present[ref.index] &&
                    generations[ref.index] == ref.generation;
            }
        };

        struct BindingIndex
        {
            // One-based linked rows, rebuilt once for each boundary inventory.
            std::vector<uint32_t> values, operations, valueNext, operationNext;
            std::span<const grh::SemanticBinding> bindings;

            explicit BindingIndex(const grh::Graph &graph) : bindings(graph.semanticBindings())
            {
                if (bindings.size() >= std::numeric_limits<uint32_t>::max())
                    throw std::runtime_error("too many node evolution source bindings");
                uint32_t maxValue = 0, maxOperation = 0;
                for (const auto &binding : bindings)
                {
                    maxValue = std::max(maxValue, binding.value.index);
                    maxOperation = std::max(maxOperation, binding.operation.index);
                }
                values.resize(static_cast<std::size_t>(maxValue) + 1);
                operations.resize(static_cast<std::size_t>(maxOperation) + 1);
                valueNext.resize(bindings.size());
                operationNext.resize(bindings.size());
                for (std::size_t row = bindings.size(); row-- > 0;)
                {
                    const auto &binding = bindings[row];
                    if (binding.value)
                    {
                        valueNext[row] = values[binding.value.index];
                        values[binding.value.index] = static_cast<uint32_t>(row + 1);
                    }
                    if (binding.operation)
                    {
                        operationNext[row] = operations[binding.operation.index];
                        operations[binding.operation.index] = static_cast<uint32_t>(row + 1);
                    }
                }
            }

            std::vector<uint32_t> rows(Ref ref) const
            {
                const auto &heads = ref.operation ? operations : values;
                const auto &next = ref.operation ? operationNext : valueNext;
                std::vector<uint32_t> result;
                if (ref.index >= heads.size()) return result;
                for (uint32_t row = heads[ref.index]; row; row = next[row - 1])
                    result.push_back(row - 1);
                return result;
            }
        };

        int64_t coordinate(const std::vector<int64_t> &values, std::size_t axis, int64_t fallback)
        {
            return values.empty() ? fallback : values[axis];
        }

        bool indexedWriterBinding(const grh::Graph &graph, const grh::SemanticBinding &binding)
        {
            if (binding.role != grh::SemanticBindingRole::RegisterNext || !binding.operation) return false;
            const auto kind = graph.opKind(binding.operation);
            if (kind != grh::OperationKind::kRegisterWriteArrayPort &&
                kind != grh::OperationKind::kRegisterWriteArrayMaskPort) return false;
            const auto count = grh::arrayPortIndexCount(graph, binding.operation);
            if (!count || *count > binding.shape.size()) return false;
            for (std::size_t axis = 0; axis < *count; ++axis)
                if (coordinate(binding.projection, axis, -1) == -1) return true;
            return false;
        }

        struct SourceBindingHash
        {
            const BindingIndex &index;
            const grh::Graph &graph;
            const grh::SemanticTree *tree;

            std::size_t operator()(uint32_t row) const
            {
                const auto &binding = index.bindings[row];
                std::size_t hash = 0;
                const auto mix = [&](std::size_t value) {
                    hash ^= value + std::size_t{0x9e3779b9} + (hash << 6) + (hash >> 2);
                };
                mix(static_cast<std::size_t>(binding.role));
                mix(std::hash<int32_t>{}(binding.sourceWidth));
                mix(binding.sourceSigned);
                if (indexedWriterBinding(graph, binding))
                {
                    mix(binding.operation.index);
                    mix(binding.operation.generation);
                }
                mix(binding.shape.size());
                for (std::size_t axis = 0; axis < binding.shape.size(); ++axis)
                {
                    mix(std::hash<int64_t>{}(binding.shape[axis]));
                    mix(std::hash<int64_t>{}(coordinate(binding.shapeFieldDepths, axis, 0)));
                    mix(std::hash<int64_t>{}(coordinate(binding.projection, axis, -1)));
                }
                // Hash native token views, without constructing full path strings.
                // Exact token equality below resolves every hash collision.
                if (!tree) throw std::runtime_error("node evolution binding has no source tree");
                mix(std::hash<std::string_view>{}(tree->name(binding.semantic)));
                for (auto path = tree->node(binding.semantic).parent; path; path = tree->pathEntry(path).parent)
                    mix(std::hash<std::string_view>{}(tree->pathName(path)));
                return hash;
            }
        };

        struct SourceBindingEqual
        {
            const BindingIndex &index;
            const grh::Graph &graph;
            const grh::SemanticTree *tree;

            bool operator()(uint32_t left, uint32_t right) const
            {
                const auto &a = index.bindings[left], &b = index.bindings[right];
                if (a.role != b.role || a.shape != b.shape || a.sourceWidth != b.sourceWidth ||
                    a.sourceSigned != b.sourceSigned) return false;
                if ((indexedWriterBinding(graph, a) || indexedWriterBinding(graph, b)) &&
                    a.operation != b.operation) return false;
                for (std::size_t axis = 0; axis < a.shape.size(); ++axis)
                    if (coordinate(a.shapeFieldDepths, axis, 0) != coordinate(b.shapeFieldDepths, axis, 0) ||
                        coordinate(a.projection, axis, -1) != coordinate(b.projection, axis, -1)) return false;
                if (a.semantic == b.semantic) return true;
                if (!tree) return false;
                return tree->path(a.semantic) == tree->path(b.semantic);
            }
        };
    } // namespace

    struct GraphEvolutionTrace::Impl
    {
        struct Step
        {
            std::string action, reason;
            std::vector<Ref> before, after;
            std::unique_ptr<Geometry> geometry;
        };
        struct Vertex
        {
            Ref ref;
            std::size_t parent;
            bool before = false, after = false;
        };
        struct Snapshot
        {
            uint64_t offset = 0;
            uint32_t length = 0, generation = 0;
        };
        struct Pending
        {
            Ref ref;
            Snapshot after;
        };

        grh::Graph &graph;
        std::ofstream out;
        bool sourceOnly = false, finished = false;
        uint64_t beforeCount = 0, afterCount = 0, eventCount = 0;
        Generations beforeValues, beforeOperations;
        std::vector<Snapshot> valueSnapshots, operationSnapshots;
        Snapshot graphSnapshot;
        std::fstream beforeCache, afterCache;
        uint64_t beforeCacheBytes = 0, afterCacheBytes = 0;
        uint64_t beforeReadOffset = std::numeric_limits<uint64_t>::max();
        uint64_t afterReadOffset = std::numeric_limits<uint64_t>::max();
        std::filesystem::path beforeCachePath, afterCachePath;
        std::vector<Pending> pending;
        uint64_t mutationCount = 0, violationCount = 0;
        std::vector<Step> steps;
        std::vector<Vertex> vertices;
        std::unordered_map<Ref, std::size_t, RefHash> byRef;

        Impl(grh::Graph &graph, const std::string &path, const std::string &pass,
             const std::string &digest, bool sourceOnly)
            : graph(graph), sourceOnly(sourceOnly)
        {
            if (graph.semanticTree())
                throw std::runtime_error("F28 SemanticTree Node evolution is not supported by the legacy source-endpoint trace; migrate the pass trace before use");
            const std::filesystem::path target(path);
            if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path());
            out.open(target, std::ios::trunc);
            check();
            if (!sourceOnly)
            {
                beforeCachePath = path + ".before-cache";
                afterCachePath = path + ".after-cache";
                beforeCache.open(beforeCachePath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
                afterCache.open(afterCachePath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
                if (!beforeCache || !afterCache) throw std::runtime_error("cannot open native replay cache");
            }
            out << "{\"record\":\"header\",\"schema\":\"wolvrix.node-evolution.v1\","
                   "\"engine\":\"grhsim\",\"pass\":";
            quoted(out, pass);
            out << ",\"graph\":";
            quoted(out, graph.symbol());
            out << ",\"implicitIdentity\":true,\"inventoryScope\":";
            quoted(out, sourceOnly ? "source-endpoints" : "all-live");
            if (!sourceOnly)
                out << ",\"replayContract\":\"native-field-delta.v1\","
                       "\"replayScope\":\"active-graph\","
                       "\"replayGranularity\":\"pass-boundary\","
                       "\"replayExcluded\":[\"allocator-and-intern-pools\",\"source-tree-provenance-nodes\",\"derived-reverse-use-cache\"]";
            if (!digest.empty())
            {
                out << ",\"inputIdentity\":{\"sha256\":";
                quoted(out, digest);
                out << ",\"authority\":\"caller\"}";
            }
            out << "}\n";
            inventory(true);
        }

        ~Impl()
        {
            beforeCache.close();
            afterCache.close();
            std::error_code ignored;
            if (!beforeCachePath.empty()) std::filesystem::remove(beforeCachePath, ignored);
            if (!afterCachePath.empty()) std::filesystem::remove(afterCachePath, ignored);
        }

        Snapshot *snapshot(Ref ref)
        {
            if (ref.graph) return graphSnapshot.length ? &graphSnapshot : nullptr;
            auto &rows = ref.operation ? operationSnapshots : valueSnapshots;
            if (ref.index >= rows.size() || !rows[ref.index].length ||
                rows[ref.index].generation != ref.generation) return nullptr;
            return &rows[ref.index];
        }

        Snapshot save(std::fstream &cache, Ref ref, std::string_view body)
        {
            if (body.size() >= (uint32_t{1} << 31)) throw std::runtime_error("native replay entity exceeds cache limit");
            auto &written = &cache == &beforeCache ? beforeCacheBytes : afterCacheBytes;
            const auto offset = written;
            cache.write(body.data(), static_cast<std::streamsize>(body.size()));
            if (!cache) throw std::runtime_error("cannot write native replay cache");
            written += body.size();
            return {offset, static_cast<uint32_t>(body.size()), ref.generation};
        }

        std::string read(std::fstream &cache, Snapshot row)
        {
            std::string body(row.length & ~(uint32_t{1} << 31), '\0');
            auto &cursor = &cache == &beforeCache ? beforeReadOffset : afterReadOffset;
            cache.clear();
            if (cursor != row.offset) cache.seekg(static_cast<std::streamoff>(row.offset));
            cache.read(body.data(), static_cast<std::streamsize>(body.size()));
            if (!cache) throw std::runtime_error("cannot read native replay cache");
            cursor = row.offset + body.size();
            return body;
        }

        void snapshotBody(bool before, Ref ref, const std::string &body)
        {
            if (sourceOnly) return;
            if (before)
            {
                const Snapshot row = save(beforeCache, ref, body);
                if (ref.graph) graphSnapshot = row;
                else
                {
                    auto &rows = ref.operation ? operationSnapshots : valueSnapshots;
                    if (ref.index >= rows.size()) rows.resize(static_cast<std::size_t>(ref.index) + 1);
                    rows[ref.index] = row;
                }
                return;
            }
            auto *old = snapshot(ref);
            if (old)
            {
                old->length |= uint32_t{1} << 31;
                if (read(beforeCache, *old) == body) return;
            }
            pending.push_back({ref, save(afterCache, ref, body)});
        }

        void emitBody(bool before, Ref ref, const std::string &body)
        {
            if (before)
            {
                if (!ref.graph) (ref.operation ? beforeOperations : beforeValues).add(ref);
            }
            else if (const auto found = byRef.find(ref); found != byRef.end()) vertices[found->second].after = true;
            out << "{\"record\":\"entity\",\"stage\":\"" << (before ? "before" : "after") << "\",\"ref\":";
            reference(out, ref);
            out << ',' << std::string_view(body).substr(1) << '\n';
            snapshotBody(before, ref, body);
            if (before) ++beforeCount;
            else ++afterCount;
        }

        void check()
        {
            if (!out) throw std::runtime_error("cannot write native GRH node evolution trace");
        }

        bool wasPresent(Ref ref) const
        {
            return ref.graph ? graphSnapshot.length != 0 :
                (ref.operation ? beforeOperations : beforeValues).contains(ref);
        }

        std::size_t vertex(Ref ref)
        {
            const auto [found, inserted] = byRef.emplace(ref, vertices.size());
            if (inserted) vertices.push_back({ref, found->second, wasPresent(ref), false});
            return found->second;
        }

        std::size_t root(std::size_t index)
        {
            std::size_t target = index;
            while (vertices[target].parent != target) target = vertices[target].parent;
            while (vertices[index].parent != index)
            {
                const auto next = vertices[index].parent;
                vertices[index].parent = target;
                index = next;
            }
            return target;
        }

        void addStep(std::string action, std::string reason, std::vector<Ref> before, std::vector<Ref> after,
                     std::unique_ptr<Geometry> geometry = {})
        {
            if (finished) throw std::logic_error("node evolution step follows completed trace");
            if (reason.empty()) throw std::invalid_argument("node evolution reason is empty");
            if (action != "split" && action != "rewrite" && action != "replace" &&
                action != "delete" && action != "retain" && action != "merge" && action != "create")
                throw std::invalid_argument("unknown node evolution action");
            const auto normalize = [](std::vector<Ref> &refs) {
                refs.erase(std::remove_if(refs.begin(), refs.end(), [](Ref ref) { return ref.index == 0; }), refs.end());
                std::sort(refs.begin(), refs.end(), [](Ref left, Ref right) {
                    if (left.graph != right.graph) return left.graph < right.graph;
                    if (left.operation != right.operation) return left.operation < right.operation;
                    if (left.index != right.index) return left.index < right.index;
                    return left.generation < right.generation;
                });
                refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
            };
            normalize(before);
            normalize(after);
            if (before.empty() && after.empty()) return;
            std::optional<std::size_t> component;
            const auto join = [&](Ref ref) {
                const auto index = vertex(ref);
                if (!component) component = index;
                else
                {
                    const auto left = root(*component), right = root(index);
                    if (left != right) vertices[right].parent = left;
                }
            };
            for (Ref ref : before) join(ref);
            for (Ref ref : after) join(ref);
            steps.push_back({std::move(action), std::move(reason), std::move(before), std::move(after),
                             std::move(geometry)});
        }

        std::optional<grh::OperationId> indexedTarget(grh::OperationId writer) const
        {
            const auto kind = graph.opKind(writer);
            if (kind != grh::OperationKind::kRegisterWriteArrayPort &&
                kind != grh::OperationKind::kRegisterWriteArrayMaskPort) return std::nullopt;
            const auto symbol = graph.opAttrRange(writer).find("regSymbol");
            if (!symbol) return std::nullopt;
            const auto name = symbol->value().get<std::string>();
            if (!name) return std::nullopt;
            const auto target = graph.findOperation(*name);
            if (!target || graph.opKind(target) != grh::OperationKind::kRegister) return std::nullopt;
            return target;
        }

        std::optional<grh::OperationId> stateTarget(grh::OperationId operation) const
        {
            const auto symbol = graph.opAttrRange(operation).find("regSymbol");
            if (!symbol) return std::nullopt;
            const auto name = symbol->value().get<std::string>();
            if (!name) return std::nullopt;
            const auto target = graph.findOperation(*name);
            if (!target || (graph.opKind(target) != grh::OperationKind::kRegister &&
                            graph.opKind(target) != grh::OperationKind::kLatch)) return std::nullopt;
            return target;
        }

        struct IndexedUpdate
        {
            grh::LogicalType stateType;
            std::vector<grh::ValueId> indices;
            std::vector<std::optional<uint64_t>> constants;
            std::vector<int64_t> indexedAxes;
            std::vector<int64_t> payloadShape;
        };

        std::optional<IndexedUpdate> indexedUpdate(const grh::SemanticBinding &binding) const
        {
            if (binding.role != grh::SemanticBindingRole::RegisterNext ||
                !binding.operation || !binding.value) return std::nullopt;
            const auto target = indexedTarget(binding.operation);
            if (!target) return std::nullopt;
            const auto count = grh::arrayPortIndexCount(graph, binding.operation);
            const auto operands = graph.opOperands(binding.operation);
            if (!count || !*count || operands.size() <= *count + 1 ||
                operands[*count + 1] != binding.value) return std::nullopt;
            IndexedUpdate result;
            result.stateType = grh::stateLogicalType(graph, *target);
            if (binding.shape != result.stateType.shape || *count > binding.shape.size() ||
                binding.sourceWidth != result.stateType.leafWidth() ||
                binding.sourceSigned != result.stateType.isSigned) return std::nullopt;
            result.payloadShape.assign(binding.shape.begin() + *count, binding.shape.end());
            auto payload = result.stateType;
            for (std::size_t axis = 0; axis < *count; ++axis)
            {
                if (binding.shape[axis] <= 0) return std::nullopt;
                payload.width /= binding.shape[axis];
                const auto index = operands[axis + 1];
                const auto fixed = detail::constantIndex(graph, index);
                const int64_t coordinate = binding.projection.empty() ? -1 : binding.projection[axis];
                if (coordinate != -1 && (!fixed || *fixed != static_cast<uint64_t>(coordinate)))
                    return std::nullopt;
                if (coordinate == -1) result.indexedAxes.push_back(static_cast<int64_t>(axis));
                result.indices.push_back(index);
                result.constants.push_back(fixed);
            }
            for (std::size_t axis = *count; axis < binding.shape.size(); ++axis)
                if (!binding.projection.empty() && binding.projection[axis] != -1) return std::nullopt;
            payload.shape = result.payloadShape;
            if (result.indexedAxes.empty() || graph.valueLogicalType(binding.value) != payload)
                return std::nullopt;
            return result;
        }

        void source(std::ostream &out, const grh::SemanticBinding &binding, bool context)
        {
            out << "{\"path\":[";
            const auto *tree = graph.semanticTree();
            if (!tree || !tree->contains(binding.semantic))
                throw std::runtime_error("node evolution binding has no source tree node");
            auto path = tree->path(binding.semantic);
            for (std::size_t i = 1; i < path.size(); ++i)
            {
                if (i != 1) out.put(',');
                quoted(out, path[i]);
            }
            out << "],\"role\":";
            quoted(out, grh::toString(binding.role));
            out << ",\"shape\":";
            integers(out, binding.shape);
            out << ",\"shapeFieldDepths\":";
            if (binding.shapeFieldDepths.empty()) integers(out, std::vector<int64_t>(binding.shape.size(), 0));
            else integers(out, binding.shapeFieldDepths);
            out << ",\"projection\":";
            if (binding.projection.empty()) integers(out, std::vector<int64_t>(binding.shape.size(), -1));
            else integers(out, binding.projection);
            out << ",\"width\":" << binding.sourceWidth << ",\"signed\":"
                << (binding.sourceSigned ? "true" : "false");
            const bool broadcast = binding.role == grh::SemanticBindingRole::RegisterNext && binding.operation &&
                binding.value && graph.opKind(binding.operation) == grh::OperationKind::kRegisterFillPort &&
                graph.valueShape(binding.value).empty();
            if (broadcast) out << ",\"realization\":\"broadcast\"";
            const auto indexed = indexedUpdate(binding);
            if (indexed)
            {
                out << ",\"realization\":\"indexed-update\",\"writer\":";
                reference(out, GraphEvolutionTrace::ref(binding.operation));
                out << ",\"targetStateShape\":";
                integers(out, indexed->stateType.shape);
                out << ",\"consumedAxes\":[";
                for (std::size_t axis = 0; axis < indexed->indices.size(); ++axis)
                {
                    if (axis) out.put(',');
                    out << axis;
                }
                out << "],\"indexedAxes\":";
                integers(out, indexed->indexedAxes);
                out << ",\"indexOperands\":[";
                for (std::size_t axis = 0; axis < indexed->indices.size(); ++axis)
                {
                    if (axis) out.put(',');
                    reference(out, GraphEvolutionTrace::ref(indexed->indices[axis]));
                }
                out << "],\"indexConstants\":[";
                for (std::size_t axis = 0; axis < indexed->constants.size(); ++axis)
                {
                    if (axis) out.put(',');
                    if (indexed->constants[axis]) out << *indexed->constants[axis];
                    else out << "null";
                }
                out << "],\"payloadShape\":";
                integers(out, indexed->payloadShape);
            }
            const bool packedRole = binding.role == grh::SemanticBindingRole::Value ||
                binding.role == grh::SemanticBindingRole::RegisterCurrent ||
                binding.role == grh::SemanticBindingRole::RegisterNext ||
                binding.role == grh::SemanticBindingRole::MemoryReadAddress;
            if (!broadcast && !indexed && packedRole && binding.value && !binding.shape.empty() &&
                graph.valueType(binding.value) == grh::ValueType::Logic && graph.valueShape(binding.value).empty())
            {
                int64_t volume = 1;
                bool valid = binding.sourceWidth > 0;
                for (std::size_t axis = 0; axis < binding.shape.size() && valid; ++axis)
                {
                    if (!binding.projection.empty() && binding.projection[axis] != -1) continue;
                    const int64_t extent = binding.shape[axis];
                    if (extent <= 0 || volume > std::numeric_limits<int64_t>::max() / extent) valid = false;
                    else volume *= extent;
                }
                if (valid && volume > 1 && volume <= std::numeric_limits<int64_t>::max() / binding.sourceWidth &&
                    graph.valueWidth(binding.value) == volume * binding.sourceWidth)
                    out << ",\"realization\":\"packed\",\"packing\":\"row-major-lsb-first\"";
            }
            if (binding.role == grh::SemanticBindingRole::MemoryStorage && binding.operation)
            {
                const auto depth = graph.opAttrRange(binding.operation).find("firrtl.memoryDepth");
                if (depth) out << ",\"storageDepth\":" << depth->value().asInt();
            }
            if (context)
            {
                out << ",\"semantic\":" << binding.semantic;
                if (binding.value)
                {
                    out << ",\"value\":";
                    reference(out, GraphEvolutionTrace::ref(binding.value));
                }
                if (binding.operation)
                {
                    out << ",\"operation\":";
                    reference(out, GraphEvolutionTrace::ref(binding.operation));
                }
            }
            out.put('}');
        }

        void entity(bool before, Ref ref, const BindingIndex &index)
        {
            std::ostringstream body;
            auto &out = body;
            const grh::OperationId operation{ref.index, ref.generation, graph.id()};
            const grh::ValueId value{ref.index, ref.generation, graph.id()};
            auto rows = index.rows(ref);
            std::sort(rows.begin(), rows.end(), [&](uint32_t left, uint32_t right) {
                const auto &a = index.bindings[left], &b = index.bindings[right];
                return std::tie(a.semantic, a.role, a.shape, a.shapeFieldDepths, a.projection,
                                a.sourceWidth, a.sourceSigned, a.value.index, a.value.generation,
                                a.operation.index, a.operation.generation) <
                       std::tie(b.semantic, b.role, b.shape, b.shapeFieldDepths, b.projection,
                                b.sourceWidth, b.sourceSigned, b.value.index, b.value.generation,
                                b.operation.index, b.operation.generation);
            });
            if (sourceOnly && rows.empty() && (before || !wasPresent(ref))) return;
            out << "{\"name\":";
            quoted(out, ref.operation ? graph.operationDisplayName(operation) : graph.valueDisplayName(value));
            const auto symbol = ref.operation ? graph.operationSymbol(operation) : graph.valueSymbol(value);
            out << ",\"symbol\":";
            if (symbol.valid()) quoted(out, graph.symbolText(symbol));
            else out << "null";
            out << ",\"declared\":" << (symbol.valid() && graph.isDeclaredSymbol(symbol) ? "true" : "false");
            out << ",\"srcLoc\":";
            location(out, ref.operation ? graph.opSrcLoc(operation) : graph.valueSrcLoc(value));
            grh::LogicalType type{grh::ValueType::Logic, 0, false, grh::LogicDomain::FourState, {}};
            if (!ref.operation) type = graph.valueLogicalType(value);
            else if (graph.opKind(operation) == grh::OperationKind::kRegister ||
                     graph.opKind(operation) == grh::OperationKind::kLatch)
                type = grh::stateLogicalType(graph, operation);
            else if (!graph.opResults(operation).empty())
                type = graph.valueLogicalType(graph.opResults(operation)[0]);
            out << ",\"physical\":{\"width\":" << (type.width ? type.leafWidth() : 0)
                << ",\"packedWidth\":" << type.width
                << ",\"signed\":" << (type.isSigned ? "true" : "false") << ",\"shape\":";
            integers(out, type.shape);
            out << "},\"sources\":[";
            std::unordered_set<uint32_t, SourceBindingHash, SourceBindingEqual> exported(
                0, SourceBindingHash{index, graph, graph.semanticTree()}, SourceBindingEqual{index, graph, graph.semanticTree()});
            std::optional<uint32_t> firstExported;
            std::size_t exportedCount = 0;
            for (const uint32_t row : rows)
            {
                const auto &binding = index.bindings[row];
                if (ref.operation && binding.value) continue;
                if (firstExported)
                {
                    if (exported.empty())
                    {
                        exported.reserve(rows.size());
                        exported.insert(*firstExported);
                    }
                    if (!exported.insert(row).second) continue;
                }
                else firstExported = row;
                if (exportedCount++) out.put(',');
                source(out, binding, false);
            }
            out << "],\"structure\":{\"ownerSemanticIds\":";
            integers(out, ref.operation ? graph.opSemanticIds(operation) : graph.valueSemanticIds(value));
            out << ",\"bindings\":[";
            for (std::size_t i = 0; i < rows.size(); ++i)
            {
                if (i) out.put(',');
                source(out, index.bindings[rows[i]], true);
            }
            out.put(']');
            if (ref.operation)
            {
                out << ",\"type\":";
                quoted(out, grh::toString(type.kind));
                out << ",\"domain\":";
                quoted(out, grh::toString(type.domain));
                out << ",\"opKind\":";
                quoted(out, grh::toString(graph.opKind(operation)));
                out << ",\"operands\":[";
                const auto writeValues = [&](auto values) {
                    for (std::size_t i = 0; i < values.size(); ++i)
                    {
                        if (i) out.put(',');
                        reference(out, GraphEvolutionTrace::ref(values[i]));
                    }
                };
                writeValues(graph.opOperands(operation));
                out << "],\"results\":[";
                writeValues(graph.opResults(operation));
                out.put(']');
                if (const auto target = stateTarget(operation))
                {
                    const auto stateType = grh::stateLogicalType(graph, *target);
                    out << ",\"stateTarget\":{\"ref\":";
                    reference(out, GraphEvolutionTrace::ref(*target));
                    out << ",\"width\":" << stateType.leafWidth()
                        << ",\"packedWidth\":" << stateType.width
                        << ",\"signed\":" << (stateType.isSigned ? "true" : "false") << ",\"shape\":";
                    integers(out, stateType.shape);
                    out << ",\"type\":";
                    quoted(out, grh::toString(stateType.kind));
                    out << ",\"domain\":";
                    quoted(out, grh::toString(stateType.domain));
                    out.put('}');
                }
                out << ",\"attrs\":[";
                std::vector<grh::AttrRef> attrs;
                for (const auto attr : graph.opAttrRange(operation)) attrs.push_back(attr);
                std::sort(attrs.begin(), attrs.end(), [](auto left, auto right) { return left.key() < right.key(); });
                for (std::size_t i = 0; i < attrs.size(); ++i)
                {
                    if (i) out.put(',');
                    out << "{\"key\":";
                    quoted(out, attrs[i].key());
                    out << ",\"value\":";
                    attribute(out, attrs[i].value());
                    out.put('}');
                }
                out.put(']');
            }
            else
            {
                out << ",\"def\":";
                if (const auto def = graph.valueDef(value)) reference(out, GraphEvolutionTrace::ref(def));
                else out << "null";
                out << ",\"type\":";
                quoted(out, grh::toString(type.kind));
                out << ",\"domain\":";
                quoted(out, grh::toString(type.domain));
                out << ",\"input\":" << (graph.valueIsInput(value) ? "true" : "false")
                    << ",\"output\":" << (graph.valueIsOutput(value) ? "true" : "false")
                    << ",\"inout\":" << (graph.valueIsInout(value) ? "true" : "false");
            }
            out << "}}";
            emitBody(before, ref, body.str());
        }

        void graphEntity(bool before)
        {
            if (sourceOnly) return;
            std::ostringstream body;
            auto &out = body;
            out << "{\"name\":";
            quoted(out, graph.symbol());
            out << ",\"sources\":[],\"metadata\":[";
            std::vector<grh::AttrKV> attrs(graph.metadata().begin(), graph.metadata().end());
            std::sort(attrs.begin(), attrs.end(), [](const auto &left, const auto &right) { return left.key < right.key; });
            for (std::size_t i = 0; i < attrs.size(); ++i)
            {
                if (i) out.put(',');
                out << "{\"key\":"; quoted(out, attrs[i].key);
                out << ",\"value\":"; attribute(out, grh::AttrValueView(&attrs[i].value));
                out.put('}');
            }
            out << "],\"declaredSymbols\":[";
            const auto declared = graph.declaredSymbols();
            for (std::size_t i = 0; i < declared.size(); ++i)
            {
                if (i) out.put(',');
                quoted(out, graph.symbolText(declared[i]));
            }
            out << "],\"generateGroups\":[";
            const auto &groups = graph.generateGroups();
            for (std::size_t i = 0; i < groups.size(); ++i)
            {
                if (i) out.put(',');
                out << "{\"scope\":"; quoted(out, graph.symbolText(groups[i].scope));
                out << ",\"name\":"; quoted(out, graph.symbolText(groups[i].name));
                out << ",\"symbols\":[";
                for (std::size_t j = 0; j < groups[i].symbols.size(); ++j)
                {
                    if (j) out.put(',');
                    quoted(out, graph.symbolText(groups[i].symbols[j]));
                }
                out << "]}";
            }
            const auto ports = [&](std::string_view name, auto values) {
                out << "],\"" << name << "\":[";
                for (std::size_t i = 0; i < values.size(); ++i)
                {
                    if (i) out.put(',');
                    out << "{\"name\":"; quoted(out, values[i].name);
                    out << ",\"value\":"; reference(out, GraphEvolutionTrace::ref(values[i].value));
                    out.put('}');
                }
            };
            ports("inputs", graph.inputPorts());
            ports("outputs", graph.outputPorts());
            out << "],\"inouts\":[";
            const auto inouts = graph.inoutPorts();
            for (std::size_t i = 0; i < inouts.size(); ++i)
            {
                if (i) out.put(',');
                out << "{\"name\":"; quoted(out, inouts[i].name);
                out << ",\"in\":"; reference(out, GraphEvolutionTrace::ref(inouts[i].in));
                out << ",\"out\":"; reference(out, GraphEvolutionTrace::ref(inouts[i].out));
                out << ",\"oe\":"; reference(out, GraphEvolutionTrace::ref(inouts[i].oe));
                out.put('}');
            }
            out << "],\"sourceTreeContext\":";
            if (const auto *tree = graph.semanticTree())
            {
                out << "{\"sourceUnit\":"; quoted(out, tree->sourceUnit());
                out << ",\"circuit\":"; quoted(out, tree->circuit());
                out << ",\"nodeCount\":" << tree->size() << '}';
            }
            else out << "null";
            out.put('}');
            emitBody(before, GraphEvolutionTrace::graphRef(graph), body.str());
        }

        void inventory(bool before)
        {
            const BindingIndex index(graph);
            graphEntity(before);
            for (const auto operation : graph.operations()) entity(before, GraphEvolutionTrace::ref(operation), index);
            for (const auto value : graph.values()) entity(before, GraphEvolutionTrace::ref(value), index);
            out.flush();
            check();
        }

        void mutation(Ref ref, const Snapshot *after,
                      const std::unordered_map<std::size_t, uint64_t> &componentEvents)
        {
            const auto vertexFound = byRef.find(ref);
            const auto event = vertexFound == byRef.end() ? componentEvents.end() :
                componentEvents.find(root(vertexFound->second));
            if (event == componentEvents.end())
            {
                out << "{\"record\":\"replay_violation\",\"reason\":\"boundary_change_without_native_event\",\"ref\":";
                reference(out, ref);
                out << "}\n";
                ++violationCount;
                return;
            }
            auto *old = snapshot(ref);
            const std::string beforeBody = old ? read(beforeCache, *old) : std::string{};
            const std::string afterBody = after ? read(afterCache, *after) : std::string{};
            const auto beforeFields = old ? fields(beforeBody) : std::map<std::string, std::string_view>{};
            const auto afterFields = after ? fields(afterBody) : std::map<std::string, std::string_view>{};
            std::map<std::string, std::string_view> set, expect;
            std::vector<std::string> unset;
            for (const auto &[name, value] : afterFields)
            {
                const auto previous = beforeFields.find(name);
                if (previous == beforeFields.end() || previous->second != value)
                {
                    set.emplace(name, value);
                    if (previous != beforeFields.end()) expect.emplace(name, previous->second);
                }
            }
            for (const auto &[name, value] : beforeFields)
            {
                if (!afterFields.contains(name))
                {
                    unset.push_back(name);
                    expect.emplace(name, value);
                }
            }
            const auto object = [&](const auto &values) {
                out.put('{');
                bool first = true;
                for (const auto &[name, value] : values)
                {
                    if (!first) out.put(',');
                    first = false;
                    quoted(out, name);
                    out << ':' << value;
                }
                out.put('}');
            };
            out << "{\"record\":\"mutation\",\"seq\":" << ++mutationCount
                << ",\"eventSeq\":" << event->second << ",\"action\":";
            quoted(out, !old ? "create" : !after ? "erase" : "update");
            out << ",\"ref\":"; reference(out, ref);
            out << ",\"set\":"; object(set);
            out << ",\"unset\":[";
            // Erase removes the entity as a unit; expect already contains its
            // complete old body, and set/unset must remain empty.
            if (after)
                for (std::size_t i = 0; i < unset.size(); ++i)
                {
                    if (i) out.put(',');
                    quoted(out, unset[i]);
                }
            out.put(']');
            out << ",\"expect\":"; object(expect);
            out << "}\n";
        }

        void finish()
        {
            if (finished) return;
            inventory(false);
            struct Component
            {
                std::vector<Ref> before, after;
                std::vector<std::size_t> steps;
            };
            std::unordered_map<std::size_t, std::size_t> components;
            std::unordered_map<std::size_t, uint64_t> componentEvents;
            std::vector<Component> groups;
            for (std::size_t i = 0; i < steps.size(); ++i)
            {
                const auto &step = steps[i];
                const Ref representative = step.before.empty() ? step.after.front() : step.before.front();
                const auto key = root(byRef.at(representative));
                const auto [found, inserted] = components.emplace(key, groups.size());
                if (inserted) groups.emplace_back();
                groups[found->second].steps.push_back(i);
            }
            for (std::size_t i = 0; i < vertices.size(); ++i)
            {
                const auto &vertex = vertices[i];
                auto &group = groups[components.at(root(i))];
                if (vertex.before) group.before.push_back(vertex.ref);
                if (vertex.after) group.after.push_back(vertex.ref);
            }
            for (const auto &group : groups)
            {
                if (group.before.empty() && group.after.empty()) continue;
                const auto &initial = steps[group.steps.front()];
                const bool allRetained = std::all_of(group.steps.begin(), group.steps.end(),
                    [&](std::size_t step) { return steps[step].action == "retain"; });
                const char *action = group.before.empty() ? "create" : group.after.empty() ? "delete" :
                    group.after.size() > group.before.size() ? "split" : "rewrite";
                if (allRetained && group.before == group.after)
                    action = "retain";
                out << "{\"record\":\"event\",\"seq\":" << ++eventCount << ",\"action\":";
                const Ref representative = initial.before.empty() ? initial.after.front() : initial.before.front();
                if (!allRetained) componentEvents.emplace(root(byRef.at(representative)), eventCount);
                quoted(out, action);
                out << ",\"reason\":";
                quoted(out, initial.reason);
                out << ",\"before\":";
                references(out, group.before);
                out << ",\"after\":";
                references(out, group.after);
                out << ",\"geometry\":{\"partition\":false},\"steps\":[";
                for (std::size_t i = 0; i < group.steps.size(); ++i)
                {
                    if (i) out.put(',');
                    const auto &step = steps[group.steps[i]];
                    out << "{\"seq\":" << group.steps[i] + 1 << ",\"action\":";
                    quoted(out, step.action);
                    out << ",\"reason\":";
                    quoted(out, step.reason);
                    out << ",\"before\":";
                    references(out, step.before);
                    out << ",\"after\":";
                    references(out, step.after);
                    out << ",\"geometry\":{\"partition\":false";
                    if (step.geometry)
                    {
                        const auto &geometry = *step.geometry;
                        out << ",\"kind\":";
                        quoted(out, geometry.kind);
                        out << ",\"base\":";
                        reference(out, geometry.base);
                        out << ",\"physicalShape\":";
                        integers(out, geometry.physicalShape);
                        out << ",\"offset\":" << geometry.offset << ",\"width\":" << geometry.width;
                        if (geometry.element >= 0)
                        {
                            out << ",\"element\":" << geometry.element << ",\"coordinates\":";
                            integers(out, geometry.coordinates);
                        }
                    }
                    out.put('}');
                    out.put('}');
                }
                out << "]}\n";
            }
            if (!sourceOnly)
            {
                for (const auto &change : pending) mutation(change.ref, &change.after, componentEvents);
                const auto erased = [&](const auto &rows, bool operation) {
                    for (std::size_t i = 1; i < rows.size(); ++i)
                        if (rows[i].length && !(rows[i].length & (uint32_t{1} << 31)))
                            mutation({operation, static_cast<uint32_t>(i), rows[i].generation}, nullptr, componentEvents);
                };
                erased(operationSnapshots, true);
                erased(valueSnapshots, false);
            }
            out << "{\"record\":\"footer\",\"complete\":true,\"beforeCount\":" << beforeCount
                << ",\"afterCount\":" << afterCount << ",\"eventCount\":" << eventCount;
            if (!sourceOnly)
                out << ",\"mutationCount\":" << mutationCount << ",\"replayViolationCount\":" << violationCount
                    << ",\"replayComplete\":" << (violationCount ? "false" : "true");
            out << "}\n";
            out.flush();
            check();
            finished = true;
        }
    };

    GraphEvolutionTrace::GraphEvolutionTrace(grh::Graph &graph, const std::string &path,
                                             const std::string &pass, const std::string &inputSha256,
                                             bool sourceEndpointsOnly)
    {
        if (!path.empty()) impl_ = std::make_unique<Impl>(graph, path, pass, inputSha256, sourceEndpointsOnly);
    }

    GraphEvolutionTrace::~GraphEvolutionTrace() = default;

    GraphEvolutionTrace::Ref GraphEvolutionTrace::ref(grh::OperationId operation) noexcept
    {
        return {true, operation.index, operation.generation};
    }

    GraphEvolutionTrace::Ref GraphEvolutionTrace::ref(grh::ValueId value) noexcept
    {
        return {false, value.index, value.generation};
    }

    GraphEvolutionTrace::Ref GraphEvolutionTrace::graphRef(const grh::Graph &graph) noexcept
    {
        return {false, graph.id().index, graph.id().generation, true};
    }

    void GraphEvolutionTrace::step(std::string action, std::string reason, std::vector<Ref> before,
                                  std::vector<Ref> after)
    {
        if (impl_) impl_->addStep(std::move(action), std::move(reason), std::move(before), std::move(after));
    }

    void GraphEvolutionTrace::step(std::string action, std::string reason, std::vector<Ref> before,
                                  std::vector<Ref> after, Geometry geometry)
    {
        if (impl_) impl_->addStep(std::move(action), std::move(reason), std::move(before), std::move(after),
                                 std::make_unique<Geometry>(std::move(geometry)));
    }

    void GraphEvolutionTrace::retain(grh::OperationId operation, std::string reason)
    {
        step("retain", std::move(reason), {ref(operation)}, {ref(operation)});
    }

    void GraphEvolutionTrace::finish()
    {
        if (impl_) impl_->finish();
    }

} // namespace wolvrix::lib::transform
