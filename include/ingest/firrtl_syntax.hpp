#ifndef WOLVRIX_INGEST_FIRRTL_SYNTAX_HPP
#define WOLVRIX_INGEST_FIRRTL_SYNTAX_HPP

#include "ingest/ingest.hpp"

#include <map>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace wolvrix::lib::ingest::firrtl {

using TypeId = uint32_t;
using ExprId = uint32_t;
using StmtId = uint32_t;

// Byte offsets refer to the single source buffer. Lines and columns are one-based.
struct Range {
    uint64_t begin = 0;
    uint64_t end = 0;
    uint64_t line = 1;
    uint64_t column = 1;
    std::string_view info;
};

enum class TypeKind {
    Unknown, UInt, SInt, Clock, Reset, AsyncReset, Analog, Bundle, Vector,
    Probe, RWProbe, Alias, Enum, Integer, String, Bool, Double, Path, AnyRef, Inst, List
};

struct Field {
    std::string_view name;
    TypeId type = 0;
    bool flip = false;
    bool operator==(const Field&) const = default;
};

struct Type {
    TypeKind kind = TypeKind::Unknown;
    int64_t width = -1;
    int64_t extent = 0;
    TypeId element = 0;
    bool constant = false;
    std::string_view name;
    std::string_view layer;
    std::vector<Field> fields;
    bool operator==(const Type&) const = default;
};

enum class LiteralKind { Integer, String, RawString, Real };
struct Parameter {
    std::string_view name;
    LiteralKind kind = LiteralKind::Integer;
    std::string_view value;
    Range range;
};

enum class ExprKind {
    Reference, Field, Index, Integer, UInt, SInt, Primitive, Mux, Probe, RWProbe,
    Read, Intrinsic, String, PropertyLiteral, PropertyOperation, Enum
};
struct Expression {
    ExprKind kind = ExprKind::Reference;
    std::string_view name;
    std::string_view literal;
    TypeId type = 0;
    std::vector<ExprId> operands;
    std::vector<Parameter> parameters;
    Range range;
};

enum class StmtKind {
    Node, Wire, Reg, RegReset, Instance, Object, Memory, CMem, SMem, MPort,
    Connect, Invalidate, Attach, Define, PropAssign, When, Match, MatchCase,
    LayerBlock, Stop, Printf, Fprintf, Fflush, Assert, Assume, Cover, Intrinsic,
    Force, ForceInitial, Release, ReleaseInitial, PropAssert, Skip
};
struct MemoryField {
    std::string_view name;
    std::string_view value;
    TypeId type = 0;
    Range range;
};
struct Statement {
    StmtKind kind = StmtKind::Skip;
    std::string_view name;
    std::string_view target;
    TypeId type = 0;
    std::vector<ExprId> operands;
    std::vector<StmtId> body;
    std::vector<StmtId> otherwise;
    std::vector<MemoryField> memory;
    Range range;
};

// Names are interned views into the source; types are structurally interned.
// A module arena is released independently of the source/interface index.
class Arena {
public:
    std::string_view intern(std::string_view name);
    TypeId intern(Type type);
    std::vector<Type> types{Type{}};
    std::vector<Expression> expressions;
    std::vector<Statement> statements;
private:
    std::unordered_set<std::string_view> names_;
    std::unordered_map<std::size_t, std::vector<TypeId>> typeBuckets_;
};

struct Port {
    std::string_view name;
    LeafDirection direction = LeafDirection::Input;
    TypeId type = 0;
    Range range;
};
enum class ModuleKind { Module, External, Intrinsic, Class, ExternalClass };
struct Module {
    ModuleKind kind = ModuleKind::Module;
    std::string_view name;
    std::string_view implementation;
    bool isPublic = false;
    std::vector<Port> ports;
    std::vector<Parameter> parameters;
    std::vector<std::string_view> enabledLayers;
    std::vector<std::string_view> knownLayers;
    Range range;
    Range body;
    uint64_t indentation = 0;
};
struct Layer {
    std::string_view name;
    std::string_view convention;
    std::string_view directory;
    std::vector<Layer> children;
    Range range;
};
struct TypeAlias {
    std::string_view name;
    TypeId type = 0;
    Range range;
};
struct FormalValue {
    enum class Kind { Integer, String, RawString, Array, Object } kind = Kind::Integer;
    std::string_view name;
    std::string_view value;
    std::vector<FormalValue> children;
};
struct Formal {
    std::string_view name;
    std::string_view module;
    std::vector<FormalValue> parameters;
    Range range;
};

namespace detail { struct Access; }
class Source final : public SourceOwner {
public:
    struct Impl;
    explicit Source(std::unique_ptr<Impl> impl);
    ~Source() override;
    const std::filesystem::path& path() const;
    std::string_view unit() const;
    std::string_view version() const;
    std::string_view circuit() const;
    std::span<const Module> modules() const;
    std::span<const Layer> layers() const;
    std::span<const TypeAlias> aliases() const;
    std::span<const Formal> formals() const;
    const Arena& arena() const;
    std::optional<std::size_t> findModule(std::string_view name) const;
    std::string_view annotations() const;
    std::string_view text() const;
    SourcePosition position(Range range) const;
private:
    std::unique_ptr<Impl> impl_;
    friend struct detail::Access;
};

struct IndexResult {
    std::shared_ptr<const Source> source;
    std::vector<IngestDiagnostic> diagnostics;
    bool success() const noexcept;
};
struct ModuleBody {
    Arena arena;
    Module interface;
    std::vector<StmtId> statements;
};
struct ModuleResult {
    // Keeps every string_view in body valid even after releasing the index result.
    std::shared_ptr<const Source> source;
    std::optional<ModuleBody> body;
    std::vector<IngestDiagnostic> diagnostics;
    bool success() const noexcept;
};
struct ValidationResult {
    std::vector<IngestDiagnostic> diagnostics;
    uint64_t modules = 0;
    uint64_t statements = 0;
    uint64_t expressions = 0;
    uint64_t maxModuleStatements = 0;
    std::map<std::string, uint64_t> constructs;
    bool success() const noexcept;
};

// Indexing checks lexical structure and interfaces, without accepting module bodies.
IndexResult indexFile(const std::filesystem::path& path, std::string unit,
                      const FirrtlOptions& options = {});
IndexResult indexText(std::string text, std::filesystem::path path, std::string unit,
                      const FirrtlOptions& options = {});
ModuleResult parseModule(std::shared_ptr<const Source> source, std::size_t module);
// Checks every body, retaining at most one module AST at a time.
ValidationResult validate(std::shared_ptr<const Source> source);
std::string decodeString(std::string_view literal);

} // namespace wolvrix::lib::ingest::firrtl

#endif // WOLVRIX_INGEST_FIRRTL_SYNTAX_HPP
