#ifndef WOLVRIX_GRH_HPP
#define WOLVRIX_GRH_HPP

#include <array>
#include <cctype>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <span>
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
    kReplicate,
    kSliceStatic,
    kSliceDynamic,
    kSliceArray,
    kLatch,
    kLatchReadPort,
    kLatchWritePort,
    kRegister,
    kRegisterReadPort,
    kRegisterWritePort,
    kMemory,
    kMemoryReadPort,
    kMemoryWritePort,
    kMemoryFillPort,
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

inline constexpr std::string_view kMemoryWritePriorityGroupAttr = "memoryWrite.priorityGroup";
inline constexpr std::string_view kMemoryWritePriorityAttr = "memoryWrite.priority";

enum class ValueType : uint8_t {
    Logic,
    Real,
    String
};

std::string_view toString(ValueType type) noexcept;
std::optional<ValueType> parseValueType(std::string_view text) noexcept;

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

using DebugInfo = SrcLoc;

[[nodiscard]] bool attributeValueIsJsonSerializable(const AttributeValue &value);

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

    SymbolId intern(std::string_view text);
    SymbolId lookup(std::string_view text) const;
    bool contains(std::string_view text) const;
    std::string_view text(SymbolId id) const;
    bool valid(SymbolId id) const noexcept;
    void reserve(std::size_t count);

private:
    struct StringHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view value) const noexcept;
        std::size_t operator()(const std::string &value) const noexcept;
    };

    struct StringEq {
        using is_transparent = void;
        bool operator()(const std::string &lhs, const std::string &rhs) const noexcept;
        bool operator()(std::string_view lhs, std::string_view rhs) const noexcept;
        bool operator()(const std::string &lhs, std::string_view rhs) const noexcept;
        bool operator()(std::string_view lhs, const std::string &rhs) const noexcept;
    };

    std::unordered_map<std::string, SymbolId, StringHash, StringEq> symbolsByText_;
    std::deque<std::string> textById_;
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

class GraphView {
public:
    GraphView() = default;

    std::span<const OperationId> operations() const noexcept;
    std::span<const ValueId> values() const noexcept;
    std::span<const Port> inputPorts() const noexcept;
    std::span<const Port> outputPorts() const noexcept;
    std::span<const InoutPort> inoutPorts() const noexcept;
    OperationKind opKind(OperationId op) const;
    std::span<const ValueId> opOperands(OperationId op) const;
    std::span<const ValueId> opResults(OperationId op) const;
    SymbolId opSymbol(OperationId op) const;
    std::span<const AttrKV> opAttrs(OperationId op) const;
    std::optional<AttributeValue> opAttr(OperationId op, std::string_view key) const;
    std::optional<SrcLoc> opSrcLoc(OperationId op) const;
    SymbolId valueSymbol(ValueId value) const;
    int32_t valueWidth(ValueId value) const;
    bool valueSigned(ValueId value) const;
    ValueType valueType(ValueId value) const;
    bool valueIsInput(ValueId value) const;
    bool valueIsOutput(ValueId value) const;
    bool valueIsInout(ValueId value) const;
    OperationId valueDef(ValueId value) const;
    std::span<const ValueUser> valueUsers(ValueId value) const;
    std::optional<SrcLoc> valueSrcLoc(ValueId value) const;
    ValueId findValue(SymbolId symbol) const noexcept;
    OperationId findOperation(SymbolId symbol) const noexcept;

private:
    friend class GraphBuilder;

    enum class SymbolKind {
        kValue,
        kOperation
    };

    struct SymbolBinding {
        SymbolKind kind;
        uint32_t index = 0;
    };

    GraphId graphId_{};
    std::vector<OperationId> operations_;
    std::vector<ValueId> values_;
    std::vector<Port> inputPorts_;
    std::vector<Port> outputPorts_;
    std::vector<InoutPort> inoutPorts_;
    std::vector<OperationKind> opKinds_;
    std::vector<SymbolId> opSymbols_;
    std::vector<Range> opOperandRanges_;
    std::vector<Range> opResultRanges_;
    std::vector<Range> opAttrRanges_;
    std::vector<ValueId> operands_;
    std::vector<ValueId> results_;
    std::vector<AttrKV> opAttrs_;
    std::vector<std::optional<SrcLoc>> opSrcLocs_;
    std::unordered_map<uint32_t, SymbolBinding> symbolIndex_;
    std::vector<SymbolId> valueSymbols_;
    std::vector<int32_t> valueWidths_;
    std::vector<uint8_t> valueSigned_;
    std::vector<uint8_t> valueTypes_;
    std::vector<uint8_t> valueIsInput_;
    std::vector<uint8_t> valueIsOutput_;
    std::vector<uint8_t> valueIsInout_;
    std::vector<OperationId> valueDefs_;
    std::vector<Range> valueUserRanges_;
    std::vector<ValueUser> useList_;
    std::vector<std::optional<SrcLoc>> valueSrcLocs_;

