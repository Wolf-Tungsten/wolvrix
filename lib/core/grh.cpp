#include "core/grh.hpp"
#include "core/detail/attribute_keys.hpp"
#include "core/detail/block_executor.hpp"
#include "core/op_descriptor.hpp"
#include "slang/util/SmallVector.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include "slang/text/Json.h"

namespace wolvrix::lib::grh
{

    namespace {
        template <typename Function>
        void fillBlockRows(std::size_t count, std::size_t threads, Function&& fill) {
            // Preparation reserves all vectors and imports payload IDs. Workers
            // only write disjoint scalar rows, with no allocation or graph API.
            threads = std::min(threads, count / 16384);
            if (threads < 2) {
                fill(0, count);
                return;
            }
            std::vector<std::jthread> workers;
            workers.reserve(threads - 1);
            for (std::size_t worker = 1; worker < threads; ++worker) {
                const std::size_t begin = count * worker / threads;
                const std::size_t end = count * (worker + 1) / threads;
                workers.emplace_back([&, begin, end] { fill(begin, end); });
            }
            fill(0, count / threads);
        }

        struct ProfileCounters {
            std::atomic<uint64_t> symbolInserts{0};
            std::atomic<uint64_t> attributeWrites{0};
            std::atomic<uint64_t> attributeBytes{0};
            std::atomic<uint64_t> srcLocWrites{0};
            std::atomic<uint64_t> srcLocBytes{0};
            std::atomic<uint64_t> valueAllocations{0};
            std::atomic<uint64_t> operationAllocations{0};
            std::atomic<uint64_t> freezeCount{0};
            std::atomic<uint64_t> thawCount{0};
            std::atomic<bool> enabled{[] {
                const char *env = std::getenv("WOLVRIX_GRH_PROFILE");
                return env && std::string_view(env) == "1";
            }()};
        };

        ProfileCounters profileCounters;

        uint64_t srcLocBytes(const SrcLocView& loc) noexcept {
            return static_cast<uint64_t>(loc.file.size() + loc.origin.size() +
                                         loc.pass.size() + loc.note.size());
        }

