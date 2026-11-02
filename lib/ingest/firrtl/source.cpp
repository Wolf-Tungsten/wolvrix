#include "internal.hpp"

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <limits>
#include <system_error>
#if defined(__unix__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace wolvrix::lib::ingest::firrtl {
namespace {
void hashCombine(std::size_t& hash, std::size_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
}
bool noErrors(const std::vector<IngestDiagnostic>& diagnostics) {
    return std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.kind == IngestSeverity::Error;
    });
}
std::string_view sourceLine(std::string_view text, Range range) {
    const auto offset = std::min<uint64_t>(range.begin, text.size());
    const auto previous = offset ? text.rfind('\n', offset - 1) : std::string_view::npos;
    const auto begin = previous == std::string_view::npos ? 0 : previous + 1;
    const auto next = text.find('\n', offset);
    return text.substr(begin, (next == std::string_view::npos ? text.size() : next) - begin);
}
std::string_view lineInfo(std::string_view line, std::string_view info) {
    if (!info.empty()) return info;


    char quote = 0;
    for (std::size_t i = 0; i + 1 < line.size(); ++i) {
        const auto c = line[i];
        if (quote) { if (c == '\\') ++i; else if (c == quote) quote = 0; }
        else if (c == '"' || c == '\'' || c == '`') quote = c;
        else if (c == ';') break;
        else if (c == '@' && line[i + 1] == '[') {
            const auto start = i;
            for (i += 2; i < line.size(); ++i) {
                if (line[i] == '\\') ++i;
                else if (line[i] == ']') return line.substr(start, i + 1 - start);
            }
            break;
        }
    }
    return {};
}
}

std::string_view detail::sourceInfoAt(const Source& source, Range range) {
    return range.info.empty() ? lineInfo(sourceLine(source.text(), range), {}) : range.info;
}

std::string_view Arena::intern(std::string_view name) { return *names_.insert(name).first; }
TypeId Arena::intern(Type type) {
    std::size_t hash = static_cast<std::size_t>(type.kind);
    hashCombine(hash, std::hash<int64_t>{}(type.width));
    hashCombine(hash, std::hash<int64_t>{}(type.extent));
    hashCombine(hash, type.element); hashCombine(hash, type.constant);
    hashCombine(hash, std::hash<std::string_view>{}(type.name));
    hashCombine(hash, std::hash<std::string_view>{}(type.layer));
    for (const auto& field : type.fields) {
        hashCombine(hash, std::hash<std::string_view>{}(field.name));
        hashCombine(hash, field.type); hashCombine(hash, field.flip);
    }
    auto& bucket = typeBuckets_[hash];
    for (const auto id : bucket) if (types[id] == type) return id;
    if (types.size() >= std::numeric_limits<TypeId>::max()) throw std::length_error("too many FIRRTL types");
    const auto id = static_cast<TypeId>(types.size());
    types.push_back(std::move(type)); bucket.push_back(id);
    return id;
}