    std::size_t opIndex(OperationId op) const;
    std::size_t valueIndex(ValueId value) const;
};

class GraphBuilder {
public:
    explicit GraphBuilder(GraphId graphId);
    GraphBuilder(GraphSymbolTable& symbols, GraphId graphId = GraphId{1, 0});
    static GraphBuilder fromView(const GraphView& view, GraphSymbolTable& symbols);

    void reserveValues(std::size_t count);
    void reserveOperations(std::size_t count);
    void reserveSymbols(std::size_t count);
    void reserveOpOperands(OperationId op, std::size_t count);
    void reserveOpResults(OperationId op, std::size_t count);
    void reserveOpAttrs(OperationId op, std::size_t count);

    ValueId addValue(SymbolId sym, int32_t width, bool isSigned,
                     ValueType type = ValueType::Logic);
    OperationId addOp(OperationKind kind, SymbolId sym);
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
    void setOpKind(OperationId op, OperationKind kind);
    bool eraseAttr(OperationId op, std::string_view key);
    void setValueSrcLoc(ValueId value, SrcLoc loc);
    void setOpSrcLoc(OperationId op, SrcLoc loc);
    void setOpSymbol(OperationId op, SymbolId sym);
    void setValueSymbol(ValueId value, SymbolId sym);
    void clearOpSymbol(OperationId op);
    void clearValueSymbol(ValueId value);
    GraphView freeze() const;

private:
    friend class Graph;

    struct ValueData {
        SymbolId symbol;
        int32_t width = 0;
        bool isSigned = false;
        ValueType type = ValueType::Logic;
        bool isInput = false;
        bool isOutput = false;
        bool isInout = false;
        OperationId definingOp = OperationId::invalid();
        std::optional<SrcLoc> srcLoc;
        bool alive = true;
    };

    struct OperationData {
        OperationKind kind = OperationKind::kConstant;
        SymbolId symbol;
        std::vector<ValueId> operands;
        std::vector<ValueId> results;
        std::vector<AttrKV> attrs;
        std::optional<SrcLoc> srcLoc;
        bool alive = true;
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
    bool valueAlive(ValueId value) const;
    bool opAlive(OperationId op) const;
    std::size_t countValueUses(ValueId value, std::optional<OperationId> skipOp) const;
    void addValueUser(ValueId value, OperationId op, std::size_t operandIndex);
    void removeValueUser(ValueId value, OperationId op, std::size_t operandIndex);
    void removeOpUses(OperationId op, const std::vector<ValueId>& operands);
    void addOpUses(OperationId op, const std::vector<ValueId>& operands);
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

    GraphId graphId_;
    GraphSymbolTable* symbols_ = nullptr;
    std::vector<ValueData> values_;
    std::vector<std::vector<ValueUser>> valueUsers_;
    std::vector<OperationData> operations_;
    std::vector<Port> inputPorts_;
    std::vector<Port> outputPorts_;
    std::vector<InoutPort> inoutPorts_;
    std::unordered_map<uint32_t, SymbolBinding> symbolIndex_;
};

class Value {
public:
    const ValueId& id() const noexcept { return id_; }
    SymbolId symbol() const noexcept { return symbol_; }
    std::string_view symbolText() const noexcept { return symbolText_; }
    int32_t width() const noexcept { return width_; }
    bool isSigned() const noexcept { return isSigned_; }
    ValueType type() const noexcept { return type_; }
    bool isInput() const noexcept { return isInput_; }
    bool isOutput() const noexcept { return isOutput_; }
    bool isInout() const noexcept { return isInout_; }
    OperationId definingOp() const noexcept { return definingOp_; }
    std::span<const ValueUser> users() const noexcept;
    const std::optional<SrcLoc>& srcLoc() const noexcept { return srcLoc_; }

private:
    friend class Graph;

    Value(ValueId id, SymbolId symbol, std::string symbolText, int32_t width, bool isSigned,
          ValueType type, bool isInput, bool isOutput, bool isInout,
          OperationId definingOp, std::span<const ValueUser> users,
          std::optional<SrcLoc> srcLoc, const Graph* owner, bool usersLoaded);

    ValueId id_{};
    SymbolId symbol_{};
    std::string symbolText_;
    int32_t width_ = 0;
    bool isSigned_ = false;
    ValueType type_ = ValueType::Logic;
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

class Design;

class Graph {
public:
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

    bool frozen() const noexcept { return !builder_.has_value(); }
    void freeze();

    std::span<const OperationId> operations() const;
    std::span<const ValueId> values() const;
    std::span<const Port> inputPorts() const;
    std::span<const Port> outputPorts() const;
    std::span<const InoutPort> inoutPorts() const;

