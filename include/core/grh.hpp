#ifndef WOLVRIX_GRH_HPP
#define WOLVRIX_GRH_HPP

#include "core/detail/string_pool.hpp"

#include "core/detail/pool_vector.hpp"
#include "core/detail/payload_pool.hpp"
#include "core/detail/shared_column.hpp"
#include "core/semantic_tree.hpp"
#include "slang/util/FlatMap.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace slang {
class JsonWriter;
}

namespace wolvrix::lib::grh {

enum class OperationKind {
    kConstant,
    kAdd,
    kSub,
    kMul,
    kDiv,
    kMod,
    kEq,
    kNe,
    kCaseEq,
    kCaseNe,
    kWildcardEq,
    kWildcardNe,
    kLt,
    kLe,
    kGt,
    kGe,
    kAnd,
    kOr,
    kXor,
    kXnor,
    kNot,
    kLogicAnd,
    kLogicOr,
    kLogicNot,
    kReduceAnd,
    kReduceOr,
    kReduceXor,
    kReduceNor,
    kReduceNand,
    kReduceXnor,
    kShl,
    kLShr,
    kAShr,
    kMux,
    kAssign,
    kConcat,
    kArrayCreate,
    kReplicate,
    kSliceStatic,
    kSliceDynamic,
    kSliceArray,
    kLatch,
    kLatchReadPort,
    kLatchWritePort,
    kRegister,
    kRegisterReadPort,
    kRegisterReadArrayPort,
    kRegisterWritePort,
    kRegisterWriteMaskPort,
    kRegisterWriteArrayPort,
    kRegisterWriteArrayMaskPort,
    kRegisterFillPort,
    kInstance,
    kBlackbox,
    kSystemFunction,
    kSystemTask,
    kDpicImport,
    kDpicCall,
    kXMRRead,
    kXMRWrite
};

std::string_view toString(OperationKind kind) noexcept;
std::optional<OperationKind> parseOperationKind(std::string_view text) noexcept;

enum class ValueType : uint8_t {
    Logic,
    Real,
    String
};

std::string_view toString(ValueType type) noexcept;
std::optional<ValueType> parseValueType(std::string_view text) noexcept;

enum class LogicDomain : uint8_t {
    TwoState,
    FourState
};

std::string_view toString(LogicDomain domain) noexcept;
std::optional<LogicDomain> parseLogicDomain(std::string_view text) noexcept;

struct LogicalType {
    ValueType kind = ValueType::Logic;
    int32_t width = 1;
    bool isSigned = false;
    LogicDomain domain = LogicDomain::FourState;
    std::vector<int64_t> shape;

    void validate() const;
    int32_t leafWidth() const;
    LogicalType normalized() const;
    friend bool operator==(const LogicalType&, const LogicalType&) = default;
};

using AttributeValue = std::variant<
    bool,
    int64_t,
    double,
    std::string,
    std::vector<bool>,
    std::vector<int64_t>,
    std::vector<double>,
    std::vector<std::string>>;

struct SrcLoc
{
    std::string file;
    uint32_t line = 0;
    uint32_t column = 0;
    uint32_t endLine = 0;
    uint32_t endColumn = 0;
    std::string origin;
    std::string pass;
    std::string note;
};

// Borrowed provenance strings are consumed by the setter before it returns.
struct SrcLocView
{
    std::string_view file;
    uint32_t line = 0;
    uint32_t column = 0;
    uint32_t endLine = 0;
    uint32_t endColumn = 0;
    std::string_view origin;
    std::string_view pass;
    std::string_view note;

    SrcLocView() = default;
    explicit SrcLocView(const SrcLoc& loc) noexcept
        : file(loc.file), line(loc.line), column(loc.column), endLine(loc.endLine),
          endColumn(loc.endColumn), origin(loc.origin), pass(loc.pass), note(loc.note) {}
};

using DebugInfo = SrcLoc;

[[nodiscard]] bool attributeValueIsJsonSerializable(const AttributeValue &value);

// Optional low-overhead counters for the F17 storage baseline. Counters are
// disabled by default and become active when WOLVRIX_GRH_PROFILE=1 (or when
// explicitly enabled by a profiling harness). They are process-wide because
// graph construction may be distributed across worker threads.
struct GraphProfileStats {
    uint64_t symbolInserts = 0;
    uint64_t attributeWrites = 0;
    uint64_t attributeBytes = 0;
    uint64_t srcLocWrites = 0;
    uint64_t srcLocBytes = 0;
    uint64_t valueAllocations = 0;
    uint64_t operationAllocations = 0;
    uint64_t freezeCount = 0;
    uint64_t thawCount = 0;
};

[[nodiscard]] bool graphProfileEnabled() noexcept;
void setGraphProfileEnabled(bool enabled) noexcept;
void resetGraphProfileStats() noexcept;
[[nodiscard]] GraphProfileStats graphProfileStats() noexcept;

class Graph;

struct SymbolId {
    uint32_t value = 0;

    constexpr bool valid() const noexcept { return value != 0; }
    static constexpr SymbolId invalid() noexcept { return {}; }
    friend constexpr bool operator==(SymbolId lhs, SymbolId rhs) noexcept { return lhs.value == rhs.value; }
    friend constexpr bool operator!=(SymbolId lhs, SymbolId rhs) noexcept { return !(lhs == rhs); }
};

class SymbolTable {
public:
    SymbolTable();
    SymbolTable(const SymbolTable& other);
    SymbolTable& operator=(const SymbolTable& other);
    SymbolTable(SymbolTable&&) noexcept = default;
    SymbolTable& operator=(SymbolTable&&) noexcept = default;

    SymbolId intern(std::string_view text);
    SymbolId lookup(std::string_view text) const;
    bool contains(std::string_view text) const;
    std::string_view text(SymbolId id) const;
    bool valid(SymbolId id) const noexcept;
    void reserve(std::size_t count);

private:
    friend class Graph;
    friend class GraphStore;
    SymbolTable(const SymbolTable& other, std::shared_ptr<detail::StringPool> strings);
    void swapStringState(SymbolTable& other) noexcept;
    // Insert once; an existing reservation is a miss, not a reusable symbol.
    SymbolId tryInternFresh(std::string_view text);
    SymbolId bindStringId(uint32_t stringId);

    std::shared_ptr<detail::StringPool> stringPool_;
    // Payload text may have a StringId without a symbol reservation.
    std::vector<SymbolId> symbolsByStringId_{SymbolId::invalid()};
    std::vector<std::string_view> textById_;
};

struct GraphId;

class DesignSymbolTable final : public SymbolTable {
public:
    DesignSymbolTable();

    GraphId allocateGraphId(SymbolId symbol);
    void releaseGraphId(SymbolId symbol);
    GraphId lookupGraphId(SymbolId symbol) const noexcept;
    SymbolId symbolForGraph(GraphId graph) const noexcept;

private:
    uint32_t nextGraphIndex_ = 1;
    std::vector<SymbolId> symbolByGraph_;
    std::unordered_map<uint32_t, uint32_t> graphIndexBySymbol_;
};

class GraphSymbolTable final : public SymbolTable {};

struct GraphId {
    uint32_t index = 0;
    uint32_t generation = 0;

    constexpr bool valid() const noexcept { return index != 0; }
    static constexpr GraphId invalid() noexcept { return {}; }
    friend constexpr bool operator==(GraphId lhs, GraphId rhs) noexcept
    {
        return lhs.index == rhs.index && lhs.generation == rhs.generation;
    }
    friend constexpr bool operator!=(GraphId lhs, GraphId rhs) noexcept { return !(lhs == rhs); }
};

struct ValueId {
    uint32_t index = 0;
    uint32_t generation = 0;
    GraphId graph;

    constexpr bool valid() const noexcept { return index != 0; }
    static constexpr ValueId invalid() noexcept { return {}; }
    void assertGraph(GraphId expected) const;
    explicit constexpr operator bool() const noexcept { return valid(); }
    friend constexpr bool operator==(ValueId lhs, ValueId rhs) noexcept
    {
        return lhs.index == rhs.index && lhs.generation == rhs.generation && lhs.graph == rhs.graph;
    }
    friend constexpr bool operator!=(ValueId lhs, ValueId rhs) noexcept { return !(lhs == rhs); }
};

struct OperationId {
    uint32_t index = 0;
    uint32_t generation = 0;
    GraphId graph;