Source::Impl::~Impl() {
#if defined(__unix__)
    if (mapped) munmap(const_cast<char*>(mapped), mappedSize);
#endif
}
Source::Source(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Source::~Source() = default;
const std::filesystem::path& Source::path() const { return impl_->path; }
std::string_view Source::unit() const { return impl_->unit; }
std::string_view Source::version() const { return impl_->version; }
std::string_view Source::circuit() const { return impl_->circuit; }
std::span<const Module> Source::modules() const { return impl_->modules; }
std::span<const Layer> Source::layers() const { return impl_->layers; }
std::span<const TypeAlias> Source::aliases() const { return impl_->aliases; }
std::span<const Formal> Source::formals() const { return impl_->formals; }
const Arena& Source::arena() const { return impl_->arena; }
std::string_view Source::annotations() const { return impl_->annotations; }
std::string_view Source::text() const { return impl_->text; }
std::optional<std::size_t> Source::findModule(std::string_view name) const {
    const auto found = impl_->moduleNames.find(name);
    if (found == impl_->moduleNames.end()) return std::nullopt;
    return found->second;
}
SourcePosition Source::position(Range range) const {
    SourcePosition position;
    position.unit = impl_->unit; position.file = impl_->path;
    position.line = range.line; position.column = range.column;
    const auto line = sourceLine(impl_->text, range);
    position.sourceLine = line;
    position.sourceInfo = lineInfo(line, range.info);
    return position;
}

bool IndexResult::success() const noexcept { return source && noErrors(diagnostics); }
bool ModuleResult::success() const noexcept { return body.has_value() && noErrors(diagnostics); }
bool ValidationResult::success() const noexcept { return noErrors(diagnostics); }

IndexResult indexText(std::string text, std::filesystem::path path, std::string unit, const FirrtlOptions& options) {
    auto impl = std::make_unique<Source::Impl>();
    impl->path = std::move(path); impl->unit = std::move(unit);
    impl->storage = std::move(text); impl->text = impl->storage;
    return detail::index(std::move(impl), options);
}
IndexResult indexFile(const std::filesystem::path& path, std::string unit, const FirrtlOptions& options) {
    auto impl = std::make_unique<Source::Impl>();
    impl->path = path; impl->unit = std::move(unit);
    try {
#if defined(__unix__)
        const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) throw std::system_error(errno, std::generic_category(), "open");
        struct stat status{};
        if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0) {
            const auto code = errno ? errno : EINVAL;
            close(fd); throw std::system_error(code, std::generic_category(), "regular input file required");
        }
        const auto size = static_cast<uint64_t>(status.st_size);
        void* mapping = size ? mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0) : nullptr;
        const int code = errno;
        close(fd);
        if (size && mapping == MAP_FAILED) throw std::system_error(code, std::generic_category(), "mmap");
        impl->mapped = static_cast<const char*>(mapping); impl->mappedSize = size;
        if (size) impl->text = {impl->mapped, static_cast<std::size_t>(size)};
#else
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream) throw std::runtime_error("open failed");
        const auto size = stream.tellg();
        if (size < 0) throw std::runtime_error("cannot determine file size");
        impl->storage.resize(static_cast<std::size_t>(size)); stream.seekg(0);
        if (!stream.read(impl->storage.data(), impl->storage.size())) throw std::runtime_error("read failed");
        impl->text = impl->storage;
#endif
        return detail::index(std::move(impl), options);
    } catch (const std::exception& error) {
        IndexResult result;
        SourcePosition position; position.file = path; position.unit = impl ? impl->unit : "";
        result.diagnostics.push_back({IngestSeverity::Error, "cannot read FIRRTL source: " + std::string(error.what()),
                                      {}, "ingest-firrtl", {}, std::move(position)});
        return result;
    }
}

std::string decodeString(std::string_view literal) {
    if (literal.size() < 2 || (literal.front() != '"' && literal.front() != '\'') || literal.back() != literal.front())
        throw std::invalid_argument("invalid FIRRTL string literal");
    std::string result;
    for (std::size_t i = 1; i + 1 < literal.size(); ++i) {
        char c = literal[i];
        if (c == '\\') {
            if (++i + 1 >= literal.size()) throw std::invalid_argument("truncated FIRRTL string escape");
            switch (literal[i]) {
            case 'n': c = '\n'; break; case 'r': c = '\r'; break; case 't': c = '\t'; break;
            case 'b': c = '\b'; break; case 'f': c = '\f'; break; case 'v': c = '\v'; break;
            case '\\': c = '\\'; break; case '"': c = '"'; break; case '\'': c = '\''; break;
            case '{':
                if (i + 1 >= literal.size() - 1 || literal[i + 1] != '{')
                    throw std::invalid_argument("unsupported FIRRTL string escape");
                c = '{'; break;
            default: throw std::invalid_argument("unsupported FIRRTL string escape");
            }
        }
        result += c;
    }
    return result;
}

namespace detail {
IngestDiagnostic diagnostic(const Source& source, const Error& error, std::string_view module) {
    return {IngestSeverity::Error, error.what(), std::string(module), "ingest-firrtl",
            std::string(module), source.position(error.range)};
}
}
} // namespace wolvrix::lib::ingest::firrtl