        uint64_t attributePayloadBytes(const AttributeValue& value) noexcept {
            return std::visit([](const auto& item) -> uint64_t {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, std::string>) return item.size();
                else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
                    uint64_t bytes = 0; for (const auto& text : item) bytes += text.size(); return bytes;
                } else if constexpr (std::is_same_v<T, std::vector<bool>>) return (item.size() + 7) / 8;
                else if constexpr (std::is_same_v<T, std::vector<int64_t>>) return item.size() * sizeof(int64_t);
                else if constexpr (std::is_same_v<T, std::vector<double>>) return item.size() * sizeof(double);
                else return sizeof(T);
            }, value);
        }
    }

    bool graphProfileEnabled() noexcept { return profileCounters.enabled.load(std::memory_order_relaxed); }
    void setGraphProfileEnabled(bool enabled) noexcept { profileCounters.enabled.store(enabled, std::memory_order_relaxed); }
    void resetGraphProfileStats() noexcept {
        profileCounters.symbolInserts.store(0, std::memory_order_relaxed);
        profileCounters.attributeWrites.store(0, std::memory_order_relaxed);
        profileCounters.attributeBytes.store(0, std::memory_order_relaxed);
        profileCounters.srcLocWrites.store(0, std::memory_order_relaxed);
        profileCounters.srcLocBytes.store(0, std::memory_order_relaxed);
        profileCounters.valueAllocations.store(0, std::memory_order_relaxed);
        profileCounters.operationAllocations.store(0, std::memory_order_relaxed);
        profileCounters.freezeCount.store(0, std::memory_order_relaxed);
        profileCounters.thawCount.store(0, std::memory_order_relaxed);
    }
    GraphProfileStats graphProfileStats() noexcept {
        return {
            profileCounters.symbolInserts.load(std::memory_order_relaxed),
            profileCounters.attributeWrites.load(std::memory_order_relaxed),
            profileCounters.attributeBytes.load(std::memory_order_relaxed),
            profileCounters.srcLocWrites.load(std::memory_order_relaxed),
            profileCounters.srcLocBytes.load(std::memory_order_relaxed),
            profileCounters.valueAllocations.load(std::memory_order_relaxed),
            profileCounters.operationAllocations.load(std::memory_order_relaxed),
            profileCounters.freezeCount.load(std::memory_order_relaxed),
            profileCounters.thawCount.load(std::memory_order_relaxed)};
    }

    bool attributeValueIsJsonSerializable(const AttributeValue &value)
    {
        struct Visitor
        {
            bool operator()(bool) const noexcept { return true; }
            bool operator()(int64_t) const noexcept { return true; }
            bool operator()(double v) const noexcept { return std::isfinite(v); }
            bool operator()(const std::string &) const noexcept { return true; }
            bool operator()(const std::vector<bool> &) const noexcept { return true; }
            bool operator()(const std::vector<int64_t> &) const noexcept { return true; }
            bool operator()(const std::vector<double> &arr) const noexcept
            {
                for (double entry : arr)
                {
                    if (!std::isfinite(entry))
                    {
                        return false;
                    }
                }
                return true;
            }
            bool operator()(const std::vector<std::string> &) const noexcept { return true; }
        };

        return std::visit(Visitor{}, value);
    }

    AttrValueKind AttrValueView::kind() const
    {
        if (payload_) return payload_->attribute(record_).kind;
        if (!value_) throw std::logic_error("invalid attribute value view");
        return static_cast<AttrValueKind>(value_->index());
    }

    bool AttrValueView::asBool() const
    {
        if (kind() != AttrValueKind::Bool) throw std::logic_error("attribute value is not bool");
        return payload_ ? payload_->attribute(record_).payload != 0 : std::get<bool>(*value_);
    }

    int64_t AttrValueView::asInt() const
    {
        if (kind() != AttrValueKind::Int) throw std::logic_error("attribute value is not int64");
        if (!payload_) return std::get<int64_t>(*value_);
        const uint64_t bits = payload_->attribute(record_).payload;
        int64_t result;
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    }

    double AttrValueView::asDouble() const
    {
        if (kind() != AttrValueKind::Double) throw std::logic_error("attribute value is not double");
        if (!payload_) return std::get<double>(*value_);
        const uint64_t bits = payload_->attribute(record_).payload;
        double result;
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    }

    std::string_view AttrValueView::asString() const
    {
        if (kind() != AttrValueKind::String) throw std::logic_error("attribute value is not string");
        return payload_ ? payload_->string(static_cast<uint32_t>(payload_->attribute(record_).payload))
                        : std::string_view(std::get<std::string>(*value_));
    }

    BoolListView AttrValueView::asBoolList() const
    {
        if (kind() != AttrValueKind::BoolList) throw std::logic_error("attribute value is not bool list");
        if (!payload_) return BoolListView(std::get<std::vector<bool>>(*value_));
        const auto record = payload_->attribute(record_);
        return {payload_->bitPayload(record.slice), record.slice.count};
    }

    std::span<const int64_t> AttrValueView::asIntList() const
    {
        if (kind() != AttrValueKind::IntList) throw std::logic_error("attribute value is not int list");
        return payload_ ? payload_->int64Payload(payload_->attribute(record_).slice)
                        : std::span<const int64_t>(std::get<std::vector<int64_t>>(*value_));
    }

    std::span<const double> AttrValueView::asDoubleList() const
    {
        if (kind() != AttrValueKind::DoubleList) throw std::logic_error("attribute value is not double list");
        return payload_ ? payload_->doublePayload(payload_->attribute(record_).slice)
                        : std::span<const double>(std::get<std::vector<double>>(*value_));
    }

    StringListView AttrValueView::asStringList() const
    {
        if (kind() != AttrValueKind::StringList) throw std::logic_error("attribute value is not string list");
        return payload_ ? StringListView(payload_, payload_->stringListPayload(payload_->attribute(record_).slice))
                        : StringListView(std::get<std::vector<std::string>>(*value_));
    }

    AttributeValue AttrValueView::toOwned() const
    {
        if (!payload_) {
            if (!value_) throw std::logic_error("invalid attribute value view");
            return *value_;
        }
        switch (kind()) {
        case AttrValueKind::Bool: return asBool();
        case AttrValueKind::Int: return asInt();
        case AttrValueKind::Double: return asDouble();
        case AttrValueKind::String: return std::string(asString());
        case AttrValueKind::BoolList: { const auto values = asBoolList(); return std::vector<bool>(values.begin(), values.end()); }
        case AttrValueKind::IntList: { const auto values = asIntList(); return std::vector<int64_t>(values.begin(), values.end()); }
        case AttrValueKind::DoubleList: { const auto values = asDoubleList(); return std::vector<double>(values.begin(), values.end()); }
        case AttrValueKind::StringList: { const auto values = asStringList(); return std::vector<std::string>(values.begin(), values.end()); }
        }
        throw std::logic_error("unknown attribute value alternative");
    }

    std::string_view StringListView::operator[](std::size_t index) const
    {
        return payload_ ? payload_->string(ids_[index]) : std::string_view(legacy_[index]);
    }

    bool operator==(AttrValueView left, AttrValueView right)
    {
        if (!left.valid() || !right.valid()) return left.valid() == right.valid();
        if (left.kind() != right.kind()) return false;
        const auto equal = [](const auto& a, const auto& b) {
            return std::equal(a.begin(), a.end(), b.begin(), b.end());
        };
        switch (left.kind()) {
        case AttrValueKind::Bool: return left.asBool() == right.asBool();
        case AttrValueKind::Int: return left.asInt() == right.asInt();
        case AttrValueKind::Double: return left.asDouble() == right.asDouble();
        case AttrValueKind::String: return left.asString() == right.asString();
        case AttrValueKind::BoolList: return equal(left.asBoolList(), right.asBoolList());
        case AttrValueKind::IntList: return equal(left.asIntList(), right.asIntList());
        case AttrValueKind::DoubleList: return equal(left.asDoubleList(), right.asDoubleList());
        case AttrValueKind::StringList: return equal(left.asStringList(), right.asStringList());
        }
        return false;
    }

    AttrRef AttrRange::const_iterator::operator*() const
    {
        if (!payload_) return AttrRef(attrs_[index_].key, &attrs_[index_].value);
        const uint32_t id = records_[index_];
        return AttrRef(payload_->string(payload_->attribute(id).key), AttrValueView(payload_, id));
    }

    std::optional<AttrRef> AttrRange::find(std::string_view key) const noexcept
    {
        for (const auto attr : *this) if (attr.key() == key) return attr;
        return std::nullopt;
    }

    GraphPayload::GraphPayload(std::shared_ptr<detail::StringPool> strings)
        : stringPool_(strings ? std::move(strings) : std::make_shared<detail::StringPool>())
    {
        // ID zero is the null payload reference.  Keeping it in every pool
        // makes malformed/optional records cheap to inspect and preserves
        // stable one-based record IDs across compact().
        attrKeys_.push_back(0);
        attrTags_.push_back(AttrValueKind::Bool);
        attrPayloads_.push_back(0);
        srcLocs_.push_back({});
    }

    GraphPayload::StringId GraphPayload::internString(std::string_view text)
    {
        return stringPool_->intern(text);
    }

    GraphPayload::StringId GraphPayload::internAttrKey(std::string_view text)
    {
        const auto& keys = detail::kBuiltinAttributeKeys;
        const auto found = std::lower_bound(keys.begin(), keys.end(), text);
        if (found != keys.end() && *found == text)
            return uint32_t{0x80000000} | static_cast<uint32_t>(found - keys.begin());
        return internString(text);
    }

    std::string_view GraphPayload::string(StringId id) const noexcept
    {
        if ((id & uint32_t{0x80000000}) != 0) {
            const auto index = id & uint32_t{0x7fffffff};
            return index < detail::kBuiltinAttributeKeys.size() ? detail::kBuiltinAttributeKeys[index] : std::string_view{};
        }
        return stringPool_->text(id);
    }

    GraphPayload::RecordId GraphPayload::internAttribute(std::string_view key, const AttributeValue& value)
    {
        return internAttribute(key, AttrValueView(&value));
    }

    GraphPayload::RecordId GraphPayload::internAttribute(std::string_view key, AttrValueView value)
    {
        if (value.payload_ == this) {
            const auto record = attribute(value.record_);
            attrKeys_.push_back(internAttrKey(key));
            attrTags_.push_back(record.kind);
            attrPayloads_.push_back(record.payload);
            return static_cast<RecordId>(attrKeys_.size() - 1);
        }
        const auto kind = value.kind();
        uint64_t payload = 0;
        Slice slice;
        const auto slicePayload = [&] {
            return static_cast<uint64_t>(slice.offset) | (static_cast<uint64_t>(slice.count) << 32);
        };
        switch (kind) {
        case AttrValueKind::Bool: payload = value.asBool() ? 1 : 0; break;
        case AttrValueKind::Int: { const int64_t item = value.asInt(); std::memcpy(&payload, &item, sizeof(item)); break; }
        case AttrValueKind::Double: { const double item = value.asDouble(); std::memcpy(&payload, &item, sizeof(item)); break; }
        case AttrValueKind::String: payload = internString(value.asString()); break;
        case AttrValueKind::BoolList: {
            const auto items = value.asBoolList();
            slice = {static_cast<uint32_t>(bitPayload_.size()), static_cast<uint32_t>(items.size())};
            bitPayload_.appendGenerated((items.size() + 63) / 64, [&](std::size_t index) {
                const std::size_t i = index * 64;
                uint64_t word = 0;
                for (std::size_t bit = i; bit < std::min(items.size(), i + 64); ++bit)
                    if (items[bit]) word |= uint64_t{1} << (bit - i);
                return word;
            });
            payload = slicePayload(); break;
        }
        case AttrValueKind::IntList: {
            const auto items = value.asIntList();
            slice = {static_cast<uint32_t>(int64Payload_.size()), static_cast<uint32_t>(items.size())};
            int64Payload_.append(items);
            payload = slicePayload(); break;
        }
        case AttrValueKind::DoubleList: {
            const auto items = value.asDoubleList();
            slice = {static_cast<uint32_t>(doublePayload_.size()), static_cast<uint32_t>(items.size())};
            doublePayload_.append(items);
            payload = slicePayload(); break;
        }
        case AttrValueKind::StringList: {
            const auto items = value.asStringList();
            slice = {static_cast<uint32_t>(stringListPayload_.size()), static_cast<uint32_t>(items.size())};
            stringListPayload_.appendGenerated(items.size(), [&](std::size_t i) { return internString(items[i]); });
            payload = slicePayload(); break;
        }
        }
        attrKeys_.push_back(internAttrKey(key));
        attrTags_.push_back(kind);
        attrPayloads_.push_back(payload);
        return static_cast<RecordId>(attrKeys_.size() - 1);
    }

    GraphPayload::RecordId GraphPayload::internStringListAttribute(std::string_view key, std::span<const StringId> values)
    {
        const uint64_t payload = static_cast<uint32_t>(stringListPayload_.size()) |
                                 (static_cast<uint64_t>(values.size()) << 32);
        stringListPayload_.append(values);
        attrKeys_.push_back(internAttrKey(key));
        attrTags_.push_back(AttrValueKind::StringList);
        attrPayloads_.push_back(payload);
        return static_cast<RecordId>(attrKeys_.size() - 1);
    }

    GraphPayload::RecordId GraphPayload::internIntListAttribute(std::string_view key, std::span<const int64_t> values)
    {
        const uint64_t payload = static_cast<uint32_t>(int64Payload_.size()) |
                                 (static_cast<uint64_t>(values.size()) << 32);
        int64Payload_.append(values);
        attrKeys_.push_back(internAttrKey(key));
        attrTags_.push_back(AttrValueKind::IntList);
        attrPayloads_.push_back(payload);
        return static_cast<RecordId>(attrKeys_.size() - 1);
    }

    GraphPayload::RecordId GraphPayload::internSrcLoc(const SrcLoc& loc)
    {
        return internSrcLocView(SrcLocView(loc));
    }

    GraphPayload::RecordId GraphPayload::internSrcLocView(SrcLocView loc)
    {
        const SrcLocRecord record{internString(loc.file), internString(loc.origin),
                                  internString(loc.pass), internString(loc.note), loc.line,
                                  loc.column, loc.endLine, loc.endColumn};
        if (const auto it = srcLocIds_.find(record); it != srcLocIds_.end()) return it->second;
        const RecordId id = static_cast<RecordId>(srcLocs_.size());
        srcLocs_.push_back(record);
        srcLocIds_.emplace(record, id);
        return id;
    }

    std::size_t GraphPayload::SrcLocHash::operator()(const SrcLocRecord& loc) const noexcept
    {
        std::size_t hash = 0;
        for (uint32_t item : {loc.file, loc.origin, loc.pass, loc.note, loc.line, loc.column,
                              loc.endLine, loc.endColumn})
            hash ^= std::hash<uint32_t>{}(item) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
        return hash;
    }

    std::optional<SrcLoc> GraphPayload::srcLocValue(RecordId id) const
    {
        if (!id) return std::nullopt;
        const SrcLocRecord& record = srcLoc(id);
        SrcLoc result;
        result.file = string(record.file);
        result.origin = string(record.origin);
        result.pass = string(record.pass);
        result.note = string(record.note);
        result.line = record.line;
        result.column = record.column;
        result.endLine = record.endLine;
        result.endColumn = record.endColumn;
        return result;
    }

    GraphPayload::RecordId GraphPayload::importSrcLoc(const GraphPayload& source, RecordId id)
    {
        if (!id) return 0;
        const SrcLocRecord& sourceRecord = source.srcLoc(id);
        SrcLocRecord record = sourceRecord;
        record.file = internString(source.string(sourceRecord.file));
        record.origin = internString(source.string(sourceRecord.origin));
        record.pass = internString(source.string(sourceRecord.pass));
        record.note = internString(source.string(sourceRecord.note));
        if (const auto it = srcLocIds_.find(record); it != srcLocIds_.end()) return it->second;
        const RecordId result = static_cast<RecordId>(srcLocs_.size());
        srcLocs_.push_back(record);
        srcLocIds_.emplace(record, result);
        return result;
    }

    GraphPayload::AttributeRecord GraphPayload::attribute(RecordId id) const
    {
        if (id == 0 || id >= attrKeys_.size()) throw std::out_of_range("GraphStore attribute id");
        const uint64_t payload = attrPayloads_[id];
        return {attrKeys_[id], attrTags_[id], payload,
                {static_cast<uint32_t>(payload), static_cast<uint32_t>(payload >> 32)}};
    }

    void GraphPayload::replaceAttribute(RecordId target, RecordId replacement)
    {
        attrKeys_[target] = attrKeys_[replacement];
        attrTags_[target] = attrTags_[replacement];
        attrPayloads_[target] = attrPayloads_[replacement];
        // The replacement record was just appended; its variable payload is
        // retained, and any superseded payload is reclaimed by compact().
        attrKeys_.pop_back();
        attrTags_.pop_back();
        attrPayloads_.pop_back();
    }

    const GraphPayload::SrcLocRecord& GraphPayload::srcLoc(RecordId id) const
    {
        if (id == 0 || id >= srcLocs_.size()) throw std::out_of_range("GraphStore SrcLoc id");
        return srcLocs_[id];
    }

    std::span<const int64_t> GraphPayload::int64Payload(Slice slice) const noexcept
    {
        if (slice.offset > int64Payload_.size() || slice.count > int64Payload_.size() - slice.offset)
            return {};
        return int64Payload_.slice(slice.offset, slice.count);
    }

    std::span<const double> GraphPayload::doublePayload(Slice slice) const noexcept
    {
        if (slice.offset > doublePayload_.size() || slice.count > doublePayload_.size() - slice.offset)
            return {};
        return doublePayload_.slice(slice.offset, slice.count);
    }

    std::span<const uint64_t> GraphPayload::bitPayload(Slice slice) const noexcept
    {
        const std::size_t words = (static_cast<std::size_t>(slice.count) + 63) / 64;
        if (slice.offset > bitPayload_.size() || words > bitPayload_.size() - slice.offset)
            return {};
        return bitPayload_.slice(slice.offset, words);
    }

    std::span<const GraphPayload::StringId> GraphPayload::stringListPayload(Slice slice) const noexcept
    {
        if (slice.offset > stringListPayload_.size() || slice.count > stringListPayload_.size() - slice.offset)
            return {};
        return stringListPayload_.slice(slice.offset, slice.count);
    }

    GraphPayload::Stats GraphPayload::stats() const noexcept
    {
        return {0, 0, 0, 0, 0,
                stringPool_->size(), attrKeys_.size() - 1, srcLocs_.size() - 1,
                int64Payload_.size(), doublePayload_.size(), bitPayload_.size(),
                stringListPayload_.size()};
    }

    void GraphPayload::compact()
    {
        stringPool_->compact();
        int64Payload_.shrink_to_fit();
        doublePayload_.shrink_to_fit();
        bitPayload_.shrink_to_fit();
        stringListPayload_.shrink_to_fit();
        attrKeys_.shrink_to_fit();
        attrTags_.shrink_to_fit();
        attrPayloads_.shrink_to_fit();
        srcLocs_.shrink_to_fit();
    }

    namespace
    {
        template <typename T>
        std::span<const T> spanForRange(const std::vector<T> &storage, const Range &range)
        {
            if (range.count == 0)
            {
                return {};
            }
            return std::span<const T>(storage.data() + range.offset, range.count);
        }

        std::string symbolTextOrEmpty(const GraphSymbolTable &symbols, SymbolId sym)
        {
            return sym.valid() ? std::string(symbols.text(sym)) : std::string();
        }

        // Hard invariant for array values (see docs/grh/grh-ir.md "数组类型"):
        // `width` is the total bit width, every shape dimension is positive,
        // the dimension product must not overflow int64 and must divide the
        // total width, and only Logic values may carry a shape.
        void validateValueShape(int32_t width, ValueType type, std::span<const int64_t> shape,
                                std::string_view context)
        {
            if (shape.empty())
            {
                return;
            }
            if (type != ValueType::Logic)
            {
                throw std::runtime_error(std::string(context) + " shape requires a Logic value");
            }
            int64_t elementCount = 1;
            for (const int64_t dim : shape)
            {
                if (dim <= 0)
                {
                    throw std::runtime_error(std::string(context) + " shape dimensions must be positive");
                }
                if (elementCount > std::numeric_limits<int64_t>::max() / dim)
                {
                    throw std::runtime_error(std::string(context) + " shape dimension product overflows int64");
                }
                elementCount *= dim;
            }
            if (width <= 0 || elementCount > width || width % elementCount != 0)
            {
                throw std::runtime_error(std::string(context) +
                                         " shape dimension product must divide the total width");
            }
        }

        std::string formatSrcLocForDebug(const std::optional<SrcLoc> &srcLoc)
        {
            if (!srcLoc)
            {
                return {};
            }
            if (!srcLoc->file.empty() && srcLoc->line != 0)
            {
                std::string out = srcLoc->file;
                out.push_back(':');
                out += std::to_string(srcLoc->line);
                if (srcLoc->column != 0)
                {
                    out.push_back(':');
                    out += std::to_string(srcLoc->column);
                }
                return out;
            }
            if (!srcLoc->origin.empty() || !srcLoc->pass.empty() || !srcLoc->note.empty())
            {
                std::string out;
                if (!srcLoc->origin.empty())
                {
                    out.append(srcLoc->origin);
                }
                if (!srcLoc->pass.empty())
                {
                    if (!out.empty())
                    {
                        out.push_back(':');
                    }
                    out.append(srcLoc->pass);
                }
                if (!srcLoc->note.empty())
                {
                    if (!out.empty())
                    {
                        out.push_back(':');
                    }
                    out.append(srcLoc->note);
                }
                return out;
            }
            return {};
        }

        ValueId findPortValue(std::span<const Port> ports, std::string_view name) noexcept
        {
            if (name.empty())
            {
                return ValueId::invalid();
            }
            for (const auto &port : ports)
            {
                if (port.name == name)
                {
                    return port.value;
                }
            }
            return ValueId::invalid();
        }

    } // namespace

    void Graph::cloneContentsTo(Graph& clone) const
    {
        const Graph& source = *this;
        const auto sourceValues = source.values();
        const auto sourceOperations = source.operations();
        clone.reserveSymbolCapacity(sourceValues.size() + sourceOperations.size());
        if (!sourceValues.empty()) clone.reserveValueCapacity(sourceValues.size());
        if (!sourceOperations.empty()) clone.reserveOperationCapacity(sourceOperations.size());
        for (const AttrKV& entry : source.metadata()) clone.setMetadata(entry.key, entry.value);
        if (source.semanticTree_) clone.semanticTree_ = source.semanticTree_;

        std::vector<SymbolId> symbolMap(source.symbols_.textById_.size());
        const auto mapSymbol = [&](SymbolId symbol) {
            if (!symbol.valid()) return SymbolId::invalid();
            SymbolId& result = symbolMap.at(symbol.value);
            if (result.valid()) return result;
            const std::string_view text = source.symbolText(symbol);
            if (text.empty()) throw std::runtime_error("Source symbol text is empty during clone");
            result = clone.lookupSymbol(text);
            if (!result.valid()) result = clone.internSymbol(text);
            if (!result.valid()) throw std::runtime_error("Failed to clone symbol: " + std::string(text));
            return result;
        };
        std::vector<GraphPayload::RecordId> srcLocMap(source.store_->GraphPayload::stats().srcLocs + 1);
        const auto copySrcLoc = [&](GraphPayload::RecordId id) {
            if (!id) return GraphPayload::RecordId{0};
            GraphPayload::RecordId& mapped = srcLocMap.at(id);
            if (!mapped) mapped = clone.store_->importSrcLoc(*source.store_, id);
            if (graphProfileEnabled()) {
                const auto& record = source.store_->srcLoc(id);
                const uint64_t bytes = source.store_->string(record.file).size() +
                    source.store_->string(record.origin).size() + source.store_->string(record.pass).size() +
                    source.store_->string(record.note).size();
                profileCounters.srcLocWrites.fetch_add(1, std::memory_order_relaxed);
                profileCounters.srcLocBytes.fetch_add(bytes, std::memory_order_relaxed);
            }
            return mapped;
        };
        std::vector<ValueId> valueMap(source.store_->values_.size() + 1);
        const auto mapValue = [&](ValueId id, const char* error) {
            if (id.graph != source.graphId_ || id.generation || id.index >= valueMap.size() ||
                !valueMap[id.index].valid()) throw std::runtime_error(error);
            return valueMap[id.index];
        };
        for (ValueId sourceId : sourceValues) {
            const auto value = source.store_->values_[sourceId.index - 1];
            const auto mappedSymbol = mapSymbol(value.symbol);
            const ValueId destination = mappedSymbol.valid() ?
                clone.createValue(mappedSymbol, value.width, value.isSigned, value.type, value.shape, value.domain) :
                clone.createTemporaryValue(value.width, value.isSigned, value.type, value.shape, value.domain);
            clone.store_->values_[destination.index - 1].srcLoc = copySrcLoc(value.srcLoc);
            clone.setValueSemanticIds(destination, source.valueSemanticIds(sourceId));
            valueMap[sourceId.index] = destination;
        }
        for (OperationId sourceId : sourceOperations) {
            const auto operation = source.store_->operations_[sourceId.index - 1];
            const auto mappedSymbol = mapSymbol(operation.symbol);
            const OperationId destination = mappedSymbol.valid() ?
                clone.createOperation(operation.kind, mappedSymbol) :
                clone.createTemporaryOperation(operation.kind);
            clone.reserveOpOperandCapacity(destination, operation.operands.size());
            clone.reserveOpResultCapacity(destination, operation.results.size());
            clone.reserveOpAttrCapacity(destination, operation.attrs.size());
            for (AttrRef attr : source.opAttrRange(sourceId))
                clone.setAttrView(destination, attr.key(), attr.value());
            for (ValueId operand : operation.operands)
                clone.addOperand(destination, mapValue(operand, "Missing operand during graph clone"));
            for (ValueId result : operation.results)
                clone.addResult(destination, mapValue(result, "Missing result during graph clone"));
            clone.store_->operations_[destination.index - 1].srcLoc = copySrcLoc(operation.srcLoc);
            clone.setOpSemanticIds(destination, source.opSemanticIds(sourceId));
        }
        for (const Port& port : source.inputPorts())
            clone.bindInputPort(port.name, mapValue(port.value, "Missing input port value during graph clone"));
        for (const Port& port : source.outputPorts())
            clone.bindOutputPort(port.name, mapValue(port.value, "Missing output port value during graph clone"));
        for (const InoutPort& port : source.inoutPorts())
            clone.bindInoutPort(port.name, mapValue(port.in, "Missing inout port value during graph clone"),
                mapValue(port.out, "Missing inout port value during graph clone"),
                mapValue(port.oe, "Missing inout port value during graph clone"));

        for (const auto srcSym : source.declaredSymbols())
        {
            if (!srcSym.valid())
            {
                continue;
            }
            std::string_view text = source.symbolText(srcSym);
            if (text.empty())
            {
                throw std::runtime_error("Declared symbol text is empty during clone");
            }
            SymbolId dstSym = clone.lookupSymbol(text);
            if (!dstSym.valid())
            {
                dstSym = clone.internSymbol(text);
            }
            if (!dstSym.valid())
            {
                throw std::runtime_error("Failed to clone declared symbol: " + std::string(text));
            }
            clone.addDeclaredSymbol(dstSym);
        }

        for (const auto &srcGroup : source.generateGroups())
        {
            const SymbolId dstScope = mapSymbol(srcGroup.scope);
            const SymbolId dstName = mapSymbol(srcGroup.name);
            const std::size_t dstGroup = clone.addGenerateGroup(dstScope, dstName);
            for (const auto srcSym : srcGroup.symbols)
            {
                if (!srcSym.valid())
                {
                    continue;
                }
                clone.addGenerateGroupSymbol(dstGroup, mapSymbol(srcSym));
            }
        }
    }

    SymbolTable::SymbolTable() : stringPool_(std::make_shared<detail::StringPool>())
    {
        textById_.emplace_back();
    }

    SymbolTable::SymbolTable(const SymbolTable& other)
        : SymbolTable(other, std::make_shared<detail::StringPool>())
    {
    }

    SymbolTable::SymbolTable(const SymbolTable& other, std::shared_ptr<detail::StringPool> strings)
        : stringPool_(std::move(strings))
    {
        textById_.reserve(other.textById_.size());
        symbolsByStringId_.reserve(stringPool_->size() + other.textById_.size());
        textById_.emplace_back();
        for (std::size_t index = 1; index < other.textById_.size(); ++index)
        {
            bindStringId(stringPool_->intern(other.textById_[index]));
        }
    }

    SymbolTable& SymbolTable::operator=(const SymbolTable& other)
    {
        if (this != &other)
        {
            SymbolTable copy(other);
            *this = std::move(copy);
        }
        return *this;
    }

    void SymbolTable::swapStringState(SymbolTable& other) noexcept
    {
        stringPool_.swap(other.stringPool_);
        symbolsByStringId_.swap(other.symbolsByStringId_);
        textById_.swap(other.textById_);
    }

    DesignSymbolTable::DesignSymbolTable()
    {
        symbolByGraph_.push_back(SymbolId::invalid());
    }

    GraphId DesignSymbolTable::allocateGraphId(SymbolId symbol)
    {
        if (!symbol.valid())
        {
            throw std::runtime_error("Graph symbol is invalid");
        }
        if (!valid(symbol))
        {
            throw std::runtime_error("Graph symbol is not in the symbol table");
        }
        if (graphIndexBySymbol_.contains(symbol.value))
        {
            throw std::runtime_error("Graph symbol already has an assigned GraphId");
        }
        GraphId graphId;
        graphId.index = nextGraphIndex_++;
        graphId.generation = 0;
        if (symbolByGraph_.size() <= graphId.index)
        {
            symbolByGraph_.resize(graphId.index + 1);
        }
        symbolByGraph_[graphId.index] = symbol;
        graphIndexBySymbol_[symbol.value] = graphId.index;
        return graphId;
    }

    void DesignSymbolTable::releaseGraphId(SymbolId symbol)
    {
        if (!symbol.valid())
        {
            return;
        }
        auto it = graphIndexBySymbol_.find(symbol.value);
        if (it == graphIndexBySymbol_.end())
        {
            return;
        }
        const uint32_t graphIndex = it->second;
        graphIndexBySymbol_.erase(it);
        if (graphIndex < symbolByGraph_.size())
        {
            symbolByGraph_[graphIndex] = SymbolId::invalid();
        }
    }

    GraphId DesignSymbolTable::lookupGraphId(SymbolId symbol) const noexcept
    {
        if (!symbol.valid())
        {
            return GraphId::invalid();
        }
        auto it = graphIndexBySymbol_.find(symbol.value);
        if (it == graphIndexBySymbol_.end())
        {
            return GraphId::invalid();
        }
        GraphId graphId;
        graphId.index = it->second;
        graphId.generation = 0;
        return graphId;
    }

    SymbolId DesignSymbolTable::symbolForGraph(GraphId graph) const noexcept
    {
        if (!graph.valid())
        {
            return SymbolId::invalid();
        }
        if (graph.index >= symbolByGraph_.size())
        {
            return SymbolId::invalid();
        }
        return symbolByGraph_[graph.index];
    }

    SymbolId SymbolTable::intern(std::string_view text)
    {
        const auto stringId = stringPool_->intern(text);
        if (stringId < symbolsByStringId_.size() && symbolsByStringId_[stringId].valid())
        {
            return symbolsByStringId_[stringId];
        }
        const SymbolId id = bindStringId(stringId);
        if (graphProfileEnabled()) profileCounters.symbolInserts.fetch_add(1, std::memory_order_relaxed);
        return id;
    }

    SymbolId SymbolTable::tryInternFresh(std::string_view text)
    {
        const auto stringId = stringPool_->intern(text);
        if (stringId < symbolsByStringId_.size() && symbolsByStringId_[stringId].valid())
        {
            return SymbolId::invalid();
        }
        const SymbolId id = bindStringId(stringId);
        if (graphProfileEnabled()) profileCounters.symbolInserts.fetch_add(1, std::memory_order_relaxed);
        return id;
    }

    SymbolId SymbolTable::bindStringId(uint32_t stringId)
    {
        if (symbolsByStringId_.size() <= stringId)
        {
            symbolsByStringId_.resize(static_cast<std::size_t>(stringId) + 1);
        }
        const SymbolId id{static_cast<uint32_t>(textById_.size())};
        textById_.push_back(stringPool_->text(stringId));
        symbolsByStringId_[stringId] = id;
        return id;
    }

    SymbolId SymbolTable::lookup(std::string_view text) const
    {
        const auto stringId = stringPool_->lookup(text);
        if ((!stringId && !text.empty()) || stringId >= symbolsByStringId_.size())
        {
            return SymbolId::invalid();
        }
        return symbolsByStringId_[stringId];
    }

    bool SymbolTable::contains(std::string_view text) const
    {
        return lookup(text).valid();
    }

    std::string_view SymbolTable::text(SymbolId id) const
    {
        if (!valid(id))
        {
            throw std::runtime_error("Invalid SymbolId");
        }
        return textById_[id.value];
    }

    bool SymbolTable::valid(SymbolId id) const noexcept
    {
        return id.value != 0 && id.value < textById_.size();
    }

    void SymbolTable::reserve(std::size_t count)
    {
        symbolsByStringId_.reserve(std::max(count + 1, stringPool_->size() + 1));
        textById_.reserve(count + 1);
        stringPool_->reserve(count);
    }

    void ValueId::assertGraph(GraphId expected) const
    {
        if (!expected.valid())
        {
            throw std::runtime_error("Expected GraphId is invalid");
        }
        if (!valid())
        {
            throw std::runtime_error("ValueId is invalid");
        }
        if (!graph.valid())
        {
            throw std::runtime_error("ValueId has invalid GraphId");
        }
        if (graph != expected)
        {
            if (std::getenv("WOLF_DEBUG_GRAPH_ID"))
            {
                std::fprintf(stderr,
                             "[graph-id] value index=%u gen=%u graph=%u/%u expected=%u/%u\n",
                             index, generation, graph.index, graph.generation,
                             expected.index, expected.generation);
            }
            throw std::runtime_error("ValueId used with mismatched GraphId");
        }
    }

    void OperationId::assertGraph(GraphId expected) const
    {
        if (!expected.valid())
        {
            throw std::runtime_error("Expected GraphId is invalid");
        }
        if (!valid())
        {
            throw std::runtime_error("OperationId is invalid");
        }
        if (!graph.valid())
        {
            throw std::runtime_error("OperationId has invalid GraphId");
        }
        if (graph != expected)
        {
            if (std::getenv("WOLF_DEBUG_GRAPH_ID"))
            {
                std::fprintf(stderr,
                             "[graph-id] operation index=%u gen=%u graph=%u/%u expected=%u/%u\n",
                             index, generation, graph.index, graph.generation,
                             expected.index, expected.generation);
            }
            throw std::runtime_error("OperationId used with mismatched GraphId");
        }
    }

    namespace {
        template <typename Id>
        void markLiveErased(std::vector<Id>& ids, Id erased)
        {
            const auto it = std::lower_bound(ids.begin(), ids.end(), erased.index,
                [](Id id, uint32_t index) { return id.index < index; });
            if (it == ids.end() || it->index != erased.index)
                throw std::logic_error("live graph ID list is inconsistent");
            // Keep the index for ordered lookup while deferring linear movement
            // until enumeration. This marker never escapes the store.
            it->graph = GraphId::invalid();
        }
    }

    std::span<const OperationId> GraphStore::operations() const noexcept
    {
        if (operationsHaveTombstones_) {
            std::erase_if(liveOperations_, [](OperationId id) { return !id.graph.valid(); });
            operationsHaveTombstones_ = false;
        }
        return liveOperations_;
    }

    std::span<const ValueId> GraphStore::values() const noexcept
    {
        if (valuesHaveTombstones_) {
            std::erase_if(liveValues_, [](ValueId id) { return !id.graph.valid(); });
            valuesHaveTombstones_ = false;
        }
        return liveValues_;
    }

    GraphStore::GraphStore(GraphId graphId)
        : graphId_(graphId), topology_(std::make_shared<TopologyPools>()),
          values_(topology_), operations_(topology_)
    {
        if (!graphId_.valid()) throw std::runtime_error("GraphStore requires a valid GraphId");
    }

    GraphStore::GraphStore(GraphSymbolTable& symbols, GraphId graphId)
        : GraphPayload(symbols.stringPool_), graphId_(graphId), topology_(std::make_shared<TopologyPools>()),
          values_(topology_), operations_(topology_)
    {
        if (!graphId_.valid()) throw std::runtime_error("GraphStore requires a valid GraphId");
        symbols_ = &symbols;
    }

    void GraphStore::reserveSemanticIds(std::size_t additional)
    {
        auto& payload = topology_->semantics;
        if (additional > std::numeric_limits<uint32_t>::max() - payload.size())
            throw std::length_error("SemanticId pool capacity");
        const std::size_t required = payload.size() + additional;
        if (payload.capacity() >= required) return;
        const std::size_t grown = payload.empty() ? 16 : payload.capacity() + payload.capacity() / 2;
        payload.reserve(std::max(required, std::min<std::size_t>(grown,
            std::numeric_limits<uint32_t>::max())));
    }

    Range GraphStore::storeSemanticIds(std::span<const SemanticId> ids)
    {
        if (ids.empty()) return {};
        auto& payload = topology_->semantics;
        if (ids.size() == 1) {
            const SemanticId id = ids.front();
            reserveSemanticIds(1);
            const Range result{payload.size(), 1};
            payload.push_back(id);
            return result;
        }
        if (ids.size() < 64) {
            std::vector<SemanticId> aliased;
            const std::less<const SemanticId*> before;
            if (!payload.empty() && !before(ids.data(), payload.data()) &&
                before(ids.data(), payload.data() + payload.size())) {
                aliased.assign(ids.begin(), ids.end());
                ids = aliased;
            }
            reserveSemanticIds(ids.size());
            const Range result{payload.size(), ids.size()};
            payload.insert(payload.end(), ids.begin(), ids.end());
            return result;
        }

        std::size_t hash = ids.size();
        for (const SemanticId id : ids)
            hash ^= std::hash<SemanticId>{}(id) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
        const auto found = topology_->semanticListIds.find(hash);
        uint32_t first = found == topology_->semanticListIds.end() ? 0 : found->second;
        for (uint32_t current = first; current; current = topology_->semanticHashNext[current]) {
            const Range candidate = topology_->semanticLists[current];
            const auto existing = std::span<const SemanticId>(payload.data() + candidate.offset, candidate.count);
            if (existing.size() == ids.size() && std::equal(existing.begin(), existing.end(), ids.begin()))
                return candidate;
        }

        constexpr std::size_t cacheLimit = 4096;
        if (topology_->semanticLists.size() > cacheLimit) {
            topology_->semanticListIds.clear();
            topology_->semanticLists.resize(1);
            topology_->semanticHashNext.resize(1);
            first = 0;
        }
        std::vector<SemanticId> aliased;
        const std::less<const SemanticId*> before;
        if (!payload.empty() && !before(ids.data(), payload.data()) &&
            before(ids.data(), payload.data() + payload.size())) {
            aliased.assign(ids.begin(), ids.end());
            ids = aliased;
        }
        reserveSemanticIds(ids.size());
        if (topology_->semanticLists.size() >= std::numeric_limits<uint32_t>::max())
            throw std::length_error("SemanticId list record capacity");
        const Range result{payload.size(), ids.size()};
        payload.insert(payload.end(), ids.begin(), ids.end());
        topology_->semanticLists.push_back(result);
        topology_->semanticHashNext.push_back(first);
        topology_->semanticListIds.insert_or_assign(hash,
            static_cast<uint32_t>(topology_->semanticLists.size() - 1));
        return result;
    }

    GraphStore::Stats GraphStore::stats() const noexcept
    {
        Stats result = GraphPayload::stats();
        result.shapes = topology_->shapes.ranges.size() - 1;
        result.shapeDims = topology_->shapes.dims.size();
        result.useSlots = topology_->users.size();
        result.operandIndexSlots = topology_->operandUseIndices.size();
        result.semanticIds = topology_->semantics.size();
        result.semanticLists = topology_->semanticLists.size() - 1;
        for (std::size_t i = 0; i < operations_.size(); ++i) {
            const auto op = operations_[i];
            if (!op.alive) continue;
            ++result.operations;
            result.operands += op.operands.size();
            result.results += op.results.size();
        }
        for (std::size_t i = 0; i < values_.size(); ++i) {
            if (!values_[i].alive) continue;
            ++result.values;
            result.uses += valueUsers_[i].size();
        }
        return result;
    }

    void GraphStore::compact()
    {
        std::vector<uint32_t> valueRemap(values_.size() + 1, 0);
        std::vector<uint32_t> opRemap(operations_.size() + 1, 0);
        std::size_t valueCount = 0, opCount = 0;
        for (std::size_t i = 0; i < values_.size(); ++i)
            if (values_[i].alive) valueRemap[i + 1] = static_cast<uint32_t>(++valueCount);
        for (std::size_t i = 0; i < operations_.size(); ++i)
            if (operations_[i].alive) opRemap[i + 1] = static_cast<uint32_t>(++opCount);

        const auto remapValue = [&](ValueId value) {
            value.assertGraph(graphId_);
            if (!value.index || value.index >= valueRemap.size())
                throw std::runtime_error("ValueId out of range during freeze");
            if (!valueRemap[value.index])
                throw std::runtime_error("ValueId refers to erased value during freeze");
            return ValueId{valueRemap[value.index], 0, graphId_};
        };
        const auto remapOp = [&](OperationId op) {
            if (!op.valid()) return OperationId::invalid();
            op.assertGraph(graphId_);
            if (op.index >= opRemap.size()) throw std::runtime_error("OperationId out of range during freeze");
            return opRemap[op.index] ? OperationId{opRemap[op.index], 0, graphId_} : OperationId::invalid();
        };

        // Repack into the same column layout. No immutable representation or
        // thaw is created: the prepared columns replace the old columns only
        // after validation and allocation have succeeded.
        GraphStore next(graphId_);
        next.symbols_ = symbols_;
        std::vector<RecordId> srcLocRemap(GraphPayload::stats().srcLocs + 1, 0);
        const auto remapSrcLoc = [&](RecordId id) {
            if (!id) return RecordId{0};
            RecordId& mapped = srcLocRemap.at(id);
            if (!mapped) mapped = next.importSrcLoc(*this, id);
            return mapped;
        };
        const auto copySemantics = [&](Range range) {
            if (!range.count) return Range{};
            const auto& sourceIds = topology_->semantics;
            if (range.offset > sourceIds.size() || range.count > sourceIds.size() - range.offset)
                throw std::out_of_range("semantic list is out of range during compact");
            return next.storeSemanticIds(std::span<const SemanticId>(
                sourceIds.data() + range.offset, range.count));
        };
        next.reserveValues(valueCount);
        next.reserveOperations(opCount);
        next.reserveSymbols(valueCount + opCount);
        for (std::size_t i = 0; i < values_.size(); ++i) {
            const auto source = values_[i];
            if (!source.alive) continue;
            auto value = next.values_.emplace_back();
            value.symbol = source.symbol;
            value.width = source.width;
            value.isSigned = source.isSigned;
            value.type = source.type;
            value.domain = source.domain;
            value.shape.set(source.shape);
            value.isInput = source.isInput;
            value.isOutput = source.isOutput;
            value.isInout = source.isInout;
            value.definingOp = remapOp(source.definingOp);
            value.srcLoc = remapSrcLoc(source.srcLoc);
            value.semantics = copySemantics(source.semantics);
            next.valueUsers_.emplace_back(next.topology_->users);
            next.valueUsers_.back().reserve(valueUsers_[i].size());
            next.liveValues_.push_back(ValueId{valueRemap[i + 1], 0, graphId_});
            if (value.symbol.valid())
                next.bindSymbol(value.symbol, SymbolKind::kValue, valueRemap[i + 1], "Value");
        }
        for (std::size_t i = 0; i < operations_.size(); ++i) {
            const auto source = operations_[i];
            if (!source.alive) continue;
            auto operation = next.operations_.emplace_back();
            operation.kind = source.kind;
            operation.symbol = source.symbol;
            operation.srcLoc = remapSrcLoc(source.srcLoc);
            operation.semantics = copySemantics(source.semantics);
            operation.operands.reserve(source.operands.size());
            operation.operandUseIndices.reserve(source.operands.size());
            operation.results.reserve(source.results.size());
            operation.attrs.reserve(source.attrs.size());
            for (ValueId operand : source.operands) operation.operands.push_back(remapValue(operand));
            for (ValueId result : source.results) operation.results.push_back(remapValue(result));
            for (const RecordId attr : source.attrs)
                operation.attrs.push_back(next.internAttribute(string(attribute(attr).key), AttrValueView(this, attr)));
            const OperationId id{opRemap[i + 1], 0, graphId_};
            next.liveOperations_.push_back(id);
            if (operation.symbol.valid())
                next.bindSymbol(operation.symbol, SymbolKind::kOperation, id.index, "Operation");
            next.addOpUses(id, operation.operands);
        }
        next.inputPorts_.reserve(inputPorts_.size());
        next.outputPorts_.reserve(outputPorts_.size());
        next.inoutPorts_.reserve(inoutPorts_.size());
        for (const Port& port : inputPorts_) next.inputPorts_.push_back({port.name, remapValue(port.value)});
        for (const Port& port : outputPorts_) next.outputPorts_.push_back({port.name, remapValue(port.value)});
        for (const InoutPort& port : inoutPorts_)
            next.inoutPorts_.push_back({port.name, remapValue(port.in), remapValue(port.out), remapValue(port.oe)});
        const auto portLess = [](const auto& lhs, const auto& rhs) { return lhs.name < rhs.name; };
        std::sort(next.inputPorts_.begin(), next.inputPorts_.end(), portLess);
        std::sort(next.outputPorts_.begin(), next.outputPorts_.end(), portLess);
        std::sort(next.inoutPorts_.begin(), next.inoutPorts_.end(), portLess);
        next.topology_->operands.shrink_to_fit();
        next.topology_->results.shrink_to_fit();
        next.topology_->operandUseIndices.shrink_to_fit();
        next.topology_->users.shrink_to_fit();
        next.topology_->attrs.shrink_to_fit();
        next.topology_->semantics.shrink_to_fit();
        next.topology_->shapes.dims.shrink_to_fit();
        next.topology_->shapes.ranges.shrink_to_fit();
        next.topology_->shapes.hashNext.shrink_to_fit();
        std::optional<SymbolTable> nextSymbols;
        if (symbols_) nextSymbols = SymbolTable(*symbols_, next.stringPool_);
        next.GraphPayload::compact();
        static_assert(std::is_nothrow_move_assignable_v<GraphStore>);
        if (nextSymbols) symbols_->swapStringState(*nextSymbols);
        *this = std::move(next);
        if (graphProfileEnabled()) profileCounters.freezeCount.fetch_add(1, std::memory_order_relaxed);
    }

    void GraphStore::reserveValues(std::size_t count)
    {
        values_.reserve(count);
        valueUsers_.reserve(count);
        liveValues_.reserve(count);
    }

    void GraphStore::reserveOperations(std::size_t count)
    {
        operations_.reserve(count);
        liveOperations_.reserve(count);
    }

    void GraphStore::reserveSymbols(std::size_t count)
    {
        symbolIndex_.reserve(count);
    }

    void GraphStore::reserveOpOperands(OperationId op, std::size_t count)
    {
        operations_[opIndex(op)].operands.reserve(count);
    }

    void GraphStore::reserveOpResults(OperationId op, std::size_t count)
    {
        operations_[opIndex(op)].results.reserve(count);
    }

    void GraphStore::reserveOpAttrs(OperationId op, std::size_t count)
    {
        operations_[opIndex(op)].attrs.reserve(count);
    }

    ValueId GraphStore::addValue(SymbolId sym, int32_t width, bool isSigned, ValueType type,
                                   std::span<const int64_t> shape, LogicDomain domain,
                                   bool allowInvalidSymbol)
    {
        // Normalize scalar fields without copying the borrowed shape twice.
        const LogicalType logical = LogicalType{type, width, isSigned, domain, {}}.normalized();
        validateValueShape(width, type, shape, "LogicalType");
        width = logical.width;
        isSigned = logical.isSigned;
        domain = logical.domain;
        if (sym.valid() || !allowInvalidSymbol) validateSymbol(sym, "Value");

        ValueId id;
        id.index = static_cast<uint32_t>(values_.size() + 1);
        id.generation = 0;
        id.graph = graphId_;
        if (sym.valid()) bindSymbol(sym, SymbolKind::kValue, id.index, "Value");
        auto data = values_.emplace_back();
        data.symbol = sym;
        data.width = width;
        data.isSigned = isSigned;
        data.type = type;
        data.domain = domain;
        data.shape.set(shape);
        valueUsers_.emplace_back(topology_->users);
        liveValues_.push_back(id);
        if (graphProfileEnabled()) profileCounters.valueAllocations.fetch_add(1, std::memory_order_relaxed);
        return id;
    }

    OperationId GraphStore::addOp(OperationKind kind, SymbolId sym, bool allowInvalidSymbol)
    {
        if (sym.valid() || !allowInvalidSymbol) validateSymbol(sym, "Operation");

        OperationId id;
        id.index = static_cast<uint32_t>(operations_.size() + 1);
        id.generation = 0;
        id.graph = graphId_;
        if (sym.valid()) bindSymbol(sym, SymbolKind::kOperation, id.index, "Operation");
        auto data = operations_.emplace_back();
        data.kind = kind;
        data.symbol = sym;
        liveOperations_.push_back(id);
        if (graphProfileEnabled()) profileCounters.operationAllocations.fetch_add(1, std::memory_order_relaxed);
        return id;
    }

    void GraphStore::addOperand(OperationId op, ValueId value)
    {
        const std::size_t opIdx = opIndex(op);
        const std::size_t valIdx = valueIndex(value);
        if (!operations_[opIdx].alive)
        {
            throw std::runtime_error("OperationId refers to erased operation");
        }
        if (!values_[valIdx].alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }
        auto &operands = operations_[opIdx].operands;
        operands.push_back(value);
        addValueUser(value, op, operands.size() - 1);
    }

    void GraphStore::insertOperand(OperationId op, std::size_t index, ValueId value)
    {
        const std::size_t opIdx = opIndex(op);
        const std::size_t valIdx = valueIndex(value);
        if (!operations_[opIdx].alive)
        {
            throw std::runtime_error("OperationId refers to erased operation");
        }
        if (!values_[valIdx].alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }
        auto &operands = operations_[opIdx].operands;
        if (index > operands.size())
        {
            throw std::runtime_error("Operand index out of range");
        }
        const std::vector<ValueId> oldOperands = operands;
        operands.insert(operands.begin() + static_cast<std::ptrdiff_t>(index), value);
        removeOpUses(op, oldOperands);
        addOpUses(op, operands);
    }

    void GraphStore::addResult(OperationId op, ValueId value)
    {
        const std::size_t opIdx = opIndex(op);
        const std::size_t valIdx = valueIndex(value);
        if (!operations_[opIdx].alive)
        {
            throw std::runtime_error("OperationId refers to erased operation");
        }
        if (!values_[valIdx].alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }
        auto data = values_[valIdx];
        if (data.definingOp.valid())
        {
            auto describeValue = [&](ValueId id) -> std::string {
                if (!id.valid())
                {
                    return "value=<invalid>";
                }
                std::string out = "value_" + std::to_string(id.index);
                if (id.graph != graphId_)
                {
                    out += " graph_mismatch";
                    return out;
                }
                const std::size_t idx = static_cast<std::size_t>(id.index);
                if (idx == 0 || idx > values_.size())
                {
                    out += " out_of_range";
                    return out;
                }
                const auto valueData = values_[idx - 1];
                if (symbols_ && valueData.symbol.valid())
                {
                    out += " (" + std::string(symbols_->text(valueData.symbol)) + ")";
                }
                out += " w=" + std::to_string(valueData.width);
                out += valueData.isSigned ? " signed" : " unsigned";
                if (valueData.isInput)
                {
                    out += " input";
                }
                if (valueData.isOutput)
                {
                    out += " output";
                }
                const std::string loc = formatSrcLocForDebug(srcLocValue(valueData.srcLoc));
                if (!loc.empty())
                {
                    out += " @";
                    out += loc;
                }
                return out;
            };
            auto describeOp = [&](OperationId id) -> std::string {
                if (!id.valid())
                {
                    return "op=<invalid>";
                }
                std::string out = "op_" + std::to_string(id.index);
                if (id.graph != graphId_)
                {
                    out += " graph_mismatch";
                    return out;
                }
                const std::size_t idx = static_cast<std::size_t>(id.index);
                if (idx == 0 || idx > operations_.size())
                {
                    out += " out_of_range";
                    return out;
                }
                const auto opData = operations_[idx - 1];
                out += " kind=";
                out += std::string(toString(opData.kind));
                if (symbols_ && opData.symbol.valid())
                {
                    out += " (" + std::string(symbols_->text(opData.symbol)) + ")";
                }
                const std::string loc = formatSrcLocForDebug(srcLocValue(opData.srcLoc));
                if (!loc.empty())
                {
                    out += " @";
                    out += loc;
                }
                return out;
            };

            std::string message = "Value already has a defining operation; ";
            message += describeValue(value);
            message += "; new_def=";
            message += describeOp(op);
            message += "; existing_def=";
            message += describeOp(data.definingOp);
            throw std::runtime_error(message);
        }
        data.definingOp = op;
        if (!data.srcLoc && operations_[opIdx].srcLoc)
        {
            data.srcLoc = operations_[opIdx].srcLoc;
        }
        operations_[opIdx].results.push_back(value);
    }

    void GraphStore::insertResult(OperationId op, std::size_t index, ValueId value)
    {
        const std::size_t opIdx = opIndex(op);
        const std::size_t valIdx = valueIndex(value);
        if (!operations_[opIdx].alive)
        {
            throw std::runtime_error("OperationId refers to erased operation");
        }
        if (!values_[valIdx].alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }
        auto &results = operations_[opIdx].results;
        if (index > results.size())
        {
            throw std::runtime_error("Result index out of range");
        }
        auto data = values_[valIdx];
        if (data.definingOp.valid())
        {
            auto describeValue = [&](ValueId id) -> std::string {
                if (!id.valid())
                {
                    return "value=<invalid>";
                }
                std::string out = "value_" + std::to_string(id.index);
                if (id.graph != graphId_)
                {
                    out += " graph_mismatch";
                    return out;
                }
                const std::size_t idx = static_cast<std::size_t>(id.index);
                if (idx == 0 || idx > values_.size())
                {
                    out += " out_of_range";
                    return out;
                }
                const auto valueData = values_[idx - 1];
                if (symbols_ && valueData.symbol.valid())
                {
                    out += " (" + std::string(symbols_->text(valueData.symbol)) + ")";
                }
                out += " w=" + std::to_string(valueData.width);
                out += valueData.isSigned ? " signed" : " unsigned";
                if (valueData.isInput)
                {
                    out += " input";
                }
                if (valueData.isOutput)
                {
                    out += " output";
                }
                const std::string loc = formatSrcLocForDebug(srcLocValue(valueData.srcLoc));
                if (!loc.empty())
                {
                    out += " @";
                    out += loc;
                }
                return out;
            };
            auto describeOp = [&](OperationId id) -> std::string {
                if (!id.valid())
                {
                    return "op=<invalid>";
                }
                std::string out = "op_" + std::to_string(id.index);
                if (id.graph != graphId_)
                {
                    out += " graph_mismatch";
                    return out;
                }
                const std::size_t idx = static_cast<std::size_t>(id.index);
                if (idx == 0 || idx > operations_.size())
                {
                    out += " out_of_range";
                    return out;
                }
                const auto opData = operations_[idx - 1];
                out += " kind=";
                out += std::string(toString(opData.kind));
                if (symbols_ && opData.symbol.valid())
                {
                    out += " (" + std::string(symbols_->text(opData.symbol)) + ")";
                }
                const std::string loc = formatSrcLocForDebug(srcLocValue(opData.srcLoc));
                if (!loc.empty())
                {
                    out += " @";
                    out += loc;
                }
                return out;
            };

            std::string message = "Value already has a defining operation; ";
            message += describeValue(value);
            message += "; new_def=";
            message += describeOp(op);
            message += "; existing_def=";
            message += describeOp(data.definingOp);
            throw std::runtime_error(message);
        }
        data.definingOp = op;
        if (!data.srcLoc && operations_[opIdx].srcLoc)
        {
            data.srcLoc = operations_[opIdx].srcLoc;
        }
        results.insert(results.begin() + static_cast<std::ptrdiff_t>(index), value);
    }

    void GraphStore::replaceOperand(OperationId op, std::size_t index, ValueId value)
    {
        const std::size_t opIdx = opIndex(op);
        const std::size_t valIdx = valueIndex(value);
        if (!operations_[opIdx].alive)
        {
            throw std::runtime_error("OperationId refers to erased operation");
        }
        if (!values_[valIdx].alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }
        auto &operands = operations_[opIdx].operands;
        if (index >= operands.size())
        {
            throw std::runtime_error("Operand index out of range");
        }
        const ValueId current = operands[index];
        if (current == value)
        {
            return;
        }
        operands[index] = value;
        removeValueUser(current, op, index);
        addValueUser(value, op, index);
    }

    void GraphStore::replaceResult(OperationId op, std::size_t index, ValueId value)
    {
        const std::size_t opIdx = opIndex(op);
        if (!operations_[opIdx].alive)
        {
            throw std::runtime_error("OperationId refers to erased operation");
        }
        const std::size_t valIdx = valueIndex(value);
        if (!values_[valIdx].alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }

        auto &results = operations_[opIdx].results;
        if (index >= results.size())
        {
            throw std::runtime_error("Result index out of range");
        }

        const ValueId current = results[index];
        if (current == value)
        {
            return;
        }
        if (values_[valIdx].definingOp.valid())
        {
            auto describeValue = [&](ValueId id) -> std::string {
                if (!id.valid())
                {
                    return "value=<invalid>";
                }
                std::string out = "value_" + std::to_string(id.index);
                if (id.graph != graphId_)
                {
                    out += " graph_mismatch";
                    return out;
                }
                const std::size_t idx = static_cast<std::size_t>(id.index);
                if (idx == 0 || idx > values_.size())
                {
                    out += " out_of_range";
                    return out;
                }
                const auto valueData = values_[idx - 1];
                if (symbols_ && valueData.symbol.valid())
                {
                    out += " (" + std::string(symbols_->text(valueData.symbol)) + ")";
                }
                out += " w=" + std::to_string(valueData.width);
                out += valueData.isSigned ? " signed" : " unsigned";
                if (valueData.isInput)
                {
                    out += " input";
                }
                if (valueData.isOutput)
                {
                    out += " output";
                }
                const std::string loc = formatSrcLocForDebug(srcLocValue(valueData.srcLoc));
                if (!loc.empty())
                {
                    out += " @";
                    out += loc;
                }
                return out;
            };
            auto describeOp = [&](OperationId id) -> std::string {
                if (!id.valid())
                {
                    return "op=<invalid>";
                }
                std::string out = "op_" + std::to_string(id.index);
                if (id.graph != graphId_)
                {
                    out += " graph_mismatch";
                    return out;
                }
                const std::size_t idx = static_cast<std::size_t>(id.index);
                if (idx == 0 || idx > operations_.size())
                {
                    out += " out_of_range";
                    return out;
                }
                const auto opData = operations_[idx - 1];
                out += " kind=";
                out += std::string(toString(opData.kind));
                if (symbols_ && opData.symbol.valid())
                {
                    out += " (" + std::string(symbols_->text(opData.symbol)) + ")";
                }
                const std::string loc = formatSrcLocForDebug(srcLocValue(opData.srcLoc));
                if (!loc.empty())
                {
                    out += " @";
                    out += loc;
                }
                return out;
            };

            std::string message = "Value already has a defining operation while replacing result ";
            message += std::to_string(index);
            message += "; ";
            message += describeValue(value);
            message += "; new_def=";
            message += describeOp(op);
            message += "; existing_def=";
            message += describeOp(values_[valIdx].definingOp);
            throw std::runtime_error(message);
        }

        const std::size_t currentIdx = valueIndex(current);
        if (values_[currentIdx].definingOp == op)
        {
            values_[currentIdx].definingOp = OperationId::invalid();
        }
        values_[valIdx].definingOp = op;
        results[index] = value;
    }

    void GraphStore::replaceAllUses(ValueId from, ValueId to)
    {
        replaceAllUsesInternal(from, to, std::nullopt);
    }

    bool GraphStore::eraseOperand(OperationId op, std::size_t index)
    {
        const std::size_t opIdx = opIndex(op);
        if (!operations_[opIdx].alive)
        {
            return false;
        }
        auto &operands = operations_[opIdx].operands;
        if (index >= operands.size())
        {
            return false;
        }
        const std::vector<ValueId> oldOperands = operands;
        operands.erase(operands.begin() + static_cast<std::ptrdiff_t>(index));
        removeOpUses(op, oldOperands);
        addOpUses(op, operands);
        return true;
    }

    bool GraphStore::eraseResult(OperationId op, std::size_t index)
    {
        const std::size_t opIdx = opIndex(op);
        if (!operations_[opIdx].alive)
        {
            return false;
        }
        auto &results = operations_[opIdx].results;
        if (index >= results.size())
        {
            return false;
        }
        const ValueId value = results[index];
        const std::size_t valIdx = valueIndex(value);
        if (!values_[valIdx].alive)
        {
            return false;
        }
        if (countValueUses(value, std::nullopt) != 0)
        {
            return false;
        }
        if (values_[valIdx].isInput || values_[valIdx].isOutput)
        {
            return false;
        }
        for (const auto &port : inoutPorts_)
        {
            if (port.in == value || port.out == value || port.oe == value)
            {
                return false;
            }
        }
        values_[valIdx].definingOp = OperationId::invalid();
        results.erase(results.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }

    bool GraphStore::eraseOp(OperationId op)
    {
        const std::size_t opIdx = opIndex(op);
        if (!operations_[opIdx].alive)
        {
            return false;
        }
        const auto &results = operations_[opIdx].results;
        for (const auto &result : results)
        {
            if (countValueUses(result, op) != 0)
            {
                return false;
            }
        }
        for (const auto &result : results)
        {
            const std::size_t valIdx = valueIndex(result);
            if (values_[valIdx].definingOp == op)
            {
                values_[valIdx].definingOp = OperationId::invalid();
            }
        }
        removeOpUses(op, operations_[opIdx].operands);
        const SymbolId symbol = operations_[opIdx].symbol;
        if (symbol.valid())
        {
            unbindSymbol(symbol, SymbolKind::kOperation, static_cast<uint32_t>(opIdx + 1));
        }
        operations_[opIdx].alive = false;
        markLiveErased(liveOperations_, op);
        operationsHaveTombstones_ = true;
        return true;
    }

    bool GraphStore::eraseOpUnchecked(OperationId op)
    {
        const std::size_t opIdx = opIndex(op);
        if (!operations_[opIdx].alive)
        {
            return false;
        }
        const auto &results = operations_[opIdx].results;
        for (const auto &result : results)
        {
            const std::size_t valIdx = valueIndex(result);
            if (values_[valIdx].definingOp == op)
            {
                values_[valIdx].definingOp = OperationId::invalid();
            }
        }
        removeOpUses(op, operations_[opIdx].operands);
        const SymbolId symbol = operations_[opIdx].symbol;
        if (symbol.valid())
        {
            unbindSymbol(symbol, SymbolKind::kOperation, static_cast<uint32_t>(opIdx + 1));
        }
        operations_[opIdx].alive = false;
        markLiveErased(liveOperations_, op);
        operationsHaveTombstones_ = true;
        return true;
    }

    bool GraphStore::eraseOp(OperationId op, std::span<const ValueId> replacementResults)
    {
        const std::size_t opIdx = opIndex(op);
        if (!operations_[opIdx].alive)
        {
            return false;
        }
        const auto &results = operations_[opIdx].results;
        if (replacementResults.size() != results.size())
        {
            return false;
        }
        for (const auto &value : replacementResults)
        {
            const std::size_t valIdx = valueIndex(value);
            if (!values_[valIdx].alive)
            {
                throw std::runtime_error("Replacement value is erased");
            }
        }
        for (std::size_t i = 0; i < results.size(); ++i)
        {
            const ValueId from = results[i];
            const ValueId to = replacementResults[i];
            if (from == to)
            {
                continue;
            }
            replaceAllUsesInternal(from, to, op);
        }
        for (const auto &result : results)
        {
            const std::size_t valIdx = valueIndex(result);
            if (values_[valIdx].definingOp == op)
            {
                values_[valIdx].definingOp = OperationId::invalid();
            }
        }
        removeOpUses(op, operations_[opIdx].operands);
        const SymbolId symbol = operations_[opIdx].symbol;
        if (symbol.valid())
        {
            unbindSymbol(symbol, SymbolKind::kOperation, static_cast<uint32_t>(opIdx + 1));
        }
        operations_[opIdx].alive = false;
        markLiveErased(liveOperations_, op);
        operationsHaveTombstones_ = true;
        return true;
    }

    bool GraphStore::eraseValue(ValueId value)
    {
        const std::size_t valIdx = valueIndex(value);
        if (!values_[valIdx].alive)
        {
            return false;
        }
        if (countValueUses(value, std::nullopt) != 0)
        {
            return false;
        }
        if (values_[valIdx].isInput || values_[valIdx].isOutput)
        {
            return false;
        }
        for (const auto &port : inoutPorts_)
        {
            if (port.in == value || port.out == value || port.oe == value)
            {
                return false;
            }
        }
        if (values_[valIdx].definingOp.valid())
        {
            return false;
        }
        const SymbolId symbol = values_[valIdx].symbol;
        if (symbol.valid())
        {
            unbindSymbol(symbol, SymbolKind::kValue, static_cast<uint32_t>(valIdx + 1));
        }
        valueUsers_[valIdx].clear();
        values_[valIdx].alive = false;
        markLiveErased(liveValues_, value);
        valuesHaveTombstones_ = true;
        return true;
    }

    bool GraphStore::eraseValueUnchecked(ValueId value)
    {
        const std::size_t valIdx = valueIndex(value);
        if (!values_[valIdx].alive)
        {
            return false;
        }
        if (values_[valIdx].isInput || values_[valIdx].isOutput)
        {
            return false;
        }
        for (const auto &port : inoutPorts_)
        {
            if (port.in == value || port.out == value || port.oe == value)
            {
                return false;
            }
        }
        if (values_[valIdx].definingOp.valid())
        {
            return false;
        }
        const SymbolId symbol = values_[valIdx].symbol;
        if (symbol.valid())
        {
            unbindSymbol(symbol, SymbolKind::kValue, static_cast<uint32_t>(valIdx + 1));
        }
        values_[valIdx].alive = false;
        markLiveErased(liveValues_, value);
        valuesHaveTombstones_ = true;
        return true;
    }

    void GraphStore::bindPort(std::vector<Port> &ports, std::string_view name, ValueId value, std::string_view context)
    {
        if (name.empty())
        {
            throw std::runtime_error(std::string(context) + " name is empty");
        }
        const std::size_t valIdx = valueIndex(value);
        if (!values_[valIdx].alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }
        bool updated = false;
        for (auto &port : ports)
        {
            if (port.name == name)
            {
                port.value = value;
                updated = true;
                break;
            }
        }
        if (!updated)
        {
            ports.push_back(Port{std::string(name), value});
        }
        recomputePortFlags();
    }

    void GraphStore::bindInputPorts(std::span<const Port> ports)
    {
        for (const auto &port : ports)
        {
            if (port.name.empty())
            {
                throw std::runtime_error("Input port name is empty");
            }
            const std::size_t valIdx = valueIndex(port.value);
            if (!values_[valIdx].alive)
            {
                throw std::runtime_error("ValueId refers to erased value");
            }
            bool updated = false;
            for (auto &entry : inputPorts_)
            {
                if (entry.name == port.name)
                {
                    entry.value = port.value;
                    updated = true;
                    break;
                }
            }
            if (!updated)
            {
                inputPorts_.push_back(Port{port.name, port.value});
            }
        }
        recomputePortFlags();
    }

    void GraphStore::bindOutputPorts(std::span<const Port> ports)
    {
        for (const auto &port : ports)
        {
            if (port.name.empty())
            {
                throw std::runtime_error("Output port name is empty");
            }
            const std::size_t valIdx = valueIndex(port.value);
            if (!values_[valIdx].alive)
            {
                throw std::runtime_error("ValueId refers to erased value");
            }
            bool updated = false;
            for (auto &entry : outputPorts_)
            {
                if (entry.name == port.name)
                {
                    entry.value = port.value;
                    updated = true;
                    break;
                }
            }
            if (!updated)
            {
                outputPorts_.push_back(Port{port.name, port.value});
            }
        }
        recomputePortFlags();
    }

    void GraphStore::bindInoutPorts(std::span<const InoutPort> ports)
    {
        for (const auto &port : ports)
        {
            if (port.name.empty())
            {
                throw std::runtime_error("Inout port name is empty");
            }
            const std::size_t inIdx = valueIndex(port.in);
            const std::size_t outIdx = valueIndex(port.out);
            const std::size_t oeIdx = valueIndex(port.oe);
            if (!values_[inIdx].alive || !values_[outIdx].alive || !values_[oeIdx].alive)
            {
                throw std::runtime_error("ValueId refers to erased value");
            }
            bool updated = false;
            for (auto &entry : inoutPorts_)
            {
                if (entry.name == port.name)
                {
                    entry.in = port.in;
                    entry.out = port.out;
                    entry.oe = port.oe;
                    updated = true;
                    break;
                }
            }
            if (!updated)
            {
                inoutPorts_.push_back(InoutPort{port.name, port.in, port.out, port.oe});
            }
        }
        recomputePortFlags();
    }

    void GraphStore::bindInoutPort(std::vector<InoutPort> &ports, std::string_view name, ValueId in,
                                     ValueId out, ValueId oe, std::string_view context)
    {
        if (name.empty())
        {
            throw std::runtime_error(std::string(context) + " name is empty");
        }
        const std::size_t inIdx = valueIndex(in);
        const std::size_t outIdx = valueIndex(out);
        const std::size_t oeIdx = valueIndex(oe);
        if (!values_[inIdx].alive || !values_[outIdx].alive || !values_[oeIdx].alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }
        bool updated = false;
        for (auto &port : ports)
        {
            if (port.name == name)
            {
                port.in = in;
                port.out = out;
                port.oe = oe;
                updated = true;
                break;
            }
        }
        if (!updated)
        {
            ports.push_back(InoutPort{std::string(name), in, out, oe});
        }
        recomputePortFlags();
    }

    bool GraphStore::removePort(std::vector<Port> &ports, std::string_view name, std::string_view context)
    {
        if (name.empty())
        {
            throw std::runtime_error(std::string(context) + " name is empty");
        }
        for (auto it = ports.begin(); it != ports.end(); ++it)
        {
            if (it->name == name)
            {
                ports.erase(it);
                recomputePortFlags();
                return true;
            }
        }
        return false;
    }

    bool GraphStore::removeInoutPort(std::vector<InoutPort> &ports, std::string_view name, std::string_view context)
    {
        if (name.empty())
        {
            throw std::runtime_error(std::string(context) + " name is empty");
        }
        for (auto it = ports.begin(); it != ports.end(); ++it)
        {
            if (it->name == name)
            {
                ports.erase(it);
                recomputePortFlags();
                return true;
            }
        }
        return false;
    }

    void GraphStore::bindInputPort(std::string_view name, ValueId value)
    {
        bindPort(inputPorts_, name, value, "Input port");
    }

    void GraphStore::bindOutputPort(std::string_view name, ValueId value)
    {
        bindPort(outputPorts_, name, value, "Output port");
    }

    void GraphStore::bindInoutPort(std::string_view name, ValueId in, ValueId out, ValueId oe)
    {
        bindInoutPort(inoutPorts_, name, in, out, oe, "Inout port");
    }

    bool GraphStore::removeInputPort(std::string_view name)
    {
        return removePort(inputPorts_, name, "Input port");
    }

    bool GraphStore::removeOutputPort(std::string_view name)
    {
        return removePort(outputPorts_, name, "Output port");
    }

    bool GraphStore::removeInoutPort(std::string_view name)
    {
        return removeInoutPort(inoutPorts_, name, "Inout port");
    }

    void GraphStore::setAttr(OperationId op, std::string_view key, AttributeValue value)
    {
        setAttrView(op, key, AttrValueView(&value));
    }

    void GraphStore::setAttrView(OperationId op, std::string_view key, AttrValueView value)
    {
        if (!value.valid()) throw std::invalid_argument("invalid attribute value view");
        if ((value.kind() == AttrValueKind::Double && !std::isfinite(value.asDouble())) ||
            (value.kind() == AttrValueKind::DoubleList &&
             std::any_of(value.asDoubleList().begin(), value.asDoubleList().end(),
                         [](double item) { return !std::isfinite(item); })))
            throw std::runtime_error("Attribute value must be JSON-serializable");
        const std::size_t opIdx = opIndex(op);
        if (!operations_[opIdx].alive) throw std::runtime_error("OperationId refers to erased operation");
        if (graphProfileEnabled()) {
            uint64_t bytes = 0;
            switch (value.kind()) {
            case AttrValueKind::Bool: bytes = sizeof(bool); break;
            case AttrValueKind::Int: bytes = sizeof(int64_t); break;
            case AttrValueKind::Double: bytes = sizeof(double); break;
            case AttrValueKind::String: bytes = value.asString().size(); break;
            case AttrValueKind::BoolList: bytes = value.asBoolList().size(); break;
            case AttrValueKind::IntList: bytes = value.asIntList().size_bytes(); break;
            case AttrValueKind::DoubleList: bytes = value.asDoubleList().size_bytes(); break;
            case AttrValueKind::StringList: for (const auto item : value.asStringList()) bytes += item.size(); break;
            }
            profileCounters.attributeWrites.fetch_add(1, std::memory_order_relaxed);
            profileCounters.attributeBytes.fetch_add(key.size() + bytes, std::memory_order_relaxed);
        }
        const RecordId record = internAttribute(key, value);
        setAttrRecord(opIdx, key, record);
    }

    void GraphStore::setAttrRecord(std::size_t opIdx, std::string_view key, RecordId record)
    {
        auto& attrs = operations_[opIdx].attrs;
        const auto it = std::lower_bound(attrs.begin(), attrs.end(), key,
            [&](RecordId attr, std::string_view name) { return string(attribute(attr).key) < name; });
        const auto index = attrs.empty() ? 0 : static_cast<std::size_t>(it - attrs.begin());
        if (index < attrs.size() && string(attribute(attrs[index]).key) == key)
            replaceAttribute(attrs[index], record);
        else
            attrs.insert(attrs.empty() ? attrs.begin() : attrs.begin() + index, record);
    }

    void GraphStore::setCanonicalNodeAttrs(OperationId op, std::string_view canonicalKey,
                                             bool root, int64_t rootResult)
    {
        constexpr std::string_view kMembers = "firrtl.node.members";
        constexpr std::string_view kOrigins = "firrtl.node.origins";
        constexpr std::string_view kRoots = "firrtl.node.roots";
        constexpr std::string_view kRootResults = "firrtl.node.rootResults";
        if (canonicalKey.empty())
        {
            throw std::invalid_argument("FIRRTL node identity requires a canonical key");
        }
        const std::size_t opIdx = opIndex(op);
        auto operation = operations_[opIdx];
        if (!operation.alive)
        {
            throw std::runtime_error("OperationId refers to erased operation");
        }
        if (root && (rootResult < 0 || static_cast<uint64_t>(rootResult) >= operation.results.size()))
        {
            throw std::invalid_argument("node root result index out of range");
        }

        const StringId canonical = internString(canonicalKey);
        const std::span<const StringId> single(&canonical, 1);

        // Existing attributes retain generic replacement semantics and profile
        // counts, without materializing owned strings or temporary lists.
        if (!operation.attrs.empty())
        {
            const auto profileWrite = [&](std::string_view key, std::size_t bytes) {
                if (graphProfileEnabled()) {
                    profileCounters.attributeWrites.fetch_add(1, std::memory_order_relaxed);
                    profileCounters.attributeBytes.fetch_add(key.size() + bytes, std::memory_order_relaxed);
                }
            };
            const auto writeStringList = [&](std::string_view key) {
                profileWrite(key, canonicalKey.size());
                setAttrRecord(opIdx, key, internStringListAttribute(key, single));
            };
            writeStringList(kMembers);
            writeStringList(kOrigins);
            if (root)
            {
                writeStringList(kRoots);
                profileWrite(kRootResults, sizeof(rootResult));
                setAttrRecord(opIdx, kRootResults,
                              internIntListAttribute(kRootResults, std::span(&rootResult, 1)));
            }
            else
            {
                eraseAttr(op, kRoots);
                eraseAttr(op, kRootResults);
            }
            return;
        }

        auto& attrs = operation.attrs;
        attrs.reserve(root ? 4 : 2);
        attrs.push_back(internStringListAttribute(kMembers, single));
        attrs.push_back(internStringListAttribute(kOrigins, single));
        if (root) {
            attrs.push_back(internIntListAttribute(kRootResults, std::span(&rootResult, 1)));
            attrs.push_back(internStringListAttribute(kRoots, single));
        }
    }

    void GraphStore::setOpKind(OperationId op, OperationKind kind)
    {
        const std::size_t opIdx = opIndex(op);
        if (!operations_[opIdx].alive)
        {
            throw std::runtime_error("OperationId refers to erased operation");
        }
        operations_.kind_.writableAt(opIdx) = kind;
    }

    bool GraphStore::eraseAttr(OperationId op, std::string_view key)
    {
        const std::size_t opIdx = opIndex(op);
        if (!operations_[opIdx].alive)
        {
            return false;
        }
        auto &attrs = operations_[opIdx].attrs;
        for (auto it = attrs.begin(); it != attrs.end(); ++it)
        {
            if (string(attribute(*it).key) == key)
            {
                attrs.erase(it);
                return true;
            }
        }
        return false;
    }

    void GraphStore::setValueSrcLoc(ValueId value, SrcLoc loc)
    {
        setValueSrcLocView(value, SrcLocView(loc));
    }

    void GraphStore::setValueSrcLocView(ValueId value, SrcLocView loc)
    {
        const std::size_t valIdx = valueIndex(value);
        if (!values_[valIdx].alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }
        if (graphProfileEnabled()) {
            profileCounters.srcLocWrites.fetch_add(1, std::memory_order_relaxed);
            profileCounters.srcLocBytes.fetch_add(srcLocBytes(loc), std::memory_order_relaxed);
        }
        values_[valIdx].srcLoc = internSrcLocView(loc);
    }

    void GraphStore::setValueShape(ValueId value, std::span<const int64_t> shape)
    {
        const std::size_t valIdx = valueIndex(value);
        auto data = values_[valIdx];
        if (!data.alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }
        if (!data.shape.empty())
        {
            throw std::runtime_error("Value shape is already set");
        }
        validateValueShape(data.width, data.type, shape, "Value");
        data.shape.set(shape);
    }

    void GraphStore::setOpSrcLoc(OperationId op, SrcLoc loc)
    {
        setOpSrcLocView(op, SrcLocView(loc));
    }

    void GraphStore::setOpSrcLocView(OperationId op, SrcLocView loc)
    {
        const std::size_t opIdx = opIndex(op);
        if (!operations_[opIdx].alive)
        {
            throw std::runtime_error("OperationId refers to erased operation");
        }
        if (graphProfileEnabled()) {
            profileCounters.srcLocWrites.fetch_add(1, std::memory_order_relaxed);
            profileCounters.srcLocBytes.fetch_add(srcLocBytes(loc), std::memory_order_relaxed);
        }
        operations_[opIdx].srcLoc = internSrcLocView(loc);
    }

    void GraphStore::setOpAndValueSrcLocView(OperationId op, ValueId value, SrcLocView loc)
    {
        const std::size_t opIdx = opIndex(op);
        if (!operations_[opIdx].alive)
        {
            throw std::runtime_error("OperationId refers to erased operation");
        }
        const std::size_t valIdx = valueIndex(value);
        if (!values_[valIdx].alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }
        if (graphProfileEnabled()) {
            profileCounters.srcLocWrites.fetch_add(2, std::memory_order_relaxed);
            profileCounters.srcLocBytes.fetch_add(2 * srcLocBytes(loc), std::memory_order_relaxed);
        }
        const RecordId record = internSrcLocView(loc);
        operations_[opIdx].srcLoc = record;
        values_[valIdx].srcLoc = record;
    }

    void GraphStore::setOpSymbol(OperationId op, SymbolId sym)
    {
        validateSymbol(sym, "Operation");
        const std::size_t opIdx = opIndex(op);
        if (!operations_[opIdx].alive)
        {
            throw std::runtime_error("OperationId refers to erased operation");
        }
        const SymbolId old = operations_[opIdx].symbol;
        if (old == sym)
        {
            return;
        }
        if (symbolIndex_.find(sym.value) != symbolIndex_.end())
        {
            throw std::runtime_error("Operation symbol already bound to value or operation");
        }
        if (old.valid())
        {
            unbindSymbol(old, SymbolKind::kOperation, static_cast<uint32_t>(opIdx + 1));
        }
        bindSymbol(sym, SymbolKind::kOperation, static_cast<uint32_t>(opIdx + 1), "Operation");
        operations_[opIdx].symbol = sym;
    }

    void GraphStore::setValueSymbol(ValueId value, SymbolId sym)
    {
        validateSymbol(sym, "Value");
        const std::size_t valIdx = valueIndex(value);
        if (!values_[valIdx].alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }
        const SymbolId old = values_[valIdx].symbol;
        if (old == sym)
        {
            return;
        }
        if (symbolIndex_.find(sym.value) != symbolIndex_.end())
        {
            throw std::runtime_error("Value symbol already bound to value or operation");
        }
        if (old.valid())
        {
            unbindSymbol(old, SymbolKind::kValue, static_cast<uint32_t>(valIdx + 1));
        }
        bindSymbol(sym, SymbolKind::kValue, static_cast<uint32_t>(valIdx + 1), "Value");
        values_[valIdx].symbol = sym;
    }

    void GraphStore::clearOpSymbol(OperationId op)
    {
        throw std::runtime_error("Clearing operation symbol is not allowed");
    }

    void GraphStore::clearValueSymbol(ValueId value)
    {
        throw std::runtime_error("Clearing value symbol is not allowed");
    }

    std::size_t GraphStore::valueIndex(ValueId value) const
    {
        value.assertGraph(graphId_);
        const std::size_t index = static_cast<std::size_t>(value.index);
        if (index == 0 || index > values_.size())
        {
            throw std::runtime_error("ValueId out of range");
        }
        return index - 1;
    }

    std::size_t GraphStore::opIndex(OperationId op) const
    {
        op.assertGraph(graphId_);
        const std::size_t index = static_cast<std::size_t>(op.index);
        if (index == 0 || index > operations_.size())
        {
            throw std::runtime_error("OperationId out of range");
        }
        return index - 1;
    }

    bool GraphStore::valueAlive(ValueId value) const
    {
        return values_[valueIndex(value)].alive;
    }

    bool GraphStore::opAlive(OperationId op) const
    {
        return operations_[opIndex(op)].alive;
    }

    std::size_t GraphStore::countValueUses(ValueId value, std::optional<OperationId> skipOp) const
    {
        const std::size_t valIdx = valueIndex(value);
        if (!skipOp)
        {
            return valueUsers_[valIdx].size();
        }
        skipOp->assertGraph(graphId_);
        std::size_t count = 0;
        for (const auto &user : valueUsers_[valIdx])
        {
            if (user.operation != *skipOp)
            {
                ++count;
            }
        }
        return count;
    }

    void GraphStore::addValueUser(ValueId value, OperationId op, std::size_t operandIndex)
    {
        const std::size_t valIdx = valueIndex(value);
        op.assertGraph(graphId_);
        if (!values_[valIdx].alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }
        auto& indices = operations_[opIndex(op)].operandUseIndices;
        if (indices.size() <= operandIndex) indices.resize(operandIndex + 1);
        indices[operandIndex] = valueUsers_[valIdx].size();
        valueUsers_[valIdx].push_back(ValueUser{op, static_cast<uint32_t>(operandIndex)});
    }

    void GraphStore::removeValueUser(ValueId value, OperationId op, std::size_t operandIndex)
    {
        const std::size_t valIdx = valueIndex(value);
        op.assertGraph(graphId_);
        auto &users = valueUsers_[valIdx];
        const auto& indices = operations_[opIndex(op)].operandUseIndices;
        if (operandIndex < indices.size())
        {
            const std::size_t i = indices[operandIndex];
            if (i < users.size() && users[i].operation == op && users[i].operandIndex == operandIndex)
            {
                const ValueUser moved = users.back();
                users[i] = moved;
                operations_[opIndex(moved.operation)].operandUseIndices[moved.operandIndex] = i;
                users.pop_back();
                return;
            }
        }
        throw std::runtime_error("Value use-list out of sync while removing user");
    }

    void GraphStore::removeOpUses(OperationId op, std::span<const ValueId> operands)
    {
        for (std::size_t operandIndex = 0; operandIndex < operands.size(); ++operandIndex)
        {
            removeValueUser(operands[operandIndex], op, operandIndex);
        }
        operations_[opIndex(op)].operandUseIndices.clear();
    }

    void GraphStore::addOpUses(OperationId op, std::span<const ValueId> operands)
    {
        for (std::size_t operandIndex = 0; operandIndex < operands.size(); ++operandIndex)
        {
            addValueUser(operands[operandIndex], op, operandIndex);
        }
    }

    void GraphStore::replaceAllUsesInternal(ValueId from, ValueId to, std::optional<OperationId> skipOp)
    {
        if (from == to)
        {
            return;
        }
        const std::size_t fromIdx = valueIndex(from);
        const std::size_t toIdx = valueIndex(to);
        if (!values_[fromIdx].alive || !values_[toIdx].alive)
        {
            throw std::runtime_error("replaceAllUses requires live values");
        }
        if (skipOp)
        {
            skipOp->assertGraph(graphId_);
        }

        std::vector<ValueUser> users = valueUsers_[fromIdx];
        valueUsers_[fromIdx].clear();
        valueUsers_[fromIdx].reserve(users.size());
        auto &destination = valueUsers_[toIdx];
        const std::size_t required = destination.size() + users.size();
        if (required > destination.capacity())
        {
            // Repeated aliases often converge on one clock, enable or constant.
            // Exact-size reserves copy its complete use-list on every merge.
            destination.reserve(std::max(required, destination.capacity() * 2));
        }

        for (const auto &user : users)
        {
            if (skipOp && user.operation == *skipOp)
            {
                addValueUser(from, user.operation, user.operandIndex);
                continue;
            }
            const std::size_t opIdx = opIndex(user.operation);
            if (!operations_[opIdx].alive)
            {
                continue;
            }
            auto &operands = operations_[opIdx].operands;
            if (user.operandIndex >= operands.size())
            {
                throw std::runtime_error("Value use-list out of sync with operands");
            }
            if (operands[user.operandIndex] != from)
            {
                throw std::runtime_error("Value use-list out of sync with operands");
            }
            operands[user.operandIndex] = to;
            addValueUser(to, user.operation, user.operandIndex);
        }
    }

    void GraphStore::recomputePortFlags()
    {
        for (std::size_t valueIndex = 0; valueIndex < values_.size(); ++valueIndex)
        {
            auto value = values_[valueIndex];
            value.isInput = false;
            value.isOutput = false;
            value.isInout = false;
        }
        for (const auto &port : inputPorts_)
        {
            const std::size_t valIdx = valueIndex(port.value);
            if (!values_[valIdx].alive)
            {
                throw std::runtime_error("Input port references erased value");
            }
            values_[valIdx].isInput = true;
        }
        for (const auto &port : outputPorts_)
        {
            const std::size_t valIdx = valueIndex(port.value);
            if (!values_[valIdx].alive)
            {
                throw std::runtime_error("Output port references erased value");
            }
            values_[valIdx].isOutput = true;
        }
        for (const auto &port : inoutPorts_)
        {
            auto markInout = [&](ValueId value)
            {
                const std::size_t valIdx = valueIndex(value);
                if (!values_[valIdx].alive)
                {
                    throw std::runtime_error("Inout port references erased value");
                }
                if (values_[valIdx].isInput || values_[valIdx].isOutput)
                {
                    throw std::runtime_error("Inout port references value bound as input/output");
                }
                values_[valIdx].isInout = true;
            };
            markInout(port.in);
            markInout(port.out);
            markInout(port.oe);
        }
    }

    void GraphStore::validateSymbol(SymbolId sym, std::string_view context) const
    {
        if (!sym.valid())
        {
            throw std::runtime_error(std::string(context) + " symbol is invalid");
        }
        if (symbols_ && !symbols_->valid(sym))
        {
            throw std::runtime_error(std::string(context) + " symbol is not in the symbol table");
        }
    }

    void GraphStore::bindSymbol(SymbolId sym, SymbolKind kind, uint32_t index, std::string_view context)
    {
        if (!sym.valid())
        {
            throw std::runtime_error(std::string(context) + " symbol is invalid");
        }
        auto [it, inserted] = symbolIndex_.emplace(sym.value, SymbolBinding{kind, index});
        if (!inserted)
        {
            if (it->second.kind == kind && it->second.index == index)
            {
                return;
            }
            const char *owner = it->second.kind == SymbolKind::kValue ? "value" : "operation";
            std::string message = std::string(context) + " symbol already bound to " + owner;
            if (symbols_ && symbols_->valid(sym))
            {
                std::string_view symbolText = symbols_->text(sym);
                if (!symbolText.empty())
                {
                    message.append(": ");
                    message.append(symbolText);
                }
            }
            throw std::runtime_error(message);
        }
    }

    void GraphStore::unbindSymbol(SymbolId sym, SymbolKind kind, uint32_t index)
    {
        if (!sym.valid())
        {
            return;
        }
        auto it = symbolIndex_.find(sym.value);
        if (it == symbolIndex_.end())
        {
            throw std::runtime_error("Symbol binding missing during unbind");
        }
        if (it->second.kind != kind || it->second.index != index)
        {
            throw std::runtime_error("Symbol binding mismatch during unbind");
        }
        symbolIndex_.erase(it);
    }

    namespace {
        uint64_t nextDesignInstance() noexcept
        {
            static std::atomic<uint64_t> next{1};
            return next.fetch_add(1, std::memory_order_relaxed);
        }
    }

    Design::Design() : instanceId_(nextDesignInstance()) {}

    Design::ParallelMutationScope::ParallelMutationScope(Design& design) : design_(design) {
        if (design_.deferGraphRevisions_)
            throw std::logic_error("Parallel graph mutation scopes cannot be nested");
        ++design_.revision_;
        design_.deferGraphRevisions_ = true;
    }

    Design::ParallelMutationScope::~ParallelMutationScope() noexcept {
        design_.deferGraphRevisions_ = false;
        ++design_.revision_;
    }

    Design::ParallelMutationScope Design::parallelGraphMutation() {
        return ParallelMutationScope(*this);
    }

    Design::Design(Design &&other) noexcept
    {
        *this = std::move(other);
    }

    Design &Design::operator=(Design &&other) noexcept
    {
        if (this != &other)
        {
            instanceId_ = nextDesignInstance();
            ++revision_;
            other.instanceId_ = nextDesignInstance();
            ++other.revision_;
            graphs_ = std::move(other.graphs_);
            graphAliasBySymbol_ = std::move(other.graphAliasBySymbol_);
            graphOrder_ = std::move(other.graphOrder_);
            topGraphs_ = std::move(other.topGraphs_);
            declaredSymbols_ = std::move(other.declaredSymbols_);
            declaredSymbolSet_ = std::move(other.declaredSymbolSet_);
            designSymbols_ = std::move(other.designSymbols_);
            resetGraphOwners();

            other.graphAliasBySymbol_.clear();
            other.graphOrder_.clear();
            other.topGraphs_.clear();
            other.declaredSymbols_.clear();
            other.declaredSymbolSet_.clear();
        }
        return *this;
    }

    void Design::resetGraphOwners()
    {
        for (auto &entry : graphs_)
        {
            if (entry.second)
            {
                entry.second->owner_ = this;
            }
        }
    }

    namespace
    {

        template <class... Ts>
        struct Overloaded : Ts...
        {
            using Ts::operator()...;
        };
        template <class... Ts>
        Overloaded(Ts...) -> Overloaded<Ts...>;

        constexpr std::string_view kOperationNames[] = {
            "kConstant",
            "kAdd",
            "kSub",
            "kMul",
            "kDiv",
            "kMod",
            "kEq",
            "kNe",
            "kCaseEq",
            "kCaseNe",
            "kWildcardEq",
            "kWildcardNe",
            "kLt",
            "kLe",
            "kGt",
            "kGe",
            "kAnd",
            "kOr",
            "kXor",
            "kXnor",
            "kNot",
            "kLogicAnd",
            "kLogicOr",
            "kLogicNot",
            "kReduceAnd",
            "kReduceOr",
            "kReduceXor",
            "kReduceNor",
            "kReduceNand",
            "kReduceXnor",
            "kShl",
            "kLShr",
            "kAShr",
            "kMux",
            "kAssign",
            "kConcat",
            "kArrayCreate",
            "kReplicate",
            "kSliceStatic",
            "kSliceDynamic",
            "kSliceArray",
            "kLatch",
            "kLatchReadPort",
            "kLatchWritePort",
            "kRegister",
            "kRegisterReadPort",
            "kRegisterReadArrayPort",
            "kRegisterWritePort",
            "kRegisterWriteMaskPort",
            "kRegisterWriteArrayPort",
            "kRegisterWriteArrayMaskPort",
            "kRegisterFillPort",
            "kInstance",
            "kBlackbox",
            "kSystemFunction",
            "kSystemTask",
            "kDpicImport",
            "kDpicCall",
            "kXMRRead",
            "kXMRWrite"};

        void writeAttributeValue(slang::JsonWriter &writer, const AttributeValue &value)
        {
            if (!attributeValueIsJsonSerializable(value))
            {
                throw std::runtime_error("Attribute value is not JSON serializable");
            }
            std::visit(
                Overloaded{
                    [&](bool v)
                    {
                        writer.writeProperty("t");
                        writer.writeValue(std::string_view("bool"));
                        writer.writeProperty("v");
                        writer.writeValue(v);
                    },
                    [&](int64_t v)
                    {
                        writer.writeProperty("t");
                        writer.writeValue(std::string_view("int"));
                        writer.writeProperty("v");
                        writer.writeValue(v);
                    },
                    [&](double v)
                    {
                        writer.writeProperty("t");
                        writer.writeValue(std::string_view("double"));
                        writer.writeProperty("v");
                        writer.writeValue(v);
                    },
                    [&](const std::string &v)
                    {
                        writer.writeProperty("t");
                        writer.writeValue(std::string_view("string"));
                        writer.writeProperty("v");
                        writer.writeValue(v);
                    },
                    [&](const std::vector<bool> &arr)
                    {
                        writer.writeProperty("t");
                        writer.writeValue(std::string_view("bool[]"));
                        writer.writeProperty("vs");
                        writer.startArray();
                        for (bool entry : arr)
                        {
                            writer.writeValue(entry);
                        }
                        writer.endArray();
                    },
                    [&](const std::vector<int64_t> &arr)
                    {
                        writer.writeProperty("t");
                        writer.writeValue(std::string_view("int[]"));
                        writer.writeProperty("vs");
                        writer.startArray();
                        for (int64_t entry : arr)
                        {
                            writer.writeValue(entry);
                        }
                        writer.endArray();
                    },
                    [&](const std::vector<double> &arr)
                    {
                        writer.writeProperty("t");
                        writer.writeValue(std::string_view("double[]"));
                        writer.writeProperty("vs");
                        writer.startArray();
                        for (double entry : arr)
                        {
                            writer.writeValue(entry);
                        }
                        writer.endArray();
                    },
                    [&](const std::vector<std::string> &arr)
                    {
                        writer.writeProperty("t");
                        writer.writeValue(std::string_view("string[]"));
                        writer.writeProperty("vs");
                        writer.startArray();
                        for (const auto &entry : arr)
                        {
                            writer.writeValue(entry);
                        }
                        writer.endArray();
                    }},
                value);
        }

        void writeSrcLoc(slang::JsonWriter &writer, const std::optional<SrcLoc> &srcLoc)
        {
            if (!srcLoc)
            {
                return;
            }
            if (srcLoc->file.empty() && srcLoc->line == 0 && srcLoc->column == 0 &&
                srcLoc->endLine == 0 && srcLoc->endColumn == 0 &&
                srcLoc->origin.empty() && srcLoc->pass.empty() && srcLoc->note.empty())
            {
                return;
            }
            writer.writeProperty("loc");
            writer.startObject();
            if (!srcLoc->file.empty())
            {
                writer.writeProperty("file");
                writer.writeValue(srcLoc->file);
            }
            if (srcLoc->line != 0)
            {
                writer.writeProperty("line");
                writer.writeValue(static_cast<int64_t>(srcLoc->line));
            }
            if (srcLoc->column != 0)
            {
                writer.writeProperty("col");
                writer.writeValue(static_cast<int64_t>(srcLoc->column));
            }
            if (srcLoc->endLine != 0)
            {
                writer.writeProperty("endLine");
                writer.writeValue(static_cast<int64_t>(srcLoc->endLine));
            }
            if (srcLoc->endColumn != 0)
            {
                writer.writeProperty("endCol");
                writer.writeValue(static_cast<int64_t>(srcLoc->endColumn));
            }
            if (!srcLoc->origin.empty())
            {
                writer.writeProperty("origin");
                writer.writeValue(srcLoc->origin);
            }
            if (!srcLoc->pass.empty())
            {
                writer.writeProperty("pass");
                writer.writeValue(srcLoc->pass);
            }
            if (!srcLoc->note.empty())
            {
                writer.writeProperty("note");
                writer.writeValue(srcLoc->note);
            }
            writer.endObject();
        }

    } // namespace

    std::string_view toString(OperationKind kind) noexcept
    {
        auto index = static_cast<std::size_t>(kind);
        if (index >= std::size(kOperationNames))
        {
            const auto* descriptor = findOpDescriptor(kind);
            return descriptor ? descriptor->name : "<unknown>";
        }
        return kOperationNames[index];
    }

    std::optional<OperationKind> parseOperationKind(std::string_view text) noexcept
    {
        for (std::size_t i = 0; i < std::size(kOperationNames); ++i)
        {
            if (kOperationNames[i] == text)
            {
                return static_cast<OperationKind>(i);
            }
        }
        return findDialectOperation(text);
    }

    bool isRegisterReadPortKind(OperationKind kind) noexcept
    {
        return kind == OperationKind::kRegisterReadPort ||
               kind == OperationKind::kRegisterReadArrayPort;
    }

    bool isRegisterWritePortKind(OperationKind kind) noexcept
    {
        switch (kind)
        {
        case OperationKind::kRegisterWritePort:
        case OperationKind::kRegisterWriteMaskPort:
        case OperationKind::kRegisterWriteArrayPort:
        case OperationKind::kRegisterWriteArrayMaskPort:
        case OperationKind::kRegisterFillPort:
            return true;
        default:
            return false;
        }
    }

    namespace
    {
        std::optional<std::size_t> eventEdgeAttrCount(AttrRange attrs)
        {
            const auto attr = attrs.find("eventEdge");
            if (!attr) return std::nullopt;
            const auto edges = attr->value().get<std::vector<std::string>>();
            return edges ? std::optional(edges->size()) : std::nullopt;
        }

        std::optional<std::size_t> eventEdgeAttrCount(std::span<const AttrKV> attrs)
        {
            for (const auto &attr : attrs)
            {
                if (attr.key == "eventEdge")
                {
                    if (const auto *edges = std::get_if<std::vector<std::string>>(&attr.value))
                    {
                        return edges->size();
                    }
                    return std::nullopt;
                }
            }
            return std::nullopt;
        }

        std::optional<std::size_t> firstEventOperandIndexImpl(OperationKind kind,
                                                              std::size_t operandCount,
                                                              std::optional<std::size_t> eventCount)
        {
            switch (kind)
            {
            case OperationKind::kRegisterWritePort:
            case OperationKind::kRegisterFillPort:
                // [updateCond, nextValue|data, events...]
                if (operandCount < 2)
                {
                    return std::nullopt;
                }
                return std::size_t{2};
            case OperationKind::kRegisterWriteMaskPort:
            case OperationKind::kLatchWritePort:
                // [updateCond, nextValue, mask, events...] (latch writes carry no events)
                if (operandCount < 3)
                {
                    return std::nullopt;
                }
                return std::size_t{3};
            case OperationKind::kRegisterWriteArrayPort:
                // [updateCond, i0..ik-1, data, events...]; k >= 1 needs >= 3 head operands
                if (!eventCount || *eventCount > operandCount || operandCount - *eventCount < 3)
                {
                    return std::nullopt;
                }
                return operandCount - *eventCount;
            case OperationKind::kRegisterWriteArrayMaskPort:
                // [updateCond, i0..ik-1, data, mask, events...]; k >= 1 needs >= 4 head operands
                if (!eventCount || *eventCount > operandCount || operandCount - *eventCount < 4)
                {
                    return std::nullopt;
                }
                return operandCount - *eventCount;
            default:
                return std::nullopt;
            }
        }

        std::optional<std::size_t> arrayPortIndexCountImpl(OperationKind kind,
                                                           std::size_t operandCount,
                                                           std::optional<std::size_t> eventCount)
        {
            switch (kind)
            {
            case OperationKind::kRegisterReadArrayPort:
                if (operandCount < 1)
                {
                    return std::nullopt;
                }
                return operandCount;
            case OperationKind::kRegisterWriteArrayPort:
            case OperationKind::kRegisterWriteArrayMaskPort:
            {
                const std::optional<std::size_t> eventStart =
                    firstEventOperandIndexImpl(kind, operandCount, eventCount);
                if (!eventStart)
                {
                    return std::nullopt;
                }
                const std::size_t fixedTail =
                    kind == OperationKind::kRegisterWriteArrayPort ? 2 : 3;
                return *eventStart - fixedTail;
            }
            default:
                return std::nullopt;
            }
        }
    } // namespace

    std::optional<std::size_t> firstEventOperandIndex(const Operation &op)
    {
        return firstEventOperandIndexImpl(op.kind(), op.operands().size(),
                                          eventEdgeAttrCount(op.attrs()));
    }


    std::optional<std::size_t> firstEventOperandIndex(const Graph &graph, OperationId op)
    {
        return firstEventOperandIndexImpl(graph.opKind(op), graph.opOperands(op).size(),
                                          eventEdgeAttrCount(graph.opAttrRange(op)));
    }

    std::optional<std::size_t> arrayPortIndexCount(const Operation &op)
    {
        return arrayPortIndexCountImpl(op.kind(), op.operands().size(),
                                       eventEdgeAttrCount(op.attrs()));
    }


    std::optional<std::size_t> arrayPortIndexCount(const Graph &graph, OperationId op)
    {
        return arrayPortIndexCountImpl(graph.opKind(op), graph.opOperands(op).size(),
                                       eventEdgeAttrCount(graph.opAttrRange(op)));
    }

    std::string_view toString(ValueType type) noexcept
    {
        switch (type)
        {
        case ValueType::Logic:
            return "logic";
        case ValueType::Real:
            return "real";
        case ValueType::String:
            return "string";
        default:
            return "<unknown>";
        }
    }

    std::optional<ValueType> parseValueType(std::string_view text) noexcept
    {
        if (text == "logic")
        {
            return ValueType::Logic;
        }
        if (text == "real")
        {
            return ValueType::Real;
        }
        if (text == "string")
        {
            return ValueType::String;
        }
        return std::nullopt;
    }

    std::string_view toString(LogicDomain domain) noexcept
    {
        switch (domain)
        {
        case LogicDomain::TwoState: return "2state";
        case LogicDomain::FourState: return "4state";
        }
        return "<unknown>";
    }

    std::optional<LogicDomain> parseLogicDomain(std::string_view text) noexcept
    {
        if (text == "2state") return LogicDomain::TwoState;
        if (text == "4state") return LogicDomain::FourState;
        return std::nullopt;
    }

    void LogicalType::validate() const
    {
        if (kind != ValueType::Logic && kind != ValueType::Real && kind != ValueType::String)
            throw std::invalid_argument("Unknown logical value kind");
        if (domain != LogicDomain::TwoState && domain != LogicDomain::FourState)
            throw std::invalid_argument("Unknown logical domain");
        if (kind == ValueType::Logic && width <= 0)
            throw std::invalid_argument("Logic width must be positive");
        validateValueShape(width, kind, shape, "LogicalType");
    }

    LogicalType LogicalType::normalized() const
    {
        validate();
        LogicalType result = *this;
        if (kind != ValueType::Logic)
        {
            result.width = 1;
            result.isSigned = false;
            result.domain = LogicDomain::FourState;
        }
        return result;
    }

    int32_t LogicalType::leafWidth() const
    {
        validate();
        int64_t count = 1;
        for (const int64_t dim : shape) count *= dim;
        return kind == ValueType::Logic ? static_cast<int32_t>(width / count) : 1;
    }

    LogicDomain Graph::valueDomain(ValueId id) const
    {
        const auto data = store_->values_.at(store_->valueIndex(id));
        if (!data.alive) throw std::runtime_error("ValueId refers to erased value");
        return data.domain;
    }


    LogicalType Graph::valueLogicalType(ValueId value) const
    {
        const auto shape = valueShape(value);
        return {valueType(value), valueWidth(value), valueSigned(value), valueDomain(value),
                {shape.begin(), shape.end()}};
    }

    void Graph::setValueLogicalType(ValueId value, const LogicalType& type)
    {
        const LogicalType logical = type.normalized();
        GraphStore& builder = mutableStore();
        const auto index = builder.valueIndex(value);
        if (!builder.values_.alive_[index]) throw std::runtime_error("Cannot type an erased value");
        builder.values_.width_.writableAt(index) = logical.width;
        builder.values_.isSigned_.writableAt(index) = logical.isSigned;
        builder.values_.type_.writableAt(index) = logical.kind;
        builder.values_.domain_.writableAt(index) = logical.domain;
        builder.values_[index].shape = logical.shape;
    }

    Value::Value(ValueId id, SymbolId symbol, std::string symbolText, int32_t width, bool isSigned,
                 ValueType type, LogicDomain domain, std::vector<int64_t> shape, bool isInput, bool isOutput, bool isInout,
                 OperationId definingOp, std::span<const ValueUser> users,
                 std::optional<SrcLoc> srcLoc, const Graph *owner, bool usersLoaded)
        : id_(id),
          symbol_(symbol),
          symbolText_(std::move(symbolText)),
          width_(width),
          isSigned_(isSigned),
          type_(type),
          domain_(domain),
          shape_(std::move(shape)),
          isInput_(isInput),
          isOutput_(isOutput),
          isInout_(isInout),
          definingOp_(definingOp),
          usersLoaded_(usersLoaded),
          owner_(owner),
          srcLoc_(std::move(srcLoc))
    {
        if (usersLoaded_)
        {
            users_.assign(users);
        }
    }

    std::span<const ValueUser> Value::users() const noexcept
    {
        if (!usersLoaded_)
        {
            if (owner_ != nullptr)
            {
                users_.assign(owner_->valueUsersSpan(id_));
            }
            usersLoaded_ = true;
        }
        return users_.span();
    }

    Operation::Operation(OperationId id, OperationKind kind, SymbolId symbol, std::string symbolText,
                         std::vector<ValueId> operands, std::vector<ValueId> results,
                         std::vector<AttrKV> attrs, std::optional<SrcLoc> srcLoc)
        : id_(id),
          kind_(kind),
          symbol_(symbol),
          symbolText_(std::move(symbolText)),
          operands_(std::move(operands)),
          results_(std::move(results)),
          attrs_(std::move(attrs)),
          srcLoc_(std::move(srcLoc))
    {
    }

    std::optional<AttributeValue> Operation::attr(std::string_view key) const
    {
        for (const auto &entry : attrs_)
        {
            if (entry.key == key)
            {
                return entry.value;
            }
        }
        return std::nullopt;
    }

    Graph::Graph(Design& owner, std::string symbol, GraphId graphId)
        : owner_(&owner), symbol_(std::move(symbol)), graphId_(graphId),
          store_(std::make_shared<GraphStore>(symbols_, graphId))
    {
        if (symbol_.empty()) throw std::invalid_argument("Graph symbol must not be empty");
        if (!graphId_.valid()) throw std::invalid_argument("GraphId must be valid");
    }

    SemanticTree& Graph::getOrCreateSemanticTree(std::string sourceUnit, std::string circuit,
                                                 std::string module)
    {
        if (semanticTree_) {
            if (semanticTree_->sourceUnit() != sourceUnit || semanticTree_->circuit() != circuit ||
                semanticTree_->name(semanticTree_->root()) != module)
                throw std::invalid_argument("Graph SemanticTree identity mismatch");
            return *semanticTree_;
        }
        mutableStore();
        semanticTree_.emplace(std::move(sourceUnit), std::move(circuit), std::move(module));
        return *semanticTree_;
    }

    SemanticId Graph::appendSemanticTree(const Graph& source, SemanticId parent,
                                         std::string_view rootName)
    {
        if (!source.semanticTree_ || this == &source) return 0;
        if (!semanticTree_) throw std::invalid_argument("target Graph has no SemanticTree");
        mutableStore();
        return semanticTree_->appendTree(*source.semanticTree_, parent, rootName);
    }

    std::span<const SemanticId> Graph::valueSemanticIds(ValueId value) const
    {
        const std::size_t row = store_->valueIndex(value);
        const Range range = store_->values_.semantics_[row];
        const auto& ids = store_->topology_->semantics;
        if (range.offset > ids.size() || range.count > ids.size() - range.offset)
            throw std::out_of_range("value semantic list is out of range");
        return std::span<const SemanticId>(ids.data() + range.offset, range.count);
    }

    void Graph::setValueSemanticIds(ValueId value, std::span<const SemanticId> ids)
    {
        GraphStore& builder = mutableStore();
        const std::size_t row = builder.valueIndex(value);
        if (!ids.empty() && !semanticTree_) throw std::invalid_argument("value has no SemanticTree");
        for (const SemanticId id : ids)
            if (!semanticTree_->contains(id)) throw std::invalid_argument("invalid value SemanticId");
        builder.values_.semantics_[row] = builder.storeSemanticIds(ids);
    }

    void Graph::addValueSemanticIds(ValueId value, std::span<const SemanticId> ids)
    {
        if (ids.empty()) return;
        slang::SmallVector<SemanticId, 4> merged;
        const auto existing = valueSemanticIds(value);
        merged.append(existing.begin(), existing.end());
        for (const SemanticId id : ids)
            if (std::find(merged.begin(), merged.end(), id) == merged.end()) merged.push_back(id);
        if (merged.size() != existing.size()) setValueSemanticIds(value,
            std::span<const SemanticId>(merged.data(), merged.size()));
    }

    void Graph::mergeValueSemanticIds(ValueId target, ValueId source)
    {
        if (target == source) return;
        addValueSemanticIds(target, valueSemanticIds(source));
    }

    std::span<const SemanticId> Graph::opSemanticIds(OperationId op) const
    {
        const std::size_t row = store_->opIndex(op);
        const Range range = store_->operations_.semantics_[row];
        const auto& ids = store_->topology_->semantics;
        if (range.offset > ids.size() || range.count > ids.size() - range.offset)
            throw std::out_of_range("operation semantic list is out of range");
        return std::span<const SemanticId>(ids.data() + range.offset, range.count);
    }

    void Graph::setOpSemanticIds(OperationId op, std::span<const SemanticId> ids)
    {
        GraphStore& builder = mutableStore();
        const std::size_t row = builder.opIndex(op);
        if (!ids.empty() && !semanticTree_) throw std::invalid_argument("operation has no SemanticTree");
        for (const SemanticId id : ids)
            if (!semanticTree_->contains(id)) throw std::invalid_argument("invalid operation SemanticId");
        builder.operations_.semantics_[row] = builder.storeSemanticIds(ids);
    }

    void Graph::addOpSemanticIds(OperationId op, std::span<const SemanticId> ids)
    {
        if (ids.empty()) return;
        slang::SmallVector<SemanticId, 4> merged;
        const auto existing = opSemanticIds(op);
        merged.append(existing.begin(), existing.end());
        for (const SemanticId id : ids)
            if (std::find(merged.begin(), merged.end(), id) == merged.end()) merged.push_back(id);
        if (merged.size() != existing.size()) setOpSemanticIds(op,
            std::span<const SemanticId>(merged.data(), merged.size()));
    }

    void Graph::mergeOpSemanticIds(OperationId target, OperationId source)
    {
        if (target == source) return;
        addOpSemanticIds(target, opSemanticIds(source));
    }

    std::size_t Graph::remapValueBlockSemantics(const Graph& source,
                                                std::span<const ValueId> sourceValues,
                                                std::span<const ValueId> targetValues,
                                                SemanticId copiedRoot)
    {
        if (sourceValues.size() != targetValues.size())
            throw std::invalid_argument("semantic value block column lengths differ");
        std::size_t total = 0, annotated = 0;
        for (const ValueId value : sourceValues) {
            const auto ids = source.valueSemanticIds(value);
            annotated += !ids.empty();
            total += ids.size();
            for (const SemanticId id : ids)
                if (!source.semanticTree_ || !source.semanticTree_->contains(id))
                    throw std::invalid_argument("invalid source value SemanticId");
        }
        if (!total) return 0;
        if (!copiedRoot || !semanticTree_ || copiedRoot > semanticTree_->size() ||
            source.semanticTree_->size() > semanticTree_->size() - copiedRoot + 1)
            throw std::invalid_argument("missing copied SemanticTree root");
        GraphStore& builder = mutableStore();
        std::vector<SemanticId> mapped;
        const auto storeMapped = [&](std::span<const SemanticId> ids) {
            if (ids.empty()) return Range{};
            if (ids.size() == 1) {
                const SemanticId id = copiedRoot + ids.front() - 1;
                return builder.storeSemanticIds(std::span<const SemanticId>(&id, 1));
            }
            mapped.resize(ids.size());
            std::transform(ids.begin(), ids.end(), mapped.begin(),
                [copiedRoot](SemanticId id) { return copiedRoot + id - 1; });
            return builder.storeSemanticIds(mapped);
        };
        for (std::size_t i = 0; i < sourceValues.size(); ++i) {
            const auto range = storeMapped(source.valueSemanticIds(sourceValues[i]));
            builder.values_.semantics_[builder.valueIndex(targetValues[i])] = range;
        }
        return annotated;
    }

    std::size_t Graph::remapOperationBlockSemantics(const Graph& source,
                                                    std::span<const OperationId> sourceOps,
                                                    std::span<const OperationId> targetOps,
                                                    SemanticId copiedRoot)
    {
        if (sourceOps.size() != targetOps.size())
            throw std::invalid_argument("semantic operation block column lengths differ");
        std::size_t total = 0, annotated = 0;
        for (const OperationId op : sourceOps) {
            const auto ids = source.opSemanticIds(op);
            annotated += !ids.empty();
            total += ids.size();
            for (const SemanticId id : ids)
                if (!source.semanticTree_ || !source.semanticTree_->contains(id))
                    throw std::invalid_argument("invalid source operation SemanticId");
        }
        if (!total) return 0;
        if (!copiedRoot || !semanticTree_ || copiedRoot > semanticTree_->size() ||
            source.semanticTree_->size() > semanticTree_->size() - copiedRoot + 1)
            throw std::invalid_argument("missing copied SemanticTree root");
        GraphStore& builder = mutableStore();
        std::vector<SemanticId> mapped;
        const auto storeMapped = [&](std::span<const SemanticId> ids) {
            if (ids.empty()) return Range{};
            if (ids.size() == 1) {
                const SemanticId id = copiedRoot + ids.front() - 1;
                return builder.storeSemanticIds(std::span<const SemanticId>(&id, 1));
            }
            mapped.resize(ids.size());
            std::transform(ids.begin(), ids.end(), mapped.begin(),
                [copiedRoot](SemanticId id) { return copiedRoot + id - 1; });
            return builder.storeSemanticIds(mapped);
        };
        for (std::size_t i = 0; i < sourceOps.size(); ++i) {
            const auto range = storeMapped(source.opSemanticIds(sourceOps[i]));
            builder.operations_.semantics_[builder.opIndex(targetOps[i])] = range;
        }
        return annotated;
    }


    std::optional<AttributeValue> Graph::metadata(std::string_view key) const
    {
        for (const AttrKV& entry : metadata_)
            if (entry.key == key) return entry.value;
        return std::nullopt;
    }

    void Graph::setMetadata(std::string_view key, AttributeValue value)
    {
        if (key.empty() || !attributeValueIsJsonSerializable(value))
            throw std::invalid_argument("Invalid graph metadata");
        owner_->touch();
        compacted_ = false;
        for (AttrKV& entry : metadata_)
        {
            if (entry.key != key) continue;
            entry.value = std::move(value);
            return;
        }
        metadata_.push_back({std::string(key), std::move(value)});
    }

    bool Graph::eraseMetadata(std::string_view key)
    {
        const auto it = std::find_if(metadata_.begin(), metadata_.end(),
                                     [&](const AttrKV& entry) { return entry.key == key; });
        if (it == metadata_.end()) return false;
        owner_->touch();
        compacted_ = false;
        metadata_.erase(it);
        return true;
    }

    SymbolId Graph::internSymbol(std::string_view text)
    {
        const std::size_t count = symbols_.textById_.size();
        const SymbolId symbol = symbols_.intern(text);
        if (symbols_.textById_.size() != count)
        {
            owner_->touch();
            compacted_ = false;
        }
        return store_->symbolIndex_.contains(symbol.value) ? SymbolId::invalid() : symbol;
    }


    SymbolId Graph::lookupSymbol(std::string_view text) const
    {
        return symbols_.lookup(text);
    }

    std::string_view Graph::symbolText(SymbolId id) const
    {
        if (!id.valid())
        {
            return std::string_view{};
        }
        return symbols_.text(id);
    }

    SymbolId Graph::makeInternalOpSym()
    {
        std::string base = Graph::makeInternalBase("op");
        for (;;)
        {
            std::string candidate = base;
            candidate.push_back('_');
            candidate.append(std::to_string(nextInternalOpSym_++));
            if (const SymbolId symbol = symbols_.tryInternFresh(candidate); symbol.valid())
            {
                owner_->touch();
        compacted_ = false;
                return symbol;
            }
        }
    }

    SymbolId Graph::makeInternalValSym()
    {
        std::string base = Graph::makeInternalBase("val");
        for (;;)
        {
            std::string candidate = base;
            candidate.push_back('_');
            candidate.append(std::to_string(nextInternalValSym_++));
            if (const SymbolId symbol = symbols_.tryInternFresh(candidate); symbol.valid())
            {
                owner_->touch();
        compacted_ = false;
                return symbol;
            }
        }
    }

    void Graph::addDeclaredSymbol(SymbolId sym)
    {
        owner_->touch();
        compacted_ = false;
        if (!sym.valid())
        {
            throw std::runtime_error("Declared symbol is invalid");
        }
        if (!symbols_.valid(sym))
        {
            throw std::runtime_error("Declared symbol is not in the graph symbol table");
        }
        if (declaredSymbolSet_.insert(sym.value).second)
        {
            declaredSymbols_.push_back(sym);
        }
    }

    bool Graph::removeDeclaredSymbol(SymbolId sym)
    {
        owner_->touch();
        compacted_ = false;
        if (!sym.valid())
        {
            throw std::runtime_error("Declared symbol is invalid");
        }
        if (!symbols_.valid(sym))
        {
            throw std::runtime_error("Declared symbol is not in the graph symbol table");
        }
        if (declaredSymbolSet_.erase(sym.value) == 0)
        {
            return false;
        }
        auto it = std::remove_if(declaredSymbols_.begin(), declaredSymbols_.end(),
                                 [&](SymbolId entry) { return entry == sym; });
        if (it != declaredSymbols_.end())
        {
            declaredSymbols_.erase(it, declaredSymbols_.end());
        }
        return true;
    }

    void Graph::clearDeclaredSymbols()
    {
        owner_->touch();
        compacted_ = false;
        declaredSymbols_.clear();
        declaredSymbolSet_.clear();
    }

    bool Graph::isDeclaredSymbol(SymbolId sym) const noexcept
    {
        if (!sym.valid())
        {
            return false;
        }
        return declaredSymbolSet_.find(sym.value) != declaredSymbolSet_.end();
    }

    std::span<const SymbolId> Graph::declaredSymbols() const noexcept
    {
        return std::span<const SymbolId>(declaredSymbols_.data(), declaredSymbols_.size());
    }

    std::size_t Graph::addGenerateGroup(SymbolId scope, SymbolId name)
    {
        owner_->touch();
        compacted_ = false;
        if (!scope.valid() || !name.valid())
        {
            throw std::runtime_error("Generate group scope/name symbol is invalid");
        }
        if (!symbols_.valid(scope) || !symbols_.valid(name))
        {
            throw std::runtime_error(
                "Generate group scope/name symbol is not in the graph symbol table");
        }
        generateGroups_.push_back(GenerateGroup{scope, name, {}});
        return generateGroups_.size() - 1;
    }

    void Graph::addGenerateGroupSymbol(std::size_t group, SymbolId symbol)
    {
        owner_->touch();
        compacted_ = false;
        if (group >= generateGroups_.size())
        {
            throw std::runtime_error("Generate group index out of range");
        }
        if (!symbol.valid())
        {
            throw std::runtime_error("Generate group symbol is invalid");
        }
        if (!symbols_.valid(symbol))
        {
            throw std::runtime_error("Generate group symbol is not in the graph symbol table");
        }
        generateGroups_[group].symbols.push_back(symbol);
        auto& memberships = generateGroupsBySymbol_[symbol.value];
        if (std::find(memberships.begin(), memberships.end(), group) == memberships.end())
            memberships.push_back(group);
    }

    const std::vector<Graph::GenerateGroup>& Graph::generateGroups() const noexcept
    {
        return generateGroups_;
    }

    std::vector<std::string> Graph::validateDeclaredSymbols() const
    {
        std::vector<std::string> issues;
        auto memberText = [&](SymbolId sym) -> std::string
        {
            if (sym.valid() && symbols_.valid(sym))
            {
                return std::string(symbolText(sym));
            }
            return std::string("<invalid>");
        };
        auto checkMember = [&](SymbolId sym, std::string_view prefix)
        {
            if (!sym.valid() || !symbols_.valid(sym) ||
                (!findValue(sym).valid() && !findOperation(sym).valid()))
            {
                issues.push_back(std::string(prefix) + memberText(sym));
            }
        };
        for (const SymbolId sym : declaredSymbols_)
        {
            checkMember(sym, "declared:");
        }
        for (const GenerateGroup& group : generateGroups_)
        {
            if (!group.scope.valid() || !symbols_.valid(group.scope))
            {
                issues.push_back("generateGroupScope:" + memberText(group.scope));
            }
            if (!group.name.valid() || !symbols_.valid(group.name))
            {
                issues.push_back("generateGroupName:" + memberText(group.name));
            }
            for (const SymbolId sym : group.symbols)
            {
                checkMember(sym, "generateGroup:");
            }
        }
        return issues;
    }

    void Graph::removeSymbolFromGenerateGroups(SymbolId sym) noexcept
    {
        if (!sym.valid())
        {
            return;
        }
        const auto found = generateGroupsBySymbol_.find(sym.value);
        if (found == generateGroupsBySymbol_.end()) return;
        for (const auto index : found->second)
        {
            std::vector<SymbolId>& symbols = generateGroups_[index].symbols;
            symbols.erase(std::remove(symbols.begin(), symbols.end(), sym), symbols.end());
        }
        generateGroupsBySymbol_.erase(found);
    }

    void Graph::compact()
    {
        preparedFlattenFreeze_.reset();
        if (compacted_) return;
        owner_->touch();
        compacted_ = false;
        store_->compact();
        ++srcLocEpoch_;
        compacted_ = true;

    }

    void Graph::compactForFlatten()
    {
        if (compacted_) return;
        compact();
        preparedFlattenFreeze_ = mutationEpoch_;
    }

    bool Graph::consumePreparedFlattenFreeze() noexcept
    {
        const bool prepared = compacted_ && preparedFlattenFreeze_ == mutationEpoch_;
        preparedFlattenFreeze_.reset();
        return prepared;
    }


    void Graph::freeze()
    {
        compact();
    }

    GraphStore::Stats Graph::payloadStats() const noexcept { return store_->stats(); }


    std::span<const OperationId> Graph::operations() const { return store_->operations(); }

    std::span<const ValueId> Graph::values() const { return store_->values(); }

    std::span<const Port> Graph::inputPorts() const { return store_->inputPorts_; }

    std::span<const Port> Graph::outputPorts() const { return store_->outputPorts_; }

    std::span<const InoutPort> Graph::inoutPorts() const { return store_->inoutPorts_; }

    ValueId Graph::createValue(SymbolId symbol, int32_t width, bool isSigned, ValueType type,
                               std::span<const int64_t> shape, LogicDomain domain)
    {
        GraphStore &builder = mutableStore();
        ValueId id = builder.addValue(symbol, width, isSigned, type, shape, domain);
        return id;
    }

    ValueId Graph::createValue(int32_t width, bool isSigned, ValueType type,
                               std::span<const int64_t> shape, LogicDomain domain)
    {
        return createValue(makeInternalValSym(), width, isSigned, type, shape, domain);
    }

    ValueId Graph::createValue(SymbolId symbol, const LogicalType& type)
    {
        return createValue(symbol, type.width, type.isSigned, type.kind, type.shape, type.domain);
    }

    ValueId Graph::createValue(const LogicalType& type)
    {
        return createValue(makeInternalValSym(), type);
    }

    OperationId Graph::createOperation(OperationKind kind, SymbolId symbol)
    {
        GraphStore &builder = mutableStore();
        OperationId id = builder.addOp(kind, symbol);
        return id;
    }

    OperationId Graph::createOperation(OperationKind kind)
    {
        return createOperation(kind, makeInternalOpSym());
    }

    ValueId Graph::createTemporaryValue(SymbolId symbol, int32_t width, bool isSigned,
                                        ValueType type, std::span<const int64_t> shape,
                                        LogicDomain domain)
    {
        // The symbol overload is useful to block import callers that already
        // have a semantic symbol.  Ordinary temporary construction passes an
        // invalid symbol and therefore performs no string-table work.
        GraphStore &builder = mutableStore();
        return builder.addValue(symbol, width, isSigned, type, shape, domain, true);
    }

    ValueId Graph::createTemporaryValue(int32_t width, bool isSigned, ValueType type,
                                        std::span<const int64_t> shape, LogicDomain domain)
    {
        return createTemporaryValue(SymbolId::invalid(), width, isSigned, type, shape, domain);
    }

    ValueId Graph::createTemporaryValue(SymbolId symbol, const LogicalType& type)
    {
        return createTemporaryValue(symbol, type.width, type.isSigned, type.kind, type.shape, type.domain);
    }

    ValueId Graph::createTemporaryValue(const LogicalType& type)
    {
        return createTemporaryValue(SymbolId::invalid(), type);
    }

    OperationId Graph::createTemporaryOperation(OperationKind kind)
    {
        GraphStore &builder = mutableStore();
        return builder.addOp(kind, SymbolId::invalid(), true);
    }

    namespace {

        std::string generatedDisplayName(const Graph& graph, std::string_view prefix, uint32_t index)
        {
            std::string base;
            base.reserve(prefix.size() + 24);
            base.append(prefix);
            base.push_back('_');
            base.append(std::to_string(index));
            std::string candidate = base;
            uint32_t suffix = 0;
            while (graph.lookupSymbol(candidate).valid()) {
                candidate = base;
                candidate.push_back('_');
                candidate.append(std::to_string(++suffix));
            }
            return candidate;
        }

    }

    std::string Graph::valueDisplayName(ValueId value) const
    {
        const auto symbol = valueSymbol(value);
        if (symbol.valid()) return std::string(symbolText(symbol));
        if (value.graph != graphId_ || !value.valid()) return {};
        return generatedDisplayName(*this, "tmp_val", value.index);
    }

    std::string Graph::operationDisplayName(OperationId op) const
    {
        const auto symbol = operationSymbol(op);
        if (symbol.valid()) return std::string(symbolText(symbol));
        if (op.graph != graphId_ || !op.valid()) return {};
        return generatedDisplayName(*this, "tmp_op", op.index);
    }

    void Graph::materializeInternalSymbols()
    {
        // Materialization is deliberately a boundary operation.  Allocate all
        // names before mutating rows so a collision cannot leave a half named
        // graph behind after a caller catches an exception.
        std::vector<std::pair<ValueId, SymbolId>> values;
        std::vector<std::pair<OperationId, SymbolId>> operations;
        values.reserve(store_->values_.size());
        operations.reserve(store_->operations_.size());
        for (const auto value : this->values()) {
            if (valueSymbol(value).valid()) continue;
            const auto name = valueDisplayName(value);
            const auto symbol = internSymbol(name);
            if (!symbol.valid()) throw std::runtime_error("Failed to materialize temporary value symbol");
            values.emplace_back(value, symbol);
        }
        for (const auto op : this->operations()) {
            if (operationSymbol(op).valid()) continue;
            const auto name = operationDisplayName(op);
            const auto symbol = internSymbol(name);
            if (!symbol.valid()) throw std::runtime_error("Failed to materialize temporary operation symbol");
            operations.emplace_back(op, symbol);
        }
        for (const auto [value, symbol] : values) setValueSymbol(value, symbol);
        for (const auto [op, symbol] : operations) setOpSymbol(op, symbol);
    }

    void Graph::appendValueBlock(const Graph& source, std::span<const ValueId> sourceValues,
                                  std::span<const SymbolId> symbols, std::span<ValueId> output,
                                  SrcLocImportContext& context, std::size_t threads,
                                  bool retainSourceColumns)
    {
        if (this == &source) throw std::invalid_argument("cannot append a graph block to itself");
        const std::size_t count = sourceValues.size();
        if (symbols.size() != count || output.size() != count)
            throw std::invalid_argument("value block column lengths differ");
        auto& target = *store_;
        const auto& input = *source.store_;
        const std::size_t base = target.values_.size();
        if (count > std::numeric_limits<uint32_t>::max() - base)
            throw std::length_error("value block ID capacity");
        const std::size_t symbolCount = static_cast<std::size_t>(
            std::count_if(symbols.begin(), symbols.end(), [](SymbolId symbol) { return symbol.valid(); }));
        slang::flat_hash_set<uint32_t, std::hash<uint32_t>> bindings;
        bindings.reserve(symbolCount);
        std::vector<std::size_t> rows(count);
        for (std::size_t i = 0; i < count; ++i) {
            rows[i] = input.valueIndex(sourceValues[i]);
            if (!input.values_[rows[i]].alive) throw std::runtime_error("ValueId refers to erased value");
            if (symbols[i].valid()) target.validateSymbol(symbols[i], "Value");
            if (symbols[i].valid() &&
                (target.symbolIndex_.contains(symbols[i].value) || !bindings.insert(symbols[i].value).second))
                throw std::invalid_argument("value block symbol already bound");
        }
        if (!count) return;
        mutableStore();
        std::vector<GraphPayload::RecordId> locations(count), shapes(count);
        std::vector<GraphPayload::RecordId> shapeMap(input.topology_->shapes.ranges.size(), 0);
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t row = rows[i];
            locations[i] = importSrcLoc(source, input.values_.srcLoc_[row], context);
            const auto shape = input.values_.shape_[row];
            if (shape && !shapeMap[shape])
                shapeMap[shape] = target.topology_->shapes.intern(input.topology_->shapes.shape(shape));
            shapes[i] = shapeMap[shape];
        }
        target.reserveValues(base + count);
        target.symbolIndex_.reserve(target.symbolIndex_.size() + symbolCount);
        auto& values = target.values_;
        const std::size_t size = base + count;
        values.symbol_.resize(size);
        if (retainSourceColumns) {
            auto& sourceColumns = source.store_->values_;
            values.width_.appendSharedRowsFrom(sourceColumns.width_, rows);
            values.isSigned_.appendSharedRowsFrom(sourceColumns.isSigned_, rows);
            values.type_.appendSharedRowsFrom(sourceColumns.type_, rows);
            values.domain_.appendSharedRowsFrom(sourceColumns.domain_, rows);
        } else {
            values.width_.resize(size);
            values.isSigned_.resize(size);
            values.type_.resize(size);
            values.domain_.resize(size);
        }
        values.shape_.resize(size);
        values.isInput_.resize(size, 0);
        values.isOutput_.resize(size, 0);
        values.isInout_.resize(size, 0);
        values.definingOp_.resize(size);
        values.srcLoc_.resize(size);
        values.semantics_.resize(size);
        values.alive_.resize(size, 1);
        target.valueUsers_.resize(size);
        target.liveValues_.resize(target.liveValues_.size() + count);
        const std::size_t liveBase = target.liveValues_.size() - count;
        for (std::size_t i = 0; i < count; ++i)
            if (symbols[i].valid())
                target.bindSymbol(symbols[i], GraphStore::SymbolKind::kValue,
                                  static_cast<uint32_t>(base + i + 1), "Value");
        fillBlockRows(count, threads, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                const std::size_t from = rows[i], to = base + i;
                values.symbol_[to] = symbols[i];
                if (!retainSourceColumns) {
                    values.width_[to] = input.values_.width_[from];
                    values.isSigned_[to] = input.values_.isSigned_[from];
                    values.type_[to] = input.values_.type_[from];
                    values.domain_[to] = input.values_.domain_[from];
                }
                values.shape_[to] = shapes[i];
                values.srcLoc_[to] = locations[i];
                target.valueUsers_[to].bind(target.topology_->users);
                const ValueId id{static_cast<uint32_t>(to + 1), 0, graphId_};
                output[i] = target.liveValues_[liveBase + i] = id;
            }
        });
        if (graphProfileEnabled()) {
            profileCounters.valueAllocations.fetch_add(count, std::memory_order_relaxed);
            for (std::size_t row : rows) {
                const auto recordId = input.values_.srcLoc_[row];
                if (!recordId) continue;
                const auto& loc = input.srcLoc(recordId);
                profileCounters.srcLocWrites.fetch_add(1, std::memory_order_relaxed);
                profileCounters.srcLocBytes.fetch_add(input.string(loc.file).size() + input.string(loc.origin).size() +
                    input.string(loc.pass).size() + input.string(loc.note).size(), std::memory_order_relaxed);
            }
        }
    }

    void Graph::appendOperationBlock(const Graph& source, std::span<const OperationId> sourceOps,
                                      std::span<const SymbolId> symbols, std::span<OperationId> output,
                                      SrcLocImportContext& context, std::size_t threads,
                                      bool retainSourceColumns)
    {
        if (this == &source) throw std::invalid_argument("cannot append a graph block to itself");
        const std::size_t count = sourceOps.size();
        if (symbols.size() != count || output.size() != count)
            throw std::invalid_argument("operation block column lengths differ");
        auto& target = *store_;
        const auto& input = *source.store_;
        const std::size_t base = target.operations_.size();
        if (count > std::numeric_limits<uint32_t>::max() - base)
            throw std::length_error("operation block ID capacity");
        const std::size_t symbolCount = static_cast<std::size_t>(
            std::count_if(symbols.begin(), symbols.end(), [](SymbolId symbol) { return symbol.valid(); }));
        slang::flat_hash_set<uint32_t, std::hash<uint32_t>> bindings;
        bindings.reserve(symbolCount);
        std::vector<std::size_t> rows(count);
        for (std::size_t i = 0; i < count; ++i) {
            rows[i] = input.opIndex(sourceOps[i]);
            if (!input.operations_[rows[i]].alive) throw std::runtime_error("OperationId refers to erased operation");
            if (symbols[i].valid()) target.validateSymbol(symbols[i], "Operation");
            if (symbols[i].valid() &&
                (target.symbolIndex_.contains(symbols[i].value) || !bindings.insert(symbols[i].value).second))
                throw std::invalid_argument("operation block symbol already bound");
        }
        if (!count) return;
        mutableStore();
        std::vector<GraphPayload::RecordId> locations(count);
        for (std::size_t i = 0; i < count; ++i)
            locations[i] = importSrcLoc(source, input.operations_.srcLoc_[rows[i]], context);
        target.reserveOperations(base + count);
        target.symbolIndex_.reserve(target.symbolIndex_.size() + symbolCount);
        auto& operations = target.operations_;
        const std::size_t size = base + count;
        if (retainSourceColumns)
            operations.kind_.appendSharedRowsFrom(source.store_->operations_.kind_, rows);
        else operations.kind_.resize(size);
        operations.symbol_.resize(size);
        operations.operands_.resize(size);
        operations.operandUseIndices_.resize(size);
        operations.results_.resize(size);
        operations.attrs_.resize(size);
        operations.srcLoc_.resize(size);
        operations.semantics_.resize(size);
        operations.alive_.resize(size, 1);
        target.liveOperations_.resize(target.liveOperations_.size() + count);
        const std::size_t liveBase = target.liveOperations_.size() - count;
        for (std::size_t i = 0; i < count; ++i)
            if (symbols[i].valid())
                target.bindSymbol(symbols[i], GraphStore::SymbolKind::kOperation,
                                  static_cast<uint32_t>(base + i + 1), "Operation");
        fillBlockRows(count, threads, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                const std::size_t to = base + i;
                if (!retainSourceColumns) operations.kind_[to] = input.operations_.kind_[rows[i]];
                operations.symbol_[to] = symbols[i];
                operations.srcLoc_[to] = locations[i];
                operations.operands_[to].bind(target.topology_->operands);
                operations.operandUseIndices_[to].bind(target.topology_->operandUseIndices);
                operations.results_[to].bind(target.topology_->results);
                operations.attrs_[to].bind(target.topology_->attrs);
                const OperationId id{static_cast<uint32_t>(to + 1), 0, graphId_};
                output[i] = target.liveOperations_[liveBase + i] = id;
            }
        });
        if (graphProfileEnabled()) {
            profileCounters.operationAllocations.fetch_add(count, std::memory_order_relaxed);
            for (std::size_t row : rows) {
                const auto recordId = input.operations_.srcLoc_[row];
                if (!recordId) continue;
                const auto& loc = input.srcLoc(recordId);
                profileCounters.srcLocWrites.fetch_add(1, std::memory_order_relaxed);
                profileCounters.srcLocBytes.fetch_add(input.string(loc.file).size() + input.string(loc.origin).size() +
                    input.string(loc.pass).size() + input.string(loc.note).size(), std::memory_order_relaxed);
            }
        }
    }

    void Graph::setOperationBlockEdges(std::span<const OperationId> ops,
                                        std::span<const uint32_t> operandOffsets,
                                        std::span<const ValueId> operands,
                                        std::span<const uint32_t> resultOffsets,
                                        std::span<const ValueId> results)
    {
        const auto validateOffsets = [&](auto offsets, std::size_t count) {
            if (offsets.size() != ops.size() + 1 || offsets.front() != 0 ||
                offsets.back() != count || !std::is_sorted(offsets.begin(), offsets.end()))
                throw std::invalid_argument("invalid operation block edge offsets");
        };
        validateOffsets(operandOffsets, operands.size());
        validateOffsets(resultOffsets, results.size());
        auto& target = *store_;
        slang::flat_hash_set<uint32_t, std::hash<uint32_t>> definitions;
        slang::flat_hash_set<uint32_t, std::hash<uint32_t>> operations;
        for (OperationId op : ops) {
            const auto row = target.operations_[target.opIndex(op)];
            if (!row.alive) throw std::runtime_error("OperationId refers to erased operation");
            if (!operations.insert(op.index).second)
                throw std::invalid_argument("duplicate operation in block edges");
            if (!row.operands.empty() || !row.results.empty())
                throw std::invalid_argument("operation block edges already set");
        }
        for (ValueId value : operands)
            if (!target.values_[target.valueIndex(value)].alive)
                throw std::runtime_error("ValueId refers to erased value");
        for (ValueId value : results) {
            const auto row = target.values_[target.valueIndex(value)];
            if (!row.alive) throw std::runtime_error("ValueId refers to erased value");
            if (row.definingOp.valid() || !definitions.insert(value.index).second)
                throw std::invalid_argument("operation block result already has defining op");
        }
        if (ops.empty()) return;
        mutableStore();
        auto& pools = *target.topology_;
        std::vector<ValueId> savedOperands, savedResults;
        const auto borrows = [](std::span<const ValueId> input, const std::vector<ValueId>& pool) {
            const std::less<const ValueId*> before;
            return !input.empty() && !pool.empty() && !before(input.data(), pool.data()) &&
                before(input.data(), pool.data() + pool.size());
        };
        // Public callers may copy existing edges in this target. Snapshot only
        // that alias case before range-insert reallocates the underlying pool.
        if (borrows(operands, pools.operands) || borrows(operands, pools.results)) {
            savedOperands.assign(operands.begin(), operands.end());
            operands = savedOperands;
        }
        if (borrows(results, pools.operands) || borrows(results, pools.results)) {
            savedResults.assign(results.begin(), results.end());
            results = savedResults;
        }
        const std::size_t operandBase = pools.operands.size(), resultBase = pools.results.size();
        pools.operands.insert(pools.operands.end(), operands.begin(), operands.end());
        pools.results.insert(pools.results.end(), results.begin(), results.end());
        for (std::size_t i = 0; i < ops.size(); ++i) {
            auto row = target.operations_[ops[i].index - 1];
            row.operands.bindSlice(pools.operands, operandBase + operandOffsets[i],
                                   operandOffsets[i + 1] - operandOffsets[i]);
            row.results.bindSlice(pools.results, resultBase + resultOffsets[i],
                                  resultOffsets[i + 1] - resultOffsets[i]);
            for (ValueId result : row.results) {
                const auto index = result.index - 1;
                target.values_.definingOp_[index] = ops[i];
                // addResult inherits an operation's provenance when the value
                // has none. Bulk definition publication preserves that rule.
                if (!target.values_.srcLoc_[index] && row.srcLoc)
                    target.values_.srcLoc_[index] = row.srcLoc;
            }
        }
    }

    void Graph::setOperationBlockResults(std::span<const OperationId> ops,
                                          std::span<const uint32_t> resultOffsets,
                                          std::span<const ValueId> results)
    {
        const std::vector<uint32_t> noOperands(ops.size() + 1, 0);
        setOperationBlockEdges(ops, noOperands, {}, resultOffsets, results);
    }

    void Graph::setOperationBlockOperands(std::span<const OperationId> ops,
                                           std::span<const uint32_t> operandOffsets,
                                           std::span<const ValueId> operands)
    {
        if (operandOffsets.size() != ops.size() + 1 || operandOffsets.front() != 0 ||
            operandOffsets.back() != operands.size() ||
            !std::is_sorted(operandOffsets.begin(), operandOffsets.end()))
            throw std::invalid_argument("invalid operation block edge offsets");
        auto& target = *store_;
        slang::flat_hash_set<uint32_t, std::hash<uint32_t>> seen;
        for (OperationId op : ops) {
            const auto row = target.operations_[target.opIndex(op)];
            if (!row.alive) throw std::runtime_error("OperationId refers to erased operation");
            if (!seen.insert(op.index).second) throw std::invalid_argument("duplicate operation in block edges");
            if (!row.operands.empty()) throw std::invalid_argument("operation block operands already set");
        }
        for (ValueId value : operands)
            if (!target.values_[target.valueIndex(value)].alive)
                throw std::runtime_error("ValueId refers to erased value");
        if (ops.empty()) return;
        auto& pool = target.topology_->operands;
        std::vector<ValueId> saved;
        const std::less<const ValueId*> before;
        if (!operands.empty() && !pool.empty() && !before(operands.data(), pool.data()) &&
            before(operands.data(), pool.data() + pool.size())) {
            saved.assign(operands.begin(), operands.end());
            operands = saved;
        }
        mutableStore();
        const std::size_t base = pool.size();
        pool.insert(pool.end(), operands.begin(), operands.end());
        for (std::size_t i = 0; i < ops.size(); ++i)
            target.operations_[ops[i].index - 1].operands.bindSlice(pool, base + operandOffsets[i],
                operandOffsets[i + 1] - operandOffsets[i]);
    }

    void Graph::consumeBlockTopology(Graph& source, std::span<const TopologyBlock> blocks,
                                      std::span<const ValueId> mappedValues)
    {
        if (&source == this || source.owner_->findGraph(source.symbol_) != &source)
            throw std::invalid_argument("topology source must be a distinct published graph");
        auto& input = *source.store_;
        auto& target = *store_;
        auto& fromPools = *input.topology_;
        auto& toPools = *target.topology_;
        const auto ownedPool = [&](const std::vector<ValueId>* pool) {
            if (pool == &fromPools.operands || pool == &fromPools.results) return true;
            return std::any_of(fromPools.retainedEdges.begin(), fromPools.retainedEdges.end(),
                [&](const auto& owned) { return owned.get() == pool; });
        };
        slang::flat_hash_set<uint32_t, std::hash<uint32_t>> sources, destinations;
        for (const auto& block : blocks) {
            if (block.sourceOps.size() != block.targetOps.size())
                throw std::invalid_argument("topology block column lengths differ");
            for (std::size_t i = 0; i < block.sourceOps.size(); ++i) {
                const auto from = input.operations_[input.opIndex(block.sourceOps[i])];
                const auto to = target.operations_[target.opIndex(block.targetOps[i])];
                if (!from.alive || !to.alive)
                    throw std::runtime_error("OperationId refers to erased operation");
                if (!sources.insert(block.sourceOps[i].index).second ||
                    !destinations.insert(block.targetOps[i].index).second)
                    throw std::invalid_argument("duplicate operation in topology blocks");
                if (!to.operands.empty() || to.results.size() != from.results.size())
                    throw std::invalid_argument("topology target requires empty operands and published results");
                if (!ownedPool(from.operands.backingPool()) || !ownedPool(from.results.backingPool()))
                    throw std::invalid_argument("topology slice has no source owner");
                for (ValueId value : from.operands) {
                    if (!input.values_[input.valueIndex(value)].alive || value.index >= mappedValues.size())
                        throw std::invalid_argument("invalid topology source value");
                    const auto mapped = mappedValues[value.index];
                    if (!target.values_[target.valueIndex(mapped)].alive)
                        throw std::runtime_error("ValueId refers to erased value");
                }
            }
        }
        // Allocate everything that can fail before consuming the source. The
        // vector objects stay at stable addresses for their PoolVector slices.
        const std::string sourceName = source.symbol_;
        Design* sourceOwner = source.owner_;
        auto operands = std::make_unique<std::vector<ValueId>>();
        auto results = std::make_unique<std::vector<ValueId>>();
        toPools.retainedEdges.reserve(toPools.retainedEdges.size() + fromPools.retainedEdges.size() + 2);
        mutableStore();
        *operands = std::move(fromPools.operands);
        *results = std::move(fromPools.results);
        const auto relocatePool = [&](const std::vector<ValueId>* pool) -> std::vector<ValueId>& {
            if (pool == &fromPools.operands) return *operands;
            if (pool == &fromPools.results) return *results;
            return *const_cast<std::vector<ValueId>*>(pool);
        };
        for (const auto& block : blocks) {
            for (std::size_t i = 0; i < block.sourceOps.size(); ++i) {
                const auto fromIndex = block.sourceOps[i].index - 1;
                const auto toIndex = block.targetOps[i].index - 1;
                const auto& fromOperands = input.operations_.operands_[fromIndex];
                const auto& fromResults = input.operations_.results_[fromIndex];
                auto& toOperands = target.operations_.operands_[toIndex];
                auto& toResults = target.operations_.results_[toIndex];
                auto& operandPool = relocatePool(fromOperands.backingPool());
                auto& resultPool = relocatePool(fromResults.backingPool());
                for (std::size_t j = 0; j < fromOperands.size(); ++j) {
                    auto& value = operandPool[fromOperands.offset() + j];
                    value = mappedValues[value.index];
                }
                // Definitions were published before union-find resolved port
                // aliases. Preserve those result IDs; only uses follow roots.
                std::copy(toResults.begin(), toResults.end(), resultPool.begin() + fromResults.offset());
                toOperands.bindSlice(operandPool, fromOperands.offset(), fromOperands.size());
                toResults.bindSlice(resultPool, fromResults.offset(), fromResults.size());
            }
        }
        toPools.retainedEdges.push_back(std::move(operands));
        toPools.retainedEdges.push_back(std::move(results));
        for (auto& pool : fromPools.retainedEdges) toPools.retainedEdges.push_back(std::move(pool));
        sourceOwner->deleteGraph(sourceName);
    }

    void Graph::rebuildUseLists()
    {
        auto& target = *store_;
        std::vector<std::size_t> counts(target.values_.size(), 0);
        std::size_t edges = 0;
        for (OperationId op : target.operations()) {
            for (ValueId value : target.operations_[op.index - 1].operands) {
                const auto row = target.valueIndex(value);
                if (!target.values_.alive_[row]) throw std::runtime_error("ValueId refers to erased value");
                ++counts[row];
                ++edges;
            }
        }
        auto& pools = *target.topology_;
        pools.users.resize(edges);
        pools.operandUseIndices.resize(edges);
        mutableStore();
        std::size_t offset = 0;
        for (std::size_t i = 0; i < counts.size(); ++i) {
            target.valueUsers_[i].bindSlice(pools.users, offset, counts[i]);
            offset += counts[i];
            counts[i] = 0;
        }
        offset = 0;
        for (OperationId op : target.operations()) {
            auto row = target.operations_[op.index - 1];
            row.operandUseIndices.bindSlice(pools.operandUseIndices, offset, row.operands.size());
            offset += row.operands.size();
            for (std::size_t i = 0; i < row.operands.size(); ++i) {
                const std::size_t value = row.operands[i].index - 1;
                const std::size_t local = counts[value]++;
                row.operandUseIndices[i] = local;
                target.valueUsers_[value][local] = ValueUser{op, static_cast<uint32_t>(i)};
            }
        }
    }

    void Graph::prepareBlockImport(const Graph& source, BlockImportContext& context,
                                    SrcLocImportContext& locations)
    {
        if (this == &source) throw std::invalid_argument("cannot import a graph block from itself");
        const bool changed = !context.prepared_ ||
            context.source_.owner_before(source.store_) || source.store_.owner_before(context.source_) ||
            context.target_.owner_before(store_) || store_.owner_before(context.target_) ||
            context.sourceEpoch_ != source.srcLocEpoch_ || context.targetEpoch_ != srcLocEpoch_ ||
            context.sourceMutation_ != source.mutationEpoch_;
        if (!changed) return;
        context.prepared_ = false;
        auto& target = mutableStore();
        const auto& input = *source.store_;
        context.strings_ = std::make_shared<std::vector<GraphPayload::StringId>>(
            input.GraphPayload::stats().strings + 1, 0);
        const auto remapString = [&](GraphPayload::StringId id) {
            if (!id || (id & uint32_t{0x80000000})) return id;
            auto& mapped = (*context.strings_)[id];
            if (!mapped) mapped = target.stringPool_->intern(input.string(id));
            return mapped;
        };
        const auto capacity = [](std::size_t base, std::size_t count) {
            if (base > std::numeric_limits<uint32_t>::max() ||
                count > std::numeric_limits<uint32_t>::max() - base)
                throw std::length_error("block payload pool capacity");
        };
        capacity(target.int64Payload_.size(), input.int64Payload_.size());
        capacity(target.doublePayload_.size(), input.doublePayload_.size());
        capacity(target.bitPayload_.size(), input.bitPayload_.size());
        capacity(target.stringListPayload_.size(), input.stringListPayload_.size());
        const auto ints = target.int64Payload_.share(input.int64Payload_);
        const auto doubles = target.doublePayload_.share(input.doublePayload_);
        const auto bits = target.bitPayload_.share(input.bitPayload_);
        const auto strings = target.stringListPayload_.size();
        input.stringListPayload_.forEachSegment([&](std::size_t, std::span<const uint32_t> ids) {
            target.stringListPayload_.appendGenerated(ids.size(), [&](std::size_t i) { return remapString(ids[i]); });
        });
        context.attributes_.resize(input.attrKeys_.size());
        for (std::size_t i = 1; i < context.attributes_.size(); ++i) {
            auto record = input.attribute(static_cast<GraphPayload::RecordId>(i));
            record.key = remapString(record.key);
            std::size_t base = 0;
            switch (record.kind) {
            case AttrValueKind::String: record.payload = remapString(static_cast<uint32_t>(record.payload)); break;
            case AttrValueKind::BoolList: base = bits; break;
            case AttrValueKind::IntList: base = ints; break;
            case AttrValueKind::DoubleList: base = doubles; break;
            case AttrValueKind::StringList: base = strings; break;
            default: break;
            }
            if (record.kind == AttrValueKind::BoolList || record.kind == AttrValueKind::IntList ||
                record.kind == AttrValueKind::DoubleList || record.kind == AttrValueKind::StringList) {
                record.slice.offset += static_cast<uint32_t>(base);
                record.payload = record.slice.offset | (static_cast<uint64_t>(record.slice.count) << 32);
            }
            context.attributes_[i] = record;
        }
        context.source_ = source.store_;
        context.target_ = store_;
        context.sourceEpoch_ = source.srcLocEpoch_;
        context.targetEpoch_ = srcLocEpoch_;
        context.sourceMutation_ = source.mutationEpoch_;
        context.prepared_ = true;

        locations.records_.resize(input.srcLocs_.size());
        for (GraphPayload::RecordId id = 1; id < input.srcLocs_.size(); ++id) {
            auto record = input.srcLoc(id);
            record.file = remapString(record.file);
            record.origin = remapString(record.origin);
            record.pass = remapString(record.pass);
            record.note = remapString(record.note);
            if (const auto found = target.srcLocIds_.find(record); found != target.srcLocIds_.end()) {
                locations.records_[id] = found->second;
                continue;
            }
            const auto mapped = static_cast<GraphPayload::RecordId>(target.srcLocs_.size());
            target.srcLocs_.push_back(record);
            target.srcLocIds_.emplace(record, mapped);
            locations.records_[id] = mapped;
        }
        locations.source_ = source.store_;
        locations.target_ = store_;
        locations.sourceEpoch_ = source.srcLocEpoch_;
        locations.targetEpoch_ = srcLocEpoch_;
        locations.strings_.clear();
        locations.blockStrings_ = context.strings_;
    }

    void Graph::appendOperationBlockAttributes(const Graph& source,
                                               std::span<const OperationId> sourceOps,
                                               std::span<const OperationId> targetOps,
                                               BlockImportContext& context)
    {
        if (sourceOps.size() != targetOps.size())
            throw std::invalid_argument("attribute block column lengths differ");
        if (!context.prepared_ || context.source_.owner_before(source.store_) ||
            source.store_.owner_before(context.source_) || context.target_.owner_before(store_) ||
            store_.owner_before(context.target_) || context.sourceEpoch_ != source.srcLocEpoch_ ||
            context.targetEpoch_ != srcLocEpoch_ || context.sourceMutation_ != source.mutationEpoch_)
            throw std::invalid_argument("attribute block import context is stale");
        auto& target = *store_;
        const auto& input = *source.store_;
        std::size_t count = 0;
        slang::flat_hash_set<uint32_t, std::hash<uint32_t>> destinations;
        for (std::size_t i = 0; i < sourceOps.size(); ++i) {
            const auto from = input.operations_[input.opIndex(sourceOps[i])];
            const auto to = target.operations_[target.opIndex(targetOps[i])];
            if (!from.alive || !to.alive) throw std::runtime_error("OperationId refers to erased operation");
            if (!destinations.insert(targetOps[i].index).second)
                throw std::invalid_argument("duplicate operation in attribute block");
            if (!to.attrs.empty()) throw std::invalid_argument("operation block attributes already set");
            count += from.attrs.size();
        }
        if (count > std::numeric_limits<uint32_t>::max() - target.attrKeys_.size())
            throw std::length_error("attribute block record capacity");
        if (!count) return;
        mutableStore();
        const auto base = target.attrKeys_.size();
        target.attrKeys_.resize(base + count);
        target.attrTags_.resize(base + count);
        target.attrPayloads_.resize(base + count);
        auto& ids = target.topology_->attrs;
        const auto sliceBase = ids.size();
        ids.resize(sliceBase + count);
        std::size_t position = 0;
        for (std::size_t i = 0; i < sourceOps.size(); ++i) {
            const auto from = input.operations_[sourceOps[i].index - 1];
            auto to = target.operations_[targetOps[i].index - 1];
            to.attrs.bindSlice(ids, sliceBase + position, from.attrs.size());
            for (auto id : from.attrs) {
                const auto& record = context.attributes_.at(id);
                target.attrKeys_[base + position] = record.key;
                target.attrTags_[base + position] = record.kind;
                target.attrPayloads_[base + position] = record.payload;
                ids[sliceBase + position] = static_cast<uint32_t>(base + position);
                ++position;
            }
        }
        if (graphProfileEnabled()) {
            profileCounters.attributeWrites.fetch_add(count, std::memory_order_relaxed);
            uint64_t bytes = 0;
            for (OperationId op : sourceOps) for (auto attr : source.opAttrRange(op)) {
                bytes += attr.key().size();
                const auto value = attr.value();
                switch (value.kind()) {
                case AttrValueKind::Bool: bytes += sizeof(bool); break;
                case AttrValueKind::Int: bytes += sizeof(int64_t); break;
                case AttrValueKind::Double: bytes += sizeof(double); break;
                case AttrValueKind::String: bytes += value.asString().size(); break;
                case AttrValueKind::BoolList: bytes += value.asBoolList().size(); break;
                case AttrValueKind::IntList: bytes += value.asIntList().size_bytes(); break;
                case AttrValueKind::DoubleList: bytes += value.asDoubleList().size_bytes(); break;
                case AttrValueKind::StringList: for (auto text : value.asStringList()) bytes += text.size(); break;
                }
            }
            profileCounters.attributeBytes.fetch_add(bytes, std::memory_order_relaxed);
        }
    }

    void Graph::appendOperationBlocksAttributes(std::span<const OperationBlockImport> blocks,
                                                detail::BlockExecutor& executor)
    {
        struct Range {
            const OperationBlockImport* block;
            std::size_t recordBase;
            std::size_t sliceBase;
            std::size_t count;
        };
        std::vector<Range> ranges;
        ranges.reserve(blocks.size());
        auto& target = *store_;
        std::size_t records = 0;
        slang::flat_hash_set<uint32_t, std::hash<uint32_t>> destinations;
        for (const auto& block : blocks) {
            if (!block.source || !block.context || block.source == this ||
                block.sourceOps.size() != block.targetOps.size())
                throw std::invalid_argument("invalid operation attribute block");
            const auto& source = *block.source;
            const auto& context = *block.context;
            if (!context.prepared_ || context.source_.owner_before(source.store_) ||
                source.store_.owner_before(context.source_) || context.target_.owner_before(store_) ||
                store_.owner_before(context.target_) || context.sourceEpoch_ != source.srcLocEpoch_ ||
                context.targetEpoch_ != srcLocEpoch_ || context.sourceMutation_ != source.mutationEpoch_)
                throw std::invalid_argument("attribute block import context is stale");
            std::size_t count = 0;
            for (std::size_t i = 0; i < block.sourceOps.size(); ++i) {
                const auto from = source.store_->operations_[source.store_->opIndex(block.sourceOps[i])];
                const auto to = target.operations_[target.opIndex(block.targetOps[i])];
                if (!from.alive || !to.alive) throw std::runtime_error("OperationId refers to erased operation");
                if (!to.attrs.empty()) throw std::invalid_argument("operation block attributes already set");
                if (!destinations.insert(block.targetOps[i].index).second)
                    throw std::invalid_argument("duplicate operation in attribute blocks");
                count += from.attrs.size();
            }
            ranges.push_back({&block, target.attrKeys_.size() + records,
                               target.topology_->attrs.size() + records, count});
            records += count;
        }
        if (records > std::numeric_limits<uint32_t>::max() - target.attrKeys_.size())
            throw std::length_error("attribute block record capacity");
        if (!records) return;
        mutableStore();
        target.attrKeys_.resize(target.attrKeys_.size() + records);
        target.attrTags_.resize(target.attrTags_.size() + records);
        target.attrPayloads_.resize(target.attrPayloads_.size() + records);
        target.topology_->attrs.resize(target.topology_->attrs.size() + records);
        for (const auto& range : ranges) {
            executor.submit([&, range] {
                const auto& block = *range.block;
                const auto& input = *block.source->store_;
                const auto& prototypes = block.context->attributes_;
                std::size_t position = 0;
                auto& ids = target.topology_->attrs;
                for (std::size_t i = 0; i < block.sourceOps.size(); ++i) {
                    const auto from = input.operations_[block.sourceOps[i].index - 1];
                    auto to = target.operations_[block.targetOps[i].index - 1];
                    to.attrs.bindSlice(ids, range.sliceBase + position, from.attrs.size());
                    for (auto id : from.attrs) {
                        const auto& record = prototypes.at(id);
                        const auto destination = range.recordBase + position;
                        target.attrKeys_[destination] = record.key;
                        target.attrTags_[destination] = record.kind;
                        target.attrPayloads_[destination] = record.payload;
                        ids[range.sliceBase + position] = static_cast<uint32_t>(destination);
                        ++position;
                    }
                }
                if (graphProfileEnabled()) {
                    profileCounters.attributeWrites.fetch_add(range.count, std::memory_order_relaxed);
                    uint64_t bytes = 0;
                    for (OperationId op : block.sourceOps) for (auto attr : block.source->opAttrRange(op)) {
                        bytes += attr.key().size();
                        const auto value = attr.value();
                        switch (value.kind()) {
                        case AttrValueKind::Bool: bytes += sizeof(bool); break;
                        case AttrValueKind::Int: bytes += sizeof(int64_t); break;
                        case AttrValueKind::Double: bytes += sizeof(double); break;
                        case AttrValueKind::String: bytes += value.asString().size(); break;
                        case AttrValueKind::BoolList: bytes += value.asBoolList().size(); break;
                        case AttrValueKind::IntList: bytes += value.asIntList().size_bytes(); break;
                        case AttrValueKind::DoubleList: bytes += value.asDoubleList().size_bytes(); break;
                        case AttrValueKind::StringList: for (auto text : value.asStringList()) bytes += text.size(); break;
                        }
                    }
                    profileCounters.attributeBytes.fetch_add(bytes, std::memory_order_relaxed);
                }
            });
        }
        executor.wait();
    }

    void Graph::setOperationBlockAttr(std::span<const OperationId> ops, std::string_view key,
                                       AttrValueView value)
    {
        if (!value.valid()) throw std::invalid_argument("invalid attribute value view");
        if ((value.kind() == AttrValueKind::Double && !std::isfinite(value.asDouble())) ||
            (value.kind() == AttrValueKind::DoubleList &&
             std::any_of(value.asDoubleList().begin(), value.asDoubleList().end(),
                         [](double item) { return !std::isfinite(item); })))
            throw std::runtime_error("Attribute value must be JSON-serializable");
        for (OperationId op : ops)
            if (!store_->operations_[store_->opIndex(op)].alive)
                throw std::runtime_error("OperationId refers to erased operation");
        if (ops.empty()) return;
        auto& target = mutableStore();
        // Use one prototype payload, then duplicate only its fixed record.
        // Records must be private because replacing an attr edits it in place.
        const auto prototypeId = target.internAttribute(key, value);
        const auto prototype = target.attribute(prototypeId);
        uint64_t bytes = key.size();
        if (graphProfileEnabled()) {
            switch (value.kind()) {
            case AttrValueKind::Bool: bytes += sizeof(bool); break;
            case AttrValueKind::Int: bytes += sizeof(int64_t); break;
            case AttrValueKind::Double: bytes += sizeof(double); break;
            case AttrValueKind::String: bytes += value.asString().size(); break;
            case AttrValueKind::BoolList: bytes += value.asBoolList().size(); break;
            case AttrValueKind::IntList: bytes += value.asIntList().size_bytes(); break;
            case AttrValueKind::DoubleList: bytes += value.asDoubleList().size_bytes(); break;
            case AttrValueKind::StringList: for (auto text : value.asStringList()) bytes += text.size(); break;
            }
        }
        for (std::size_t i = 0; i < ops.size(); ++i) {
            GraphPayload::RecordId id = prototypeId;
            if (i) {
                id = static_cast<GraphPayload::RecordId>(target.attrKeys_.size());
                target.attrKeys_.push_back(prototype.key);
                target.attrTags_.push_back(prototype.kind);
                target.attrPayloads_.push_back(prototype.payload);
            }
            target.setAttrRecord(ops[i].index - 1, key, id);
        }
        if (graphProfileEnabled()) {
            profileCounters.attributeWrites.fetch_add(ops.size(), std::memory_order_relaxed);
            profileCounters.attributeBytes.fetch_add(bytes * ops.size(), std::memory_order_relaxed);
        }
    }

    ValueId Graph::createArray(std::span<const ValueId> elements)
    {
        if (elements.empty())
            throw std::invalid_argument("kArrayCreate requires at least one element");
        const Value first = getValue(elements.front());
        if (first.type() != ValueType::Logic || first.width() <= 0)
            throw std::invalid_argument("kArrayCreate elements must be positive-width Logic values");
        if (elements.size() > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) /
                                  static_cast<uint32_t>(first.width()))
            throw std::overflow_error("kArrayCreate total width exceeds int32 capacity");
        std::vector<int64_t> shape{static_cast<int64_t>(elements.size())};
        shape.insert(shape.end(), first.shape().begin(), first.shape().end());
        for (const ValueId element : elements)
        {
            const Value value = getValue(element);
            if (value.logicalType() != first.logicalType()) {
                const auto describe = [&](const Value& item) {
                    std::string text = valueDisplayName(item.id()) + ":" + std::string(toString(item.type())) +
                        "/" + std::to_string(item.width()) + "/" + (item.isSigned() ? "signed/" : "unsigned/") + std::string(toString(item.domain()));
                    for (const auto extent : item.shape()) text += "[" + std::to_string(extent) + "]";
                    return text;
                };
                throw std::invalid_argument("kArrayCreate elements must have identical full types (" + describe(first) + " versus " + describe(value) + ")");
            }
        }
        const std::vector<ValueId> inputs(elements.begin(), elements.end());
        const ValueId result = createValue(static_cast<int32_t>(elements.size() * first.width()),
                                           first.isSigned(), ValueType::Logic, shape, first.domain());
        const OperationId op = createOperation(OperationKind::kArrayCreate);
        for (const ValueId element : inputs) addOperand(op, element);
        addResult(op, result);
        return result;
    }

    ValueId Graph::findValue(SymbolId symbol) const noexcept
    {
        if (!symbol.valid()) return ValueId::invalid();
        const auto it = store_->symbolIndex_.find(symbol.value);
        if (it == store_->symbolIndex_.end() || it->second.kind != GraphStore::SymbolKind::kValue)
            return ValueId::invalid();
        return ValueId{it->second.index, 0, graphId_};
    }


    OperationId Graph::findOperation(SymbolId symbol) const noexcept
    {
        if (!symbol.valid()) return OperationId::invalid();
        const auto it = store_->symbolIndex_.find(symbol.value);
        if (it == store_->symbolIndex_.end() || it->second.kind != GraphStore::SymbolKind::kOperation)
            return OperationId::invalid();
        return OperationId{it->second.index, 0, graphId_};
    }


    ValueId Graph::findValue(std::string_view symbol) const
    {
        return findValue(lookupSymbol(symbol));
    }

    OperationId Graph::findOperation(std::string_view symbol) const
    {
        return findOperation(lookupSymbol(symbol));
    }

    SymbolId Graph::valueSymbol(ValueId id) const noexcept
    {
        if (id.graph != graphId_ || !id.index || id.index > store_->values_.size()) return SymbolId::invalid();
        const auto data = store_->values_[id.index - 1];
        return data.alive ? data.symbol : SymbolId::invalid();
    }


    SymbolId Graph::operationSymbol(OperationId id) const noexcept
    {
        if (id.graph != graphId_ || !id.index || id.index > store_->operations_.size()) return SymbolId::invalid();
        const auto data = store_->operations_[id.index - 1];
        return data.alive ? data.symbol : SymbolId::invalid();
    }


    int32_t Graph::valueWidth(ValueId id) const
    {
        const auto data = store_->values_.at(store_->valueIndex(id));
        if (!data.alive) throw std::runtime_error("ValueId refers to erased value");
        return data.width;
    }


    bool Graph::valueSigned(ValueId id) const
    {
        const auto data = store_->values_.at(store_->valueIndex(id));
        if (!data.alive) throw std::runtime_error("ValueId refers to erased value");
        return data.isSigned;
    }


    ValueType Graph::valueType(ValueId id) const
    {
        const auto data = store_->values_.at(store_->valueIndex(id));
        if (!data.alive) throw std::runtime_error("ValueId refers to erased value");
        return data.type;
    }


    std::span<const int64_t> Graph::valueShape(ValueId id) const
    {
        const auto data = store_->values_.at(store_->valueIndex(id));
        if (!data.alive) throw std::runtime_error("ValueId refers to erased value");
        return data.shape;
    }


    bool Graph::sameValueType(ValueId lhs, ValueId rhs) const
    {
        return valueWidth(lhs) == valueWidth(rhs) &&
               valueSigned(lhs) == valueSigned(rhs) &&
               valueType(lhs) == valueType(rhs) &&
               valueDomain(lhs) == valueDomain(rhs) &&
               std::ranges::equal(valueShape(lhs), valueShape(rhs));
    }

    bool Graph::valueIsInput(ValueId id) const
    {
        const auto data = store_->values_.at(store_->valueIndex(id));
        if (!data.alive) throw std::runtime_error("ValueId refers to erased value");
        return data.isInput;
    }


    bool Graph::valueIsOutput(ValueId id) const
    {
        const auto data = store_->values_.at(store_->valueIndex(id));
        if (!data.alive) throw std::runtime_error("ValueId refers to erased value");
        return data.isOutput;
    }


    bool Graph::valueIsInout(ValueId id) const
    {
        const auto data = store_->values_.at(store_->valueIndex(id));
        if (!data.alive) throw std::runtime_error("ValueId refers to erased value");
        return data.isInout;
    }


    OperationId Graph::valueDef(ValueId id) const
    {
        const auto data = store_->values_.at(store_->valueIndex(id));
        if (!data.alive) throw std::runtime_error("ValueId refers to erased value");
        return data.definingOp;
    }


    std::optional<SrcLoc> Graph::valueSrcLoc(ValueId id) const
    {
        const auto data = store_->values_.at(store_->valueIndex(id));
        if (!data.alive) throw std::runtime_error("ValueId refers to erased value");
        return store_->srcLocValue(data.srcLoc);
    }


    OperationKind Graph::opKind(OperationId id) const
    {
        const auto data = store_->operations_.at(store_->opIndex(id));
        if (!data.alive) throw std::runtime_error("OperationId refers to erased operation");
        return data.kind;
    }


    std::span<const ValueId> Graph::opOperands(OperationId id) const
    {
        const auto data = store_->operations_.at(store_->opIndex(id));
        if (!data.alive) throw std::runtime_error("OperationId refers to erased operation");
        return data.operands;
    }


    std::span<const ValueId> Graph::opResults(OperationId id) const
    {
        const auto data = store_->operations_.at(store_->opIndex(id));
        if (!data.alive) throw std::runtime_error("OperationId refers to erased operation");
        return data.results;
    }


    AttrRange Graph::opAttrs(OperationId id) const { return opAttrRange(id); }

    AttrRange Graph::opAttrRange(OperationId id) const
    {
        const auto data = store_->operations_.at(store_->opIndex(id));
        if (!data.alive) throw std::runtime_error("OperationId refers to erased operation");
        return AttrRange(store_.get(), data.attrs);
    }

    std::optional<SrcLoc> Graph::opSrcLoc(OperationId id) const
    {
        const auto data = store_->operations_.at(store_->opIndex(id));
        if (!data.alive) throw std::runtime_error("OperationId refers to erased operation");
        return store_->srcLocValue(data.srcLoc);
    }


    Value Graph::getValue(ValueId id) const { return valueFromStore(id); }


    std::span<const ValueUser> Graph::valueUsers(ValueId value) const
    {
        (void)valueDef(value);
        return valueUsersSpan(value);
    }


    Operation Graph::getOperation(OperationId id) const { return operationFromStore(id); }


    void Graph::bindInputPort(std::string_view name, ValueId value)
    {
        GraphStore &builder = mutableStore();
        builder.bindInputPort(name, value);
    }

    void Graph::bindOutputPort(std::string_view name, ValueId value)
    {
        GraphStore &builder = mutableStore();
        builder.bindOutputPort(name, value);
    }

    void Graph::bindInoutPort(std::string_view name, ValueId in, ValueId out, ValueId oe)
    {
        GraphStore &builder = mutableStore();
        builder.bindInoutPort(name, in, out, oe);
    }

    void Graph::bindInputPorts(std::span<const Port> ports)
    {
        GraphStore &builder = mutableStore();
        builder.bindInputPorts(ports);
    }

    void Graph::bindOutputPorts(std::span<const Port> ports)
    {
        GraphStore &builder = mutableStore();
        builder.bindOutputPorts(ports);
    }

    void Graph::bindInoutPorts(std::span<const InoutPort> ports)
    {
        GraphStore &builder = mutableStore();
        builder.bindInoutPorts(ports);
    }

    bool Graph::removeInputPort(std::string_view name)
    {
        GraphStore &builder = mutableStore();
        bool removed = builder.removeInputPort(name);
        return removed;
    }

    bool Graph::removeOutputPort(std::string_view name)
    {
        GraphStore &builder = mutableStore();
        bool removed = builder.removeOutputPort(name);
        return removed;
    }

    bool Graph::removeInoutPort(std::string_view name)
    {
        GraphStore &builder = mutableStore();
        bool removed = builder.removeInoutPort(name);
        return removed;
    }

    ValueId Graph::inputPortValue(std::string_view name) const noexcept
    {
        return findPortValue(std::span<const Port>(store_->inputPorts_), name);
    }


    ValueId Graph::outputPortValue(std::string_view name) const noexcept
    {
        return findPortValue(std::span<const Port>(store_->outputPorts_), name);
    }


    void Graph::addOperand(OperationId op, ValueId value)
    {
        GraphStore &builder = mutableStore();
        builder.addOperand(op, value);
    }

    void Graph::addResult(OperationId op, ValueId value)
    {
        GraphStore &builder = mutableStore();
        builder.addResult(op, value);
    }

    void Graph::insertOperand(OperationId op, std::size_t index, ValueId value)
    {
        GraphStore &builder = mutableStore();
        builder.insertOperand(op, index, value);
    }

    void Graph::insertResult(OperationId op, std::size_t index, ValueId value)
    {
        GraphStore &builder = mutableStore();
        builder.insertResult(op, index, value);
    }

    void Graph::replaceOperand(OperationId op, std::size_t index, ValueId value)
    {
        GraphStore &builder = mutableStore();
        builder.replaceOperand(op, index, value);
    }

    void Graph::guardShapeAlias(ValueId from, ValueId to, std::string_view api)
    {
        if (!from.valid() || !to.valid())
        {
            return;
        }
        if (!sameValueType(from, to))
        {
            throw std::runtime_error(std::string(api) +
                                     ": refusing to alias values with different types");
        }
    }

    void Graph::replaceResult(OperationId op, std::size_t index, ValueId value)
    {
        // 不在这里做 shape 校验：replaceResult 只改变 op 产出哪个 value，不改变
        // 任何 value 自身的类型（shape 创建即定型，不可变）。op 自身的结果类型
        // 契约由 grh.verify 事后强制。真正的别名通道是 replaceAllUses/eraseOp。
        const auto results = opResults(op);
        if (index >= results.size()) throw std::runtime_error("Result index out of range");
        const ValueId current = results[index];
        GraphStore &builder = mutableStore();
        builder.replaceResult(op, index, value);
        mergeValueSemanticIds(value, current);
    }

    void Graph::replaceAllUses(ValueId from, ValueId to)
    {
        guardShapeAlias(from, to, "replaceAllUses");
        GraphStore &builder = mutableStore();
        builder.replaceAllUses(from, to);
        mergeValueSemanticIds(to, from);
    }

    ValueId Graph::replaceAllUsesWithView(ValueId from, ValueId to)
    {
        if (!from.valid() || !to.valid())
        {
            throw std::runtime_error("replaceAllUsesWithView: invalid value");
        }
        const std::span<const int64_t> fromShape = valueShape(from);
        if (sameValueType(from, to))
        {
            replaceAllUses(from, to);
            return to;
        }
        if (valueWidth(from) != valueWidth(to))
        {
            throw std::runtime_error("replaceAllUsesWithView: width mismatch cannot be retyped");
        }
        if (valueType(from) != valueType(to) || valueDomain(from) != valueDomain(to))
        {
            throw std::runtime_error("replaceAllUsesWithView: incompatible value kinds");
        }
        // The view carries `from`'s type over `to`'s bits.
        const ValueId view = createValue(makeInternalValSym(), valueWidth(from),
                                         valueSigned(from), valueType(from), fromShape, valueDomain(from));
        const OperationId viewOp = createOperation(OperationKind::kAssign, makeInternalOpSym());
        addOperand(viewOp, to);
        addResult(viewOp, view);
        setValueSemanticIds(view, valueSemanticIds(from));
        replaceAllUses(from, view);
        return view;
    }

    bool Graph::eraseOperand(OperationId op, std::size_t index)
    {
        GraphStore &builder = mutableStore();
        return builder.eraseOperand(op, index);
    }

    bool Graph::eraseResult(OperationId op, std::size_t index)
    {
        GraphStore &builder = mutableStore();
        return builder.eraseResult(op, index);
    }

    bool Graph::eraseOp(OperationId op)
    {
        GraphStore &builder = mutableStore();
        SymbolId declaredSymbol;
        if (builder.opAlive(op))
        {
            const std::size_t opIdx = builder.opIndex(op);
            declaredSymbol = builder.operations_[opIdx].symbol;
        }
        bool result = builder.eraseOp(op);
        if (result)
        {
            if (declaredSymbol.valid() && isDeclaredSymbol(declaredSymbol))
            {
                removeDeclaredSymbol(declaredSymbol);
            }
            removeSymbolFromGenerateGroups(declaredSymbol);

        }
        return result;
    }

    bool Graph::eraseOp(OperationId op, std::span<const ValueId> replacementResults)
    {
        // The builder path aliases each old result to its replacement without
        // shape checks; keep the type-honesty invariant on this shortcut too.
        const auto results = opResults(op);
        const std::size_t n = std::min(results.size(), replacementResults.size());
        for (std::size_t i = 0; i < n; ++i)
        {
            guardShapeAlias(results[i], replacementResults[i], "eraseOp");
        }
        GraphStore &builder = mutableStore();
        SymbolId declaredSymbol;
        if (builder.opAlive(op))
        {
            const std::size_t opIdx = builder.opIndex(op);
            declaredSymbol = builder.operations_[opIdx].symbol;
        }
        bool result = builder.eraseOp(op, replacementResults);
        if (result)
        {
            for (std::size_t i = 0; i < results.size(); ++i)
                mergeValueSemanticIds(replacementResults[i], results[i]);
            if (declaredSymbol.valid() && isDeclaredSymbol(declaredSymbol))
            {
                removeDeclaredSymbol(declaredSymbol);
            }
            removeSymbolFromGenerateGroups(declaredSymbol);

        }
        return result;
    }

    bool Graph::eraseOpUnchecked(OperationId op)
    {
        GraphStore &builder = mutableStore();
        SymbolId declaredSymbol;
        if (builder.opAlive(op))
        {
            const std::size_t opIdx = builder.opIndex(op);
            declaredSymbol = builder.operations_[opIdx].symbol;
        }
        bool result = builder.eraseOpUnchecked(op);
        if (result)
        {
            if (declaredSymbol.valid() && isDeclaredSymbol(declaredSymbol))
            {
                removeDeclaredSymbol(declaredSymbol);
            }
            removeSymbolFromGenerateGroups(declaredSymbol);

        }
        return result;
    }

    bool Graph::eraseValue(ValueId value)
    {
        GraphStore &builder = mutableStore();
        SymbolId declaredSymbol;
        if (builder.valueAlive(value))
        {
            const std::size_t valIdx = builder.valueIndex(value);
            declaredSymbol = builder.values_[valIdx].symbol;
        }
        bool result = builder.eraseValue(value);
        if (result)
        {
            if (declaredSymbol.valid() && isDeclaredSymbol(declaredSymbol))
            {
                removeDeclaredSymbol(declaredSymbol);
            }
            removeSymbolFromGenerateGroups(declaredSymbol);

        }
        return result;
    }

    bool Graph::eraseValueUnchecked(ValueId value)
    {
        GraphStore &builder = mutableStore();
        SymbolId declaredSymbol;
        if (builder.valueAlive(value))
        {
            const std::size_t valIdx = builder.valueIndex(value);
            declaredSymbol = builder.values_[valIdx].symbol;
        }
        bool result = builder.eraseValueUnchecked(value);
        if (result)
        {
            if (declaredSymbol.valid() && isDeclaredSymbol(declaredSymbol))
            {
                removeDeclaredSymbol(declaredSymbol);
            }
            removeSymbolFromGenerateGroups(declaredSymbol);

        }
        return result;
    }

    void Graph::setAttr(OperationId op, std::string_view key, AttributeValue value)
    {
        GraphStore &builder = mutableStore();
        builder.setAttr(op, key, std::move(value));
    }

    void Graph::setAttrView(OperationId op, std::string_view key, AttrValueView value)
    {
        mutableStore().setAttrView(op, key, value);
    }

    void Graph::setCanonicalNodeAttrs(OperationId op, std::string_view canonicalKey,
                                      bool root, int64_t rootResult)
    {
        GraphStore &builder = mutableStore();
        builder.setCanonicalNodeAttrs(op, canonicalKey, root, rootResult);
    }

    void Graph::setOpKind(OperationId op, OperationKind kind)
    {
        GraphStore &builder = mutableStore();
        builder.setOpKind(op, kind);
    }

    bool Graph::eraseAttr(OperationId op, std::string_view key)
    {
        GraphStore &builder = mutableStore();
        return builder.eraseAttr(op, key);
    }

    void Graph::setValueSrcLoc(ValueId value, SrcLoc loc)
    {
        GraphStore &builder = mutableStore(false);
        builder.setValueSrcLoc(value, std::move(loc));
    }

    void Graph::setValueSrcLocView(ValueId value, SrcLocView loc)
    {
        mutableStore(false).setValueSrcLocView(value, loc);
    }

    GraphPayload::RecordId Graph::importSrcLoc(const Graph& source, GraphPayload::RecordId id,
                                              SrcLocImportContext& context)
    {
        if (!id) return 0;
        const bool sourceChanged = context.source_.owner_before(source.store_) ||
            source.store_.owner_before(context.source_) || context.sourceEpoch_ != source.srcLocEpoch_;
        const bool targetChanged = context.target_.owner_before(store_) ||
            store_.owner_before(context.target_) || context.targetEpoch_ != srcLocEpoch_;
        if (sourceChanged || targetChanged) {
            context.records_.clear();
            context.strings_.clear();
            context.blockStrings_.reset();
            context.source_ = source.store_;
            context.target_ = store_;
            context.sourceEpoch_ = source.srcLocEpoch_;
            context.targetEpoch_ = srcLocEpoch_;
        }
        if (id >= context.records_.size()) context.records_.resize(static_cast<std::size_t>(id) + 1);
        auto& mapped = context.records_[id];
        if (mapped) return mapped;
        const auto importString = [&](GraphPayload::StringId sourceId) {
            if (!sourceId) return GraphPayload::StringId{0};
            if (context.blockStrings_ && sourceId < context.blockStrings_->size())
                return (*context.blockStrings_)[sourceId];
            if (const auto found = context.strings_.find(sourceId); found != context.strings_.end())
                return found->second;
            const auto targetId = store_->internString(source.store_->string(sourceId));
            context.strings_.emplace(sourceId, targetId);
            return targetId;
        };
        auto record = source.store_->srcLoc(id);
        record.file = importString(record.file);
        record.origin = importString(record.origin);
        record.pass = importString(record.pass);
        record.note = importString(record.note);
        if (const auto found = store_->srcLocIds_.find(record); found != store_->srcLocIds_.end()) {
            mapped = found->second;
        } else {
            const auto result = static_cast<GraphPayload::RecordId>(store_->srcLocs_.size());
            store_->srcLocs_.push_back(record);
            store_->srcLocIds_.emplace(record, result);
            mapped = result;
        }
        return mapped;
    }

    void Graph::copyValueSrcLoc(ValueId value, const Graph& source, ValueId sourceValue)
    {
        copyValueSrcLoc(value, source, sourceValue, nullptr);
    }

    void Graph::copyValueSrcLoc(ValueId value, const Graph& source, ValueId sourceValue,
                               SrcLocImportContext& context)
    {
        copyValueSrcLoc(value, source, sourceValue, &context);
    }

    void Graph::copyValueSrcLoc(ValueId value, const Graph& source, ValueId sourceValue,
                               SrcLocImportContext* context)
    {
        const auto sourceData = source.store_->values_.at(source.store_->valueIndex(sourceValue));
        if (!sourceData.alive) throw std::runtime_error("ValueId refers to erased value");
        GraphStore& target = mutableStore(false);
        const std::size_t index = target.valueIndex(value);
        if (!target.values_[index].alive) throw std::runtime_error("ValueId refers to erased value");
        const GraphPayload::RecordId sourceId = sourceData.srcLoc;
        const GraphPayload::RecordId targetId = this == &source
            ? sourceId : context ? importSrcLoc(source, sourceId, *context)
                                 : target.importSrcLoc(*source.store_, sourceId);
        target.values_[index].srcLoc = targetId;
        if (sourceId && graphProfileEnabled()) {
            const auto& record = source.store_->srcLoc(sourceId);
            const uint64_t bytes = source.store_->string(record.file).size() +
                source.store_->string(record.origin).size() + source.store_->string(record.pass).size() +
                source.store_->string(record.note).size();
            profileCounters.srcLocWrites.fetch_add(1, std::memory_order_relaxed);
            profileCounters.srcLocBytes.fetch_add(bytes, std::memory_order_relaxed);
        }
    }

    void Graph::setValueShape(ValueId value, std::span<const int64_t> shape)
    {
        GraphStore &builder = mutableStore();
        builder.setValueShape(value, shape);
    }

    void Graph::setOpSrcLoc(OperationId op, SrcLoc loc)
    {
        GraphStore &builder = mutableStore(false);
        builder.setOpSrcLoc(op, std::move(loc));
    }

    void Graph::setOpSrcLocView(OperationId op, SrcLocView loc)
    {
        mutableStore(false).setOpSrcLocView(op, loc);
    }

    void Graph::setOpAndValueSrcLocView(OperationId op, ValueId value, SrcLocView loc)
    {
        mutableStore(false).setOpAndValueSrcLocView(op, value, loc);
    }

    void Graph::copyOpSrcLoc(OperationId op, const Graph& source, OperationId sourceOp)
    {
        copyOpSrcLoc(op, source, sourceOp, nullptr);
    }

    void Graph::copyOpSrcLoc(OperationId op, const Graph& source, OperationId sourceOp,
                            SrcLocImportContext& context)
    {
        copyOpSrcLoc(op, source, sourceOp, &context);
    }

    void Graph::copyOpSrcLoc(OperationId op, const Graph& source, OperationId sourceOp,
                            SrcLocImportContext* context)
    {
        const auto sourceData = source.store_->operations_.at(source.store_->opIndex(sourceOp));
        if (!sourceData.alive) throw std::runtime_error("OperationId refers to erased operation");
        GraphStore& target = mutableStore(false);
        const std::size_t index = target.opIndex(op);
        if (!target.operations_[index].alive) throw std::runtime_error("OperationId refers to erased operation");
        const GraphPayload::RecordId sourceId = sourceData.srcLoc;
        const GraphPayload::RecordId targetId = this == &source
            ? sourceId : context ? importSrcLoc(source, sourceId, *context)
                                 : target.importSrcLoc(*source.store_, sourceId);
        target.operations_[index].srcLoc = targetId;
        if (sourceId && graphProfileEnabled()) {
            const auto& record = source.store_->srcLoc(sourceId);
            const uint64_t bytes = source.store_->string(record.file).size() +
                source.store_->string(record.origin).size() + source.store_->string(record.pass).size() +
                source.store_->string(record.note).size();
            profileCounters.srcLocWrites.fetch_add(1, std::memory_order_relaxed);
            profileCounters.srcLocBytes.fetch_add(bytes, std::memory_order_relaxed);
        }
    }

    void Graph::setOpSymbol(OperationId op, SymbolId sym)
    {
        // Only rebinds the op's symbol. declaredSymbols_/generateGroups_
        // membership is keyed on SymbolId and is intentionally not updated:
        // symbol transplantation (handing the old SymbolId to an equivalent
        // new entity) preserves membership naturally, while a plain rename
        // leaves the old SymbolId's membership untouched.
        GraphStore &builder = mutableStore();
        builder.setOpSymbol(op, sym);
    }

    void Graph::setValueSymbol(ValueId value, SymbolId sym)
    {
        // Only rebinds the value's symbol; same declaredSymbols_/generateGroups_
        // membership contract as setOpSymbol.
        GraphStore &builder = mutableStore();
        builder.setValueSymbol(value, sym);
    }

    void Graph::clearOpSymbol(OperationId op)
    {
        GraphStore &builder = mutableStore();
        builder.clearOpSymbol(op);
    }

    void Graph::clearValueSymbol(ValueId value)
    {
        GraphStore &builder = mutableStore();
        builder.clearValueSymbol(value);
    }

    void Graph::writeJson(slang::JsonWriter &writer, bool emitDefUsers, bool emitSourceLocations) const
    {
        auto requireSymbolText = [&](SymbolId sym, std::string_view context) -> std::string_view
        {
            if (!sym.valid())
            {
                throw std::runtime_error(std::string(context) + " symbol is invalid");
            }
            if (!symbols_.valid(sym))
            {
                throw std::runtime_error(std::string(context) + " symbol is not in the symbol table");
            }
            std::string_view text = symbols_.text(sym);
            if (text.empty())
            {
                throw std::runtime_error(std::string(context) + " symbol is empty");
            }
            return text;
        };
        auto requirePortName = [&](std::string_view name, std::string_view context) -> std::string_view
        {
            if (name.empty())
            {
                throw std::runtime_error(std::string(context) + " name is empty");
            }
            return name;
        };

        uint32_t maxValueIndex = 0;
        for (const auto valueId : values()) maxValueIndex = std::max(maxValueIndex, valueId.index);
        uint32_t maxOperationIndex = 0;
        for (const auto opId : operations()) maxOperationIndex = std::max(maxOperationIndex, opId.index);
        std::vector<uint32_t> valueLocalIds(static_cast<std::size_t>(maxValueIndex) + 1, 0);
        std::vector<uint32_t> operationLocalIds(static_cast<std::size_t>(maxOperationIndex) + 1, 0);
        uint32_t nextValueId = 1;
        for (const auto valueId : values())
            valueLocalIds[valueId.index] = nextValueId++;
        uint32_t nextOperationId = 1;
        for (const auto opId : operations())
            operationLocalIds[opId.index] = nextOperationId++;
        const auto valueLocalId = [&](ValueId value) -> uint32_t {
            if (value.index >= valueLocalIds.size() || valueLocalIds[value.index] == 0)
                throw std::runtime_error("JSON value reference is not a live graph value");
            return valueLocalIds[value.index];
        };
        const auto operationLocalId = [&](OperationId op) -> uint32_t {
            if (op.index >= operationLocalIds.size() || operationLocalIds[op.index] == 0)
                throw std::runtime_error("JSON operation reference is not a live graph operation");
            return operationLocalIds[op.index];
        };

        writer.startObject();
        writer.writeProperty("symbol");
        if (symbol_.empty())
        {
            throw std::runtime_error("Graph symbol is empty");
        }
        writer.writeValue(symbol_);

        writer.writeProperty("dialects");
        writer.startObject();
        for (const auto& [dialect, version] : requiredDialects(*this))
        {
            writer.writeProperty(dialect);
            writer.writeValue(static_cast<int64_t>(version));
        }
        writer.endObject();
        if (semanticTree_)
        {
            writer.writeProperty("semanticTree");
            writer.startObject();
            writer.writeProperty("sourceUnit");
            writer.writeValue(semanticTree_->sourceUnit());
            writer.writeProperty("circuit");
            writer.writeValue(semanticTree_->circuit());
            writer.writeProperty("nodes");
            writer.startArray();
            for (SemanticId id = 1; id <= semanticTree_->size(); ++id)
            {
                const auto& node = semanticTree_->node(id);
                writer.startObject();
                writer.writeProperty("parent");
                writer.writeValue(static_cast<int64_t>(node.parent));
                writer.writeProperty("kind");
                writer.writeValue(toString(node.kind));
                writer.writeProperty("name");
                writer.writeValue(semanticTree_->name(id));
                if (node.kind == SemanticNodeKind::Element)
                {
                    writer.writeProperty("index");
                    writer.writeValue(static_cast<int64_t>(node.index));
                }
                writer.endObject();
            }
            writer.endArray();
            writer.endObject();
        }
        writer.writeProperty("metadata");
        writer.startObject();
        for (const AttrKV& entry : metadata_)
        {
            writer.writeProperty(entry.key);
            writer.startObject();
            writeAttributeValue(writer, entry.value);
            writer.endObject();
        }
        writer.endObject();

        writer.writeProperty("declaredSymbols");
        writer.startArray();
        for (const auto sym : declaredSymbols_)
        {
            writer.writeValue(requireSymbolText(sym, "Declared symbol"));
        }
        writer.endArray();

        if (!generateGroups_.empty())
        {
            writer.writeProperty("generateGroups");
            writer.startArray();
            for (const GenerateGroup &group : generateGroups_)
            {
                writer.startObject();
                writer.writeProperty("scope");
                writer.writeValue(requireSymbolText(group.scope, "Generate group scope"));
                writer.writeProperty("name");
                writer.writeValue(requireSymbolText(group.name, "Generate group name"));
                writer.writeProperty("symbols");
                writer.startArray();
                for (const auto sym : group.symbols)
                {
                    writer.writeValue(requireSymbolText(sym, "Generate group symbol"));
                }
                writer.endArray();
                writer.endObject();
            }
            writer.endArray();
        }

        writer.writeProperty("vals");
        writer.startArray();
        for (const auto &valueId : values())
        {
            const Value value = getValue(valueId);
            writer.startObject();
            writer.writeProperty("id");
            writer.writeValue(static_cast<int64_t>(valueLocalId(value.id())));
            if (value.symbol().valid())
            {
                writer.writeProperty("sym");
                writer.writeValue(requireSymbolText(value.symbol(), "Value"));
            }
            writer.writeProperty("w");
            writer.writeValue(static_cast<int64_t>(value.width()));
            writer.writeProperty("sgn");
            writer.writeValue(value.isSigned());
            writer.writeProperty("type");
            writer.writeValue(std::string(toString(value.type())));
            writer.writeProperty("domain");
            writer.writeValue(toString(value.domain()));
            writer.writeProperty("in");
            writer.writeValue(value.isInput());
            writer.writeProperty("out");
            writer.writeValue(value.isOutput());
            writer.writeProperty("inout");
            writer.writeValue(value.isInout());
            if (!value.shape().empty())
            {
                writer.writeProperty("shape");
                writer.startArray();
                for (const int64_t dim : value.shape())
                {
                    writer.writeValue(dim);
                }
                writer.endArray();
            }
            if (emitDefUsers && value.definingOp().valid())
            {
                Operation defOp = getOperation(value.definingOp());
                writer.writeProperty("def");
                writer.writeValue(static_cast<int64_t>(operationLocalId(defOp.id())));
            }

            if (emitDefUsers)
            {
                writer.writeProperty("users");
                writer.startArray();
                std::vector<ValueUser> orderedUsers(value.users().begin(), value.users().end());
                std::sort(orderedUsers.begin(), orderedUsers.end(), [&](const ValueUser &left, const ValueUser &right) {
                    if (operationLocalId(left.operation) != operationLocalId(right.operation))
                        return operationLocalId(left.operation) < operationLocalId(right.operation);
                    return left.operandIndex < right.operandIndex;
                });
                for (const auto &user : orderedUsers)
                {
                    writer.startObject();
                    writer.writeProperty("op");
                    writer.writeValue(static_cast<int64_t>(operationLocalId(user.operation)));
                    writer.writeProperty("idx");
                    writer.writeValue(static_cast<int64_t>(user.operandIndex));
                    writer.endObject();
                }
                writer.endArray();
            }
            if (!valueSemanticIds(valueId).empty())
            {
                writer.writeProperty("semanticIds");
                writer.startArray();
                for (const auto id : valueSemanticIds(valueId)) writer.writeValue(static_cast<int64_t>(id));
                writer.endArray();
            }
            if (emitSourceLocations) writeSrcLoc(writer, value.srcLoc());
            writer.endObject();
        }
        writer.endArray();

        writer.writeProperty("ports");
        writer.startObject();

        writer.writeProperty("in");
        writer.startArray();
        for (const auto &port : inputPorts())
        {
            writer.startObject();
            writer.writeProperty("name");
            writer.writeValue(requirePortName(port.name, "Input port"));
            writer.writeProperty("val");
            writer.writeValue(static_cast<int64_t>(valueLocalId(port.value)));
            writer.endObject();
        }
        writer.endArray();

        writer.writeProperty("out");
        writer.startArray();
        for (const auto &port : outputPorts())
        {
            writer.startObject();
            writer.writeProperty("name");
            writer.writeValue(requirePortName(port.name, "Output port"));
            writer.writeProperty("val");
            writer.writeValue(static_cast<int64_t>(valueLocalId(port.value)));
            writer.endObject();
        }
        writer.endArray();

        writer.writeProperty("inout");
        writer.startArray();
        for (const auto &port : inoutPorts())
        {
            writer.startObject();
            writer.writeProperty("name");
            writer.writeValue(requirePortName(port.name, "Inout port"));
            writer.writeProperty("in");
            writer.writeValue(static_cast<int64_t>(valueLocalId(port.in)));
            writer.writeProperty("out");
            writer.writeValue(static_cast<int64_t>(valueLocalId(port.out)));
            writer.writeProperty("oe");
            writer.writeValue(static_cast<int64_t>(valueLocalId(port.oe)));
            writer.endObject();
        }
        writer.endArray();

        writer.endObject(); // ports

        writer.writeProperty("ops");
        writer.startArray();
        for (const auto &opId : operations())
        {
            const Operation op = getOperation(opId);
            writer.startObject();
            writer.writeProperty("id");
            writer.writeValue(static_cast<int64_t>(operationLocalId(op.id())));
            if (op.symbol().valid())
            {
                writer.writeProperty("sym");
                writer.writeValue(requireSymbolText(op.symbol(), "Operation"));
            }
            writer.writeProperty("kind");
            writer.writeValue(toString(op.kind()));

            writer.writeProperty("in");
            writer.startArray();
            for (const auto &operand : op.operands())
            {
                writer.writeValue(static_cast<int64_t>(valueLocalId(operand)));
            }
            writer.endArray();

            writer.writeProperty("out");
            writer.startArray();
            for (const auto &result : op.results())
            {
                writer.writeValue(static_cast<int64_t>(valueLocalId(result)));
            }
            writer.endArray();

            if (!opSemanticIds(opId).empty())
            {
                writer.writeProperty("semanticIds");
                writer.startArray();
                for (const auto id : opSemanticIds(opId)) writer.writeValue(static_cast<int64_t>(id));
                writer.endArray();
            }

            if (!op.attrs().empty())
            {
                writer.writeProperty("attrs");
                writer.startObject();
                for (const auto &attr : op.attrs())
                {
                    writer.writeProperty(attr.key);
                    writer.startObject();
                    writeAttributeValue(writer, attr.value);
                    writer.endObject();
                }
                writer.endObject();
            }

            if (emitSourceLocations) writeSrcLoc(writer, op.srcLoc());
            writer.endObject();
        }
        writer.endArray();

        writer.endObject();
    }

    GraphStore& Graph::mutableStore(bool affectsLocalCycles)
    {
        const bool preserveProof = !affectsLocalCycles && localCyclesChecked();
        owner_->touch();
        compacted_ = false;
        ++mutationEpoch_;
        if (preserveProof) localCycleProof_ = mutationEpoch_;
        return *store_;
    }


    void Graph::reserveSymbolCapacity(std::size_t count)
    {
        symbols_.reserve(count);
        store_->reserveSymbols(count);
    }

    void Graph::reserveDeclaredSymbolCapacity(std::size_t count)
    {
        declaredSymbols_.reserve(count);
        declaredSymbolSet_.reserve(count);
    }

    void Graph::reserveValueCapacity(std::size_t count)
    {
        GraphStore &builder = mutableStore();
        builder.reserveValues(count);
    }

    void Graph::reserveOperationCapacity(std::size_t count)
    {
        GraphStore &builder = mutableStore();
        builder.reserveOperations(count);
    }

    void Graph::reserveOpOperandCapacity(OperationId op, std::size_t count)
    {
        mutableStore().reserveOpOperands(op, count);
    }

    void Graph::reserveOpResultCapacity(OperationId op, std::size_t count)
    {
        mutableStore().reserveOpResults(op, count);
    }

    void Graph::reserveOpAttrCapacity(OperationId op, std::size_t count)
    {
        mutableStore().reserveOpAttrs(op, count);
    }

    Value Graph::valueFromStore(ValueId id) const
    {
        id.assertGraph(graphId_);
        if (id.index == 0 || id.index > store_->values_.size())
        {
            throw std::runtime_error("ValueId out of range");
        }
        const std::size_t idx = static_cast<std::size_t>(id.index - 1);
        const auto data = store_->values_[idx];
        if (!data.alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }

        std::string symbolText = symbolTextOrEmpty(symbols_, data.symbol);
        return Value(id,
                     data.symbol,
                     std::move(symbolText),
                     data.width,
                     data.isSigned,
                     data.type,
                     data.domain,
                     data.shape,
                     data.isInput,
                     data.isOutput,
                     data.isInout,
                     data.definingOp,
                     std::span<const ValueUser>(),
                     store_->srcLocValue(data.srcLoc),
                     this,
                     false);
    }

    Operation Graph::operationFromStore(OperationId id) const
    {
        id.assertGraph(graphId_);
        if (id.index == 0 || id.index > store_->operations_.size())
        {
            throw std::runtime_error("OperationId out of range");
        }
        const std::size_t idx = static_cast<std::size_t>(id.index - 1);
        const auto data = store_->operations_[idx];
        if (!data.alive)
        {
            throw std::runtime_error("OperationId refers to erased operation");
        }

        std::string symbolText = symbolTextOrEmpty(symbols_, data.symbol);
        return Operation(id,
                         data.kind,
                         data.symbol,
                         std::move(symbolText),
                         data.operands,
                         data.results,
                         [&] {
                             std::vector<AttrKV> attrs;
                             attrs.reserve(data.attrs.size());
                             for (const auto attr : opAttrRange(id))
                                 attrs.push_back({std::string(attr.key()), attr.value().toOwned()});
                             return attrs;
                         }(),
                         store_->srcLocValue(data.srcLoc));
    }

    std::span<const ValueUser> Graph::valueUsersSpan(ValueId id) const noexcept
    {
        if (id.graph != graphId_ || !id.index || id.index > store_->valueUsers_.size()) return {};
        if (!store_->values_[id.index - 1].alive) return {};
        return store_->valueUsers_[id.index - 1];
    }


    Graph &Design::addGraphInternal(std::unique_ptr<Graph> graph)
    {
        touch();
        auto sym = graph->symbol();
        auto [it, inserted] = graphs_.emplace(sym, std::move(graph));
        if (!inserted)
        {
            throw std::runtime_error("Duplicated graph symbol: " + sym);
        }
        graphOrder_.push_back(sym);
        return *it->second;
    }

    Graph &Design::createGraph(std::string symbol)
    {
        return publishGraph(createUnpublishedGraph(std::move(symbol)));
    }

    std::unique_ptr<Graph> Design::createUnpublishedGraph(std::string symbol)
    {
        touch();
        if (symbol.empty())
        {
            throw std::invalid_argument("Graph symbol must not be empty");
        }
        if (graphs_.contains(symbol))
        {
            throw std::runtime_error("Duplicated graph symbol: " + symbol);
        }
        SymbolId graphSymbol = designSymbols_.contains(symbol) ? designSymbols_.lookup(symbol) : designSymbols_.intern(symbol);
        GraphId graphId = designSymbols_.allocateGraphId(graphSymbol);
        return std::make_unique<Graph>(*this, std::move(symbol), graphId);
    }

    Graph& Design::publishGraph(std::unique_ptr<Graph> graph) {
        if (!graph) throw std::invalid_argument("Cannot publish a null graph");
        if (&graph->owner() != this)
            throw std::invalid_argument("Cannot publish a graph owned by another design");
        const auto symbol = designSymbols_.lookup(graph->symbol());
        if (!symbol.valid() || designSymbols_.lookupGraphId(symbol) != graph->id())
            throw std::invalid_argument("Cannot publish a graph without its reserved GraphId");
        if (graphs_.contains(graph->symbol()))
            throw std::runtime_error("Duplicated graph symbol: " + graph->symbol());
        return addGraphInternal(std::move(graph));
    }

    bool Design::deleteGraph(std::string_view name)
    {
        touch();
        std::string key(name);
        std::string symbol;
        if (auto it = graphs_.find(key); it != graphs_.end())
        {
            symbol = key;
        }
        else if (auto aliasIt = graphAliasBySymbol_.find(key); aliasIt != graphAliasBySymbol_.end())
        {
            symbol = aliasIt->second;
        }
        else
        {
            return false;
        }

        SymbolId declaredSymbol = designSymbols_.lookup(symbol);
        if (declaredSymbol.valid())
        {
            removeDeclaredSymbol(declaredSymbol);
            designSymbols_.releaseGraphId(declaredSymbol);
        }

        graphs_.erase(symbol);
        auto orderIt = std::remove(graphOrder_.begin(), graphOrder_.end(), symbol);
        if (orderIt != graphOrder_.end())
        {
            graphOrder_.erase(orderIt, graphOrder_.end());
        }

        auto topIt = std::remove(topGraphs_.begin(), topGraphs_.end(), symbol);
        if (topIt != topGraphs_.end())
        {
            topGraphs_.erase(topIt, topGraphs_.end());
        }

        for (auto it = graphAliasBySymbol_.begin(); it != graphAliasBySymbol_.end();)
        {
            if (it->second == symbol)
            {
                it = graphAliasBySymbol_.erase(it);
            }
            else
            {
                ++it;
            }
        }
        return true;
    }

    Graph &Design::cloneGraph(std::string_view sourceName, std::string newName)
    {
        Graph *source = findGraph(sourceName);
        if (!source)
        {
            throw std::runtime_error("Source graph not found: " + std::string(sourceName));
        }
        if (newName.empty())
        {
            throw std::invalid_argument("Graph symbol must not be empty");
        }
        if (graphs_.contains(newName))
        {
            throw std::runtime_error("Duplicated graph symbol: " + newName);
        }

        SymbolId graphSymbol = designSymbols_.contains(newName)
                                   ? designSymbols_.lookup(newName)
                                   : designSymbols_.intern(newName);
        GraphId graphId = designSymbols_.allocateGraphId(graphSymbol);
        auto instance = std::make_unique<Graph>(*this, std::move(newName), graphId);
        Graph &clone = addGraphInternal(std::move(instance));
        source->cloneContentsTo(clone);

        return clone;
    }

    Graph &Design::importGraph(const Graph &source, std::string newName)
    {
        Graph &destination = createGraph(std::move(newName));
        source.cloneContentsTo(destination);
        return destination;
    }

    Design Design::clone() const
    {
        Design cloned;
        for (const auto &name : graphOrder_)
        {
            const Graph *source = findGraph(name);
            if (!source)
            {
                throw std::runtime_error("Graph not found during design clone: " + name);
            }
            Graph &dest = cloned.createGraph(name);
            source->cloneContentsTo(dest);
        }

        for (const auto &name : graphOrder_)
        {
            Graph *dest = cloned.findGraph(name);
            if (!dest)
            {
                throw std::runtime_error("Graph missing during design clone alias copy: " + name);
            }
            for (const auto &alias : aliasesForGraph(name))
            {
                cloned.registerGraphAlias(alias, *dest);
            }
        }

        for (const auto &name : topGraphs_)
        {
            cloned.markAsTop(name);
        }

        for (const auto sym : declaredSymbols_)
        {
            if (!sym.valid())
            {
                continue;
            }
            std::string_view text = symbolText(sym);
            if (text.empty())
            {
                throw std::runtime_error("Declared symbol text is empty during design clone");
            }
            SymbolId dstSym = cloned.internSymbol(text);
            if (!dstSym.valid())
            {
                throw std::runtime_error("Failed to clone design declared symbol: " + std::string(text));
            }
            cloned.addDeclaredSymbol(dstSym);
        }

        return cloned;
    }

    Graph *Design::findGraph(std::string_view symbol) noexcept
    {
        std::string key(symbol);
        if (auto it = graphs_.find(key); it != graphs_.end())
        {
            return it->second.get();
        }
        if (auto aliasIt = graphAliasBySymbol_.find(key); aliasIt != graphAliasBySymbol_.end())
        {
            if (auto resolved = graphs_.find(aliasIt->second); resolved != graphs_.end())
            {
                return resolved->second.get();
            }
        }
        return nullptr;
    }

    const Graph *Design::findGraph(std::string_view symbol) const noexcept
    {
        std::string key(symbol);
        if (auto it = graphs_.find(key); it != graphs_.end())
        {
            return it->second.get();
        }
        if (auto aliasIt = graphAliasBySymbol_.find(key); aliasIt != graphAliasBySymbol_.end())
        {
            if (auto resolved = graphs_.find(aliasIt->second); resolved != graphs_.end())
            {
                return resolved->second.get();
            }
        }
        return nullptr;
    }

    SymbolId Design::internSymbol(std::string_view text)
    {
        touch();
        return designSymbols_.intern(text);
    }

    SymbolId Design::lookupSymbol(std::string_view text) const
    {
        return designSymbols_.lookup(text);
    }

    std::string_view Design::symbolText(SymbolId id) const
    {
        if (!id.valid())
        {
            return std::string_view{};
        }
        return designSymbols_.text(id);
    }

    void Design::sortGraphOrder()
    {
        if (!std::is_sorted(graphOrder_.begin(), graphOrder_.end()))
        {
            std::sort(graphOrder_.begin(), graphOrder_.end());
            touch();
        }
    }

    void Design::addDeclaredSymbol(SymbolId sym)
    {
        touch();
        if (!sym.valid())
        {
            throw std::runtime_error("Declared symbol is invalid");
        }
        if (!designSymbols_.valid(sym))
        {
            throw std::runtime_error("Declared symbol is not in the design symbol table");
        }
        if (declaredSymbolSet_.insert(sym.value).second)
        {
            declaredSymbols_.push_back(sym);
        }
    }

    bool Design::removeDeclaredSymbol(SymbolId sym)
    {
        touch();
        if (!sym.valid())
        {
            throw std::runtime_error("Declared symbol is invalid");
        }
        if (!designSymbols_.valid(sym))
        {
            throw std::runtime_error("Declared symbol is not in the design symbol table");
        }
        if (declaredSymbolSet_.erase(sym.value) == 0)
        {
            return false;
        }
        auto it = std::remove_if(declaredSymbols_.begin(), declaredSymbols_.end(),
                                 [&](SymbolId entry) { return entry == sym; });
        if (it != declaredSymbols_.end())
        {
            declaredSymbols_.erase(it, declaredSymbols_.end());
        }
        return true;
    }

    void Design::clearDeclaredSymbols()
    {
        touch();
        declaredSymbols_.clear();
        declaredSymbolSet_.clear();
    }

    bool Design::isDeclaredSymbol(SymbolId sym) const noexcept
    {
        if (!sym.valid())
        {
            return false;
        }
        return declaredSymbolSet_.find(sym.value) != declaredSymbolSet_.end();
    }

    std::span<const SymbolId> Design::declaredSymbols() const noexcept
    {
        return std::span<const SymbolId>(declaredSymbols_.data(), declaredSymbols_.size());
    }

    std::vector<std::string> Design::aliasesForGraph(std::string_view symbol) const
    {
        std::vector<std::string> aliases;
        for (const auto &entry : graphAliasBySymbol_)
        {
            if (entry.second == symbol)
            {
                aliases.push_back(entry.first);
            }
        }
        std::sort(aliases.begin(), aliases.end());
        aliases.erase(std::unique(aliases.begin(), aliases.end()), aliases.end());
        return aliases;
    }

    void Design::registerGraphAlias(std::string alias, Graph &graph)
    {
        touch();
        if (alias.empty())
        {
            return;
        }
        graphAliasBySymbol_[std::move(alias)] = graph.symbol();
    }

    void Design::markAsTop(std::string_view graphSymbol)
    {
        touch();
        if (!findGraph(graphSymbol))
        {
            throw std::runtime_error("Cannot mark unknown graph as top: " + std::string(graphSymbol));
        }
        auto symbolStr = std::string(graphSymbol);
        if (std::find(topGraphs_.begin(), topGraphs_.end(), symbolStr) == topGraphs_.end())
        {
            topGraphs_.push_back(std::move(symbolStr));
        }
    }

    void Design::unmarkAsTop(std::string_view graphSymbol)
    {
        touch();
        if (!findGraph(graphSymbol))
        {
            throw std::runtime_error("Cannot unmark unknown graph as top: " + std::string(graphSymbol));
        }
        auto symbolStr = std::string(graphSymbol);
        auto it = std::remove(topGraphs_.begin(), topGraphs_.end(), symbolStr);
        if (it != topGraphs_.end())
        {
            topGraphs_.erase(it, topGraphs_.end());
        }
    }

} // namespace wolvrix::lib::grh