    constexpr bool valid() const noexcept { return index != 0; }
    static constexpr OperationId invalid() noexcept { return {}; }
    void assertGraph(GraphId expected) const;
    explicit constexpr operator bool() const noexcept { return valid(); }
    friend constexpr bool operator==(OperationId lhs, OperationId rhs) noexcept
    {
        return lhs.index == rhs.index && lhs.generation == rhs.generation && lhs.graph == rhs.graph;
    }
    friend constexpr bool operator!=(OperationId lhs, OperationId rhs) noexcept { return !(lhs == rhs); }
};

struct ValueIdHash {
    std::size_t operator()(const ValueId& id) const noexcept
    {
        std::size_t seed = static_cast<std::size_t>(id.index);
        seed = seed * 1315423911u + static_cast<std::size_t>(id.generation);
        seed = seed * 1315423911u + static_cast<std::size_t>(id.graph.index);
        seed = seed * 1315423911u + static_cast<std::size_t>(id.graph.generation);
        return seed;
    }
};

struct OperationIdHash {
    std::size_t operator()(const OperationId& id) const noexcept
    {
        std::size_t seed = static_cast<std::size_t>(id.index);
        seed = seed * 1315423911u + static_cast<std::size_t>(id.generation);
        seed = seed * 1315423911u + static_cast<std::size_t>(id.graph.index);
        seed = seed * 1315423911u + static_cast<std::size_t>(id.graph.generation);
        return seed;
    }
};

struct Range {
    std::size_t offset = 0;
    std::size_t count = 0;
};

struct ValueUser {
    OperationId operation;
    uint32_t operandIndex = 0;
};

struct Port {
    std::string name;
    ValueId value;
};

struct InoutPort {
    std::string name;
    ValueId in;
    ValueId out;
    ValueId oe;
};

struct AttrKV {
    std::string key;
    AttributeValue value;
};

// Borrowed attribute views over pooled records or owning AttrKV snapshots.
// Constructing a view does not allocate; setAttr/getOperation retain their
// owning API.
enum class AttrValueKind : uint8_t {
    Bool,
    Int,
    Double,
    String,
    BoolList,
    IntList,
    DoubleList,
    StringList
};

class GraphPayload;

class BoolListView {
public:
    BoolListView() = default;
    explicit BoolListView(const std::vector<bool>& values) : legacy_(&values), count_(values.size()) {}
    BoolListView(std::span<const uint64_t> words, std::size_t count) : words_(words), count_(count) {}
    std::size_t size() const noexcept { return count_; }
    bool empty() const noexcept { return count_ == 0; }
    bool operator[](std::size_t index) const {
        return legacy_ ? (*legacy_)[index] : ((words_[index / 64] >> (index % 64)) & 1) != 0;
    }
    class const_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = bool;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = value_type;
        const_iterator() = default;
        const_iterator(const BoolListView* view, std::size_t index) : view_(view), index_(index) {}
        bool operator*() const { return (*view_)[index_]; }
        const_iterator& operator++() { ++index_; return *this; }
        const_iterator operator++(int) { auto copy = *this; ++*this; return copy; }
        friend bool operator==(const_iterator lhs, const_iterator rhs) = default;
    private:
        const BoolListView* view_ = nullptr;
        std::size_t index_ = 0;
    };
    const_iterator begin() const { return {this, 0}; }
    const_iterator end() const { return {this, count_}; }
private:
    const std::vector<bool>* legacy_ = nullptr;
    std::span<const uint64_t> words_;
    std::size_t count_ = 0;
};

class StringListView {
public:
    StringListView() = default;
    explicit StringListView(std::span<const std::string> values) : legacy_(values) {}
    StringListView(const GraphPayload* payload, std::span<const uint32_t> ids) : payload_(payload), ids_(ids) {}
    std::size_t size() const noexcept { return payload_ ? ids_.size() : legacy_.size(); }
    bool empty() const noexcept { return size() == 0; }
    std::string_view operator[](std::size_t index) const;
    class const_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = std::string_view;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = value_type;
        const_iterator() = default;
        const_iterator(const StringListView* view, std::size_t index) : view_(view), index_(index) {}
        std::string_view operator*() const { return (*view_)[index_]; }
        const_iterator& operator++() { ++index_; return *this; }
        const_iterator operator++(int) { auto copy = *this; ++*this; return copy; }
        friend bool operator==(const_iterator lhs, const_iterator rhs) = default;
    private:
        const StringListView* view_ = nullptr;
        std::size_t index_ = 0;
    };
    const_iterator begin() const { return {this, 0}; }
    const_iterator end() const { return {this, size()}; }
private:
    const GraphPayload* payload_ = nullptr;
    std::span<const uint32_t> ids_;
    std::span<const std::string> legacy_;
};

class AttrValueView {
public:
    AttrValueView() = default;
    explicit AttrValueView(const AttributeValue* value) : value_(value) {}
    AttrValueView(const GraphPayload* payload, uint32_t record) : payload_(payload), record_(record) {}

    bool valid() const noexcept { return value_ != nullptr || (payload_ != nullptr && record_ != 0); }
    AttrValueKind kind() const;
    bool asBool() const;
    int64_t asInt() const;
    double asDouble() const;
    std::string_view asString() const;
    BoolListView asBoolList() const;
    std::span<const int64_t> asIntList() const;
    std::span<const double> asDoubleList() const;
    StringListView asStringList() const;
    AttributeValue toOwned() const;
    friend bool operator==(AttrValueView left, AttrValueView right);

    template <typename T>
    using Borrowed = std::conditional_t<std::is_same_v<T, std::string>, std::string_view,
        std::conditional_t<std::is_same_v<T, std::vector<bool>>, BoolListView,
        std::conditional_t<std::is_same_v<T, std::vector<int64_t>>, std::span<const int64_t>,
        std::conditional_t<std::is_same_v<T, std::vector<double>>, std::span<const double>,
        std::conditional_t<std::is_same_v<T, std::vector<std::string>>, StringListView, T>>>>>;

    template <typename T>
    std::optional<Borrowed<T>> get() const {
        if (!valid()) return std::nullopt;
        if constexpr (std::is_same_v<T, bool>) { if (kind() == AttrValueKind::Bool) return asBool(); }
        else if constexpr (std::is_same_v<T, int64_t>) { if (kind() == AttrValueKind::Int) return asInt(); }
        else if constexpr (std::is_same_v<T, double>) { if (kind() == AttrValueKind::Double) return asDouble(); }
        else if constexpr (std::is_same_v<T, std::string>) { if (kind() == AttrValueKind::String) return asString(); }
        else if constexpr (std::is_same_v<T, std::vector<bool>>) { if (kind() == AttrValueKind::BoolList) return asBoolList(); }
        else if constexpr (std::is_same_v<T, std::vector<int64_t>>) { if (kind() == AttrValueKind::IntList) return asIntList(); }
        else if constexpr (std::is_same_v<T, std::vector<double>>) { if (kind() == AttrValueKind::DoubleList) return asDoubleList(); }
        else if constexpr (std::is_same_v<T, std::vector<std::string>>) { if (kind() == AttrValueKind::StringList) return asStringList(); }
        return std::nullopt;
    }

private:
    friend class GraphPayload;
    const AttributeValue* value_ = nullptr;
    const GraphPayload* payload_ = nullptr;
    uint32_t record_ = 0;
};

class AttrRef {
public:
    AttrRef() = default;
    AttrRef(std::string_view key, const AttributeValue* value) : key_(key), value_(value) {}
    AttrRef(std::string_view key, AttrValueView value) : key_(key), value_(value) {}

    std::string_view key() const noexcept { return key_; }
    AttrValueView value() const noexcept { return value_; }

private:
    std::string_view key_;
    AttrValueView value_;
};

class AttrRange {
public:
    class const_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = AttrRef;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = value_type;

        const_iterator() = default;
        const_iterator(const GraphPayload* payload, const uint32_t* records, const AttrKV* attrs, std::size_t index)
            : payload_(payload), records_(records), attrs_(attrs), index_(index) {}
        AttrRef operator*() const;
        const_iterator& operator++() { ++index_; return *this; }
        const_iterator operator++(int) { auto copy = *this; ++*this; return copy; }
        friend bool operator==(const_iterator lhs, const_iterator rhs) = default;
        friend bool operator!=(const_iterator lhs, const_iterator rhs) { return !(lhs == rhs); }