    ValueId createValue(SymbolId symbol, int32_t width, bool isSigned,
                        ValueType type = ValueType::Logic);
    ValueId createValue(int32_t width, bool isSigned,
                        ValueType type = ValueType::Logic);
    OperationId createOperation(OperationKind kind, SymbolId symbol);
    OperationId createOperation(OperationKind kind);

    ValueId findValue(SymbolId symbol) const noexcept;
    OperationId findOperation(SymbolId symbol) const noexcept;
    ValueId findValue(std::string_view symbol) const;
    OperationId findOperation(std::string_view symbol) const;
    SymbolId valueSymbol(ValueId value) const noexcept;
    SymbolId operationSymbol(OperationId op) const noexcept;
    int32_t valueWidth(ValueId value) const;
    bool valueSigned(ValueId value) const;
    ValueType valueType(ValueId value) const;
    bool valueIsInput(ValueId value) const;
    bool valueIsOutput(ValueId value) const;
    bool valueIsInout(ValueId value) const;
    OperationId valueDef(ValueId value) const;
    std::optional<SrcLoc> valueSrcLoc(ValueId value) const;
    OperationKind opKind(OperationId op) const;
    std::span<const ValueId> opOperands(OperationId op) const;
    std::span<const ValueId> opResults(OperationId op) const;
    std::span<const AttrKV> opAttrs(OperationId op) const;
    std::optional<SrcLoc> opSrcLoc(OperationId op) const;
    Value getValue(ValueId id) const;
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
    void setAttr(OperationId op, std::string_view key, AttributeValue value);
    void setOpKind(OperationId op, OperationKind kind);
    bool eraseAttr(OperationId op, std::string_view key);
    void setValueSrcLoc(ValueId value, SrcLoc loc);
    void setOpSrcLoc(OperationId op, SrcLoc loc);
    void setOpSymbol(OperationId op, SymbolId sym);
    void setValueSymbol(ValueId value, SymbolId sym);
    void clearOpSymbol(OperationId op);
    void clearValueSymbol(ValueId value);

    void writeJson(slang::JsonWriter& writer) const;

private:
    friend class Design;
    friend class Value;

    void invalidateCaches() const;
    void invalidateValuesCache() const;
    void invalidateOperationsCache() const;
    void invalidatePortsCache() const;
    void ensureCaches() const;
    void ensureValuesCache() const;
    void ensureOperationsCache() const;
    void ensurePortsCache() const;
    GraphBuilder& ensureBuilder();
    void removeSymbolFromGenerateGroups(SymbolId sym) noexcept;
    const GraphView& view() const;
    Value valueFromView(ValueId id) const;
    Value valueFromBuilder(ValueId id) const;
    Operation operationFromView(OperationId id) const;
    Operation operationFromBuilder(OperationId id) const;
    std::span<const ValueUser> valueUsersSpan(ValueId id) const noexcept;

    Design* owner_;
    std::string symbol_;
    GraphId graphId_{};
    GraphSymbolTable symbols_;
    std::optional<GraphView> view_;
    std::optional<GraphBuilder> builder_;
    std::vector<SymbolId> declaredSymbols_;
    std::unordered_set<uint32_t> declaredSymbolSet_;
    std::vector<GenerateGroup> generateGroups_;
    mutable std::vector<ValueId> valuesCache_;
    mutable std::vector<OperationId> operationsCache_;
    mutable std::vector<Port> inputPortsCache_;
    mutable std::vector<Port> outputPortsCache_;
    mutable std::vector<InoutPort> inoutPortsCache_;
    mutable bool valuesCacheDirty_ = true;
    mutable bool operationsCacheDirty_ = true;
    mutable bool portsCacheDirty_ = true;
    uint32_t nextInternalOpSym_ = 0;
    uint32_t nextInternalValSym_ = 0;
};

class Design {
public:
    Design() = default;
    Design(Design&& other) noexcept;
    Design& operator=(Design&& other) noexcept;
    Design(const Design&) = delete;
    Design& operator=(const Design&) = delete;

    Graph& createGraph(std::string name);
    Graph& cloneGraph(std::string_view sourceName, std::string newName);
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

    static Design fromJsonString(std::string_view json);

private:
    Graph& addGraphInternal(std::unique_ptr<Graph> graph);
    void resetGraphOwners();

    DesignSymbolTable designSymbols_;
    std::unordered_map<std::string, std::unique_ptr<Graph>> graphs_;
    std::unordered_map<std::string, std::string> graphAliasBySymbol_;
    std::vector<std::string> graphOrder_;
    std::vector<std::string> topGraphs_;
    std::vector<SymbolId> declaredSymbols_;
    std::unordered_set<uint32_t> declaredSymbolSet_;
};

} // namespace wolvrix::lib::grh

#endif // WOLVRIX_GRH_HPP