    private:
        const GraphPayload* payload_ = nullptr;
        const uint32_t* records_ = nullptr;
        const AttrKV* attrs_ = nullptr;
        std::size_t index_ = 0;
    };

    AttrRange() = default;
    explicit AttrRange(std::span<const AttrKV> attrs) : attrs_(attrs) {}
    AttrRange(const GraphPayload* payload, std::span<const uint32_t> records) : payload_(payload), records_(records) {}

    const_iterator begin() const noexcept { return {payload_, records_.data(), attrs_.data(), 0}; }
    const_iterator end() const noexcept { return {payload_, records_.data(), attrs_.data(), size()}; }
    std::size_t size() const noexcept { return payload_ ? records_.size() : attrs_.size(); }
    bool empty() const noexcept { return size() == 0; }
    AttrRef operator[](std::size_t index) const { return *const_iterator(payload_, records_.data(), attrs_.data(), index); }
    std::optional<AttrRef> find(std::string_view key) const noexcept;

private:
    std::span<const AttrKV> attrs_;
    const GraphPayload* payload_ = nullptr;
    std::span<const uint32_t> records_;
};


// Graph-owned payload pools shared by the entity columns in GraphStore.
// Records contain integer references to strings and variable-width values.
class GraphPayload {
public:
    using StringId = uint32_t;
    using RecordId = uint32_t;

    struct Slice {
        uint32_t offset = 0;
        uint32_t count = 0;
    };

    struct AttributeRecord {
        StringId key = 0;
        AttrValueKind kind = AttrValueKind::Bool;
        uint64_t payload = 0;
        Slice slice{};
    };

    struct SrcLocRecord {
        StringId file = 0;
        StringId origin = 0;
        StringId pass = 0;
        StringId note = 0;
        uint32_t line = 0;
        uint32_t column = 0;
        uint32_t endLine = 0;
        uint32_t endColumn = 0;
        friend bool operator==(const SrcLocRecord&, const SrcLocRecord&) = default;
    };

    struct Stats {
        std::size_t operations = 0;
        std::size_t values = 0;
        std::size_t operands = 0;
        std::size_t results = 0;
        std::size_t uses = 0;
        std::size_t strings = 0;
        std::size_t attributes = 0;
        std::size_t srcLocs = 0;
        std::size_t int64Payload = 0;
        std::size_t doublePayload = 0;
        std::size_t bitWords = 0;
        std::size_t stringListPayload = 0;
        std::size_t shapes = 0;
        std::size_t shapeDims = 0;
        std::size_t useSlots = 0;
        std::size_t operandIndexSlots = 0;
        std::size_t semanticIds = 0;
        std::size_t semanticLists = 0;
    };

    explicit GraphPayload(std::shared_ptr<detail::StringPool> strings = {});
    GraphPayload(const GraphPayload&) = delete;
    GraphPayload& operator=(const GraphPayload&) = delete;
    GraphPayload(GraphPayload&&) noexcept = default;
    GraphPayload& operator=(GraphPayload&&) noexcept = default;

    StringId internString(std::string_view text);
    StringId internAttrKey(std::string_view text);
    std::string_view string(StringId id) const noexcept;
    RecordId internAttribute(std::string_view key, const AttributeValue& value);
    RecordId internAttribute(std::string_view key, AttrValueView value);
    RecordId internStringListAttribute(std::string_view key, std::span<const StringId> values);
    RecordId internSrcLoc(const SrcLoc& loc);
    RecordId internSrcLocView(SrcLocView loc);
    RecordId importSrcLoc(const GraphPayload& source, RecordId id);
    AttributeRecord attribute(RecordId id) const;
    const SrcLocRecord& srcLoc(RecordId id) const;
    std::optional<SrcLoc> srcLocValue(RecordId id) const;
    std::span<const int64_t> int64Payload(Slice slice) const noexcept;
    std::span<const double> doublePayload(Slice slice) const noexcept;
    std::span<const uint64_t> bitPayload(Slice slice) const noexcept;
    std::span<const StringId> stringListPayload(Slice slice) const noexcept;
    Stats stats() const noexcept;

    // Releases excess capacity while preserving all record IDs. GraphStore
    // reimports live records into fresh pools to reclaim unreachable payloads.
    void compact();

private:
    friend class GraphStore;
    friend class Graph;
    RecordId internIntListAttribute(std::string_view key, std::span<const int64_t> values);
    void replaceAttribute(RecordId target, RecordId replacement);
    struct SrcLocHash {
        std::size_t operator()(const SrcLocRecord& loc) const noexcept;
    };
    slang::flat_hash_map<SrcLocRecord, RecordId, SrcLocHash> srcLocIds_;
    std::shared_ptr<detail::StringPool> stringPool_;
    std::vector<StringId> attrKeys_;
    std::vector<AttrValueKind> attrTags_;
    std::vector<uint64_t> attrPayloads_;
    std::vector<SrcLocRecord> srcLocs_;
    detail::PayloadPool<int64_t> int64Payload_;
    detail::PayloadPool<double> doublePayload_;
    detail::PayloadPool<uint64_t> bitPayload_;
    detail::PayloadPool<StringId> stringListPayload_;

};

namespace detail
{
    class InlineValueUsers
    {
    public:
        InlineValueUsers() = default;

        InlineValueUsers(const InlineValueUsers &other)
        {
            assign(other.span());
        }

        InlineValueUsers &operator=(const InlineValueUsers &other)
        {
            if (this != &other)
            {
                assign(other.span());
            }
            return *this;
        }

        InlineValueUsers(InlineValueUsers &&other) noexcept
        {
            moveFrom(std::move(other));
        }

        InlineValueUsers &operator=(InlineValueUsers &&other) noexcept
        {
            if (this != &other)
            {
                moveFrom(std::move(other));
            }
            return *this;
        }

        void assign(std::span<const ValueUser> users)
        {
            if (users.size() <= kInlineCapacity)
            {
                usingInline_ = true;
                inlineSize_ = users.size();
                for (std::size_t i = 0; i < inlineSize_; ++i)
                {
                    inline_[i] = users[i];
                }
                heap_.clear();
                return;
            }
            usingInline_ = false;
            inlineSize_ = 0;
            heap_.assign(users.begin(), users.end());
        }

        void clear()
        {
            usingInline_ = true;
            inlineSize_ = 0;
            heap_.clear();
        }

        std::span<const ValueUser> span() const noexcept
        {
            if (usingInline_)
            {
                return std::span<const ValueUser>(inline_.data(), inlineSize_);
            }
            return std::span<const ValueUser>(heap_.data(), heap_.size());
        }

        std::size_t size() const noexcept
        {
            return usingInline_ ? inlineSize_ : heap_.size();
        }

    private:
        void moveFrom(InlineValueUsers &&other) noexcept
        {
            usingInline_ = other.usingInline_;
            inlineSize_ = other.inlineSize_;
            if (usingInline_)
            {
                inline_ = other.inline_;
                heap_.clear();
            }
            else
            {
                heap_ = std::move(other.heap_);
            }
            other.clear();
        }

        static constexpr std::size_t kInlineCapacity = 4;
        std::array<ValueUser, kInlineCapacity> inline_{};
        std::vector<ValueUser> heap_{};
        std::size_t inlineSize_ = 0;
        bool usingInline_ = true;
    };
} // namespace detail

class GraphStore : public GraphPayload {
public:
    explicit GraphStore(GraphId graphId);
    GraphStore(GraphSymbolTable& symbols, GraphId graphId = GraphId{1, 0});
    void compact();
    Stats stats() const noexcept;
    std::span<const OperationId> operations() const noexcept;
    std::span<const ValueId> values() const noexcept;

    void reserveValues(std::size_t count);
    void reserveOperations(std::size_t count);
    void reserveSymbols(std::size_t count);
    void reserveOpOperands(OperationId op, std::size_t count);
    void reserveOpResults(OperationId op, std::size_t count);
    void reserveOpAttrs(OperationId op, std::size_t count);

    ValueId addValue(SymbolId sym, int32_t width, bool isSigned,
                     ValueType type = ValueType::Logic,
                     std::span<const int64_t> shape = {},
                     LogicDomain domain = LogicDomain::FourState,
                     bool allowInvalidSymbol = false);
    OperationId addOp(OperationKind kind, SymbolId sym, bool allowInvalidSymbol = false);
    void addOperand(OperationId op, ValueId value);
    void addResult(OperationId op, ValueId value);
    void insertOperand(OperationId op, std::size_t index, ValueId value);
    void insertResult(OperationId op, std::size_t index, ValueId value);
    void replaceOperand(OperationId op, std::size_t index, ValueId value);
    void replaceResult(OperationId op, std::size_t index, ValueId value);
    void replaceAllUses(ValueId from, ValueId to);
    bool eraseOperand(OperationId op, std::size_t index);
    bool eraseResult(OperationId op, std::size_t index);
    bool eraseOp(OperationId op);
    bool eraseOp(OperationId op, std::span<const ValueId> replacementResults);
    bool eraseOpUnchecked(OperationId op);
    bool eraseValue(ValueId value);
    bool eraseValueUnchecked(ValueId value);
    void bindInputPort(std::string_view name, ValueId value);
    void bindOutputPort(std::string_view name, ValueId value);
    void bindInoutPort(std::string_view name, ValueId in, ValueId out, ValueId oe);
    void bindInputPorts(std::span<const Port> ports);
    void bindOutputPorts(std::span<const Port> ports);
    void bindInoutPorts(std::span<const InoutPort> ports);
    bool removeInputPort(std::string_view name);
    bool removeOutputPort(std::string_view name);
    bool removeInoutPort(std::string_view name);
    void setAttr(OperationId op, std::string_view key, AttributeValue value);
    void setAttrView(OperationId op, std::string_view key, AttrValueView value);
    // Write canonical FIRRTL node attributes, replacing existing keys while
    // preserving their deterministic ordering.
    void setCanonicalNodeAttrs(OperationId op, std::string_view canonicalKey,
                               bool root = false, int64_t rootResult = 0);
    void setOpKind(OperationId op, OperationKind kind);
    bool eraseAttr(OperationId op, std::string_view key);
    void setValueSrcLoc(ValueId value, SrcLoc loc);
    void setValueSrcLocView(ValueId value, SrcLocView loc);
    // Backfill the array shape of a value created scalar. Only allowed while
    // the current shape is empty; the new shape must satisfy the usual
    // invariants (positive dims, product divides width, Logic only). Shape is
    // a pure structural view and never changes op semantics.
    void setValueShape(ValueId value, std::span<const int64_t> shape);
    void setOpSrcLoc(OperationId op, SrcLoc loc);
    void setOpSrcLocView(OperationId op, SrcLocView loc);
    // Validate both destinations before assigning one shared provenance record.
    void setOpAndValueSrcLocView(OperationId op, ValueId value, SrcLocView loc);
    void setOpSymbol(OperationId op, SymbolId sym);
    void setValueSymbol(ValueId value, SymbolId sym);
    void clearOpSymbol(OperationId op);
    void clearValueSymbol(ValueId value);

private:
    friend class Graph;
    struct ShapePool {
        std::vector<int64_t> dims;
        std::vector<Range> ranges{{0, 0}};
        slang::flat_hash_map<std::size_t, RecordId, std::hash<std::size_t>> ids;
        std::vector<RecordId> hashNext{0};

        std::span<const int64_t> shape(RecordId id) const {
            const Range range = ranges.at(id);
            return range.count ? std::span<const int64_t>(dims.data() + range.offset, range.count)
                               : std::span<const int64_t>{};
        }
        RecordId intern(std::span<const int64_t> value) {
            if (value.empty()) return 0;
            std::size_t hash = value.size();
            for (int64_t dim : value)
                hash ^= std::hash<int64_t>{}(dim) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
            const auto found = ids.find(hash);
            const RecordId first = found == ids.end() ? 0 : found->second;
            for (RecordId current = first; current; current = hashNext[current]) {
                const auto candidate = shape(current);
                if (candidate.size() == value.size() && std::equal(candidate.begin(), candidate.end(), value.begin()))
                    return current;
            }
            // A new shape may be a subspan of an existing shape in this
            // same pool. Snapshot that range before insert can reallocate it.
            std::vector<int64_t> aliased;
            const std::less<const int64_t*> before;
            if (!dims.empty() && !before(value.data(), dims.data()) &&
                before(value.data(), dims.data() + dims.size())) {
                aliased.assign(value.begin(), value.end());
                value = aliased;
            }
            const RecordId id = static_cast<RecordId>(ranges.size());
            const std::size_t offset = dims.size();
            dims.insert(dims.end(), value.begin(), value.end());
            ranges.push_back({offset, value.size()});
            hashNext.push_back(first);
            ids.insert_or_assign(hash, id);
            return id;
        }
    };
    struct ShapeRef {
        ShapePool& pool;
        RecordId& id;
        void set(std::span<const int64_t> shape) { id = pool.intern(shape); }
        ShapeRef& operator=(const std::vector<int64_t>& shape) { set(shape); return *this; }
        std::span<const int64_t> span() const { return pool.shape(id); }
        operator std::span<const int64_t>() const { return span(); }
        operator std::vector<int64_t>() const { const auto view = span(); return {view.begin(), view.end()}; }
        auto begin() const { return span().begin(); }
        auto end() const { return span().end(); }
        std::size_t size() const { return span().size(); }
        bool empty() const { return span().empty(); }
    };
    struct TopologyPools {
        std::vector<ValueId> operands;
        std::vector<ValueId> results;
        std::vector<std::unique_ptr<std::vector<ValueId>>> retainedEdges;
        std::vector<std::size_t> operandUseIndices;
        std::vector<ValueUser> users;
        ShapePool shapes;
        std::vector<RecordId> attrs;
        std::vector<SemanticId> semantics;
        std::vector<Range> semanticLists{{0, 0}};
        slang::flat_hash_map<std::size_t, uint32_t, std::hash<std::size_t>> semanticListIds;
        std::vector<uint32_t> semanticHashNext{0};
    };
    struct ValueRow {
        SymbolId& symbol;
        int32_t& width;
        uint8_t& isSigned;
        ValueType& type;
        LogicDomain& domain;
        ShapeRef shape;
        uint8_t& isInput;
        uint8_t& isOutput;
        uint8_t& isInout;
        OperationId& definingOp;
        RecordId& srcLoc;
        Range& semantics;
        uint8_t& alive;
    };
    struct ValueColumns {
        std::shared_ptr<TopologyPools> pools_;
        explicit ValueColumns(std::shared_ptr<TopologyPools> pools) : pools_(std::move(pools)) {}
        std::vector<SymbolId> symbol_;
        detail::PagedColumn<int32_t> width_;
        detail::PagedColumn<uint8_t> isSigned_;
        detail::PagedColumn<ValueType> type_;
        detail::PagedColumn<LogicDomain> domain_;
        std::vector<RecordId> shape_;
        std::vector<uint8_t> isInput_;
        std::vector<uint8_t> isOutput_;
        std::vector<uint8_t> isInout_;
        std::vector<OperationId> definingOp_;
        std::vector<RecordId> srcLoc_;
        std::vector<Range> semantics_;
        std::vector<uint8_t> alive_;
        std::size_t size() const noexcept { return symbol_.size(); }
        void reserve(std::size_t count) {
            symbol_.reserve(count);
            width_.reserve(count);
            isSigned_.reserve(count);
            type_.reserve(count);
            domain_.reserve(count);
            shape_.reserve(count);
            isInput_.reserve(count);
            isOutput_.reserve(count);
            isInout_.reserve(count);
            definingOp_.reserve(count);
            srcLoc_.reserve(count);
            semantics_.reserve(count);
            alive_.reserve(count);
        }
        ValueRow operator[](std::size_t index) const {
            return {const_cast<SymbolId&>(symbol_[index]), const_cast<int32_t&>(width_[index]), const_cast<uint8_t&>(isSigned_[index]), const_cast<ValueType&>(type_[index]), const_cast<LogicDomain&>(domain_[index]), ShapeRef{pools_->shapes, const_cast<RecordId&>(shape_[index])}, const_cast<uint8_t&>(isInput_[index]), const_cast<uint8_t&>(isOutput_[index]), const_cast<uint8_t&>(isInout_[index]), const_cast<OperationId&>(definingOp_[index]), const_cast<RecordId&>(srcLoc_[index]), const_cast<Range&>(semantics_[index]), const_cast<uint8_t&>(alive_[index])};
        }
        ValueRow at(std::size_t index) const { if (index >= size()) throw std::out_of_range("graph column index"); return (*this)[index]; }
        ValueRow emplace_back() {
            symbol_.emplace_back();
            width_.emplace_back(0);
            isSigned_.emplace_back(0);
            type_.emplace_back(ValueType::Logic);
            domain_.emplace_back(LogicDomain::FourState);
            shape_.emplace_back(0);
            isInput_.emplace_back(0);
            isOutput_.emplace_back(0);
            isInout_.emplace_back(0);
            definingOp_.emplace_back(OperationId::invalid());
            srcLoc_.emplace_back();
            semantics_.emplace_back();
            alive_.emplace_back(1);
            return (*this)[size() - 1];
        }
    };
    struct OperationRow {
        OperationKind& kind;
        SymbolId& symbol;
        detail::PoolVector<ValueId>& operands;
        detail::PoolVector<std::size_t>& operandUseIndices;
        detail::PoolVector<ValueId>& results;
        detail::PoolVector<RecordId>& attrs;
        RecordId& srcLoc;
        Range& semantics;
        uint8_t& alive;
    };
    struct OperationColumns {
        std::shared_ptr<TopologyPools> pools_;
        explicit OperationColumns(std::shared_ptr<TopologyPools> pools) : pools_(std::move(pools)) {}
        detail::PagedColumn<OperationKind> kind_;
        std::vector<SymbolId> symbol_;
        std::vector<detail::PoolVector<ValueId>> operands_;
        std::vector<detail::PoolVector<std::size_t>> operandUseIndices_;
        std::vector<detail::PoolVector<ValueId>> results_;
        std::vector<detail::PoolVector<RecordId>> attrs_;
        std::vector<RecordId> srcLoc_;
        std::vector<Range> semantics_;
        std::vector<uint8_t> alive_;
        std::size_t size() const noexcept { return kind_.size(); }
        void reserve(std::size_t count) {
            kind_.reserve(count);
            symbol_.reserve(count);
            operands_.reserve(count);
            operandUseIndices_.reserve(count);
            results_.reserve(count);
            attrs_.reserve(count);
            srcLoc_.reserve(count);
            semantics_.reserve(count);
            alive_.reserve(count);
        }
        OperationRow operator[](std::size_t index) const {
            return {const_cast<OperationKind&>(kind_[index]), const_cast<SymbolId&>(symbol_[index]), const_cast<detail::PoolVector<ValueId>&>(operands_[index]), const_cast<detail::PoolVector<std::size_t>&>(operandUseIndices_[index]), const_cast<detail::PoolVector<ValueId>&>(results_[index]), const_cast<detail::PoolVector<RecordId>&>(attrs_[index]), const_cast<RecordId&>(srcLoc_[index]), const_cast<Range&>(semantics_[index]), const_cast<uint8_t&>(alive_[index])};
        }
        OperationRow at(std::size_t index) const { if (index >= size()) throw std::out_of_range("graph column index"); return (*this)[index]; }
        OperationRow emplace_back() {
            kind_.emplace_back(OperationKind::kConstant);
            symbol_.emplace_back();
            operands_.emplace_back(pools_->operands);
            operandUseIndices_.emplace_back(pools_->operandUseIndices);
            results_.emplace_back(pools_->results);
            attrs_.emplace_back(pools_->attrs);
            srcLoc_.emplace_back();
            semantics_.emplace_back();
            alive_.emplace_back(1);
            return (*this)[size() - 1];
        }
    };

    enum class SymbolKind {
        kValue,
        kOperation
    };

    struct SymbolBinding {
        SymbolKind kind;
        uint32_t index = 0;
    };

    std::size_t valueIndex(ValueId value) const;
    std::size_t opIndex(OperationId op) const;
    void reserveSemanticIds(std::size_t additional);
    Range storeSemanticIds(std::span<const SemanticId> ids);
    void setAttrRecord(std::size_t opIndex, std::string_view key, RecordId record);
    bool valueAlive(ValueId value) const;
    bool opAlive(OperationId op) const;
    std::size_t countValueUses(ValueId value, std::optional<OperationId> skipOp) const;
    void addValueUser(ValueId value, OperationId op, std::size_t operandIndex);
    void removeValueUser(ValueId value, OperationId op, std::size_t operandIndex);
    void removeOpUses(OperationId op, std::span<const ValueId> operands);
    void addOpUses(OperationId op, std::span<const ValueId> operands);
    void replaceAllUsesInternal(ValueId from, ValueId to, std::optional<OperationId> skipOp);
    void recomputePortFlags();
    void validateSymbol(SymbolId sym, std::string_view context) const;
    void bindSymbol(SymbolId sym, SymbolKind kind, uint32_t index, std::string_view context);
    void unbindSymbol(SymbolId sym, SymbolKind kind, uint32_t index);
    void bindPort(std::vector<Port>& ports, std::string_view name, ValueId value, std::string_view context);
    void bindInoutPort(std::vector<InoutPort>& ports, std::string_view name, ValueId in, ValueId out,
                       ValueId oe, std::string_view context);
    bool removePort(std::vector<Port>& ports, std::string_view name, std::string_view context);
    bool removeInoutPort(std::vector<InoutPort>& ports, std::string_view name, std::string_view context);

    mutable std::vector<OperationId> liveOperations_;
    mutable std::vector<ValueId> liveValues_;
    mutable bool operationsHaveTombstones_ = false;
    mutable bool valuesHaveTombstones_ = false;
    GraphId graphId_;
    GraphSymbolTable* symbols_ = nullptr;
    std::shared_ptr<TopologyPools> topology_;
    ValueColumns values_;
    std::vector<detail::PoolVector<ValueUser>> valueUsers_;
    OperationColumns operations_;
    std::vector<Port> inputPorts_;
    std::vector<Port> outputPorts_;
    std::vector<InoutPort> inoutPorts_;
    slang::flat_hash_map<uint32_t, SymbolBinding, std::hash<uint32_t>> symbolIndex_;
};

class Value {
public:
    const ValueId& id() const noexcept { return id_; }
    SymbolId symbol() const noexcept { return symbol_; }
    std::string_view symbolText() const noexcept { return symbolText_; }
    int32_t width() const noexcept { return width_; }
    bool isSigned() const noexcept { return isSigned_; }
    ValueType type() const noexcept { return type_; }
    LogicDomain domain() const noexcept { return domain_; }
    LogicalType logicalType() const { return {type_, width_, isSigned_, domain_, shape_}; }
    // Array structural view; empty = scalar. `width()` stays the total bit
    // width, so element width = width() / product(shape()).
    std::span<const int64_t> shape() const noexcept { return shape_; }
    bool isInput() const noexcept { return isInput_; }
    bool isOutput() const noexcept { return isOutput_; }
    bool isInout() const noexcept { return isInout_; }
    OperationId definingOp() const noexcept { return definingOp_; }
    std::span<const ValueUser> users() const noexcept;
    const std::optional<SrcLoc>& srcLoc() const noexcept { return srcLoc_; }

private:
    friend class Graph;

    Value(ValueId id, SymbolId symbol, std::string symbolText, int32_t width, bool isSigned,
          ValueType type, LogicDomain domain, std::vector<int64_t> shape, bool isInput, bool isOutput, bool isInout,
          OperationId definingOp, std::span<const ValueUser> users,
          std::optional<SrcLoc> srcLoc, const Graph* owner, bool usersLoaded);

    ValueId id_{};
    SymbolId symbol_{};
    std::string symbolText_;
    int32_t width_ = 0;
    bool isSigned_ = false;
    ValueType type_ = ValueType::Logic;
    LogicDomain domain_ = LogicDomain::FourState;
    std::vector<int64_t> shape_;
    bool isInput_ = false;
    bool isOutput_ = false;
    bool isInout_ = false;
    OperationId definingOp_{};
    mutable detail::InlineValueUsers users_;
    mutable bool usersLoaded_ = true;
    const Graph* owner_ = nullptr;
    std::optional<SrcLoc> srcLoc_;
};

class Operation {
public:
    const OperationId& id() const noexcept { return id_; }
    OperationKind kind() const noexcept { return kind_; }
    SymbolId symbol() const noexcept { return symbol_; }
    std::string_view symbolText() const noexcept { return symbolText_; }
    std::span<const ValueId> operands() const noexcept { return std::span<const ValueId>(operands_.data(), operands_.size()); }
    std::span<const ValueId> results() const noexcept { return std::span<const ValueId>(results_.data(), results_.size()); }
    std::span<const AttrKV> attrs() const noexcept { return std::span<const AttrKV>(attrs_.data(), attrs_.size()); }
    std::optional<AttributeValue> attr(std::string_view key) const;
    const std::optional<SrcLoc>& srcLoc() const noexcept { return srcLoc_; }

private:
    friend class Graph;

    Operation(OperationId id, OperationKind kind, SymbolId symbol, std::string symbolText,
              std::vector<ValueId> operands, std::vector<ValueId> results,
              std::vector<AttrKV> attrs, std::optional<SrcLoc> srcLoc);

    OperationId id_{};
    OperationKind kind_{};
    SymbolId symbol_{};
    std::string symbolText_;
    std::vector<ValueId> operands_;
    std::vector<ValueId> results_;
    std::vector<AttrKV> attrs_;
    std::optional<SrcLoc> srcLoc_;
};

// Register port-family helpers (see docs/grh/grh-ir.md "Registers").
//
// `isRegisterReadPortKind` covers kRegisterReadPort (whole read) and
// kRegisterReadArrayPort (indexed read). `isRegisterWritePortKind` covers
// kRegisterWritePort / kRegisterWriteMaskPort / kRegisterWriteArrayPort /
// kRegisterWriteArrayMaskPort / kRegisterFillPort.
[[nodiscard]] bool isRegisterReadPortKind(OperationKind kind) noexcept;
[[nodiscard]] bool isRegisterWritePortKind(OperationKind kind) noexcept;
// Operand index where the event operands of a write-port op begin. Event
// operands always trail and correspond one-to-one to the `eventEdge` attr;
// for the array write ports the index count varies with partial indexing, so
// the boundary is recovered from the eventEdge count (it is not derivable
// from the declaration rank alone). Fixed-header kinds return their constant
// header size. Returns nullopt for non-write-port kinds and for malformed
// array write ports (missing eventEdge attr or too few operands).
[[nodiscard]] std::optional<std::size_t> firstEventOperandIndex(const Operation &op);
[[nodiscard]] std::optional<std::size_t> firstEventOperandIndex(const Graph &graph, OperationId op);
// Number of index operands of an array port op (kRegisterReadArrayPort /
// kRegisterWriteArrayPort / kRegisterWriteArrayMaskPort); nullopt otherwise.
[[nodiscard]] std::optional<std::size_t> arrayPortIndexCount(const Operation &op);
[[nodiscard]] std::optional<std::size_t> arrayPortIndexCount(const Graph &graph, OperationId op);

class Design;
namespace detail { class BlockExecutor; }

class Graph {
public:
    // Reuse pooled provenance imports for one source/destination graph pair.
    // Ordinary mutations may append records. Compacting either graph or
    // reusing this context with another pair automatically clears its maps.
    // Weak ownership does not keep either graph's storage alive.
    class SrcLocImportContext {
    public:
        SrcLocImportContext() = default;
    private:
        friend class Graph;
        std::weak_ptr<GraphStore> source_, target_;
        uint64_t sourceEpoch_ = 0, targetEpoch_ = 0;
        std::vector<GraphPayload::RecordId> records_;
        std::shared_ptr<const std::vector<GraphPayload::StringId>> blockStrings_;
        slang::flat_hash_map<GraphPayload::StringId, GraphPayload::StringId,
                             std::hash<GraphPayload::StringId>> strings_;
    };

    // Per-definition payload relocation, shared by all instances in one
    // flatten. Attribute records remain private to each destination op, while
    // immutable list buffers retain source ownership. String IDs are remapped
    // lazily so only text referenced by imported payloads is interned.
    class BlockImportContext {
    public:
        BlockImportContext() = default;
    private:
        friend class Graph;
        std::weak_ptr<GraphStore> source_, target_;
        uint64_t sourceEpoch_ = 0, targetEpoch_ = 0, sourceMutation_ = 0;
        bool prepared_ = false;
        std::vector<GraphPayload::AttributeRecord> attributes_;
        std::shared_ptr<std::vector<GraphPayload::StringId>> strings_;
    };
    struct OperationBlockImport {
        const Graph* source = nullptr;
        std::span<const OperationId> sourceOps;
        std::span<const OperationId> targetOps;
        BlockImportContext* context = nullptr;
    };

    // Provenance annotation for declarations inside generate scopes: one group
    // per generate-scope declaration, holding the SymbolIds of its per-round
    // elaboration copies (ingest emits them in round order). Pure metadata;
    // see docs/grh/grh-ir.md 4.3.2 for the maintenance contract.
    struct GenerateGroup {
        SymbolId scope;                // generate scope path text, '$'-joined (e.g. "gen_loop", nested "outer$inner")
        SymbolId name;                 // bare declaration name (e.g. "sig")
        std::vector<SymbolId> symbols; // per-round copy symbols, index = generate-for round
    };

    Graph(Design& owner, std::string symbol, GraphId graphId);
    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;
    Graph(Graph&&) = delete;
    Graph& operator=(Graph&&) = delete;

    const std::string& symbol() const noexcept { return symbol_; }
    const GraphId& id() const noexcept { return graphId_; }
    Design& owner() const noexcept { return *owner_; }

    std::span<const AttrKV> metadata() const noexcept { return metadata_; }
    std::optional<AttributeValue> metadata(std::string_view key) const;
    void setMetadata(std::string_view key, AttributeValue value);
    bool eraseMetadata(std::string_view key);

    GraphSymbolTable& symbols() noexcept { return symbols_; }
    const GraphSymbolTable& symbols() const noexcept { return symbols_; }
    void reserveSymbolCapacity(std::size_t count);
    void reserveDeclaredSymbolCapacity(std::size_t count);
    void reserveValueCapacity(std::size_t count);
    void reserveOperationCapacity(std::size_t count);
    void reserveOpOperandCapacity(OperationId op, std::size_t count);
    void reserveOpResultCapacity(OperationId op, std::size_t count);
    void reserveOpAttrCapacity(OperationId op, std::size_t count);
    SymbolId internSymbol(std::string_view text);
    SymbolId lookupSymbol(std::string_view text) const;
    std::string_view symbolText(SymbolId id) const;
    static std::string normalizeComponent(std::string_view text)
    {
        std::string out;
        out.reserve(text.size());
        for (unsigned char ch : text)
        {
            if (std::isalnum(ch) || ch == '_')
            {
                out.push_back(static_cast<char>(ch));
            }
            else
            {
                out.push_back('_');
            }
        }
        return out;
    }
    static std::string makeInternalBase(std::string_view kind)
    {
        std::string base;
        base.reserve(kind.size() + 2);
        base.push_back('_');
        base.append(kind);
        return base;
    }
    SymbolId makeInternalOpSym();
    SymbolId makeInternalValSym();
    void addDeclaredSymbol(SymbolId sym);
    bool removeDeclaredSymbol(SymbolId sym);
    void clearDeclaredSymbols();
    bool isDeclaredSymbol(SymbolId sym) const noexcept;
    std::span<const SymbolId> declaredSymbols() const noexcept;
    std::size_t addGenerateGroup(SymbolId scope, SymbolId name);
    void addGenerateGroupSymbol(std::size_t group, SymbolId symbol);
    const std::vector<GenerateGroup>& generateGroups() const noexcept;
    // Debug consistency check over the provenance annotations: reports one
    // entry per dangling declaredSymbol or generateGroup member (a member is
    // dangling when its symbol no longer resolves to a live value/operation).
    // Returns an empty vector when the annotations are consistent.
    std::vector<std::string> validateDeclaredSymbols() const;

    bool frozen() const noexcept { return compacted_; }
    // Compact live columns and pools into canonical dense entity order.
    // This is the public compaction boundary used by ingest/flatten; it keeps
    // the historical freeze name and ID/JSON ordering contract.
    void compact();
    // Prepare a safe source in a prefix worker. A one-shot, nonserialized
    // receipt lets flatten report the same source-freeze work at its boundary.
    void compactForFlatten();
    bool consumePreparedFlattenFreeze() noexcept;
    void freeze();
    [[nodiscard]] GraphStore::Stats payloadStats() const noexcept;
    // Non-serialized proof used by ingest's linked cycle-check gate. Only a
    // successful checker may mark it; subsequent graph mutations invalidate it.
    bool localCyclesChecked() const noexcept {
        return localCycleProof_ && *localCycleProof_ == mutationEpoch_;
    }
    void markLocalCyclesChecked() noexcept { localCycleProof_ = mutationEpoch_; }
    [[nodiscard]] const GraphStore& payloadStore() const noexcept { return *store_; }

    SemanticTree* semanticTree() noexcept { return semanticTree_ ? &*semanticTree_ : nullptr; }
    const SemanticTree* semanticTree() const noexcept { return semanticTree_ ? &*semanticTree_ : nullptr; }
    SemanticTree& getOrCreateSemanticTree(std::string sourceUnit, std::string circuit,
                                          std::string module);
    SemanticId appendSemanticTree(const Graph& source, SemanticId parent,
                                  std::string_view rootName);

    std::span<const OperationId> operations() const;
    std::span<const ValueId> values() const;
    std::span<const Port> inputPorts() const;
    std::span<const Port> outputPorts() const;
    std::span<const InoutPort> inoutPorts() const;

    ValueId createValue(SymbolId symbol, int32_t width, bool isSigned,
                        ValueType type = ValueType::Logic,
                        std::span<const int64_t> shape = {},
                        LogicDomain domain = LogicDomain::FourState);
    ValueId createValue(int32_t width, bool isSigned,
                        ValueType type = ValueType::Logic,
                        std::span<const int64_t> shape = {},
                        LogicDomain domain = LogicDomain::FourState);
    ValueId createValue(SymbolId symbol, const LogicalType& type);
    ValueId createValue(const LogicalType& type);
    // Create an implementation temporary without reserving a text symbol.
    // Temporary entities are identified by their ValueId/OperationId and only
    // acquire a display name at a serialization or downstream boundary.
    ValueId createTemporaryValue(SymbolId symbol, int32_t width, bool isSigned,
                                 ValueType type = ValueType::Logic,
                                 std::span<const int64_t> shape = {},
                                 LogicDomain domain = LogicDomain::FourState);
    ValueId createTemporaryValue(int32_t width, bool isSigned,
                                 ValueType type = ValueType::Logic,
                                 std::span<const int64_t> shape = {},
                                 LogicDomain domain = LogicDomain::FourState);
    ValueId createTemporaryValue(SymbolId symbol, const LogicalType& type);
    ValueId createTemporaryValue(const LogicalType& type);
    OperationId createOperation(OperationKind kind, SymbolId symbol);
    OperationId createOperation(OperationKind kind);
    OperationId createTemporaryOperation(OperationKind kind);
    void assignWriterOrder(OperationId op, std::string_view symbol);

    // Stable, generated names for diagnostics and serialization.  A valid
    // symbol returns its text; an invalid symbol gets a deterministic name
    // derived from the current entity id without entering SymbolTable.
    std::string valueDisplayName(ValueId value) const;
    std::string operationDisplayName(OperationId op) const;
    // Materialize all temporary entities for a consumer whose ABI uses symbol
    // text as identity (for example GrhSIM IR).
    void materializeInternalSymbols();
    // result[i] = elements[i]; adds an outer dimension, including N=1.
    ValueId createArray(std::span<const ValueId> elements);

    ValueId findValue(SymbolId symbol) const noexcept;
    OperationId findOperation(SymbolId symbol) const noexcept;
    ValueId findValue(std::string_view symbol) const;
    OperationId findOperation(std::string_view symbol) const;
    SymbolId valueSymbol(ValueId value) const noexcept;
    SymbolId operationSymbol(OperationId op) const noexcept;
    int32_t valueWidth(ValueId value) const;
    bool valueSigned(ValueId value) const;
    ValueType valueType(ValueId value) const;
    LogicDomain valueDomain(ValueId value) const;
    LogicalType valueLogicalType(ValueId value) const;
    // Explicit semantic type changes; callers must update producers and users.
    void setValueLogicalType(ValueId value, const LogicalType& type);
    // Array structural view of a value; empty span = scalar. See
    // valueShape for the layout contract.
    std::span<const int64_t> valueShape(ValueId value) const;
    bool valueIsInput(ValueId value) const;
    bool valueIsOutput(ValueId value) const;
    bool valueIsInout(ValueId value) const;
    OperationId valueDef(ValueId value) const;
    std::optional<SrcLoc> valueSrcLoc(ValueId value) const;
    std::span<const SemanticId> valueSemanticIds(ValueId value) const;
    void setValueSemanticIds(ValueId value, std::span<const SemanticId> ids);
    void addValueSemanticIds(ValueId value, std::span<const SemanticId> ids);
    void mergeValueSemanticIds(ValueId target, ValueId source);
    OperationKind opKind(OperationId op) const;
    std::span<const ValueId> opOperands(OperationId op) const;
    std::span<const ValueId> opResults(OperationId op) const;
    std::span<const SemanticId> opSemanticIds(OperationId op) const;
    void setOpSemanticIds(OperationId op, std::span<const SemanticId> ids);
    void addOpSemanticIds(OperationId op, std::span<const SemanticId> ids);
    void mergeOpSemanticIds(OperationId target, OperationId source);
    AttrRange opAttrs(OperationId op) const;
    AttrRange opAttrRange(OperationId op) const;
    std::optional<SrcLoc> opSrcLoc(OperationId op) const;
    Value getValue(ValueId id) const;
    // Borrowed current use-list; invalidated by graph mutation or freeze.
    // Enumeration order is unspecified. Copy it before mutating uses.
    std::span<const ValueUser> valueUsers(ValueId value) const;
    Operation getOperation(OperationId id) const;

    void bindInputPort(std::string_view name, ValueId value);
    void bindOutputPort(std::string_view name, ValueId value);
    void bindInoutPort(std::string_view name, ValueId in, ValueId out, ValueId oe);
    void bindInputPorts(std::span<const Port> ports);
    void bindOutputPorts(std::span<const Port> ports);
    void bindInoutPorts(std::span<const InoutPort> ports);
    bool removeInputPort(std::string_view name);
    bool removeOutputPort(std::string_view name);
    bool removeInoutPort(std::string_view name);
    ValueId inputPortValue(std::string_view name) const noexcept;
    ValueId outputPortValue(std::string_view name) const noexcept;

    // Full value type equality, including shape. Does not alias or retype values.
    bool sameValueType(ValueId lhs, ValueId rhs) const;


    void addOperand(OperationId op, ValueId value);
    void addResult(OperationId op, ValueId value);
    void insertOperand(OperationId op, std::size_t index, ValueId value);
    void insertResult(OperationId op, std::size_t index, ValueId value);
    void replaceOperand(OperationId op, std::size_t index, ValueId value);
    void replaceResult(OperationId op, std::size_t index, ValueId value);
    void replaceAllUses(ValueId from, ValueId to);

    // Aliases `from`'s uses to `to` with type honesty: identical types alias
    // directly; otherwise a kAssign view typed like `from` is inserted so the
    // (bit-identical) retype stays explicit. Returns the value actually aliased
    // in place of `from`. Width-mismatched or invalid pairs throw via the
    // underlying replaceAllUses / view construction.
    ValueId replaceAllUsesWithView(ValueId from, ValueId to);

    bool eraseOperand(OperationId op, std::size_t index);
    bool eraseResult(OperationId op, std::size_t index);
    bool eraseOp(OperationId op);
    bool eraseOp(OperationId op, std::span<const ValueId> replacementResults);
    bool eraseOpUnchecked(OperationId op);
    bool eraseValue(ValueId value);
    bool eraseValueUnchecked(ValueId value);
    void setAttr(OperationId op, std::string_view key, AttributeValue value);
    void setAttrView(OperationId op, std::string_view key, AttrValueView value);
    // See GraphStore::setCanonicalNodeAttrs.
    void setCanonicalNodeAttrs(OperationId op, std::string_view canonicalKey,
                               bool root = false, int64_t rootResult = 0);
    void setOpKind(OperationId op, OperationKind kind);
    bool eraseAttr(OperationId op, std::string_view key);
    void setValueSrcLoc(ValueId value, SrcLoc loc);
    void setValueSrcLocView(ValueId value, SrcLocView loc);
    // Copy pooled provenance without materializing its owned strings.
    // An absent source location clears the destination location.
    void copyValueSrcLoc(ValueId value, const Graph& source, ValueId sourceValue);
    void copyValueSrcLoc(ValueId value, const Graph& source, ValueId sourceValue,
                         SrcLocImportContext& context);
    // See GraphStore::setValueShape.
    void setValueShape(ValueId value, std::span<const int64_t> shape);
    void setOpSrcLoc(OperationId op, SrcLoc loc);
    void setOpSrcLocView(OperationId op, SrcLocView loc);
    void setOpAndValueSrcLocView(OperationId op, ValueId value, SrcLocView loc);
    void copyOpSrcLoc(OperationId op, const Graph& source, OperationId sourceOp);
    void copyOpSrcLoc(OperationId op, const Graph& source, OperationId sourceOp,
                      SrcLocImportContext& context);
    // Internal block import used by hierarchy flattening. Symbols are reserved
    // in legacy traversal order before column writes; ports are omitted from
    // sourceValues and supplied through the caller's relocation table.
    void appendValueBlock(const Graph& source, std::span<const ValueId> sourceValues,
                          std::span<const SymbolId> symbols, std::span<ValueId> output,
                          SrcLocImportContext& context, std::size_t threads = 1,
                          bool retainSourceColumns = false);
    void appendOperationBlock(const Graph& source, std::span<const OperationId> sourceOps,
                              std::span<const SymbolId> symbols, std::span<OperationId> output,
                              SrcLocImportContext& context, std::size_t threads = 1,
                              bool retainSourceColumns = false);
    std::size_t remapValueBlockSemantics(const Graph& source,
                                         std::span<const ValueId> sourceValues,
                                         std::span<const ValueId> targetValues,
                                         SemanticId copiedRoot);
    std::size_t remapOperationBlockSemantics(const Graph& source,
                                             std::span<const OperationId> sourceOps,
                                             std::span<const OperationId> targetOps,
                                             SemanticId copiedRoot);
    // Offsets have ops.size()+1 entries, begin at zero, and terminate at the
    // corresponding flat edge count. Results acquire their definitions now;
    // reverse uses must be rebuilt before querying or mutating imported edges.
    void setOperationBlockEdges(std::span<const OperationId> ops,
                                std::span<const uint32_t> operandOffsets,
                                std::span<const ValueId> operands,
                                std::span<const uint32_t> resultOffsets,
                                std::span<const ValueId> results);
    void rebuildUseLists();
    void setOperationBlockResults(std::span<const OperationId> ops,
                                  std::span<const uint32_t> resultOffsets,
                                  std::span<const ValueId> results);
    void setOperationBlockOperands(std::span<const OperationId> ops,
                                   std::span<const uint32_t> operandOffsets,
                                   std::span<const ValueId> operands);
    struct TopologyBlock {
        std::span<const OperationId> sourceOps;
        std::span<const OperationId> targetOps;
    };
    // Consume a published, exclusively used source after all metadata imports.
    // mappedValues is indexed by one-based source ValueId.index, with slot 0
    // unused. Retain its edge buffers, relocate live slices in place, and delete
    // the source graph. Rebuild uses before querying or mutating imported edges.
    void consumeBlockTopology(Graph& source, std::span<const TopologyBlock> blocks,
                              std::span<const ValueId> mappedValues);
    void prepareBlockImport(const Graph& source, BlockImportContext& context,
                            SrcLocImportContext& locations);
    void appendOperationBlockAttributes(const Graph& source,
                                         std::span<const OperationId> sourceOps,
                                         std::span<const OperationId> targetOps,
                                         BlockImportContext& context);
    void appendOperationBlocksAttributes(std::span<const OperationBlockImport> blocks,
                                          detail::BlockExecutor& executor);
    void setOperationBlockAttr(std::span<const OperationId> ops, std::string_view key,
                               AttrValueView value);
    void setOpSymbol(OperationId op, SymbolId sym);
    void setValueSymbol(ValueId value, SymbolId sym);
    void clearOpSymbol(OperationId op);
    void clearValueSymbol(ValueId value);

    void writeJson(slang::JsonWriter& writer, bool emitDefUsers = false,
                   bool emitSourceLocations = false) const;

private:
    friend class Design;
    friend class Value;

    GraphStore& mutableStore(bool affectsLocalCycles = true);
    GraphPayload::RecordId importSrcLoc(const Graph& source, GraphPayload::RecordId id,
                                       SrcLocImportContext& context);
    void copyValueSrcLoc(ValueId value, const Graph& source, ValueId sourceValue,
                         SrcLocImportContext* context);
    void copyOpSrcLoc(OperationId op, const Graph& source, OperationId sourceOp,
                      SrcLocImportContext* context);
    void cloneContentsTo(Graph& destination) const;
    void removeSymbolFromGenerateGroups(SymbolId sym) noexcept;
    // Alias replacement requires the complete value type to be identical.
    void guardShapeAlias(ValueId from, ValueId to, std::string_view api);
    Value valueFromStore(ValueId id) const;
    Operation operationFromStore(OperationId id) const;
    std::span<const ValueUser> valueUsersSpan(ValueId id) const noexcept;

    Design* owner_;
    std::string symbol_;
    GraphId graphId_{};
    std::vector<AttrKV> metadata_;
    GraphSymbolTable symbols_;
    std::shared_ptr<GraphStore> store_;
    std::optional<SemanticTree> semanticTree_;
    bool compacted_ = true;
    uint64_t srcLocEpoch_ = 0;
    uint64_t mutationEpoch_ = 0;
    std::optional<uint64_t> localCycleProof_;
    std::optional<uint64_t> preparedFlattenFreeze_;
    std::vector<SymbolId> declaredSymbols_;
    std::unordered_set<uint32_t> declaredSymbolSet_;
    std::vector<GenerateGroup> generateGroups_;
    std::unordered_map<uint32_t, std::vector<std::size_t>> generateGroupsBySymbol_;
    std::unordered_map<std::string, int64_t> nextWriterOrders_;
    bool writerOrdersInitialized_ = false;
    uint32_t nextInternalOpSym_ = 0;
    uint32_t nextInternalValSym_ = 0;
};

class Design {
public:
    class ParallelMutationScope {
    public:
        ~ParallelMutationScope() noexcept;
        ParallelMutationScope(const ParallelMutationScope&) = delete;
        ParallelMutationScope& operator=(const ParallelMutationScope&) = delete;
        ParallelMutationScope(ParallelMutationScope&&) = delete;
        ParallelMutationScope& operator=(ParallelMutationScope&&) = delete;

    private:
        friend class Design;
        explicit ParallelMutationScope(Design& design);
        Design& design_;
    };

    Design();
    Design(Design&& other) noexcept;
    Design& operator=(Design&& other) noexcept;
    Design(const Design&) = delete;
    Design& operator=(const Design&) = delete;

    uint64_t instanceId() const noexcept { return instanceId_; }
    uint64_t revision() const noexcept { return revision_; }

    // Create and destroy on the coordinating thread. Each worker exclusively
    // mutates one precreated graph; join all workers before scope destruction.
    // Do not mutate the design catalog, move the design, use revision caches,
    // or nest scopes while graph revisions are deferred.
    [[nodiscard]] ParallelMutationScope parallelGraphMutation();

    Graph& createGraph(std::string name);
    // Allocate and publish only on the coordinating thread. An unpublished
    // graph is absent from graphs()/graphOrder(); its owner must remain alive
    // and unmoved until publication or discard. Discarding reserves its name
    // and GraphId for the remaining lifetime of the design.
    [[nodiscard]] std::unique_ptr<Graph> createUnpublishedGraph(std::string name);
    Graph& publishGraph(std::unique_ptr<Graph> graph);
    Graph& cloneGraph(std::string_view sourceName, std::string newName);
    Graph& importGraph(const Graph& source, std::string newName);
    bool deleteGraph(std::string_view name);
    Design clone() const;
    Graph* findGraph(std::string_view name) noexcept;
    const Graph* findGraph(std::string_view name) const noexcept;
    SymbolId internSymbol(std::string_view text);
    SymbolId lookupSymbol(std::string_view text) const;
    std::string_view symbolText(SymbolId id) const;
    void addDeclaredSymbol(SymbolId sym);
    bool removeDeclaredSymbol(SymbolId sym);
    void clearDeclaredSymbols();
    bool isDeclaredSymbol(SymbolId sym) const noexcept;
    std::span<const SymbolId> declaredSymbols() const noexcept;
    std::vector<std::string> aliasesForGraph(std::string_view name) const;
    void registerGraphAlias(std::string alias, Graph& graph);

    void markAsTop(std::string_view graphName);
    void unmarkAsTop(std::string_view graphName);
    const std::vector<std::string>& topGraphs() const noexcept { return topGraphs_; }

    const std::unordered_map<std::string, std::unique_ptr<Graph>>& graphs() const noexcept { return graphs_; }
    const std::vector<std::string>& graphOrder() const noexcept { return graphOrder_; }
    // Sort traversal order without moving graphs or changing their IDs.
    void sortGraphOrder();

    static Design fromJsonString(std::string_view json);

private:
    friend class Graph;
    void touch() noexcept {
        if (!deferGraphRevisions_) ++revision_;
    }
    Graph& addGraphInternal(std::unique_ptr<Graph> graph);
    void resetGraphOwners();

    DesignSymbolTable designSymbols_;
    uint64_t instanceId_ = 0;
    uint64_t revision_ = 0;
    bool deferGraphRevisions_ = false;
    std::unordered_map<std::string, std::unique_ptr<Graph>> graphs_;
    std::unordered_map<std::string, std::string> graphAliasBySymbol_;
    std::vector<std::string> graphOrder_;
    std::vector<std::string> topGraphs_;
    std::vector<SymbolId> declaredSymbols_;
    std::unordered_set<uint32_t> declaredSymbolSet_;
};

} // namespace wolvrix::lib::grh

#endif // WOLVRIX_GRH_HPP
