#include "grhsim/backend/cpu_phase_emit.hpp"

#include "emit/grhsim_runtime.hpp"
#include "emit/readmem.hpp"
#include "grhsim/backend/cpu.hpp"
#include "grhsim/backend/cpu_phase_common.hpp"
#include "grhsim/dialect/registry.hpp"
#include "grhsim/ir/verifier.hpp"
#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wolvrix::lib::grhsim
{
    namespace
    {
        // The three helpers below are verbatim copies of the legacy cpu_emit.cpp
        // support code (identifier sanitizing, typed parameter lookup and the
        // re-indenting stream buffer) so the two emitters format generated C++
        // identically.
        std::string identifier(std::string_view text)
        {
            std::string result;
            for (char ch : text)
                result += (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                          (ch >= '0' && ch <= '9') || ch == '_' ? ch : '_';
            if (result.empty() || (result.front() >= '0' && result.front() <= '9')) result.insert(0, "n_");
            return result;
        }

        // E-flat (gsim-shape emit, 2026-10-06): textual expansion of the
        // ≤64-bit runtime helpers into raw constant-masked expressions. Every
        // expansion is the corresponding helper body from grhsim_runtime.cpp,
        // so the emitted semantics are identical; the point is that no call
        // site reaches the compiler, keeping the -O3 inliner off the hot path
        // of generated TUs.
        std::string maskLiteral(uint64_t width)
        {
            if (width >= 64) return "~UINT64_C(0)";
            std::ostringstream os;
            os << "UINT64_C(0x" << std::hex << ((UINT64_C(1) << width) - 1) << ')';
            return os.str();
        }
        // grhsim_trunc_u64 body.
        std::string truncRaw(std::string expr, uint64_t width)
        {
            if (width >= 64) return expr;
            return "(" + expr + "&" + maskLiteral(width) + ")";
        }
        // grhsim_sign_extend_i64 body (the helper's return type is int64_t).
        std::string sextRaw(const std::string &expr, uint64_t width)
        {
            if (width == 0) return "INT64_C(0)";
            if (width >= 64) return "static_cast<std::int64_t>(" + expr + ")";
            std::ostringstream sign;
            sign << "UINT64_C(0x" << std::hex << (UINT64_C(1) << (width - 1)) << ')';
            const std::string mask = maskLiteral(width);
            return "static_cast<std::int64_t>(((" + expr + ")&" + mask + ")|(((" + expr + ")&" + sign.str() + ")?~" +
                   mask + ":UINT64_C(0)))";
        }

        // ----- Wide (>64-bit) two-state values: C23 _BitInt shapes -----
        // Storage is always an unsigned _BitInt of the 64-rounded container
        // width; signedness is re-derived at op points (gsim-shape, see
        // reference/gsim instsGenerator). Every value-producing shape masks
        // its result back to the semantic width, keeping the container's
        // padding bits zero — the raw-byte paths (FST word dump, whole-store
        // memcpy, dumpState hash) and value compares rely on that invariant.
        uint64_t wideContainer(uint64_t width) { return (width + 63) / 64 * 64; }
        std::string wideType(uint64_t width)
        {
            return "unsigned _BitInt(" + std::to_string(wideContainer(width)) + ")";
        }
        std::string wideSignedType(uint64_t width)
        {
            return "_BitInt(" + std::to_string(wideContainer(width)) + ")";
        }
        // bitMask(width) as a _BitInt constant (gsim util.cpp bitMask: the
        // full-container case avoids the undefined 1<<container shift).
        std::string wideMask(uint64_t width)
        {
            if (width == wideContainer(width)) return "((" + wideType(width) + ")0-1)";
            return "(((" + wideType(width) + ")1<<" + std::to_string(width) + ")-1)";
        }
        // Mask to the semantic width (identity on already-clean values).
        std::string wideTrunc(std::string expr, uint64_t width)
        {
            if (width == wideContainer(width)) return "(" + expr + ")";
            return "(" + expr + "&" + wideMask(width) + ")";
        }
        // Sign-extend a width-bit value to its container (result: signed
        // _BitInt) — gsim's shift trick from instsAsSInt.
        std::string wideSext(const std::string &expr, uint64_t width)
        {
            const auto container = wideContainer(width);
            if (width == container)
                return "static_cast<" + wideSignedType(width) + ">(" + expr + ")";
            const auto shift = std::to_string(container - width);
            return "(static_cast<" + wideSignedType(width) + ">((" + wideType(width) + ")(" + expr + ")<<" + shift +
                   ")>>" + shift + ")";
        }
        // grhsim_index_words wide overload: any nonzero bit above word 0
        // clamps to cap. ((expr)>>64)!=0 covers every high word at once.
        std::string wideIndexRaw(const std::string &expr, uint64_t cap)
        {
            return "(((" + expr + ")>>64)!=0?UINT64_C(" + std::to_string(cap) + "):((std::uint64_t)(" + expr +
                   ")>=UINT64_C(" + std::to_string(cap) + ")?UINT64_C(" + std::to_string(cap) + "):(std::uint64_t)(" +
                   expr + ")))";
        }
        // grhsim_index_words body: min(uint64(value), cap); wide operands keep
        // the overload's rule (any nonzero high word clamps to cap).
        std::string indexRaw(const std::string &expr, uint64_t srcWidth, uint64_t cap)
        {
            if (srcWidth <= 64)
                return "((std::uint64_t)(" + expr + ")>=UINT64_C(" + std::to_string(cap) + ")?UINT64_C(" +
                       std::to_string(cap) + "):(std::uint64_t)(" + expr + "))";
            return wideIndexRaw(expr, cap);
        }

        // ----- Stage-06 shaped (array) value bridging -----
        // A shaped logic value is its total-width flat bit vector everywhere
        // in compute; state/boundary stores hold the flat per-row array (row
        // j occupies bits [j*elementWidth, (j+1)*elementWidth) of the flat
        // form, row-major). These helpers translate between the two.
        //
        // Row-major linear index of `indices` (one text per addressed
        // dimension) into a shaped object: sum_j index_j * prod(shape[j+1:
        // unitEnd]); a stride of 1 drops the multiplication. unitEnd is the
        // full rank for element-row addressing (state readArray/writeArray)
        // and the index count for result-block addressing (sliceArray).
        std::string linearIndexText(std::span<const int64_t> shape, std::size_t unitEnd,
                                    const std::vector<std::string> &indices)
        {
            std::string text;
            for (std::size_t j = 0; j < indices.size(); ++j)
            {
                uint64_t stride = 1;
                for (std::size_t d = j + 1; d < unitEnd; ++d)
                    stride *= static_cast<uint64_t>(shape[d]);
                if (!text.empty()) text += '+';
                text += indices[j];
                if (stride != 1) text += '*' + std::to_string(stride);
            }
            return text;
        }
        // Address one element of a nested, row-major physical array. Packed
        // value bridges still enumerate linear bit rows, while the store
        // declaration retains every source dimension.
        std::string rowAccessText(const std::string &store, std::span<const int64_t> shape,
                                  const std::string &row)
        {
            std::string cell = store;
            for (std::size_t dimension = 0; dimension < shape.size(); ++dimension)
            {
                uint64_t stride = 1;
                for (std::size_t next = dimension + 1; next < shape.size(); ++next)
                    stride *= static_cast<uint64_t>(shape[next]);
                std::string index = "(" + row + ")";
                if (stride != 1) index += "/" + std::to_string(stride);
                if (dimension != 0) index = "(" + index + ")%" + std::to_string(shape[dimension]);
                cell += '[' + index + ']';
            }
            return cell;
        }
        // Packs `rows` consecutive rows of the shaped store array `store`,
        // starting at the row expression `baseRow` (empty = row 0), into the
        // total-width flat value. Physical cells hold clean elementWidth-bit
        // values (the padding invariant), so each row is truncated before the
        // shift; the shift stays below the container width (no UB). With
        // rowCount > 0, rows at/after rowCount read as zero (the legacy
        // sliceArray out-of-range semantics).
        std::string packStoreRows(const std::string &store, std::span<const int64_t> shape,
                                  const std::string &baseRow, uint64_t rows,
                                  uint32_t elementWidth, uint32_t totalWidth, uint64_t rowCount = 0)
        {
            const std::string rowIndex = baseRow.empty() ? std::string("cpu_j") : baseRow + "+cpu_j";
            const std::string cell = rowAccessText(store, shape, rowIndex);
            std::string element = elementWidth > 64 ? cell : truncRaw("(std::uint64_t)(" + cell + ")", elementWidth);
            if (rowCount)
            {
                const std::string zero = elementWidth > 64 ? wideTrunc("0", elementWidth)
                                                           : "UINT64_C(0)";
                element = "((" + rowIndex + ")<" + std::to_string(rowCount) + "?" + element + ":" + zero + ")";
            }
            const std::string loop = "for(std::size_t cpu_j=0;cpu_j<" + std::to_string(rows) + ";++cpu_j)";
            if (totalWidth > 64 && baseRow.empty() && !rowCount &&
                rows * elementWidth == totalWidth &&
                (elementWidth == 8 || elementWidth == 16 || elementWidth == 32 || elementWidth == 64))
            {
                // These cells have no padding: on little-endian targets their
                // array image already is the packed value. In particular, a
                // subsequent constant slice can become a single scalar load
                // instead of repacking hundreds of rows on every read.
                return "([&](){ " + wideType(totalWidth) + " cpu_pk=0;\n"
                       "#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__\n"
                       "std::memcpy(&cpu_pk,&" + store + "," + std::to_string(totalWidth / 8) + ");\n"
                       "#else\n" + loop + "cpu_pk|=(" + wideType(totalWidth) + ")(" + element +
                       ")<<(cpu_j*" + std::to_string(elementWidth) + ");\n"
                       "#endif\nreturn cpu_pk;}())";
            }
            if (totalWidth > 64)
                return "([&](){ " + wideType(totalWidth) + " cpu_pk=0;" + loop + "cpu_pk|=(" + wideType(totalWidth) +
                       ")(" + element + ")<<(cpu_j*" + std::to_string(elementWidth) + ");return cpu_pk;}())";
            return "([&](){ std::uint64_t cpu_pk=UINT64_C(0);" + loop + "cpu_pk|=(" + element + ")<<(cpu_j*" +
                   std::to_string(elementWidth) + ");return cpu_pk;}())";
        }
        // Slices row `rowVar` (a size_t expression) out of a flat packed
        // value: bits [row*elementWidth, (row+1)*elementWidth). The three
        // shapes mirror the legacy packed-fill row slicing.
        std::string rowSliceText(const std::string &data, const std::string &rowVar, uint32_t elementWidth,
                                 uint32_t packedWidth)
        {
            const std::string shift = rowVar + '*' + std::to_string(elementWidth) + 'u';
            if (packedWidth <= 64)
                return "grhsim_slice_dynamic_u64(static_cast<std::uint64_t>(" + data + ")," + shift + ',' +
                       std::to_string(elementWidth) + "u)";
            if (elementWidth <= 64)
                return truncRaw("(std::uint64_t)((" + data + ")>>(" + shift + "))", elementWidth);
            return wideTrunc("((" + data + ")>>(" + shift + "))", elementWidth);
        }

        // Vendored libfst sources compiled into waveform-enabled model
        // libraries (the wolvrix build itself links the same objects for the
        // legacy path; generated models compile their own copies).
        std::string libfstSourceDir()
        {
#ifdef WOLVRIX_SOURCE_DIR
            return (std::filesystem::path(WOLVRIX_SOURCE_DIR) / "external/libfst/src").string();
#else
            throw std::runtime_error("waveform emit requires WOLVRIX_SOURCE_DIR (vendored libfst location)");
#endif
        }

        template <typename T>
        const T *parameter(const GrhSimModel &model, std::span<const Parameter> parameters, std::string_view name)
        {
            for (const auto &entry : parameters)
                if (model.text(entry.name) == name)
                {
                    const auto *value = std::get_if<T>(&entry.value);
                    if (!value) throw std::runtime_error("CPU emit parameter has wrong type: " + std::string(name));
                    return value;
                }
            return nullptr;
        }

        // Re-indents generated C++ on the fly: one statement per line, block braces on
        // their own lines, four spaces per brace depth. String/char literals and comments
        // are lexed so braces or semicolons inside them never affect the layout, and a
        // line comment trailing a statement stays glued to that statement.
        class IndentBuffer final : public std::streambuf
        {
        public:
            explicit IndentBuffer(std::streambuf *sink) : sink_(sink) {}
            IndentBuffer(const IndentBuffer &) = delete;
            IndentBuffer &operator=(const IndentBuffer &) = delete;
            ~IndentBuffer() override
            {
                flushPiece(false);
                emitPending();
            }

        protected:
            int_type overflow(int_type ch) override
            {
                if (traits_type::eq_int_type(ch, traits_type::eof())) return traits_type::not_eof(ch);
                put(static_cast<char>(ch));
                return ch;
            }
            std::streamsize xsputn(const char *data, std::streamsize count) override
            {
                for (std::streamsize i = 0; i < count; ++i) put(data[i]);
                return count;
            }
            int sync() override
            {
                flushPiece(false);
                emitPending();
                return failed_ || sink_->pubsync() != 0 ? -1 : 0;
            }

        private:
            enum class Context
            {
                Code,
                String,
                Character,
                LineComment,
                BlockComment
            };
            enum class Brace
            {
                Block,
                Init,
                Case
            };
            static constexpr std::size_t indentWidth = 4;

            static bool isSpace(char ch) { return ch == ' ' || ch == '\t' || ch == '\f' || ch == '\v'; }
            static bool isWord(char ch) { return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_'; }

            void append(char ch)
            {
                if (piece_.empty()) pieceParen_ = paren_;
                piece_ += ch;
            }

            // A `{` opens a statement block after `)`, after a keyword such as `else`,
            // or after a struct/class/enum tag; a braced case body is level-neutral.
            // Anything else is a braced initializer that stays glued to its statement.
            Brace braceKind() const
            {
                std::string_view text(piece_);
                while (!text.empty() && isSpace(text.back())) text.remove_suffix(1);
                if (text.empty()) return !braces_.empty() && braces_.back() == Brace::Init ? Brace::Init : Brace::Block;
                const char last = text.back();
                if (last == ')') return Brace::Block;
                if (last == ':' && (text.starts_with("case ") || text.starts_with("default:"))) return Brace::Case;
                if (last == '(' || last == '=' || last == ',' || last == '[' || last == '{') return Brace::Init;
                std::size_t begin = text.size();
                while (begin > 0 && isWord(text[begin - 1])) --begin;
                const std::string_view lastWord = text.substr(begin);
                if (lastWord == "else" || lastWord == "do" || lastWord == "try" || lastWord == "const" ||
                    lastWord == "noexcept" || lastWord == "override" || lastWord == "final") return Brace::Block;
                std::size_t end = 0;
                while (end < text.size() && isWord(text[end])) ++end;
                const std::string_view firstWord = text.substr(0, end);
                if (firstWord == "struct" || firstWord == "class" || firstWord == "union" || firstWord == "enum" ||
                    firstWord == "namespace") return Brace::Block;
                return Brace::Init;
            }

            void put(char ch)
            {
                switch (context_)
                {
                case Context::String:
                case Context::Character:
                {
                    append(ch);
                    const char quote = context_ == Context::String ? '"' : '\'';
                    if (escaped_) escaped_ = false;
                    else if (ch == '\\') escaped_ = true;
                    else if (ch == quote) context_ = Context::Code;
                    return;
                }
                case Context::LineComment:
                    if (ch == '\n')
                    {
                        context_ = Context::Code;
                        flushPiece(true);
                    }
                    else append(ch);
                    return;
                case Context::BlockComment:
                    append(ch);
                    if (commentStar_ && ch == '/') context_ = Context::Code;
                    commentStar_ = ch == '*';
                    return;
                default: break;
                }
                if (slash_)
                {
                    slash_ = false;
                    if (ch == '/')
                    {
                        append('/');
                        append('/');
                        context_ = Context::LineComment;
                        return;
                    }
                    if (ch == '*')
                    {
                        append('/');
                        append('*');
                        commentStar_ = false;
                        context_ = Context::BlockComment;
                        return;
                    }
                    append('/');
                }
                switch (ch)
                {
                case '\n': flushPiece(true); return;
                case '\r': return;
                case ' ':
                case '\t':
                case '\f':
                case '\v':
                    if (!piece_.empty()) append(ch);
                    return;
                case '/': slash_ = true; return;
                case '"':
                    append(ch);
                    escaped_ = false;
                    context_ = Context::String;
                    return;
                case '\'':
                    append(ch);
                    // A quote right after a digit is a digit separator, not a char literal.
                    if (piece_.size() < 2 || !std::isdigit(static_cast<unsigned char>(piece_[piece_.size() - 2])))
                    {
                        escaped_ = false;
                        context_ = Context::Character;
                    }
                    return;
                case '(':
                    ++paren_;
                    append(ch);
                    return;
                case ')':
                    if (paren_ > 0) --paren_;
                    append(ch);
                    return;
                case '{':
                {
                    const Brace kind = braceKind();
                    braces_.push_back(kind);
                    append(ch);
                    if (kind != Brace::Case) ++pieceDelta_;
                    if (kind != Brace::Init)
                    {
                        parenStack_.push_back(paren_);
                        paren_ = 0;
                        flushPiece(false);
                    }
                    return;
                }
                case '}':
                {
                    const Brace kind = braces_.empty() ? Brace::Block : braces_.back();
                    if (!braces_.empty()) braces_.pop_back();
                    if (kind != Brace::Init)
                    {
                        paren_ = parenStack_.empty() ? 0 : parenStack_.back();
                        if (!parenStack_.empty()) parenStack_.pop_back();
                        if (!piece_.empty()) flushPiece(false);
                    }
                    append(ch);
                    if (kind != Brace::Case) --pieceDelta_;
                    return;
                }
                case ';':
                    append(ch);
                    if (paren_ == 0) flushPiece(false);
                    return;
                default: append(ch); return;
                }
            }

            void flushPiece(bool newline)
            {
                while (!piece_.empty() && isSpace(piece_.back())) piece_.pop_back();
                if (piece_.empty())
                {
                    pieceDelta_ = 0;
                    pieceParen_ = 0;
                    if (!pending_.empty()) emitPending();
                    else if (newline) writeChar('\n');
                    return;
                }
                if (!pending_.empty() && piece_.starts_with("//"))
                {
                    pending_ += ' ';
                    pending_ += piece_;
                    piece_.clear();
                    pieceDelta_ = 0;
                    pieceParen_ = 0;
                    emitPending();
                    return;
                }
                emitPending();
                std::size_t leading = 0;
                while (leading < piece_.size() && piece_[leading] == '}') ++leading;
                std::size_t printLevel = level_ > leading ? level_ - leading : 0;
                if (pieceParen_ > 0 && leading == 0) ++printLevel;
                if (piece_ == "public:" || piece_ == "private:" || piece_ == "protected:" ||
                    piece_.starts_with("case ") || piece_.starts_with("default:"))
                    printLevel -= printLevel > 0 ? 1 : 0;
                pending_.assign(printLevel * indentWidth, ' ');
                pending_ += piece_;
                const long next = static_cast<long>(level_) + pieceDelta_;
                level_ = next > 0 ? static_cast<std::size_t>(next) : 0;
                piece_.clear();
                pieceDelta_ = 0;
                pieceParen_ = 0;
            }

            void writeChar(char ch)
            {
                if (sink_->sputc(ch) == traits_type::eof()) failed_ = true;
            }

            void emitPending()
            {
                if (pending_.empty()) return;
                const auto size = static_cast<std::streamsize>(pending_.size());
                if (sink_->sputn(pending_.data(), size) != size) failed_ = true;
                writeChar('\n');
                pending_.clear();
            }

            std::streambuf *sink_;
            std::string piece_;
            std::string pending_;
            std::vector<Brace> braces_;
            std::vector<std::size_t> parenStack_;
            std::size_t level_ = 0;
            std::size_t paren_ = 0;
            std::size_t pieceParen_ = 0;
            long pieceDelta_ = 0;
            Context context_ = Context::Code;
            bool escaped_ = false;
            bool slash_ = false;
            bool commentStar_ = false;
            bool failed_ = false;
        };

        // M5 six-phase emitter (M5d-7 multi-TU). Consumes the
        // TranslationUnits-stage mapping (named stores, event bitmaps, mem
        // write plan, fanout tables, phase task sequence, the C8 TU plan) and
        // generates the P_input/P_event/P_general/P_mem/P_publish/P_output C++
        // model as one shared header plus one .cpp per planned unit. Only the
        // "evaluation" code generation is ported from the legacy emitter;
        // every activation/commit decision is new code driven by the M4 static
        // tables.
        class SixPhaseEmitter
        {
        public:
            explicit SixPhaseEmitter(const GrhSimModel &model, bool waveform = false, bool memEnableBitmap = true,
                                     bool memWriteOnWrite = false)
                : model_(model), mapping_(*model.cpuMapping()), tree_(mapping_.partitionTree),
                  layout_(*mapping_.dataLayout), schedule_(*mapping_.schedule), stores_(*layout_.namedStores),
                  tuPlan_(*mapping_.translationUnits),
                  prefix_("grhsim_" + identifier(model.text(model.name()))),
                  class_("GrhSIM_" + identifier(model.text(model.name()))),
                  waveform_(waveform), memEnableBitmap_(memEnableBitmap), memWriteOnWrite_(memWriteOnWrite),
                  ordinalOf_(tree_.partitions.size() + 1, ~0u),
                  regFieldByState_(model.states().size() + 1),
                  memFieldByState_(model.states().size() + 1),
                  boundaryByValue_(model.values().size() + 1),
                  boundaryByUse_(model.values().size() + 1),
                  boundaryFieldsByValue_(model.values().size() + 1),
                  boundaryByInput_(model.inputs().size() + 1),
                  inputFanout_(model.values().size() + 1),
                  supernodeFanout_(model.values().size() + 1),
                  stateFanout_(model.states().size() + 1),
                  inputPortFanout_(model.inputs().size()),
                  producers_(model.values().size() + 1)
            {
                for (const auto &store : stores_)
                    switch (store.kind)
                    {
                    case CpuNamedStoreKind::SmallSink: smallSinkStore_ = &store; break;
                    case CpuNamedStoreKind::LargeSink: largeSinkStore_ = &store; break;
                    case CpuNamedStoreKind::Input: boundaryStores_[0] = &store; break;
                    case CpuNamedStoreKind::NonSinkBoundary: boundaryStores_[1] = &store; break;
                    case CpuNamedStoreKind::SmallSinkEnableBoundary: boundaryStores_[2] = &store; break;
                    case CpuNamedStoreKind::SmallSinkPayloadBoundary: boundaryStores_[3] = &store; break;
                    case CpuNamedStoreKind::LargeSinkEnableBoundary: boundaryStores_[4] = &store; break;
                    case CpuNamedStoreKind::LargeSinkPayloadBoundary: boundaryStores_[5] = &store; break;
                    case CpuNamedStoreKind::PrevEvent: prevEventStore_ = &store; break;
                    case CpuNamedStoreKind::EventAct: eventActStore_ = &store; break;
                    case CpuNamedStoreKind::TimeslotTrigger: timeslotStore_ = &store; break;
                    case CpuNamedStoreKind::ActiveFlags: activeStore_ = &store; break;
                    }
                if (!smallSinkStore_ || !largeSinkStore_ || !prevEventStore_ || !eventActStore_ ||
                    !timeslotStore_ || !activeStore_ ||
                    std::any_of(boundaryStores_.begin(), boundaryStores_.end(),
                                [](const CpuNamedStore *store) { return !store; }))
                    throw std::runtime_error("CPU six-phase emit requires all twelve named stores");

                order_ = generalSupernodeOrder(tree_);
                supernodeCount_ = static_cast<uint32_t>(order_.size());
                for (uint32_t i = 0; i < supernodeCount_; ++i) ordinalOf_[order_[i].index] = i;
                // C2 child order: non-sink supernodes first, sink supernodes
                // after — verify the prefix invariant the word scan relies on.
                for (uint32_t i = 0; i < supernodeCount_; ++i)
                {
                    const auto category = *tree_.partitions[order_[i].index - 1].attrs.supernodeCategory;
                    if (category == CpuSupernodeCategory::NonSink)
                    {
                        if (i != nonSinkCount_)
                            throw std::runtime_error("CPU six-phase emit non-sink supernodes do not form an ordinal prefix");
                        ++nonSinkCount_;
                    }
                }
                activeWords_ = layout_.types[activeStore_->fields[0].type.index - 1].count;
                supernodeOps_.resize(supernodeCount_);
                for (uint32_t ordinal = 0; ordinal < supernodeCount_; ++ordinal)
                {
                    const auto &supernode = tree_.partitions[order_[ordinal].index - 1];
                    for (const auto node : supernode.children)
                    {
                        const auto &ops = tree_.partitions[node.index - 1].ops;
                        supernodeOps_[ordinal].insert(supernodeOps_[ordinal].end(), ops.begin(), ops.end());
                    }
                }
                // Event/Output branches each hold one EmitFunction child with a
                // flat, topologically ordered op list.
                const auto &root = tree_.partitions[tree_.root.index - 1];
                for (const auto branchId : root.children)
                {
                    const auto &branch = tree_.partitions[branchId.index - 1];
                    if (branch.children.empty()) continue;
                    if (branch.attrs.phase == CpuPhase::Event)
                        eventOps_ = tree_.partitions[branch.children.front().index - 1].ops;
                    else if (branch.attrs.phase == CpuPhase::Output)
                        outputOps_ = tree_.partitions[branch.children.front().index - 1].ops;
                }
                {
                    // The gate is safe only for pure output cones: a task/dpi
                    // side effect or a staged latchWrite commit forces the
                    // unconditional path.  ($urandom re-samples flag the gate
                    // at the sampling site itself.)
                    outputGate_ = stagedOutputWrites_.empty();
                    for (const auto opId : outputOps_)
                    {
                        const auto &op = model_.operations()[opId.index - 1];
                        const auto k = model_.text(op.opType);
                        if (k == "core.system.task" || k == "core.dpi.call") { outputGate_ = false; break; }
                        for (const auto ref : model_.objectRefs(op))
                        {
                            if (ref.kind == ObjectKind::State) outputReadStates_.insert(ref.index);
                            else if (ref.kind == ObjectKind::Input) outputReadInputs_.insert(ref.index);
                        }
                        for (const auto v : model_.operands(op))
                            if (v.index < boundaryByValue_.size() && boundaryByValue_[v.index])
                                outputReadValues_.insert(v.index);
                    }
                }

                // V2 (M2): the P_event activation map (act -> non-sink
                // supernodes holding an op with that act) replaces the pre-v2
                // influence bitmaps; emitEdgeDet ORs the fired acts' words
                // straight into nonSinkActiveFlag (eventActiveFlag is gone).
                if (schedule_.eventActivation)
                    for (const auto &entry : *schedule_.eventActivation)
                        activationByAct_[entry.act] = &entry;
                for (const auto &op : model_.operations())
                {
                    if (model_.text(op.opType) != "core.event.edgeDet") continue;
                    const auto params = model_.parameters(op);
                    const auto *act = parameter<int64_t>(model_, params, "act");
                    const auto *edge = parameter<std::string>(model_, params, "edge");
                    const auto *prevInit = parameter<std::string>(model_, params, "prevInit");
                    if (!act || !edge || !prevInit || *act < 0)
                        throw std::runtime_error("CPU six-phase emit malformed edgeDet parameters");
                    DetInfo info;
                    info.act = static_cast<uint32_t>(*act);
                    info.edge = *edge;
                    info.prevInit = *prevInit;
                    info.event = model_.operands(op).front();
                    detByAct_[info.act] = info;
                    maxAct_ = std::max(maxAct_, info.act);
                }
                prevByAct_.resize(maxAct_ + 1);
                triggersByAct_.resize(maxAct_ + 1);
                for (const auto &field : prevEventStore_->fields)
                    if (field.aux < prevByAct_.size()) prevByAct_[field.aux] = &field;
                if (schedule_.timeslotTriggers)
                    for (const auto &trigger : *schedule_.timeslotTriggers)
                    {
                        if (trigger.act < triggersByAct_.size())
                            triggersByAct_[trigger.act].push_back(trigger.flag);
                        maxTimeslotFlag_ = std::max(maxTimeslotFlag_, trigger.flag);
                    }

                for (const auto &field : smallSinkStore_->fields) regFieldByState_[field.state.index] = &field;
                for (const auto &field : largeSinkStore_->fields) memFieldByState_[field.state.index] = &field;
                for (std::size_t storeIndex = 0; storeIndex < boundaryStores_.size(); ++storeIndex)
                    for (const auto &field : boundaryStores_[storeIndex]->fields)
                    {
                        boundaryKindByField_[&field] = boundaryStores_[storeIndex]->kind;
                        if (storeIndex == 0)
                            boundaryByInput_[field.aux + 1] = &field;
                        else if (field.value)
                        {
                            if (!boundaryByValue_[field.value.index]) boundaryByValue_[field.value.index] = &field;
                            boundaryByUse_[field.value.index][storeIndex - 1] = &field;
                            boundaryFieldsByValue_[field.value.index].push_back(&field);
                        }
                        // String boundary fields stay outside the trivially
                        // resettable store structs.
                        if (layout_.types[field.type.index - 1].kind == CpuTypeKind::String)
                            boundaryStringSlot_[&field] = boundaryStringCount_++;
                    }
                const auto ordinals = [&](std::span<const PartitionId> targets) {
                    std::vector<uint32_t> result;
                    result.reserve(targets.size());
                    for (const auto target : targets) result.push_back(ordinalOf_[target.index]);
                    return result;
                };
                for (const auto &row : schedule_.inputFanout)
                    inputFanout_[row.source.index] = ordinals(row.targets.activate);
                for (const auto &row : schedule_.computeSupernodeFanout)
                    supernodeFanout_[row.source.index] = ordinals(row.targets.activate);
                for (const auto &row : schedule_.commitStateFanout)
                    stateFanout_[row.source.index] = ordinals(row.targets.activate);

                // Repeated event-free writers can temporarily change a state
                // and restore it in the same round. Notify readers only for
                // the final change, otherwise these writes never quiesce.
                std::vector<uint32_t> freeWrites(model_.states().size() + 1);
                for (const auto &op : model_.operations())
                {
                    if (op.phase != SimPhase::General && op.phase != SimPhase::LargeSink) continue;
                    if (!isCpuPhaseMemWriteOp(model_.text(op.opType)) ||
                        !readCpuPhaseEventActs(model_, op).empty()) continue;
                    const auto refs = model_.objectRefs(op);
                    if (refs.size() == 1 && refs[0].kind == ObjectKind::State)
                        ++freeWrites[refs[0].index];
                }
                for (const auto &field : smallSinkStore_->fields)
                    if (freeWrites[field.state.index] > 1 && !stateFanout_[field.state.index].empty())
                        smallFinalChange_.insert(field.state.index);
                if (schedule_.memWritePlan)
                    for (const auto &entry : *schedule_.memWritePlan)
                    {
                        const auto &op = model_.operations()[entry.writeOp.index - 1];
                        const auto state = model_.objectRefs(op).front().index;
                        if (freeWrites[state] > 1 && !entry.readers.empty())
                            largeFinalChange_.try_emplace(state, entry.readers);
                    }
                activeLocals_.assign(model_.values().size() + 1, 0);

                std::unordered_map<std::string, uint32_t> randomSlots;
                for (const auto &op : model_.operations())
                {
                    for (const auto result : model_.results(op)) producers_[result.index] = op.id;
                    const auto name = model_.text(op.opType);
                    if (name == "core.system.function")
                    {
                        const auto *sample = parameter<int64_t>(model_, model_.parameters(op), "sample_id");
                        const auto key = sample && *sample > 0 ? "sample:" + std::to_string(*sample) :
                                                              "op:" + std::to_string(op.id.index);
                        const auto [slot, inserted] = randomSlots.try_emplace(key, randomSampleCount_);
                        if (inserted) ++randomSampleCount_;
                        randomFunctions_.emplace(op.id.index, slot->second);
                    }
                    if (name == "core.compute.constant" && model_.results(op).size() == 1)
                    {
                        const auto &resultType = type(model_.results(op)[0]);
                        if (isScalarLogic(resultType))
                            staticScalars_.emplace(model_.results(op)[0].index, expression(op));
                        else if (resultType.kind == TypeKind::String)
                            staticStrings_.emplace(model_.results(op)[0].index, expression(op));
                    }
                    if (name == "core.system.task")
                    {
                        hasSystemTasks_ = true;
                        const auto params = model_.parameters(op);
                        const auto *proc = parameter<std::string>(model_, params, "proc_kind");
                        const auto *timed = parameter<bool>(model_, params, "has_timing");
                        if (proc && *proc == "initial" && timed && *timed)
                            onceTasks_.emplace(op.id.index, onceTasks_.size());
                    }
                }
                for (uint32_t ordinal = 0; ordinal < supernodeCount_; ++ordinal)
                    for (const auto opId : supernodeOps_[ordinal])
                        if (randomFunctions_.contains(opId.index)) randomSupernodes_.insert(ordinal);
                // Constant-valued boundary fields are preloaded at init() so a
                // consumer can never observe a stale zero before the producer
                // supernode fires; the producer's compare-store then never fires.
                for (std::size_t storeIndex = 1; storeIndex < boundaryStores_.size(); ++storeIndex)
                    for (const auto &field : boundaryStores_[storeIndex]->fields)
                    {
                        const auto producer = producers_[field.value.index];
                        if (!producer) continue;
                        const auto &op = model_.operations()[producer.index - 1];
                        if (model_.text(op.opType) != "core.compute.constant") continue;
                        constBoundaryInit_.emplace(&field, expression(op));
                    }
                // Input port -> fanout ordinals, via the input.read producers of
                // the inputFanout row source values.
                for (uint32_t valueIndex = 0; valueIndex < inputFanout_.size(); ++valueIndex)
                {
                    if (inputFanout_[valueIndex].empty()) continue;
                    const auto producer = producers_[valueIndex];
                    if (!producer) throw std::runtime_error("CPU six-phase emit input fanout value has no producer");
                    const auto &op = model_.operations()[producer.index - 1];
                    if (model_.text(op.opType) != "core.input.read") continue;
                    const auto refs = model_.objectRefs(op);
                    if (refs.size() != 1 || refs[0].kind != ObjectKind::Input ||
                        refs[0].index == 0 || refs[0].index > model_.inputs().size())
                        throw std::runtime_error("CPU six-phase emit input fanout resolves to no input port");
                    auto &targets = inputPortFanout_[refs[0].index - 1];
                    for (const auto ordinal : inputFanout_[valueIndex])
                        if (std::find(targets.begin(), targets.end(), ordinal) == targets.end())
                            targets.push_back(ordinal);
                }
                // M5d-7: the emit TU plan drives the multi-file output. Chunk
                // vectors per kind in plan order; the index inside the vector
                // is the chunk function suffix (pEvent_c<i> etc.). Supernode
                // chunks carry their ordinal in offset.
                for (const auto &unit : tuPlan_.units)
                    for (const auto &chunk : unit.chunks)
                    {
                        const auto key = (uint64_t(static_cast<unsigned>(chunk.kind)) << 32) | chunk.offset;
                        switch (chunk.kind)
                        {
                        case CpuEmitChunkKind::Core: ++coreChunks_; break;
                        case CpuEmitChunkKind::Init: chunkIds_[key] = initChunks_.size(); initChunks_.push_back(chunk); break;
                        case CpuEmitChunkKind::Event: chunkIds_[key] = eventChunks_.size(); eventChunks_.push_back(chunk); break;
                        case CpuEmitChunkKind::GeneralScan: chunkIds_[key] = scanChunks_.size(); scanChunks_.push_back(chunk); break;
                        case CpuEmitChunkKind::Supernode: supernodeChunks_.push_back(chunk); break;
                        case CpuEmitChunkKind::SupernodePart: supernodePartChunks_.push_back(chunk); break;
                        case CpuEmitChunkKind::LargeSink: chunkIds_[key] = memChunks_.size(); memChunks_.push_back(chunk); break;
                        case CpuEmitChunkKind::Output: chunkIds_[key] = outputChunks_.size(); outputChunks_.push_back(chunk); break;
                        case CpuEmitChunkKind::Dump: chunkIds_[key] = dumpChunks_.size(); dumpChunks_.push_back(chunk); break;
                        }
                    }
                if (coreChunks_ != 1)
                    throw std::runtime_error("CPU six-phase emit TU plan holds " +
                                             std::to_string(coreChunks_) + " core chunks");
                {
                    std::vector<char> seen(supernodeCount_, 0);
                    for (const auto &chunk : supernodeChunks_)
                    {
                        if (chunk.count != 1 || chunk.offset >= supernodeCount_ || seen[chunk.offset])
                            throw std::runtime_error("CPU six-phase emit TU plan mis-covers the general supernodes");
                        seen[chunk.offset] = 1;
                    }
                    if (supernodeChunks_.size() != supernodeCount_)
                        throw std::runtime_error("CPU six-phase emit TU plan mis-covers the general supernodes");
                    // V3-M2: SupernodePart chunks trail a split supernode's
                    // wrapper. Each part index must appear exactly once per
                    // ordinal, and the recomputed part ranges (same unit cap
                    // as C8) must tile the supernode's helperChunks — this is
                    // what lets the emitter map part index -> helper range.
                    std::map<uint32_t, std::vector<uint32_t>> partsByOrdinal;
                    for (const auto &chunk : supernodePartChunks_)
                    {
                        if (chunk.offset >= supernodeCount_ || !seen[chunk.offset])
                            throw std::runtime_error("CPU six-phase emit TU plan supernode part without its wrapper");
                        partsByOrdinal[chunk.offset].push_back(chunk.count);
                    }
                    for (const auto &[ordinal, indices] : partsByOrdinal)
                    {
                        const auto ranges = cpuSupernodePartRanges(model_, tree_, order_[ordinal],
                                                                   tuPlan_.unitMaxEstimatedLines);
                        std::vector<char> partSeen(ranges.size(), 0);
                        for (const auto index : indices)
                        {
                            if (index >= ranges.size() || partSeen[index])
                                throw std::runtime_error("CPU six-phase emit TU plan mis-covers a split supernode");
                            partSeen[index] = 1;
                        }
                        if (indices.size() != ranges.size())
                            throw std::runtime_error("CPU six-phase emit TU plan mis-covers a split supernode");
                        splitSupernodes_.insert(ordinal);
                    }
                }
                // Init stream metadata (the cpu_init_<k> chunks iterate it by
                // position): flattened steps, then the constant-boundary
                // preloads in store offset order, then the prevEvent inits in
                // act order, then the smallSinkStoreNext sync.
                for (const auto &record : model_.initRecords())
                    for (const auto &step : model_.steps(record))
                        initSteps_.push_back({record.state, &step});
                for (const auto &[field, expr] : constBoundaryInit_) constBoundaryFields_.push_back(field);
                std::sort(constBoundaryFields_.begin(), constBoundaryFields_.end(),
                          [](const CpuStoreField *a, const CpuStoreField *b) { return a->offset < b->offset; });
                for (const auto &[act, det] : detByAct_) detActs_.push_back(act);
                const auto streamTotal = initSteps_.size() + constBoundaryFields_.size() + detActs_.size() + 1;
                checkChunkStream("init", initChunks_, streamTotal);
                checkChunkStream("event", eventChunks_, eventOps_.size());
                checkChunkStream("mem", memChunks_,
                                 schedule_.memWritePlan ? schedule_.memWritePlan->size() : 0);
                checkChunkStream("output", outputChunks_, outputOps_.size());
                checkChunkStream("dump", dumpChunks_, cpuDumpItemCount(model_, stores_));
                {
                    uint32_t covered = 0;
                    for (const auto &chunk : scanChunks_)
                    {
                        if (chunk.offset != covered || chunk.count == 0 ||
                            chunk.offset + chunk.count > supernodeCount_)
                            throw std::runtime_error("CPU six-phase emit TU plan scan chunks do not tile the supernodes");
                        covered += chunk.count;
                    }
                    if (covered != supernodeCount_)
                        throw std::runtime_error("CPU six-phase emit TU plan scan chunks do not tile the supernodes");
                }
                // The layout records a stable bit order for the two enable
                // boundary stores. Each bit mirrors a scalar boundary field
                // (or an input port read that bypasses a boundary field).
                const auto mapEnableBitmap = [&](EnableBitmap &bitmap, const CpuNamedStore &store,
                                                 CpuBoundaryUse use) {
                    for (const auto enable : store.enableBitValues)
                    {
                        const CpuStoreField *field = boundaryByUse_[enable.index][static_cast<std::size_t>(use)];
                        uint32_t inputIndex = 0;
                        if (!field)
                        {
                            const auto producer = producers_[enable.index];
                            const auto &producerOp = model_.operations()[producer.index - 1];
                            const auto refs = model_.objectRefs(producerOp);
                            inputIndex = refs.front().index;
                            field = boundaryByInput_[inputIndex];
                        }
                        const auto bit = static_cast<uint32_t>(bitmap.fields.size());
                        bitmap.fields.push_back(field);
                        bitmap.bitOfValue[enable.index] = bit;
                        if (inputIndex) bitmap.inputBits[inputIndex] = bit;
                        else bitmap.fieldBits[field] = bit;
                    }
                };
                mapEnableBitmap(smallEnable_, *boundaryStores_[2], CpuBoundaryUse::SmallSinkEnable);
                if (memEnableBitmap_)
                    mapEnableBitmap(largeEnable_, *boundaryStores_[4], CpuBoundaryUse::LargeSinkEnable);
                smallSinkEnableBits_.resize(supernodeCount_);
                smallSinkPriority_.resize(supernodeCount_);
                std::vector<uint32_t> perState(model_.states().size() + 1, 0);
                std::vector<uint32_t> priorityOfOp(model_.operations().size() + 1);
                for (const auto &op : model_.operations())
                {
                    if (op.phase != SimPhase::General) continue;
                    const auto name = model_.text(op.opType);
                    if (!isCpuPhaseMemWriteOp(name) && name != "core.state.latchWrite") continue;
                    const auto refs = model_.objectRefs(op);
                    if (refs.size() == 1 && refs[0].kind == ObjectKind::State)
                        priorityOfOp[op.id.index] = perState[refs[0].index]++;
                }
                for (uint32_t ordinal = nonSinkCount_; ordinal < supernodeCount_; ++ordinal)
                {
                    bool allWrites = !supernodeOps_[ordinal].empty();
                    std::vector<uint32_t> bits;
                    for (const auto opId : supernodeOps_[ordinal])
                    {
                        const auto &op = model_.operations()[opId.index - 1];
                        const auto enables = memEnableOperands(op);
                        if (enables.size() != 1) { allWrites = false; break; }
                        const auto bit = smallEnable_.bitOfValue.find(enables.front().index);
                        if (bit == smallEnable_.bitOfValue.end()) { allWrites = false; break; }
                        bits.push_back(bit->second);
                        smallSinkPriority_[ordinal] = priorityOfOp[opId.index];
                    }
                    if (allWrites)
                    {
                        std::sort(bits.begin(), bits.end());
                        bits.erase(std::unique(bits.begin(), bits.end()), bits.end());
                        smallSinkEnableBits_[ordinal] = std::move(bits);
                    }
                }
                // Spill frames: values produced in one chunk of an op list and
                // consumed in another (or at the output commit point) become
                // fields of a frame struct the driver passes to the chunks.
                supernodeFrames_.resize(supernodeCount_);
                for (uint32_t ordinal = 0; ordinal < supernodeCount_; ++ordinal)
                {
                    const auto &attrs = tree_.partitions[order_[ordinal].index - 1].attrs;
                    if (attrs.helperChunks.size() < 2) continue;
                    supernodeFrames_[ordinal] = computeFrameFields(supernodeOps_[ordinal], attrs.helperChunks, false);
                }
                if (eventChunks_.size() > 1)
                    eventFrame_ = computeFrameFields(eventOps_, eventChunks_, false);
                // The latchWrite commit point sits in the driver after every
                // chunk, so the output frame engages whenever cone-produced
                // latchWrite operands exist — even for a single-chunk cone.
                outputFrame_ = computeFrameFields(outputOps_, outputChunks_, true);
                crossingLocals_.assign(model_.values().size() + 1, 0);
                precomputeStagedOutputWrites();
                if (waveform_) collectWaveformSignals();
            }

            // Emission entry points (emitSixPhaseCpuCpp drives these).
            void validate() const;
            PassResult write(const std::filesystem::path &directory);

        private:
            struct DetInfo
            {
                uint32_t act = 0;
                std::string edge;
                std::string prevInit;
                ValueId event;
            };

            const GrhSimModel &model_;
            const CpuBackendMapping &mapping_;
            const CpuPartitionTree &tree_;
            const CpuDataLayout &layout_;
            const CpuSchedulePlan &schedule_;
            const std::vector<CpuNamedStore> &stores_;
            // M5d-7: the multi-TU emit plan (C8), required by the emit gate.
            const CpuTranslationUnitPlan &tuPlan_;
            std::string prefix_, class_;

            const CpuNamedStore *smallSinkStore_ = nullptr;
            const CpuNamedStore *largeSinkStore_ = nullptr;
            std::array<const CpuNamedStore *, 6> boundaryStores_{};
            const CpuNamedStore *prevEventStore_ = nullptr;
            const CpuNamedStore *eventActStore_ = nullptr;
            const CpuNamedStore *timeslotStore_ = nullptr;
            const CpuNamedStore *activeStore_ = nullptr;

            std::vector<PartitionId> order_;
            std::vector<uint32_t> ordinalOf_;
            uint32_t supernodeCount_ = 0;
            // V3-M1: non-sink supernodes occupy the ordinal prefix [0,
            // nonSinkCount_) (structural, C2 child order); only they read the
            // ActiveFlags bit words.
            uint32_t nonSinkCount_ = 0;
            // Word count of the ActiveFlags arrays, read back from the layout
            // field type (ceil(supernodeCount/64), at least 1).
            uint32_t activeWords_ = 1;
            std::vector<std::vector<OpId>> supernodeOps_;
            std::vector<OpId> eventOps_, outputOps_;

            std::vector<const CpuStoreField *> regFieldByState_, memFieldByState_;
            std::set<uint32_t> smallFinalChange_;
            std::map<uint32_t, std::vector<CpuMemReader>> largeFinalChange_;
            std::vector<const CpuStoreField *> boundaryByValue_, boundaryByInput_, prevByAct_;
            std::vector<std::array<const CpuStoreField *, 5>> boundaryByUse_;
            std::vector<std::vector<const CpuStoreField *>> boundaryFieldsByValue_;
            std::map<const CpuStoreField *, CpuNamedStoreKind> boundaryKindByField_;
            mutable CpuBoundaryUse readContext_ = CpuBoundaryUse::NonSink;
            std::map<uint32_t, DetInfo> detByAct_;
            std::map<uint32_t, const CpuEventActivation *> activationByAct_;
            std::vector<std::vector<uint32_t>> triggersByAct_;
            uint32_t maxAct_ = 0, maxTimeslotFlag_ = 0;

            // ----- M5d-7 multi-TU emit plan consumption -----
            uint32_t coreChunks_ = 0;
            std::vector<CpuEmitChunk> initChunks_, eventChunks_, scanChunks_, supernodeChunks_,
                memChunks_, outputChunks_, dumpChunks_;
            // V3-M2: SupernodePart chunks (offset = ordinal, count = part
            // index) and the ordinals whose Supernode chunk is a wrapper-only
            // chunk for a split supernode.
            std::vector<CpuEmitChunk> supernodePartChunks_;
            std::set<uint32_t> splitSupernodes_;
            // (kind, offset) -> the chunk's global per-kind index, which is the
            // chunk function suffix (cpu_init_<i>, pEvent_c<i>, ...).
            std::unordered_map<uint64_t, uint32_t> chunkIds_;
            // Init stream: flattened (state, step) pairs, then constant-boundary
            // preloads (store offset order), then prevEvent inits (act order),
            // then the smallSinkStoreNext sync line.
            std::vector<std::pair<StateId, const InitStep *>> initSteps_;
            std::vector<const CpuStoreField *> constBoundaryFields_;
            std::vector<uint32_t> detActs_;
            // Spill frames (values crossing chunk boundaries): per chunked
            // supernode (indexed by ordinal), plus the Event/Output cone frames.
            std::vector<std::vector<uint32_t>> supernodeFrames_;
            std::vector<uint32_t> eventFrame_, outputFrame_;
            // Scratch: values spilled to the frame of the chunk list currently
            // being emitted (their cpu_v local becomes the cpu_f.v field).
            mutable std::vector<char> crossingLocals_;

            std::vector<std::vector<uint32_t>> inputFanout_, supernodeFanout_, stateFanout_;
            // String-typed boundary fields hoisted out of the (trivially
            // copyable) BoundaryValueStore struct into boundaryStrings.
            std::map<const CpuStoreField *, uint32_t> boundaryStringSlot_;
            uint32_t boundaryStringCount_ = 0;
            // Input port index (1-based InputId) -> fanout ordinals, derived
            // from inputFanout_ rows through the producing input.read op.
            std::vector<std::vector<uint32_t>> inputPortFanout_;
            std::vector<OpId> producers_;
            std::unordered_map<uint32_t, std::string> staticScalars_, staticStrings_;
            std::map<const CpuStoreField *, std::string> constBoundaryInit_;
            bool hasSystemTasks_ = false;
            // Output-activity gate: the P_output cone chunks run only when an
            // output-read state/boundary/input changed this eval.  The flag is
            // raised only for output-read dependencies (emitted conditionally
            // at emit time, so non-output writes pay nothing).
            bool outputGate_ = false;
            std::unordered_set<uint32_t> outputReadStates_;
            std::unordered_set<uint32_t> outputReadValues_;
            std::unordered_set<uint32_t> outputReadInputs_;
            std::unordered_map<uint32_t, uint32_t> onceTasks_;
            std::unordered_map<uint32_t, uint32_t> randomFunctions_;
            std::set<uint32_t> randomSupernodes_;
            uint32_t randomSampleCount_ = 0;

            struct EnableBitmap
            {
                std::string_view storeName;
                std::vector<const CpuStoreField *> fields;
                std::unordered_map<uint32_t, uint32_t> bitOfValue;
                std::map<const CpuStoreField *, uint32_t> fieldBits;
                std::unordered_map<uint32_t, uint32_t> inputBits;
                uint32_t words() const { return static_cast<uint32_t>((fields.size() + 63) / 64); }
            };
            EnableBitmap smallEnable_{"smallSinkEnableBoundaryStore"};
            EnableBitmap largeEnable_{"largeSinkEnableBoundaryStore"};
            std::vector<std::vector<uint32_t>> smallSinkEnableBits_;
            std::vector<uint32_t> smallSinkPriority_;
            // P_large_sink bitmap can be disabled for profiling. The
            // smallSink bitmap always backs its call-site batch skip.
            bool memEnableBitmap_ = true;

            // P_mem write-occurs activation knob (memWriteOnWrite_ = the
            // --mem-write-activate on-write emit option): drop the per-cell
            // change compare and activate the array's readers once per enable
            // word run (gsim-style write-occurs). Event-free writes (no
            // eventActStore guard) keep the on-change compare — they re-enter
            // every round and would never converge with write-occurs
            // activation.
            bool memWriteOnWrite_ = false;

            // Per-function value locals: values with a live cpu_v<index> local in
            // the function currently being emitted.
            mutable std::vector<char> activeLocals_;
            // stage-07 fanout 合写：激活旗标在函数内写入局部累积变量、函数尾
            // 一次写回 ActiveFlags——store 密集函数里同一 flag 词的 |= 互相触发
            // DAGCombiner worklist 重排队（实测 visitSTORE 94%、单 TU 80min+）；
            // 累积后每函数每词仅一次 store。只含 supernode 体/助手函数；函数内
            // 对这些数组只写不读（调度器在函数间读），延后写回语义等价。
            mutable bool flagAccumActive_ = false;
            mutable std::map<std::string, std::set<uint32_t>> flagAccumTouched_;
            // Output-phase latchWrite staging: enable/data/mask texts captured
            // while the cone locals are live; committed after every output op
            // ran. enable empty means unconditional (constant-1 enable).
            struct StagedOutputWrite
            {
                const CpuStoreField *field = nullptr;
                std::string enable, data, mask;
            };
            mutable std::vector<StagedOutputWrite> stagedOutputWrites_;

            // ----- Semantic type/value helpers (ported from the legacy emitter) -----
            const Type &type(ValueId value) const;
            const Type &stateType(StateId state) const;
            bool containsString(const Type &type) const;
            bool isScalarLogic(const Type &type) const;
            std::string cppType(const Type &type) const;
            std::string cppValueType(const Type &type) const;
            std::string unpackValue(const std::string &packed, const Type &type) const;
            // Emit shaped compute values directly when all operands already
            // have row-backed storage. This avoids the packed _BitInt bridge
            // for simple element-wise array operations.
            std::optional<std::string> directShapedExpression(const SimOp &op) const;
            // A directly constant shaped value can be emitted as a constexpr
            // aggregate. This keeps the packed literal and its row slices out
            // of every supernode invocation.
            std::optional<std::string> constantArrayInitializer(ValueId value) const;
            std::string cppStoreType(CpuTypeId id) const;
            uint64_t storageBytes(const Type &type) const;
            // Stack-local scalar element view of a shaped (array) logic type:
            // width = typeElementWidth, isSigned/domain inherited.
            static Type arrayElementType(const Type &type);

            // ----- FST waveform (declared-symbols mode) -----
            // One dumpable declared symbol (or one ELEMENT of a shaped
            // symbol — stage-06 §8 Q3 per-element registration): ports read
            // from the interface members, registers/wires from their store
            // fields. ref is the member-access expression (its address feeds
            // the change detect).
            struct WaveSignal
            {
                std::string name;
                std::string ref;
                uint64_t bytes = 0;
                uint32_t width = 0;
                uint32_t prevOff = 0;
            };
            bool waveform_ = false;
            std::vector<WaveSignal> waveSignals_;
            uint64_t wavePrevWords_ = 0;
            // Setup chunking: one member function per kWaveSignalsPerChunk
            // signals, grouped kWaveChunksPerFile to a .cpp file.
            static constexpr uint32_t kWaveSignalsPerChunk = 4096;
            static constexpr uint32_t kWaveChunksPerFile = 8;
            uint32_t waveChunkCount() const
            {
                return static_cast<uint32_t>((waveSignals_.size() + kWaveSignalsPerChunk - 1) / kWaveSignalsPerChunk);
            }
            void collectWaveformSignals();
            void waveSetupFile(std::ostream &out, uint32_t fileIndex) const;
            void waveformGlue(std::ostream &out) const;
            static std::string escapeWaveName(std::string_view name);
            std::string normalize(std::string expression, const Type &type) const;
            std::string literal(std::string_view text, const Type &type) const;
            std::string initLiteral(std::string_view text, const Type &type) const;
            void randomInit(std::ostream &out, const Type &type, std::string_view destination,
                            std::string_view rng) const;
            // Resolves a value to its read text: static constant folding, a live
            // cpu_v<index> local, a boundary store field, or the producer's store
            // read (input port field / regLatch field / mem cell / wide literal).
            std::string read(ValueId value) const;
            std::string readAs(ValueId value, CpuBoundaryUse use) const;
            // Declares the value's cpu_v local (or reference) when it has no
            // boundary field; the text later read() calls resolve to.
            void declareLocal(std::ostream &out, ValueId result, const std::string &expr) const;
            template <typename Raw, typename TypeOf, typename Number>
            std::string scalarExpression(std::string_view kind, const Type &result, std::size_t arity,
                                         const Raw &raw, const TypeOf &typeOf, const Number &number) const;
            std::string fusedExpression(const SimOp &op) const;
            static std::vector<std::string_view> splitExprToken(const std::string &token);
            std::string expression(const SimOp &op) const;
            std::optional<std::uint64_t> scalarConstantValue(ValueId operand) const;
            // True when the operand is a core.compute.constant whose value is
            // all ones in `width` bits (a full write mask). Wide constants are
            // read back through the literal text (slang SVInt).
            bool isAllOnesConstant(ValueId operand, uint32_t width) const;

            // ----- Guards / activation -----
            // OR of the op's event_acts bits in eventActStore; "true" when event-free.
            std::string actBitsGuard(std::span<const int64_t> acts) const;
            std::string actGuard(const SimOp &op) const;
            // Per-op guard inside a P_general supernode body (V2-M3): sink
            // bodies drop it (the call-site gate already covers every op),
            // non-sink bodies keep it.
            std::string bodyGuard(const SimOp &op, uint32_t current) const;
            std::string callCondition(ValueId condition) const;
            // Fanout activation statements. When a firing supernode ordinal is
            // given, later ordinals raise nonSinkActiveFlag directly (same-round
            // successor firing) and earlier/self ordinals queue into
            // nonSinkActiveFlagNext; otherwise every target raises `array`.
            void activateOrdinals(std::ostream &out, std::span<const uint32_t> ordinals,
                                  std::string_view array) const;
            void activateFanout(std::ostream &out, std::span<const uint32_t> ordinals,
                                uint32_t current) const;
            // Branchless twins (E-flat publish): the batched word masks are ORed
            // under -uint64(cond) instead of guarding the ORs with a branch.
            void activateOrdinalsCond(std::ostream &out, std::span<const uint32_t> ordinals,
                                      std::string_view array, std::string_view cond) const;
            void activateFanoutCond(std::ostream &out, std::span<const uint32_t> ordinals,
                                    uint32_t current, std::string_view cond) const;
            // fanout 合写（stage-07）：累积词的声明与函数尾写回。
            void flagAccumDecls(std::ostream &out) const;
            void flagAccumWriteBack(std::ostream &out) const;
            // Compare-store of a computed boundary value; on a real change the
            // supernodeFanout ordinals fire (same-round split by current).
            void publishBoundary(std::ostream &out, ValueId result, const std::string &expr,
                                  uint32_t current, bool arrayValue = false) const;

            // ----- Per-op emission -----
            void emitCompute(std::ostream &out, const SimOp &op, uint32_t current) const;
            void emitEdgeDet(std::ostream &out, const SimOp &op) const;
            void emitRegWrite(std::ostream &out, const SimOp &op, uint32_t current) const;
            void emitMemWrite(std::ostream &out, const SimOp &op, const CpuMemWritePlanEntry &plan,
                              bool actHoisted = false, std::set<uint32_t> *burstReaders = nullptr,
                              bool forceOnChange = false, bool enableHoisted = false) const;
            // Enable operand of a write/writeArray/fill op (always the first
            // operand); empty for every other op kind.
            std::vector<ValueId> memEnableOperands(const SimOp &op) const;
            // The port enable condition: dense bitmap bit test when the enable
            // is shadowed, otherwise the plain read() text.
            std::string memEnableGuard(ValueId enable) const;
            // Keep largeSinkEnableBoundaryStore.enabledWords[bit] in sync after a write of the mirrored
            // boundary field changed it to `valueText`.
            std::string enableSync(const EnableBitmap &bitmap, uint32_t bit,
                                   const std::string &valueText) const;
            // General-phase regLatch-class array writes (M5d-6): NBA merge
            // into smallSinkStoreNext inside the owning supernode.
            void emitGeneralMemWrite(std::ostream &out, const SimOp &op, uint32_t current) const;
            void emitSystemTask(std::ostream &out, const SimOp &op, uint32_t current) const;
            void emitOutputTask(std::ostream &out, const SimOp &op) const;
            std::string sideCallExtras(const SimOp &op) const;
            void systemTaskBody(std::ostream &out, const SimOp &op) const;
            void emitDpiCall(std::ostream &out, const SimOp &op, uint32_t current) const;
            void publishDpiResult(std::ostream &out, ValueId result, const std::string &temporary,
                                  uint32_t current) const;
            std::string dpiType(TypeId id) const;
            std::string dpiDeclaration(const ExternFunction &function) const;
            void emitOutputWrite(std::ostream &out, const SimOp &op) const;
            void commitStagedOutputWrites(std::ostream &out) const;

            // ----- Validation -----
            void validatePort(std::string_view name, std::set<std::string> &names) const;
            void validateSystemTask(const SimOp &op) const;
            void validateSystemFunction(const SimOp &op) const;
            void validateDpiCall(const SimOp &op) const;
            void validateInit() const;

            // ----- init (ported; arena accesses become named-store field accesses) -----
            using InitRows = std::vector<std::pair<uint64_t, std::string>>;
            bool initZeroElidable(const InitStep &step) const;
            const InitRows &readmemRows(const InitStep &step, const Type &element, uint64_t first,
                                        uint64_t end) const;
            void initStep(std::ostream &out, StateId id, const InitStep &step) const;

            // ----- M5d-7 chunking support -----
            // Light emit-side sanity: the kind's chunk ranges tile [0, total).
            static void checkChunkStream(std::string_view kind, const std::vector<CpuEmitChunk> &chunks,
                                         uint64_t total);
            // Values produced inside `ops` that are consumed in a different
            // chunk (ranges tile the op list) — the spill frame fields. With
            // commitUse, latchWrite operands count as consumed at the trailing
            // commit point (the P_output driver, after every chunk ran).
            std::vector<uint32_t> computeFrameFields(const std::vector<OpId> &ops,
                                                     std::span<const Range> chunks, bool commitUse) const;
            std::vector<uint32_t> computeFrameFields(const std::vector<OpId> &ops,
                                                     const std::vector<CpuEmitChunk> &chunks, bool commitUse) const;
            // Captures the latchWrite staged texts with the final local/frame
            // resolution (the commit runs after all output chunks).
            void precomputeStagedOutputWrites();
            void activateCrossing(const std::vector<uint32_t> &frame) const;
            void deactivateCrossing(const std::vector<uint32_t> &frame) const;
            // cpu_v<index> or cpu_f.v<index> when the value is frame-spilled.
            std::string localRef(ValueId value) const;
            // A supernode spill frame is normally value-initialized because a
            // guarded producer may leave a field untouched.  Pure compute and
            // state-read producers are emitted as unconditional assignments;
            // their frame can therefore start uninitialized and avoid a large
            // memset on every supernode call.
            bool frameNeedsZeroInit(const std::vector<uint32_t> &fields) const;
            // Boundary field storage reference: boundaryValueStore.<name>, or
            // boundaryStrings[<i>] for hoisted string fields.
            std::string boundaryRef(const CpuStoreField *field) const;
            // Stage-06 emit fast path: a shaped (array) value that is backed
            // by per-row storage (a state field, or the per-row boundary form
            // produced by the layout for shaped values) returns that row
            // array's base text, so sliceArray can index rows directly instead
            // of packing the total-width vector first. Returns nullopt when
            // the value resolves to a flat local/constant form.
            std::optional<std::string> rowBackedArrayText(ValueId value) const;
            void frameStruct(std::ostream &out, std::string_view name,
                             const std::vector<uint32_t> &fields) const;

            // ----- File bodies -----
            void header(std::ostream &out) const;
            // Supernode member function name: sn_<ordinal>[_<first op name>].
            std::string supernodeName(uint32_t ordinal) const;
            bool storeTriviallyResettable(const CpuNamedStore &store) const;
            void storeStruct(std::ostream &out, std::string_view structName,
                             const CpuNamedStore &store) const;
            // Per-unit source body: the unit's chunks in plan order.
            void unitCpp(std::ostream &out, const CpuTranslationUnit &unit) const;
            void coreChunk(std::ostream &out) const;
            void supernodeBody(std::ostream &out, uint32_t ordinal) const;
            void supernodeWrapper(std::ostream &out, uint32_t ordinal) const;
            void supernodeHelperRange(std::ostream &out, uint32_t ordinal, uint32_t begin, uint32_t end) const;
            void supernodeChunkFns(std::ostream &out, uint32_t ordinal) const;
            void emitOpListRange(std::ostream &out, const std::vector<OpId> &ops, uint32_t offset,
                                 uint32_t count, const std::vector<uint32_t> &frame, bool outputPhase) const;
            void initChunkFn(std::ostream &out, uint32_t id, const CpuEmitChunk &chunk) const;
            void initStreamItem(std::ostream &out, uint64_t position) const;
            void eventChunkFn(std::ostream &out, uint32_t id, const CpuEmitChunk &chunk) const;
            void scanChunkFn(std::ostream &out, uint32_t id, const CpuEmitChunk &chunk) const;
            void scanRange(std::ostream &out, uint32_t begin, uint32_t end) const;
            void memChunkFn(std::ostream &out, uint32_t id, const CpuEmitChunk &chunk) const;
            void outputChunkFn(std::ostream &out, uint32_t id, const CpuEmitChunk &chunk) const;
            void dumpChunkFn(std::ostream &out, uint32_t id, const CpuEmitChunk &chunk) const;
            void dumpStreamItem(std::ostream &out, uint64_t position) const;
            void pInputBody(std::ostream &out) const;
            void pEventBody(std::ostream &out) const;
            void pGeneralBody(std::ostream &out) const;
            void pLargeSinkBody(std::ostream &out) const;
            void pPublishBody(std::ostream &out) const;
            void pOutputBody(std::ostream &out) const;
            void evalBody(std::ostream &out) const;
            void initGlue(std::ostream &out) const;
            void dumpStateBody(std::ostream &out) const;
            void dumpStateField(std::ostream &out, std::string_view store, const CpuStoreField &field) const;
            void systemTaskDriver(std::ostream &out) const;

            // Supernode ordinal whose function is being emitted (~0 = none).
            mutable std::map<const InitStep *, InitRows> readmemRows_;
            mutable bool initElidableBuilt_ = false;
            mutable std::set<const InitStep *> initElidable_;
        };

        // M5A_APPEND
        // ----- M5d-7 chunking support -----
        void SixPhaseEmitter::checkChunkStream(std::string_view kind, const std::vector<CpuEmitChunk> &chunks,
                                               uint64_t total)
        {
            uint64_t covered = 0;
            for (const auto &chunk : chunks)
            {
                if (chunk.offset != covered || chunk.count == 0 || chunk.offset + chunk.count > total)
                    throw std::runtime_error("CPU six-phase emit TU plan " + std::string(kind) +
                                             " chunks do not tile their stream");
                covered += chunk.count;
            }
            if (covered != total)
                throw std::runtime_error("CPU six-phase emit TU plan " + std::string(kind) +
                                         " chunks do not tile their stream");
        }
        std::vector<uint32_t> SixPhaseEmitter::computeFrameFields(const std::vector<OpId> &ops,
                                                                  std::span<const Range> chunks,
                                                                  bool commitUse) const
        {
            std::vector<uint32_t> fields;
            if (chunks.size() < 2 && !commitUse) return fields;
            std::vector<uint32_t> positionOf(model_.operations().size() + 1, ~0u);
            for (uint32_t i = 0; i < ops.size(); ++i) positionOf[ops[i].index] = i;
            std::vector<uint32_t> chunkOf(ops.size());
            uint32_t chunkIndex = 0, chunkEnd = chunks.empty() ? 0 : chunks[0].offset + chunks[0].count;
            for (uint32_t i = 0; i < ops.size(); ++i)
            {
                while (i >= chunkEnd && chunkIndex + 1 < chunks.size())
                {
                    ++chunkIndex;
                    chunkEnd = chunks[chunkIndex].offset + chunks[chunkIndex].count;
                }
                chunkOf[i] = chunkIndex;
            }
            // Only values whose producer creates an irreplaceable local need a
            // frame slot. Constants fold into use sites; input.read/state.read
            // results re-read their store field in any chunk (the port boundary
            // fields and smallSinkStore/largeSinkStore are stable across a chunk
            // sequence). readArray results are NOT freely re-readable: the
            // re-expression re-reads the index operands, whose locals may be
            // dead — so readArray results spill like compute values. The commit
            // point (P_output driver) additionally frames state reads: a store
            // re-read there could observe an earlier staged commit.
            const auto localBearing = [&](ValueId value, bool forCommit) {
                const auto producer = producers_[value.index];
                if (!producer || positionOf[producer.index] == ~0u) return false;
                if (boundaryByValue_[value.index] || staticScalars_.contains(value.index) ||
                    staticStrings_.contains(value.index))
                    return false;
                const auto opName = model_.text(model_.operations()[producer.index - 1].opType);
                if (opName == "core.input.read" || opName == "core.compute.constant") return false;
                if (!forCommit && opName == "core.state.read") return false;
                return true;
            };
            std::vector<char> mark(model_.values().size() + 1, 0);
            for (uint32_t q = 0; q < ops.size(); ++q)
            {
                const auto &op = model_.operations()[ops[q].index - 1];
                const bool commit = commitUse && model_.text(op.opType) == "core.state.latchWrite";
                for (const auto operand : model_.operands(op))
                {
                    if (!localBearing(operand, commit)) continue;
                    const auto p = positionOf[producers_[operand.index].index];
                    if (commit || chunkOf[p] != chunkOf[q]) mark[operand.index] = 1;
                }
            }
            for (uint32_t index = 1; index < mark.size(); ++index)
                if (mark[index]) fields.push_back(index);
            return fields;
        }
        std::vector<uint32_t> SixPhaseEmitter::computeFrameFields(const std::vector<OpId> &ops,
                                                                  const std::vector<CpuEmitChunk> &chunks,
                                                                  bool commitUse) const
        {
            std::vector<Range> ranges;
            ranges.reserve(chunks.size());
            for (const auto &chunk : chunks) ranges.push_back({chunk.offset, chunk.count});
            return computeFrameFields(ops, ranges, commitUse);
        }
        void SixPhaseEmitter::activateCrossing(const std::vector<uint32_t> &frame) const
        { for (const auto index : frame) crossingLocals_[index] = 1; }
        void SixPhaseEmitter::deactivateCrossing(const std::vector<uint32_t> &frame) const
        { for (const auto index : frame) crossingLocals_[index] = 0; }
        std::string SixPhaseEmitter::localRef(ValueId value) const
        {
            return (crossingLocals_[value.index] ? "cpu_f.v" : "cpu_v") + std::to_string(value.index);
        }
        std::string SixPhaseEmitter::boundaryRef(const CpuStoreField *field) const
        {
            if (const auto it = boundaryStringSlot_.find(field); it != boundaryStringSlot_.end())
                return "boundaryStrings[" + std::to_string(it->second) + ']';
            const auto kind = boundaryKindByField_.at(field);
            std::string_view store;
            switch (kind)
            {
            case CpuNamedStoreKind::Input: store = "inputStore"; break;
            case CpuNamedStoreKind::NonSinkBoundary: store = "nonSinkBoundaryStore"; break;
            case CpuNamedStoreKind::SmallSinkEnableBoundary: store = "smallSinkEnableBoundaryStore"; break;
            case CpuNamedStoreKind::SmallSinkPayloadBoundary: store = "smallSinkPayloadBoundaryStore"; break;
            case CpuNamedStoreKind::LargeSinkEnableBoundary: store = "largeSinkEnableBoundaryStore"; break;
            case CpuNamedStoreKind::LargeSinkPayloadBoundary: store = "largeSinkPayloadBoundaryStore"; break;
            default: throw std::runtime_error("CPU boundary field has a non-boundary store kind");
            }
            return std::string(store) + '.' + std::string(model_.text(field->name));
        }
        void SixPhaseEmitter::frameStruct(std::ostream &out, std::string_view name,
                             const std::vector<uint32_t> &fields) const
        {
            out << "struct " << name << "{\n";
            for (const auto index : fields)
                // The driver decides whether the frame needs value
                // initialization.  Keeping the members plain lets the safe
                // uninitialized case avoid zeroing the whole aggregate.
                out << cppValueType(type(ValueId{index, 0})) << " v" << index << ";\n";
            out << "};\n";
        }
        bool SixPhaseEmitter::frameNeedsZeroInit(const std::vector<uint32_t> &fields) const
        {
            for (const auto index : fields)
            {
                if (index >= producers_.size() || !producers_[index]) return true;
                const auto &producer = model_.operations()[producers_[index].index - 1];
                const auto name = model_.text(producer.opType);
                // emitDpiCall wraps the call in its condition; a missed call
                // must leave the frame result at the IR-defined zero value.
                // The ordinary compute and state-read paths assign their
                // result unconditionally whenever the enclosing supernode
                // runs.  Other producer families are kept conservative.
                if (name == "core.dpi.call") return true;
                if (!(name.starts_with("core.compute.") ||
                      name == "core.state.read" || name == "core.state.readArray" ||
                      name == "core.system.function"))
                    return true;
            }
            return false;
        }
        void SixPhaseEmitter::precomputeStagedOutputWrites()
        {
            // Runs once at construction. The commit (P_output driver) executes
            // after every chunk's locals died, so the captured texts must be
            // commit-safe: frame fields (every cone-produced latchWrite operand
            // is spilled), boundary/state store reads, or constants. No local
            // is ever live at the commit point, so none is marked here.
            stagedOutputWrites_.clear();
            activateCrossing(outputFrame_);
            std::fill(activeLocals_.begin(), activeLocals_.end(), 0);
            for (const auto opId : outputOps_)
            {
                const auto &op = model_.operations()[opId.index - 1];
                if (model_.text(op.opType) != "core.state.latchWrite") continue;
                const auto operands = model_.operands(op);
                const auto refs = model_.objectRefs(op);
                if (refs.empty() || refs[0].kind != ObjectKind::State || operands.size() != 3)
                    throw std::runtime_error("CPU six-phase emit malformed output latchWrite");
                const auto *field =
                    refs[0].index < regFieldByState_.size() ? regFieldByState_[refs[0].index] : nullptr;
                if (!field && refs[0].index < memFieldByState_.size())
                    field = memFieldByState_[refs[0].index];
                if (!field)
                    throw std::runtime_error("CPU six-phase emit output latchWrite target has no sink store field");
                StagedOutputWrite staged;
                staged.field = field;
                staged.enable = read(operands[0]);
                staged.data = read(operands[1]);
                staged.mask = read(operands[2]);
                stagedOutputWrites_.push_back(std::move(staged));
            }
            deactivateCrossing(outputFrame_);
            std::fill(activeLocals_.begin(), activeLocals_.end(), 0);
        }

        // ----- Semantic type/value helpers -----
        const Type &SixPhaseEmitter::type(ValueId value) const
        { return model_.types()[model_.values()[value.index - 1].type.index - 1]; }
        const Type &SixPhaseEmitter::stateType(StateId state) const
        { return model_.types()[model_.states()[state.index - 1].type.index - 1]; }
        bool SixPhaseEmitter::containsString(const Type &type) const
        {
            // Stage-06: shape is a structural view on logic values — only a
            // plain String kind carries a string.
            return type.kind == TypeKind::String;
        }
        bool SixPhaseEmitter::isScalarLogic(const Type &type) const
        {
            return type.kind == TypeKind::Logic && type.domain == LogicDomain::TwoState &&
                type.width > 0 && type.width <= 64;
        }
        // The packed C++ type used while evaluating logic expressions.
        std::string SixPhaseEmitter::cppType(const Type &type) const
        {
            if (type.kind == TypeKind::String) return "std::string";
            if (type.kind == TypeKind::Real) return "double";
            if (type.kind != TypeKind::Logic)
                throw std::runtime_error("CPU six-phase emit non-logic scalar storage is not implemented");
            if (type.domain != LogicDomain::TwoState || type.width == 0)
                throw std::runtime_error("CPU six-phase emit scalar logic width/domain is not implemented");
            if (type.width > 64) return wideType(type.width);
            if (type.width == 1 && !type.isSigned) return "bool";
            return "std::" + std::string(type.isSigned ? "int" : "uint") +
                   std::to_string(type.width <= 8 ? 8 : type.width <= 16 ? 16 : type.width <= 32 ? 32 : 64) + "_t";
        }
        std::string SixPhaseEmitter::cppValueType(const Type &type) const
        {
            if (type.shape.empty()) return cppType(type);
            std::string result = cppType(arrayElementType(type));
            for (std::size_t dimension = type.shape.size(); dimension-- > 0;)
                result = "std::array<" + result + "," + std::to_string(type.shape[dimension]) + ">";
            return result;
        }
        std::string SixPhaseEmitter::unpackValue(const std::string &packed, const Type &type) const
        {
            if (type.shape.empty()) return packed;
            const auto rows = shapeElementCount(type.shape);
            const auto elementWidth = typeElementWidth(type);
            if (!rows || !elementWidth)
                throw std::runtime_error("CPU six-phase emit shaped value has an invalid shape");
            const Type element = arrayElementType(type);
            return "([&](){" + cppValueType(type) + " cpu_array{};const auto cpu_packed=" + packed +
                   ";for(std::size_t cpu_j=0;cpu_j<" + std::to_string(*rows) + ";++cpu_j)" +
                   rowAccessText("cpu_array", type.shape, "cpu_j") + '=' +
                   normalize(rowSliceText("cpu_packed", "cpu_j", *elementWidth, type.width), element) +
                   ";return cpu_array;}())";
        }
        std::string SixPhaseEmitter::cppStoreType(CpuTypeId id) const
        {
            const auto &type = layout_.types[id.index - 1];
            switch (type.kind)
            {
            case CpuTypeKind::Bool: return "bool";
            case CpuTypeKind::UInt:
                if (type.width > 64) return wideType(type.width);
                return "std::uint" + std::to_string(type.width <= 8 ? 8 : type.width <= 16 ? 16 : type.width <= 32 ? 32 : 64) + "_t";
            case CpuTypeKind::SInt:
                return "std::int" + std::to_string(type.width <= 8 ? 8 : type.width <= 16 ? 16 : type.width <= 32 ? 32 : 64) + "_t";
            case CpuTypeKind::F32: return "float";
            case CpuTypeKind::F64: return "double";
            case CpuTypeKind::String: return "std::string";
            case CpuTypeKind::Array:
                return "std::array<" + cppStoreType(type.elementType) + "," + std::to_string(type.count) + ">";
            }
            throw std::runtime_error("CPU six-phase emit unknown store type kind");
        }
        uint64_t SixPhaseEmitter::storageBytes(const Type &type) const
        {
            // Flat value bytes (the shape is already folded into width); the
            // per-row physical array bytes are rows * storageBytes(element).
            if (type.kind != TypeKind::Logic || type.width == 0 || type.domain != LogicDomain::TwoState)
                throw std::runtime_error("CPU six-phase emit requires two-state logic storage");
            return type.width > 64 ? ((uint64_t(type.width) + 63u) / 64u) * 8u :
                   type.width <= 8 ? 1 : type.width <= 16 ? 2 : type.width <= 32 ? 4 : 8;
        }
        Type SixPhaseEmitter::arrayElementType(const Type &type)
        {
            const std::optional<uint32_t> elementWidth = typeElementWidth(type);
            if (type.kind != TypeKind::Logic || type.shape.empty() || !elementWidth)
                throw std::runtime_error("CPU six-phase emit array type has an invalid shape");
            Type element;
            element.kind = TypeKind::Logic;
            element.width = *elementWidth;
            element.isSigned = type.isSigned;
            element.domain = type.domain;
            return element;
        }
        std::optional<std::string> SixPhaseEmitter::constantArrayInitializer(ValueId value) const
        {
            const auto &valueType = type(value);
            if (valueType.kind != TypeKind::Logic || valueType.domain != LogicDomain::TwoState ||
                valueType.shape.empty())
                return std::nullopt;
            const auto rows = shapeElementCount(valueType.shape);
            const auto elementWidth = typeElementWidth(valueType);
            // Keep generated source bounded for very large literal arrays. The
            // existing packed path remains correct for those values.
            if (!rows || !elementWidth || *rows < 16 || *rows > 4096 || *elementWidth > 64)
                return std::nullopt;
            if (value.index >= producers_.size()) return std::nullopt;

            using Bits = std::vector<uint8_t>;
            std::unordered_map<uint32_t, Bits> cache;
            std::unordered_set<uint32_t> failed, active;
            const auto resizeBits = [](const Bits &source, uint32_t width) {
                Bits result(width, 0);
                for (uint32_t bit = 0; bit < width && bit < source.size(); ++bit) result[bit] = source[bit];
                return result;
            };
            const auto asUnsigned = [](const Bits &bits) {
                uint64_t result = 0;
                for (uint32_t bit = 0; bit < bits.size() && bit < 64; ++bit)
                    result |= uint64_t(bits[bit]) << bit;
                return result;
            };
            const auto resolve = [&](const auto &self, ValueId current) -> std::optional<Bits> {
                if (const auto it = cache.find(current.index); it != cache.end()) return it->second;
                if (failed.contains(current.index) || active.contains(current.index) ||
                    current.index >= producers_.size())
                    return std::nullopt;
                const auto &currentType = type(current);
                if (currentType.kind != TypeKind::Logic || currentType.domain != LogicDomain::TwoState ||
                    currentType.width == 0)
                    return std::nullopt;
                const auto producer = producers_[current.index];
                if (!producer) return std::nullopt;
                const auto &op = model_.operations()[producer.index - 1];
                const auto name = model_.text(op.opType);
                active.insert(current.index);
                std::optional<Bits> resolved;
                if (name == "core.compute.constant")
                {
                    std::string text;
                    const auto params = model_.parameters(op);
                    if (const auto *literalText = parameter<std::string>(model_, params, "value"))
                        text = *literalText;
                    else if (const auto *literalText = parameter<std::string>(model_, params, "constValue"))
                        text = *literalText;
                    else if (const auto *integer = parameter<int64_t>(model_, params, "value"))
                        text = std::to_string(*integer);
                    else if (const auto *integer = parameter<int64_t>(model_, params, "constValue"))
                        text = std::to_string(*integer);
                    else if (const auto *boolean = parameter<bool>(model_, params, "value"))
                        text = *boolean ? "1" : "0";
                    else if (const auto *boolean = parameter<bool>(model_, params, "constValue"))
                        text = *boolean ? "1" : "0";
                    if (!text.empty())
                        try
                        {
                            auto parsed = slang::SVInt::fromString(text).resize(currentType.width);
                            parsed.flattenUnknowns();
                            const auto *raw = parsed.getRawPtr();
                            Bits bits(currentType.width, 0);
                            for (uint32_t bit = 0; bit < currentType.width; ++bit)
                                bits[bit] = uint8_t((raw[bit / 64] >> (bit % 64)) & 1u);
                            resolved = std::move(bits);
                        }
                        catch (const std::exception &) { }
                }
                else if (name.starts_with("core.compute."))
                {
                    const auto kind = name.substr(13);
                    const auto operands = model_.operands(op);
                    const auto operand = [&](std::size_t index) -> std::optional<Bits> {
                        if (index >= operands.size()) return std::nullopt;
                        return self(self, operands[index]);
                    };
                    if (kind == "not" && operands.size() == 1)
                    {
                        if (auto source = operand(0))
                        {
                            auto bits = resizeBits(*source, currentType.width);
                            for (auto &bit : bits) bit ^= 1u;
                            resolved = std::move(bits);
                        }
                    }
                    else if (kind == "assign" && operands.size() == 1)
                    {
                        if (auto source = operand(0)) resolved = resizeBits(*source, currentType.width);
                    }
                    else if (kind == "concat" && operands.size() >= 2)
                    {
                        Bits bits(currentType.width, 0);
                        uint64_t offset = 0;
                        bool ok = true;
                        for (std::size_t index = operands.size(); index-- > 0;)
                        {
                            auto source = operand(index);
                            if (!source) { ok = false; break; }
                            for (uint64_t bit = 0; bit < source->size() && offset + bit < bits.size(); ++bit)
                                bits[offset + bit] = (*source)[bit];
                            offset += source->size();
                        }
                        if (ok) resolved = std::move(bits);
                    }
                    else if ((kind == "and" || kind == "or" || kind == "xor" || kind == "xnor") &&
                             operands.size() == 2)
                    {
                        auto lhs = operand(0); auto rhs = operand(1);
                        if (lhs && rhs)
                        {
                            lhs = resizeBits(*lhs, currentType.width);
                            rhs = resizeBits(*rhs, currentType.width);
                            Bits bits(currentType.width, 0);
                            for (uint32_t bit = 0; bit < currentType.width; ++bit)
                            {
                                bits[bit] = kind == "and" ? ((*lhs)[bit] & (*rhs)[bit]) :
                                            kind == "or" ? ((*lhs)[bit] | (*rhs)[bit]) :
                                            ((*lhs)[bit] ^ (*rhs)[bit]);
                                if (kind == "xnor") bits[bit] ^= 1u;
                            }
                            resolved = std::move(bits);
                        }
                    }
                    else if (kind == "mux" && operands.size() == 3)
                    {
                        auto select = operand(0); auto lhs = operand(1); auto rhs = operand(2);
                        if (select && lhs && rhs)
                            resolved = resizeBits(asUnsigned(*select) ? *lhs : *rhs, currentType.width);
                    }
                }
                active.erase(current.index);
                if (resolved) cache.emplace(current.index, *resolved);
                else failed.insert(current.index);
                return resolved;
            };
            const auto constantBits = resolve(resolve, value);
            if (!constantBits) return std::nullopt;
            const Type element = arrayElementType(valueType);
            const auto scalar = [&](uint64_t row) {
                const uint64_t bitBase = row * uint64_t(*elementWidth);
                uint64_t elementBits = 0;
                for (uint32_t bit = 0; bit < *elementWidth; ++bit)
                    if ((*constantBits)[bitBase + bit]) elementBits |= UINT64_C(1) << bit;
                return literal(std::to_string(elementBits), element);
            };
            // Assignment form avoids the fragile nested-brace rules of
            // std::array-of-std::array initialization for rank >= 3 while
            // remaining a constant expression. The lambda is evaluated once
            // at compile time for a static constexpr local.
            std::string result = "([]() constexpr {" + cppValueType(valueType) + " cpu_array{};";
            for (uint64_t row = 0; row < *rows; ++row)
                result += rowAccessText("cpu_array", valueType.shape, std::to_string(row)) + '=' + scalar(row) + ';';
            result += "return cpu_array;}())";
            return result;
        }
        std::string SixPhaseEmitter::normalize(std::string expression, const Type &type) const
        {
            if (type.kind == TypeKind::String || type.kind == TypeKind::Real) return expression;
            // Wide two-state: mask back to the semantic width (the padding
            // invariant); signedness is re-derived at op points, so signed
            // and unsigned share the same shape here.
            if (type.kind == TypeKind::Logic && type.domain == LogicDomain::TwoState && type.width > 64)
                return wideTrunc(std::move(expression), type.width);
            // E-flat: grhsim_trunc_u64/grhsim_sign_extend_i64 expanded inline.
            return "static_cast<" + cppType(type) + ">(" +
                   (type.isSigned ? sextRaw(expression, type.width)
                                  : truncRaw(std::move(expression), type.width)) +
                   ")";
        }
        std::string SixPhaseEmitter::literal(std::string_view text, const Type &type) const
        {
            auto parsed = [&]() {
                try { return slang::SVInt::fromString(text).resize(type.width); }
                catch (const std::exception &) { throw std::runtime_error("CPU six-phase emit unsupported constant literal: " + std::string(text)); }
            }();
            parsed.flattenUnknowns(); const auto bits = parsed.as<uint64_t>();
            if (type.kind == TypeKind::Logic && type.domain == LogicDomain::TwoState && type.width > 64)
            {
                // _BitInt constants fold from 64-bit words, most-significant
                // first (gsim legalCppCons shape); resize(width) already
                // cleaned the top word.
                const auto words = (type.width + 63u) / 64u;
                const auto *raw = parsed.getRawPtr();
                bool allZero = true;
                for (uint32_t i = 0; i < words; ++i)
                {
                    if (raw[i] != 0)
                    {
                        allZero = false;
                        break;
                    }
                }
                if (allZero)
                {
                    return "(" + wideType(type.width) + ")0";
                }
                if (words > 16)
                {
                    // stage-07: statement form for wide constants — a
                    // left-nested ((x<<64)|w) expression needs one paren pair
                    // per word and overflows the clang parser stack on
                    // multi-hundred-word init values (XS aggregate memories).
                    std::string body = "([&](){ " + wideType(type.width) + " cpu_lit=(" +
                                       wideType(type.width) + ")UINT64_C(" +
                                       std::to_string(raw[words - 1]) + ");\n";
                    for (uint32_t i = words - 1; i-- > 0;)
                        body += "cpu_lit=((" + wideType(type.width) + ")(cpu_lit)<<64)|UINT64_C(" +
                                std::to_string(raw[i]) + ");\n";
                    body += "return cpu_lit&" + wideMask(type.width) + ";}())";
                    return body;
                }
                std::string result = "((" + wideType(type.width) + ")UINT64_C(" + std::to_string(raw[words - 1]) + "))";
                for (uint32_t i = words - 1; i-- > 0;)
                    result = "((" + result + "<<64)|UINT64_C(" + std::to_string(raw[i]) + "))";
                return result;
            }
            if (!bits) throw std::runtime_error("cannot encode CPU scalar literal");
            return normalize("UINT64_C(" + std::to_string(*bits) + ")", type);
        }
        std::string SixPhaseEmitter::initLiteral(std::string_view text, const Type &type) const
        {
            if (text == "x" || text == "X" || text == "z" || text == "Z" ||
                text == "'x" || text == "'X" || text == "'z" || text == "'Z" || text == "'0") text = "0";
            else if (text == "'1") text = "-1";
            return literal(text, type);
        }
        void SixPhaseEmitter::randomInit(std::ostream &out, const Type &type, std::string_view destination,
                                         std::string_view rng) const
        {
            if (type.width <= 64)
                out << destination << '=' << normalize("grhsim_random_u64(" + std::string(rng) + "," + std::to_string(type.width) + ")", type) << ";\n";
            else
            {
                // Fill the _BitInt object through its u64-word image (the
                // header static_assert pins the layout), then mask the top.
                const auto words = (type.width + 63u) / 64u;
                out << "{std::uint64_t cpu_rw[" << words << "];for(std::size_t w=0;w<" << words
                    << ";++w) cpu_rw[w]=grhsim_splitmix64_next(" << rng << ");\n"
                    << "std::memcpy(&" << destination << ",cpu_rw,sizeof(cpu_rw));\n"
                    << destination << "&=" << wideMask(type.width) << ";}\n";
            }
        }

        std::string SixPhaseEmitter::read(ValueId value) const
        {
            if (const auto it = staticScalars_.find(value.index); it != staticScalars_.end()) return it->second;
            if (type(value).kind == TypeKind::String)
                if (const auto it = staticStrings_.find(value.index); it != staticStrings_.end()) return it->second;
            // Frame-spilled (M5d-7) values resolve through the chunk driver's
            // frame in every chunk; plain locals only live in one function.
            if (crossingLocals_[value.index] || activeLocals_[value.index])
            {
                const auto &valueType = type(value);
                if (valueType.shape.empty()) return localRef(value);
                const auto rows = shapeElementCount(valueType.shape);
                const auto elementWidth = typeElementWidth(valueType);
                if (!rows || !elementWidth)
                    throw std::runtime_error("CPU six-phase emit shaped local value has an invalid shape");
                return packStoreRows(localRef(value), valueType.shape, "", *rows,
                                     *elementWidth, valueType.width);
            }
            const auto *field = boundaryByUse_[value.index][static_cast<std::size_t>(readContext_)];
            if (!field && readContext_ != CpuBoundaryUse::NonSink)
                throw std::runtime_error("CPU six-phase sink operand lacks its enable/payload boundary field: v" +
                                         std::to_string(value.index));
            if (!field) field = boundaryByValue_[value.index];
            if (field)
            {
                // A shaped (array) value's boundary field is the flat per-row
                // array; compute consumers expect the packed total-width
                // vector, so re-pack on read.
                const auto &valueType = type(value);
                if (!valueType.shape.empty())
                {
                    const auto rows = shapeElementCount(valueType.shape);
                    const auto elementWidth = typeElementWidth(valueType);
                    if (!rows || !elementWidth)
                        throw std::runtime_error("CPU six-phase emit shaped value has an invalid shape");
                    return packStoreRows(boundaryRef(field), valueType.shape, "", *rows, *elementWidth, valueType.width);
                }
                return boundaryRef(field);
            }
            const auto producer = producers_[value.index];
            if (!producer)
                throw std::runtime_error("CPU six-phase emit value has no producer: v" + std::to_string(value.index));
            const auto &op = model_.operations()[producer.index - 1];
            const auto name = model_.text(op.opType);
            if (name == "core.input.read" || name == "core.state.read" || name == "core.state.readArray" ||
                name == "core.compute.constant")
                return expression(op);
            throw std::runtime_error("CPU six-phase emit value is not readable: v" + std::to_string(value.index) +
                                     " produced by " + std::string(name) + " (op " +
                                     std::to_string(producer.index) + ")");
        }
        std::string SixPhaseEmitter::readAs(ValueId value, CpuBoundaryUse use) const
        {
            const auto previous = readContext_;
            readContext_ = use;
            const auto result = read(value);
            readContext_ = previous;
            return result;
        }
        std::optional<std::string> SixPhaseEmitter::rowBackedArrayText(ValueId value) const
        {
            // Flat local/constant forms win (read() resolves them first); only
            // values that would pack from per-row storage qualify.
            if (value.index >= producers_.size()) return std::nullopt;
            if (staticScalars_.contains(value.index) || staticStrings_.contains(value.index))
                return std::nullopt;
            if (crossingLocals_[value.index] || activeLocals_[value.index])
                return type(value).shape.empty() ? std::nullopt
                                                 : std::optional<std::string>(localRef(value));
            if (value.index < boundaryByValue_.size() && boundaryByValue_[value.index])
            {
                // The layout maps a shaped value to a per-row array field;
                // double-check the physical type (a flat-scalar field means
                // the value's storage is not row-indexable).
                const CpuStoreField *field = boundaryByUse_[value.index][
                    static_cast<std::size_t>(readContext_)];
                if (!field) field = boundaryByValue_[value.index];
                if (layout_.types[field->type.index - 1].kind != CpuTypeKind::Array)
                    return std::nullopt;
                return boundaryRef(field);
            }
            const auto producer = producers_[value.index];
            if (!producer) return std::nullopt;
            const auto &op = model_.operations()[producer.index - 1];
            const auto name = model_.text(op.opType);
            if (name == "core.state.read")
            {
                // Only a SHAPED state stores rows; a flat state read that
                // yields a shaped value view stores a flat scalar.
                const auto refs = model_.objectRefs(op);
                if (refs.size() != 1 || refs[0].kind != ObjectKind::State) return std::nullopt;
                const auto &target = stateType(StateId{refs[0].index, 0});
                if (target.shape.empty()) return std::nullopt;
                if (refs[0].index < memFieldByState_.size() && memFieldByState_[refs[0].index])
                    return "largeSinkStore." + std::string(model_.text(memFieldByState_[refs[0].index]->name));
                if (refs[0].index < regFieldByState_.size() && regFieldByState_[refs[0].index])
                    return "smallSinkStore." + std::string(model_.text(regFieldByState_[refs[0].index]->name));
                return std::nullopt;
            }
            if (name == "core.input.read")
            {
                const auto refs = model_.objectRefs(op);
                if (refs.size() != 1 || refs[0].kind != ObjectKind::Input ||
                    refs[0].index >= boundaryByInput_.size() || !boundaryByInput_[refs[0].index])
                    return std::nullopt;
                const auto &inputType =
                    model_.types()[model_.inputs()[refs[0].index - 1].type.index - 1];
                if (inputType.shape.empty()) return std::nullopt;
                return boundaryRef(boundaryByInput_[refs[0].index]);
            }
            return std::nullopt;
        }
        std::optional<std::string> SixPhaseEmitter::directShapedExpression(const SimOp &op) const
        {
            const auto results = model_.results(op);
            if (results.size() != 1) return std::nullopt;
            const auto result = results[0];
            const auto &resultType = type(result);
            if (resultType.kind != TypeKind::Logic || resultType.domain != LogicDomain::TwoState ||
                resultType.shape.empty())
                return std::nullopt;
            const auto rows = shapeElementCount(resultType.shape);
            const auto resultWidth = typeElementWidth(resultType);
            if (!rows || !resultWidth || !*rows || !*resultWidth) return std::nullopt;
            const auto operands = model_.operands(op);
            const auto name = model_.text(op.opType);

            // A partial array read already has row-backed storage at its
            // source.  The generic expression path packs that row block into
            // a flat value and unpackValue() immediately slices it back into
            // an std::array.  Keep the value in its physical row form and
            // copy the addressed rows directly into the result aggregate.
            // This is particularly common in aggregate XiangShan models,
            // where a single dynamic index reads a small sub-array from a
            // large table.
            if (name == "core.state.read" || name == "core.state.readArray")
            {
                const auto refs = model_.objectRefs(op);
                if (refs.size() != 1 || refs[0].kind != ObjectKind::State)
                    return std::nullopt;
                const StateId target{refs[0].index, 0};
                const auto &array = stateType(target);
                if (array.kind != TypeKind::Logic || array.shape.empty())
                    return std::nullopt;
                // A whole-array state.read has no index operands. The result
                // must retain the state's row shape for this direct bridge;
                // a flat view still uses the packed expression path below.
                if (name == "core.state.read" &&
                    (resultType.shape != array.shape ||
                     typeElementWidth(resultType) != typeElementWidth(array)))
                    return std::nullopt;
                if (name == "core.state.readArray" &&
                    (operands.empty() || operands.size() >= array.shape.size()))
                    return std::nullopt;
                const auto sourceWidth = typeElementWidth(array);
                const auto blockRows = name == "core.state.read"
                    ? shapeElementCount(array.shape)
                    : shapeElementCount(std::span<const int64_t>(array.shape).subspan(operands.size()));
                if (!sourceWidth || !blockRows || *sourceWidth != *resultWidth || *blockRows != *rows)
                    return std::nullopt;
                const bool inMem = refs[0].index < memFieldByState_.size() && memFieldByState_[refs[0].index];
                const CpuStoreField *field = inMem ? memFieldByState_[refs[0].index]
                     : refs[0].index < regFieldByState_.size() ? regFieldByState_[refs[0].index] : nullptr;
                if (!field) return std::nullopt;
                const std::string store = (inMem ? "largeSinkStore." : "smallSinkStore.") +
                                          std::string(model_.text(field->name));
                std::string baseRow;
                if (name == "core.state.readArray")
                {
                    std::vector<std::string> indices;
                    indices.reserve(operands.size());
                    for (const auto index : operands)
                        indices.push_back("static_cast<std::size_t>(" + read(index) + ")");
                    baseRow = linearIndexText(array.shape, array.shape.size(), indices);
                }
                const Type element = arrayElementType(resultType);
                // A whole-array read and a contiguous sub-array read have the
                // same row element representation on both sides.  Copy the
                // row image in one operation instead of emitting one nested
                // indexing expression per element.  The physical store keeps
                // row-major std::array storage, so the address of the first
                // addressed row covers the complete result aggregate.
                const std::string sourceRow = baseRow.empty() ? "0" : baseRow;
                if (name == "core.state.read" ||
                    (name == "core.state.readArray" && *rows > 1))
                    return "([&]{" + cppValueType(resultType) + " cpu_array{};std::memcpy(&cpu_array,&" +
                           rowAccessText(store, array.shape, sourceRow) + ",sizeof(cpu_array));return cpu_array;}())";
                std::string body = "([&]{" + cppValueType(resultType) + " cpu_array{};";
                for (uint64_t row = 0; row < *rows; ++row)
                {
                    const std::string rowText = baseRow + '+' + std::to_string(row);
                    body += rowAccessText("cpu_array", resultType.shape, std::to_string(row)) + '=' +
                            normalize(rowAccessText(store, array.shape, rowText), element) + ';';
                }
                body += "return cpu_array;}())";
                return body;
            }

            if (!name.starts_with("core.compute.")) return std::nullopt;
            const auto kind = name.substr(13);

            // A shaped sliceArray with fewer indices than the base rank is
            // another row-block read.  rowBackedArrayText() covers state and
            // boundary arrays as well as shaped locals produced earlier in
            // the same supernode, so the copy remains entirely in row form.
            if (kind == "sliceArray" && !operands.empty())
            {
                const auto &baseType = type(operands[0]);
                const std::size_t indexCount = operands.size() - 1;
                if (!baseType.shape.empty() && indexCount < baseType.shape.size())
                    if (const auto source = rowBackedArrayText(operands[0]))
                    {
                        const auto sourceWidth = typeElementWidth(baseType);
                        const auto blockRows = shapeElementCount(
                            std::span<const int64_t>(baseType.shape).subspan(indexCount));
                        if (sourceWidth && blockRows && *sourceWidth == *resultWidth && *blockRows == *rows)
                        {
                            std::vector<std::string> indices;
                            indices.reserve(indexCount);
                            for (std::size_t j = 0; j < indexCount; ++j)
                                indices.push_back("static_cast<std::size_t>(" + read(operands[1 + j]) + ")");
                            const std::string baseRow =
                                linearIndexText(baseType.shape, baseType.shape.size(), indices);
                            const Type element = arrayElementType(resultType);
                            if (*rows > 1)
                            {
                                // The addressed suffix is contiguous in the
                                // row-backed source. Preserve the same
                                // row-major image with one copy instead of
                                // rebuilding every element of the aggregate.
                                return "([&]{" + cppValueType(resultType) + " cpu_array{};std::memcpy(&cpu_array,&" +
                                       rowAccessText(*source, baseType.shape, baseRow) +
                                       ",sizeof(cpu_array));return cpu_array;}())";
                            }
                            std::string body = "([&]{" + cppValueType(resultType) + " cpu_array{};";
                            for (uint64_t row = 0; row < *rows; ++row)
                            {
                                const std::string sourceRow = baseRow + '+' + std::to_string(row);
                                body += rowAccessText("cpu_array", resultType.shape, std::to_string(row)) + '=' +
                                        normalize(rowAccessText(*source, baseType.shape, sourceRow), element) + ';';
                            }
                            body += "return cpu_array;}())";
                            return body;
                        }
                    }
            }

            // A statically aligned slice of a row-backed aggregate can use
            // the same direct row copy.  Unaligned or differently shaped
            // slices retain the packed path because their element boundaries
            // no longer coincide with physical rows.
            if (kind == "sliceStatic" && !operands.empty())
            {
                const auto &baseType = type(operands[0]);
                if (!baseType.shape.empty())
                    if (const auto source = rowBackedArrayText(operands[0]))
                    {
                        const auto sourceWidth = typeElementWidth(baseType);
                        const auto start = parameter<int64_t>(model_, model_.parameters(op), "sliceStart");
                        const auto sourceRows = shapeElementCount(baseType.shape);
                        if (sourceWidth && start && *start >= 0 && *start % *sourceWidth == 0 &&
                            typeElementWidth(baseType) == resultWidth &&
                            *start / *sourceWidth + *rows <= sourceRows.value_or(0))
                        {
                            const Type element = arrayElementType(resultType);
                            const uint64_t firstRow = static_cast<uint64_t>(*start) / *sourceWidth;
                            if (firstRow == 0 && *rows == sourceRows && *rows > 1)
                                return "([&]{" + cppValueType(resultType) + " cpu_array{};std::memcpy(&cpu_array,&" +
                                       *source + ",sizeof(cpu_array));return cpu_array;}())";
                            std::string body = "([&]{" + cppValueType(resultType) + " cpu_array{};";
                            for (uint64_t row = 0; row < *rows; ++row)
                            {
                                const std::string sourceRow = std::to_string(firstRow + row);
                                body += rowAccessText("cpu_array", resultType.shape, std::to_string(row)) + '=' +
                                        normalize(rowAccessText(*source, baseType.shape, sourceRow), element) + ';';
                            }
                            body += "return cpu_array;}())";
                            return body;
                        }
                    }
            }

            const auto sameShape = [&](ValueId value) {
                const auto &sourceType = type(value);
                if (sourceType.kind != TypeKind::Logic || sourceType.domain != LogicDomain::TwoState ||
                    sourceType.shape != resultType.shape || typeElementWidth(sourceType) != resultWidth)
                    return std::optional<std::string>{};
                return rowBackedArrayText(value);
            };
            if (kind == "replicate" && operands.size() == 1)
            {
                // Replication of one scalar element into an aggregate can be
                // materialized directly in the row representation. The
                // generic path first builds a wide _BitInt and then slices it
                // back into rows, which is especially costly for 64-bit
                // elements repeated hundreds of times.
                const auto &sourceType = type(operands[0]);
                const auto rep = parameter<int64_t>(model_, model_.parameters(op), "rep");
                if (sourceType.kind == TypeKind::Logic && sourceType.domain == LogicDomain::TwoState &&
                    sourceType.shape.empty() && resultType.shape.size() == 1 && sourceType.width == resultWidth &&
                    rep && *rep >= 0 &&
                    static_cast<uint64_t>(*rep) >= *rows)
                {
                    const Type element = arrayElementType(resultType);
                    const auto value = normalize(read(operands[0]), element);
                    std::string body = "([&]{" + cppValueType(resultType) + " cpu_array{};";
                    body += "cpu_array.fill(" + value + ");return cpu_array;}())";
                    return body;
                }
            }
            if (kind == "concat" && operands.size() >= 2)
            {
                struct Source
                {
                    std::string text;
                    const Type *type = nullptr;
                    uint64_t rows = 0;
                };
                std::vector<Source> sources;
                sources.reserve(operands.size());
                uint64_t totalRows = 0;
                for (const auto operand : operands)
                {
                    const auto &sourceType = type(operand);
                    const auto elementWidth = typeElementWidth(sourceType);
                    const auto sourceRows = shapeElementCount(sourceType.shape);
                    const auto sourceText = sourceType.shape.empty() ? std::nullopt : rowBackedArrayText(operand);
                    if (sourceType.kind != TypeKind::Logic || sourceType.domain != LogicDomain::TwoState ||
                        !elementWidth || !sourceRows || *elementWidth != *resultWidth || !sourceText)
                        return std::nullopt;
                    sources.push_back({*sourceText, &sourceType, *sourceRows});
                    totalRows += *sourceRows;
                }
                if (totalRows != *rows) return std::nullopt;
                const Type element = arrayElementType(resultType);
                if (resultType.shape.size() == 1)
                {
                    // A one-dimensional shaped value is contiguous in its
                    // row representation. Copy complete operand ranges in
                    // bulk instead of constructing one aggregate element at
                    // a time in a large generated lambda.
                    std::string body = "([&]{" + cppValueType(resultType) + " cpu_array{};";
                    uint64_t destinationRow = 0;
                    for (std::size_t index = sources.size(); index-- > 0;)
                    {
                        const auto &source = sources[index];
                        body += "std::copy_n(" + source.text + ".data()," + std::to_string(source.rows) +
                                ",cpu_array.data()+" + std::to_string(destinationRow) + ");";
                        destinationRow += source.rows;
                    }
                    body += "return cpu_array;}())";
                    return body;
                }
                std::string body = "([&]{" + cppValueType(resultType) + " cpu_array{};";
                uint64_t destinationRow = 0;
                // The last concat operand occupies the low rows, just as it
                // occupies the low bits of the packed representation.
                for (std::size_t index = sources.size(); index-- > 0;)
                {
                    const auto &source = sources[index];
                    for (uint64_t row = 0; row < source.rows; ++row, ++destinationRow)
                        body += rowAccessText("cpu_array", resultType.shape,
                                              std::to_string(destinationRow)) + '=' +
                                normalize(rowAccessText(source.text, source.type->shape,
                                                         std::to_string(row)), element) + ';';
                }
                body += "return cpu_array;}())";
                return body;
            }
            if (kind == "assign" && operands.size() == 1)
            {
                // Aggregate assignment is element-preserving when the shape
                // and element width match. C++ aggregate assignment also
                // handles the signed/unsigned spelling conversion safely.
                if (const auto source = sameShape(operands[0])) return *source;
                return std::nullopt;
            }
            if ((kind == "not" || kind == "and" || kind == "or" || kind == "xor" || kind == "xnor") &&
                (operands.size() == 1 || operands.size() == 2))
            {
                std::vector<std::string> source;
                source.reserve(operands.size());
                for (const auto operand : operands)
                {
                    const auto value = sameShape(operand);
                    if (!value) return std::nullopt;
                    source.push_back(*value);
                }
                const Type element = arrayElementType(resultType);
                std::string body = "([&]{" + cppValueType(resultType) + " cpu_array{};";
                for (uint64_t row = 0; row < *rows; ++row)
                {
                    const auto cell = [&](std::size_t index) {
                        return rowAccessText(source[index], resultType.shape, std::to_string(row));
                    };
                    std::string value;
                    if (kind == "not") value = "~(" + cell(0) + ")";
                    else value = "(" + cell(0) + (kind == "and" ? "&" : kind == "or" ? "|" : "^") +
                                      cell(1) + ")";
                    if (kind == "xnor") value = "~(" + value + ")";
                    body += rowAccessText("cpu_array", resultType.shape, std::to_string(row)) + '=' +
                            normalize(value, element) + ';';
                }
                body += "return cpu_array;}())";
                return body;
            }
            if (kind == "mux" && operands.size() == 3)
            {
                const auto lhs = sameShape(operands[1]);
                const auto rhs = sameShape(operands[2]);
                if (!lhs || !rhs) return std::nullopt;
                const Type element = arrayElementType(resultType);
                const auto select = read(operands[0]);
                std::string body = "([&]{" + cppValueType(resultType) + " cpu_array{};";
                for (uint64_t row = 0; row < *rows; ++row)
                {
                    const auto lhsCell = rowAccessText(*lhs, resultType.shape, std::to_string(row));
                    const auto rhsCell = rowAccessText(*rhs, resultType.shape, std::to_string(row));
                    body += rowAccessText("cpu_array", resultType.shape, std::to_string(row)) +
                            '=' + normalize("(" + select + ")?" + lhsCell + ":" + rhsCell, element) + ';';
                }
                body += "return cpu_array;}())";
                return body;
            }
            return std::nullopt;
        }
        void SixPhaseEmitter::declareLocal(std::ostream &out, ValueId result, const std::string &expr) const
        {
            const auto &resultType = type(result);
            if (!resultType.shape.empty())
                if (const auto initializer = constantArrayInitializer(result))
                {
                    const auto valueType = cppValueType(resultType);
                    if (crossingLocals_[result.index])
                        out << localRef(result) << '=' << *initializer << ";\n";
                    else
                    {
                        out << "static constexpr " << valueType << " cpu_v" << result.index << '='
                            << *initializer << ";\n";
                        activeLocals_[result.index] = 1;
                    }
                    return;
                }
            const std::string value = unpackValue(expr, resultType);
            // Frame-spilled values assign their pre-declared frame field; the
            // driver value-initializes the frame before the first chunk runs.
            if (crossingLocals_[result.index])
            {
                out << localRef(result) << '=' << value << ";\n";
                return;
            }
            out << "const " << cppValueType(resultType) << " cpu_v" << result.index << '=' << value << ";\n";
            activeLocals_[result.index] = 1;
        }

        template <typename Raw, typename TypeOf, typename Number>
        std::string SixPhaseEmitter::scalarExpression(std::string_view kind, const Type &result, std::size_t arity,
                                                      const Raw &raw, const TypeOf &typeOf, const Number &number) const
        {
            const auto width = result.width;
            const auto cast = [&](std::size_t i, uint32_t width) {
                const auto expr = raw(i); const auto &source = typeOf(i);
                // E-flat: grhsim_cast_u64 expanded inline — trunc to srcWidth,
                // sign-extend when signed, trunc to destWidth; narrowing
                // (srcWidth >= destWidth) makes the sign-extend unobservable.
                if (!source.isSigned) return truncRaw(expr, std::min<uint64_t>(source.width, width));
                if (source.width >= width) return truncRaw(expr, width);
                return truncRaw("static_cast<std::uint64_t>(" + sextRaw(expr, source.width) + ")", width);
            };
            if (kind == "assign") return cast(0, width);
            if ((kind == "and" || kind == "or") && arity == 2 &&
                width == 1 && !result.isSigned &&
                typeOf(0).width == 1 && !typeOf(0).isSigned &&
                typeOf(1).width == 1 && !typeOf(1).isSigned)
                return "(" + raw(0) + (kind == "and" ? "&" : "|") + raw(1) + ")";
            static const std::map<std::string_view, std::string_view> binary{
                {"add", "+"}, {"sub", "-"}, {"mul", "*"}, {"and", "&"}, {"or", "|"}, {"xor", "^"},
                {"logicAnd", "&&"}, {"logicOr", "||"}};
            if (auto it = binary.find(kind); it != binary.end())
                return "(" + (kind.starts_with("logic") ? raw(0) : cast(0, width)) + std::string(it->second) +
                       (kind.starts_with("logic") ? raw(1) : cast(1, width)) + ")";
            if (kind == "not" || kind == "logicNot") return "(" + std::string(kind == "not" ? "~" : "!") + raw(0) + ")";
            if (kind == "xnor") return "~(" + cast(0, width) + "^" + cast(1, width) + ")";
            if (kind == "mux")
            {
                if (result.domain == LogicDomain::TwoState && width <= 64 && arity == 3)
                    return "((" + cast(1, width) + ")&static_cast<std::uint64_t>(-static_cast<std::int64_t>(" + raw(0) +
                           "!=0)))|((" + cast(2, width) + ")&~static_cast<std::uint64_t>(-static_cast<std::int64_t>(" +
                           raw(0) + "!=0)))";
                return "(" + raw(0) + "?" + cast(1, width) + ":" + cast(2, width) + ")";
            }
            if (kind == "prioritySelect")
            {
                const std::size_t count = (arity - 1) / 2;
                if (count < 3 || count > 64)
                    throw std::runtime_error("CPU six-phase emit prioritySelect condition count is out of range");
                std::string expr = cast(2 * count, width);
                for (std::size_t i = count; i-- > 0;)
                    expr = "((" + cast(count + i, width) + ")&static_cast<std::uint64_t>(-static_cast<std::int64_t>(" +
                           raw(i) + "!=0)))|((" + expr + ")&~static_cast<std::uint64_t>(-static_cast<std::int64_t>(" +
                           raw(i) + "!=0)))";
                return expr;
            }
            if (kind == "bitSelect") {
                if (width == 1 && !result.isSigned)
                    return "((" + raw(0) + "&" + raw(1) + ")|((" + raw(0) + "^1)&" + raw(2) + "))";
                return "((" + cast(0, width) + "&" + cast(1, width) + ")|(~" + cast(0, width) +
                       "&" + cast(2, width) + "))";
            }
            if (kind == "shl" || kind == "lshr" || kind == "ashr")
            {
                // E-flat: grhsim_index_words + grhsim_{shl,lshr,ashr}_u64 expanded.
                const auto amount = indexRaw(raw(1), typeOf(1).width, width);
                const auto value = cast(0, width);
                if (kind == "shl")
                    return "(" + amount + ">=64?UINT64_C(0):" + truncRaw("(" + value + ")<<" + amount, width) + ")";
                if (kind == "lshr")
                    return "(" + amount + ">=64?UINT64_C(0):(" + value + ")>>" + amount + ")";
                return "(" + truncRaw("static_cast<std::uint64_t>(" + sextRaw(value, width) + ">>(" + amount +
                                      ">=64?63:" + amount + "))",
                                      width) +
                       ")";
            }
            if (kind == "div" || kind == "mod")
                return "grhsim_" + std::string(typeOf(0).isSigned && typeOf(1).isSigned ? "s" : "u") +
                       std::string(kind) + "_u64(" + cast(0, width) + "," + cast(1, width) + "," + std::to_string(width) + ")";
            static const std::map<std::string_view, std::string_view> compares{
                {"eq", "=="}, {"ne", "!="}, {"caseEq", "=="}, {"caseNe", "!="},
                {"wildcardEq", "=="}, {"wildcardNe", "!="}, {"lt", "<"}, {"le", "<="}, {"gt", ">"}, {"ge", ">="}};
            if (auto it = compares.find(kind); it != compares.end())
            {
                const auto compareWidth = std::max(typeOf(0).width, typeOf(1).width);
                if (compareWidth > 64)
                {
                    // Wide values are unsigned _BitInt objects holding clean
                    // width-bit values. Unsigned compares just widen both
                    // sides to the compare container; signed compares
                    // sign-extend each side from its own width first
                    // (grhsim_compare_extended_words semantics).
                    const bool bothSigned = typeOf(0).isSigned && typeOf(1).isSigned;
                    const auto extend = [&](std::size_t i) -> std::string {
                        const auto &source = typeOf(i);
                        if (source.width > 64)
                            return bothSigned
                                       ? "((" + wideSignedType(compareWidth) + ")(" + wideSext(raw(i), source.width) + "))"
                                       : "((" + wideType(compareWidth) + ")(" + raw(i) + "))";
                        if (bothSigned)
                            return "((" + wideSignedType(compareWidth) + ")(" + sextRaw(raw(i), source.width) + "))";
                        return "((" + wideType(compareWidth) + ")(" + truncRaw(raw(i), source.width) + "))";
                    };
                    return "(" + extend(0) + std::string(it->second) + extend(1) + ")";
                }
                // E-flat: grhsim_compare_{un}signed_u64 expanded. cast()
                // truncs/sign-extends to compareWidth; the UNSIGNED compare
                // uses the truncated patterns directly. The SIGNED compare
                // must still sign-extend the compareWidth pattern (cast()
                // alone leaves srcWidth == compareWidth values as unsigned
                // patterns — dropping the sign here was the E-flat bug that
                // CASE_003 caught: sext(-1,3) compared as (7 < 0)).
                const std::string lhs = cast(0, compareWidth), rhs = cast(1, compareWidth);
                if (typeOf(0).isSigned && typeOf(1).isSigned)
                    return "(" + sextRaw(lhs, compareWidth) + std::string(it->second) +
                           sextRaw(rhs, compareWidth) + ")";
                return "(" + lhs + std::string(it->second) + rhs + ")";
            }
            static const std::map<std::string_view, std::string_view> reduces{
                {"reduceAnd", "and"}, {"reduceNand", "nand"}, {"reduceOr", "or"}, {"reduceNor", "nor"},
                {"reduceXor", "xor"}, {"reduceXnor", "xnor"}};
            if (auto it = reduces.find(kind); it != reduces.end())
            {
                const auto operandWidth = typeOf(0).width;
                if (operandWidth > 64)
                {
                    // Native reduce on the clean _BitInt value (padding is
                    // zero, so == mask / != 0 / parity all see exactly the
                    // semantic width — grhsim_reduce_*_words semantics).
                    const auto value = raw(0);
                    if (it->second == "and") return "(" + value + "==" + wideMask(operandWidth) + ")";
                    if (it->second == "nand") return "(" + value + "!=" + wideMask(operandWidth) + ")";
                    if (it->second == "or") return "((" + value + ")!=0)";
                    if (it->second == "nor") return "((" + value + ")==0)";
                    const auto words = (operandWidth + 63u) / 64u;
                    std::string parity = "__builtin_parityll((std::uint64_t)(" + value + "))";
                    for (uint64_t i = 1; i < words; ++i)
                        parity = "(" + parity + "^__builtin_parityll((std::uint64_t)((" + value + ")>>" +
                                 std::to_string(i * 64) + ")))";
                    return it->second == "xor" ? "(" + parity + "!=0)" : "(" + parity + "==0)";
                }
                // E-flat: grhsim_reduce_*_u64 bodies expanded.
                if (operandWidth == 0) return it->second == "nand" || it->second == "nor" || it->second == "xnor" ? "true" : "false";
                const auto masked = truncRaw(raw(0), operandWidth);
                if (it->second == "and") return "(" + masked + "==" + maskLiteral(operandWidth) + ")";
                if (it->second == "nand") return "!(" + masked + "==" + maskLiteral(operandWidth) + ")";
                if (it->second == "or") return "(" + masked + "!=UINT64_C(0))";
                if (it->second == "nor") return "(" + masked + "==UINT64_C(0))";
                if (it->second == "xor") return "((__builtin_popcountll(" + masked + ")&1)!=0)";
                return "((__builtin_popcountll(" + masked + ")&1)==0)";
            }
            if (kind == "sliceStatic" || kind == "sliceDynamic" || kind == "sliceArray")
            {
                // sliceArray (stage-06): [base, i0..ik-1]; the bit offset is
                // blockIndex * resultWidth where blockIndex is the row-major
                // linear index of the k indices in result-width block units
                // (strides prod(shape[j+1:k]) over the base value's shape).
                std::string start = kind == "sliceStatic" ? std::to_string(number("sliceStart"))
                                                          : cast(1, typeOf(1).width);
                if (kind == "sliceArray")
                {
                    const auto &baseShape = typeOf(0).shape;
                    const std::size_t indexCount = arity - 1;
                    if (baseShape.empty() || indexCount < 1 || indexCount > baseShape.size())
                        throw std::runtime_error("CPU sliceArray base value is not a shaped logic value");
                    std::vector<std::string> indices;
                    indices.reserve(indexCount);
                    for (std::size_t j = 0; j < indexCount; ++j)
                        indices.push_back("static_cast<std::uint64_t>(" + raw(1 + j) + ")");
                    start = linearIndexText(baseShape, indexCount, indices);
                }
                if (typeOf(0).width > 64)
                {
                    // grhsim_slice_words_u64 on the _BitInt object: start is
                    // bounded by the container width (padding reads as zero);
                    // the ternary keeps the shift itself defined.
                    const auto container = wideContainer(typeOf(0).width);
                    if (kind == "sliceArray") start = "(" + start + ")*" + std::to_string(width);
                    // stage-07: decompose the shift into a byte-aligned move
                    // plus a <64 remainder. A non-aligned shift of a wide
                    // container defeats LLVM's load-width narrowing, so every
                    // slice read materialized the whole container — the
                    // measured DAGCombiner::visitSTORE/KnownBits quadratic on
                    // wide boundary stores (tu271/tu272 spun for an hour).
                    const std::string window =
                        "((unsigned __int128)((" + raw(0) + ")>>((" + start + ")&~UINT64_C(63))))";
                    return "((" + start + ")>=" + std::to_string(container) + "?UINT64_C(0):" +
                           truncRaw("(std::uint64_t)((" + window + ")>>((" + start + ")&UINT64_C(63)))", width) + ")";
                }
                if (kind == "sliceArray")
                    start = "((" + start + ")>=64/" + std::to_string(width) + "+1?64:(" + start + ")*" + std::to_string(width) + ")";
                // E-flat: grhsim_slice_dynamic_u64(trunc(v, srcW), start, w) expanded.
                return "(" + start + ">=64?UINT64_C(0):" +
                       truncRaw("(" + truncRaw(raw(0), typeOf(0).width) + ")>>" + start, width) + ")";
            }
            if (kind == "concat" || kind == "replicate")
            {
                const auto &sourceType = typeOf(0);
                if (kind == "replicate" && sourceType.kind == TypeKind::Logic && sourceType.width == 1 &&
                    !sourceType.isSigned && sourceType.domain == LogicDomain::TwoState)
                    return "(0-static_cast<std::uint64_t>(" + raw(0) + "))";
                std::string expr = "UINT64_C(0)"; uint64_t total = 0;
                const auto count = kind == "replicate" ? number("rep") : arity;
                if (!count || count > 64) throw std::runtime_error("invalid CPU scalar concatenation/replication count");
                for (uint64_t i = 0; i < count; ++i)
                {
                    const auto index = kind == "replicate" ? 0 : i;
                    const auto operandWidth = typeOf(index).width;
                    // E-flat: grhsim_concat_u64 expanded; the accumulator is
                    // width-clean by construction (masked at every step).
                    if (operandWidth >= 64) { expr = raw(index); total += operandWidth; continue; }
                    expr = truncRaw("((" + expr + ")<<" + std::to_string(operandWidth) + "|" +
                                        truncRaw(raw(index), operandWidth) + ")",
                                    total + operandWidth);
                    total += operandWidth;
                }
                return expr;
            }
            throw std::runtime_error("CPU six-phase emit unsupported computation: " + std::string(kind));
        }

        std::string SixPhaseEmitter::fusedExpression(const SimOp &op) const
        {
            const auto *tree = parameter<std::vector<std::string>>(model_, model_.parameters(op), "tree");
            if (!tree || tree->empty())
                throw std::runtime_error("CPU expr op requires a nonempty tree parameter");
            const auto operands = model_.operands(op);
            struct Entry
            {
                std::string text;
                Type type;
            };
            std::vector<Entry> stack;
            stack.reserve(tree->size());
            for (std::size_t cursor = 0; cursor < tree->size(); ++cursor)
            {
                const std::string &token = (*tree)[cursor];
                if (token.size() > 1 && token[0] == 'l')
                {
                    const auto leaf = static_cast<std::size_t>(std::stoul(token.substr(1)));
                    if (leaf >= operands.size())
                        throw std::runtime_error("CPU expr tree leaf operand index is out of range");
                    stack.push_back({read(operands[leaf]), type(operands[leaf])});
                    continue;
                }
                const auto fields = splitExprToken(token);
                if (fields.size() < 6 || fields[0] != "n")
                    throw std::runtime_error("CPU expr tree node token is malformed");
                Type nodeType{};
                nodeType.kind = TypeKind::Logic;
                nodeType.domain = LogicDomain::TwoState;
                nodeType.width = static_cast<uint32_t>(std::stoul(std::string(fields[2])));
                nodeType.isSigned = fields[3] == "1";
                const auto arity = static_cast<std::size_t>(std::stoul(std::string(fields[4])));
                if (arity > stack.size())
                    throw std::runtime_error("CPU expr tree node arity underflows the value stack");
                const std::size_t base = stack.size() - arity;
                const auto number = [&](std::string_view key) {
                    for (std::size_t i = 6; i < fields.size(); ++i)
                    {
                        const auto eq = fields[i].find('=');
                        if (eq != std::string_view::npos && fields[i].substr(0, eq) == key)
                            return std::stoull(std::string(fields[i].substr(eq + 1)));
                    }
                    throw std::runtime_error("missing or negative CPU slice/replication parameter");
                };
                std::string expr = scalarExpression(fields[1], nodeType, arity,
                    [&](std::size_t i) { return stack[base + i].text; },
                    [&](std::size_t i) -> const Type & { return stack[base + i].type; }, number);
                if (cursor + 1 != tree->size()) expr = normalize(std::move(expr), nodeType);
                stack.resize(base);
                stack.push_back({std::move(expr), nodeType});
            }
            if (stack.size() != 1)
                throw std::runtime_error("CPU expr tree did not reduce to a single value");
            return stack.front().text;
        }

        std::vector<std::string_view> SixPhaseEmitter::splitExprToken(const std::string &token)
        {
            std::vector<std::string_view> fields;
            std::size_t begin = 0;
            for (std::size_t i = 0; i <= token.size(); ++i)
                if (i == token.size() || token[i] == ';')
                {
                    fields.push_back(std::string_view(token).substr(begin, i - begin));
                    begin = i + 1;
                }
            return fields;
        }

        std::string SixPhaseEmitter::expression(const SimOp &op) const
        {
            const auto name = model_.text(op.opType); const auto operands = model_.operands(op);
            const auto &result = type(model_.results(op)[0]); const auto width = result.width;
            const auto raw = [&](std::size_t i) { if (i >= operands.size()) throw std::runtime_error("CPU op operand arity"); return read(operands[i]); };
            const auto number = [&](std::string_view key) {
                const auto *n = parameter<int64_t>(model_, model_.parameters(op), key);
                if (!n || *n < 0) throw std::runtime_error("missing or negative CPU slice/replication parameter");
                return static_cast<uint64_t>(*n);
            };
            if (name == "core.input.read")
            {
                const auto refs = model_.objectRefs(op);
                if (refs.size() != 1 || refs[0].kind != ObjectKind::Input ||
                    refs[0].index >= boundaryByInput_.size() || !boundaryByInput_[refs[0].index])
                    throw std::runtime_error("CPU six-phase emit input read has no boundary field");
                const std::string slot = boundaryRef(boundaryByInput_[refs[0].index]);
                // A shaped (array) port's boundary field is the flat per-row
                // array; the value is the packed total-width vector. The
                // INPUT's type decides the storage shape (the result value
                // may carry a flat view of the same total width).
                const auto &inputType =
                    model_.types()[model_.inputs()[refs[0].index - 1].type.index - 1];
                if (inputType.shape.empty()) return slot;
                const auto rows = shapeElementCount(inputType.shape);
                const auto elementWidth = typeElementWidth(inputType);
                if (!rows || !elementWidth)
                    throw std::runtime_error("CPU six-phase emit shaped input read has an invalid shape");
                return packStoreRows(slot, inputType.shape, "", *rows, *elementWidth, result.width);
            }
            if (name == "core.state.read")
            {
                const auto refs = model_.objectRefs(op);
                if (refs.size() != 1 || refs[0].kind != ObjectKind::State)
                    throw std::runtime_error("CPU six-phase emit state read requires one state reference");
                const bool inReg = refs[0].index < regFieldByState_.size() && regFieldByState_[refs[0].index];
                const bool inMem = refs[0].index < memFieldByState_.size() && memFieldByState_[refs[0].index];
                if (!inReg && !inMem)
                    throw std::runtime_error("CPU six-phase emit state read has no named-store field");
                const CpuStoreField *field = inReg ? regFieldByState_[refs[0].index] : memFieldByState_[refs[0].index];
                const std::string slot = (inReg ? "smallSinkStore." : "largeSinkStore.") + std::string(model_.text(field->name));
                // Whole-array read: pack the flat row array into the
                // total-width vector (row j -> bits [j*elementWidth, ...)).
                // The STATE's shape decides (the result value may legally
                // carry a flat or different view of the same total width).
                const auto &target = stateType(StateId{refs[0].index, 0});
                if (target.shape.empty()) return slot;
                const auto rows = shapeElementCount(target.shape);
                const auto elementWidth = typeElementWidth(target);
                if (!rows || !elementWidth)
                    throw std::runtime_error("CPU six-phase emit shaped state read has an invalid shape");
                return packStoreRows(slot, target.shape, "", *rows, *elementWidth, result.width);
            }
            if (name == "core.state.readArray")
            {
                const auto refs = model_.objectRefs(op);
                if (refs.size() != 1 || refs[0].kind != ObjectKind::State)
                    throw std::runtime_error("CPU array read requires one array state reference");
                if (operands.empty())
                    throw std::runtime_error("CPU array read requires at least one index operand");
                const StateId target{refs[0].index, 0};
                const auto &array = stateType(target);
                const auto elementWidth = typeElementWidth(array);
                if (array.shape.empty() || !elementWidth || operands.size() > array.shape.size())
                    throw std::runtime_error("CPU array read target is not a shaped logic state");
                // mem-class arrays live in the single largeSinkStore; regLatch-class
                // arrays (M5d-6) read the currently visible regLatch row (NBA:
                // same-round writes are not visible until P_publish).
                const bool inMem = refs[0].index < memFieldByState_.size() && memFieldByState_[refs[0].index];
                const CpuStoreField *field = inMem ? memFieldByState_[refs[0].index]
                     : refs[0].index < regFieldByState_.size() ? regFieldByState_[refs[0].index] : nullptr;
                if (!field)
                    throw std::runtime_error("CPU six-phase emit array read has no named-store field");
                const std::string slot = (inMem ? "largeSinkStore." : "smallSinkStore.") + std::string(model_.text(field->name));
                // Row-major linearization over the state shape: row =
                // sum_j idx_j * prod(shape[j+1:]) (stride 1 keeps the bare
                // index — the legacy single-index emission shape).
                std::vector<std::string> indices;
                indices.reserve(operands.size());
                for (std::size_t j = 0; j < operands.size(); ++j)
                    indices.push_back("static_cast<std::size_t>(" + raw(j) + ")");
                const std::string row = linearIndexText(array.shape, array.shape.size(), indices);
                if (operands.size() == array.shape.size())
                {
                    std::string cell = slot;
                    for (const auto &index : indices) cell += '[' + index + ']';
                    return cell;
                }
                // k < rank: the sub-array spans the contiguous row block
                // [row, row + prod(shape[k:])); pack it like a whole read.
                const auto blockRows = shapeElementCount(std::span<const int64_t>(array.shape).subspan(operands.size()));
                if (!blockRows)
                    throw std::runtime_error("CPU array read sub-array shape is invalid");
                return packStoreRows(slot, array.shape, row, *blockRows, *elementWidth, result.width);
            }
            if (name == "core.compute.constant")
            {
                const auto params = model_.parameters(op);
                const auto *text = parameter<std::string>(model_, params, "value");
                if (!text) text = parameter<std::string>(model_, params, "constValue");
                if (result.kind == TypeKind::String)
                {
                    if (!text) return "std::string{}";
                    std::string escaped;
                    escaped.reserve(text->size());
                    for (const char ch : *text)
                    {
                        switch (ch)
                        {
                        case '\\': escaped += "\\\\"; break;
                        case '"': escaped += "\\\""; break;
                        case '\n': escaped += "\\n"; break;
                        case '\r': escaped += "\\r"; break;
                        case '\t': escaped += "\\t"; break;
                        default: escaped += ch; break;
                        }
                    }
                    return "std::string(\"" + escaped + "\")";
                }
                if (result.kind != TypeKind::Logic)
                    throw std::runtime_error("CPU six-phase emit non-logic constant is not implemented");
                if (text) return literal(*text, result);
                const auto *integer = parameter<int64_t>(model_, params, "value");
                if (!integer) integer = parameter<int64_t>(model_, params, "constValue");
                if (integer) return literal(std::to_string(*integer), result);
                const auto *boolean = parameter<bool>(model_, params, "value");
                if (!boolean) boolean = parameter<bool>(model_, params, "constValue");
                if (boolean) return literal(*boolean ? "1" : "0", result);
                throw std::runtime_error("CPU constant requires value literal");
            }
            if (name == "core.system.function")
            {
                const auto slot = randomFunctions_.at(op.id.index);
                return "([&](){if(!cpu_random_sampled[" + std::to_string(slot) + "]){cpu_random_values[" +
                       std::to_string(slot) + "]=grhsim_random_u64(cpu_rng," + std::to_string(width) +
                       ");cpu_random_sampled[" + std::to_string(slot) + "]=true;cpu_anyOutChange_=true;}return cpu_random_values[" +
                       std::to_string(slot) + "]; }())";
            }
            if (!name.starts_with("core.compute.")) throw std::runtime_error("CPU six-phase emit unsupported operation: " + std::string(name));
            const auto kind = name.substr(13);
            // Stage-06 fast path: sliceArray on a row-backed shaped value
            // (state field / per-row boundary array) indexes the rows directly
            // — O(1) per element instead of packing the total-width vector
            // and shifting it (the generic flat-vector path below).
            if (kind == "sliceArray" && !operands.empty())
            {
                const auto &baseType = type(operands[0]);
                if (!baseType.shape.empty())
                {
                    if (const auto store = rowBackedArrayText(operands[0]))
                    {
                        const std::size_t indexCount = operands.size() - 1;
                        if (indexCount > baseType.shape.size())
                            throw std::runtime_error("CPU sliceArray index count exceeds the base rank");
                        std::vector<std::string> indices;
                        indices.reserve(indexCount);
                        for (std::size_t j = 0; j < indexCount; ++j)
                            indices.push_back("static_cast<std::size_t>(" + raw(1 + j) + ")");
                        const std::string row =
                            linearIndexText(baseType.shape, baseType.shape.size(), indices);
                        const auto rowCount = shapeElementCount(baseType.shape);
                        if (!rowCount)
                            throw std::runtime_error("CPU sliceArray base value has an invalid shape");
                        if (indexCount == baseType.shape.size())
                        {
                            // Out-of-range reads yield zero (the legacy
                            // sliceArray clamp semantics), same as the generic
                            // flat-vector path. Wide elements stay _BitInt.
                            const std::string cell = rowAccessText(*store, baseType.shape, row);
                            if (width > 64)
                                return "(static_cast<std::size_t>(" + row + ")>=" +
                                       std::to_string(*rowCount) + "?" + wideTrunc("0", width) + ":" + cell + ")";
                            return "(static_cast<std::size_t>(" + row + ")>=" +
                                   std::to_string(*rowCount) + "?" +
                                   truncRaw("UINT64_C(0)", width) + ":" +
                                   truncRaw("(std::uint64_t)(" + cell + ")", width) + ")";
                        }
                        // Sub-array read: pack the contiguous row block
                        // [row, row + prod(shape[k:])).
                        const auto blockRows = shapeElementCount(
                            std::span<const int64_t>(baseType.shape).subspan(indexCount));
                        const auto elementWidth = typeElementWidth(baseType);
                        if (!blockRows || !elementWidth)
                            throw std::runtime_error("CPU sliceArray base value has an invalid shape");
                        return packStoreRows(*store, baseType.shape, row, *blockRows, *elementWidth, width, *rowCount);
                    }
                }
            }
            // Stage-07 fast path: a static bit slice of a row-backed shaped
            // value is commonly how aggregate lowering reads individual
            // elements (for example, 6-bit entries from a 520-row table).
            // The generic path first packs every row into a wide _BitInt and
            // then shifts it back out, even when the slice is exactly one
            // physical row.  Address aligned slices directly in the backing
            // array; this preserves the packed little-endian row order while
            // making the common case O(1) instead of O(number of rows).
            if (kind == "sliceStatic" && !operands.empty())
            {
                const auto &baseType = type(operands[0]);
                if (!baseType.shape.empty())
                {
                    if (const auto store = rowBackedArrayText(operands[0]))
                    {
                        const auto elementWidth = typeElementWidth(baseType);
                        const auto rowCount = shapeElementCount(baseType.shape);
                        const auto start = number("sliceStart");
                        if (elementWidth && rowCount && *elementWidth &&
                            start % *elementWidth == 0 && start + width <= baseType.width)
                        {
                            const auto firstRow = start / *elementWidth;
                            const auto rows = (uint64_t(width) + *elementWidth - 1) / *elementWidth;
                            if (rows == 1 && width <= *elementWidth)
                            {
                                const auto cell = rowAccessText(*store, baseType.shape,
                                                                std::to_string(firstRow));
                                if (width > 64)
                                    return wideTrunc("(" + cell + ")", width);
                                return truncRaw("(std::uint64_t)(" + cell + ")", width);
                            }
                            if (width % *elementWidth == 0 && rows * *elementWidth == width)
                                return packStoreRows(*store, baseType.shape,
                                                     std::to_string(firstRow), rows,
                                                     *elementWidth, width, *rowCount);
                        }
                    }
                }
            }
            if (width > 64)
            {
                // Native _BitInt shapes (gsim-style). Operands are clean
                // width-bit values by the padding invariant; every shape
                // that can set bits above the semantic width masks its
                // result (wideTrunc), so the invariant propagates.
                const auto widen = [&](std::size_t i) { return "((" + wideType(width) + ")(" + raw(i) + "))"; };
                if (kind == "assign") return raw(0);
                if (kind == "and" || kind == "or" || kind == "xor")
                    return "(" + raw(0) + (kind == "and" ? "&" : kind == "or" ? "|" : "^") + raw(1) + ")";
                // xnor/not flip exactly the semantic bits (grhsim_not_words
                // and the xnor helper both end with a width mask).
                if (kind == "xnor") return "((" + raw(0) + "^" + raw(1) + ")^" + wideMask(width) + ")";
                if (kind == "not") return "(" + raw(0) + "^" + wideMask(width) + ")";
                if (kind == "mux")
                {
                    // grhsim_mux_words body: branchless masked select; both
                    // inputs are clean, so the result is clean.
                    const auto select = "((" + wideType(width) + ")0-((std::uint64_t)(" + raw(0) + ")!=0))";
                    return "((" + raw(1) + "&" + select + ")|(" + raw(2) + "&~" + select + "))";
                }
                if (kind == "shl" || kind == "lshr")
                {
                    // amount >= width yields 0; the ternary keeps the shift
                    // amount strictly below the container width (no UB). shl
                    // widens the value to the result container first — the
                    // operand is often far narrower than the result, and
                    // shifting in the narrow type is undefined.
                    const auto shifted = kind == "shl" ? wideTrunc(widen(0) + "<<(" + raw(1) + ")", width)
                                                       : "((" + raw(0) + ")>>(" + raw(1) + "))";
                    return "((" + raw(1) + ")>=" + std::to_string(width) + "?" + wideTrunc("0", width) + ":" + shifted + ")";
                }
                if (kind == "ashr")
                {
                    // grhsim_ashr_words: sign-extend from the semantic width,
                    // clamp the amount to container-1 (beyond-width amounts
                    // saturate to the sign), mask back down.
                    const auto container = wideContainer(width);
                    const auto amount = "((" + raw(1) + ")>=" + std::to_string(container) + "?" +
                                        std::to_string(container - 1) + ":(" + raw(1) + "))";
                    return wideTrunc("(" + wideSext(raw(0), width) + ")>>(" + amount + ")", width);
                }
                if (kind == "add" || kind == "sub")
                    return wideTrunc("(" + raw(0) + (kind == "add" ? "+" : "-") + raw(1) + ")", width);
                if (kind == "replicate")
                {
                    const auto elemWidth = type(operands[0]).width;
                    const auto rep = number("rep");
                    if (elemWidth == 1)
                        // Bit fill: 0 - bit broadcasts the bit across the container.
                        return wideTrunc("((" + wideType(width) + ")0-" + widen(0) + ")", width);
                    const auto element = elemWidth > 64 ? raw(0) : truncRaw(raw(0), elemWidth);
                    return "([&](){ " + wideType(width) + " cpu_rep=0;for(unsigned cpu_ri=0;cpu_ri<" +
                           std::to_string(rep) + "&&cpu_ri*" + std::to_string(elemWidth) + "<" +
                           std::to_string(width) + ";++cpu_ri) cpu_rep|=(" + wideType(width) + ")(" + element +
                           ")<<(cpu_ri*" + std::to_string(elemWidth) + ");return cpu_rep&" + wideMask(width) + ";}())";
                }
                if (kind == "concat" && operands.size() >= 2)
                {
                    // {joined, rhs}: rhs takes the low bits. Operands wider
                    // than the result contribute only their low `width` bits
                    // (same truncation the insert-words helpers applied).
                    const auto low = [&](std::size_t i, uint64_t limit) {
                        const auto w = type(operands[i]).width;
                        return w > 64 ? wideTrunc(raw(i), std::min<uint64_t>(w, limit))
                                      : truncRaw(raw(i), std::min<uint64_t>(w, limit));
                    };
                    if (operands.size() <= 8)
                    {
                        std::string joined = low(0, width);
                        for (std::size_t i = 1; i < operands.size(); ++i)
                        {
                            const auto rhsWidth = type(operands[i]).width;
                            if (rhsWidth >= width) { joined = low(i, width); continue; }
                            joined = wideTrunc("((" + wideType(width) + ")(" + joined + ")<<" +
                                                   std::to_string(rhsWidth) + "|(" + low(i, rhsWidth) + "))",
                                               width);
                        }
                        return joined;
                    }
                    // Balance large concats and widen only at each join. A
                    // full-result-width shift chain makes SelectionDAG combine
                    // pathological on the XS 16384-bit concat. Named partials
                    // also bound parser nesting independently of fan-in.
                    std::vector<std::pair<std::size_t, uint64_t>> parts;
                    uint64_t remaining = width;
                    for (std::size_t i = operands.size(); i-- > 0 && remaining;)
                    {
                        const auto bits = std::min<uint64_t>(type(operands[i]).width, remaining);
                        parts.emplace_back(i, bits);
                        remaining -= bits;
                    }
                    std::reverse(parts.begin(), parts.end());
                    std::string body = "([&](){\n";
                    uint32_t next = 0;
                    const auto join = [&](const auto &self, std::size_t begin, std::size_t end)
                        -> std::pair<std::string, uint64_t> {
                        if (end - begin == 1)
                        {
                            const auto [operand, bits] = parts[begin];
                            const auto nativeType = bits <= 64 ? "std::uint64_t" : wideType(bits);
                            // A signed 64-bit leaf must become unsigned before
                            // widening, or its sign bits overwrite its neighbor.
                            return {"((" + nativeType + ")(" + low(operand, bits) + "))", bits};
                        }
                        const auto middle = begin + (end - begin) / 2;
                        const auto lhs = self(self, begin, middle);
                        const auto rhs = self(self, middle, end);
                        const auto bits = lhs.second + rhs.second;
                        const auto nativeType = bits <= 64 ? "std::uint64_t" : wideType(bits);
                        const auto name = "cpu_cat_" + std::to_string(next++);
                        body += "const " + nativeType + " " + name + "=((" + nativeType + ")(" +
                                lhs.first + ")<<" + std::to_string(rhs.second) + "|(" + rhs.first + "));\n";
                        return {name, bits};
                    };
                    const auto result = join(join, 0, parts.size());
                    body += "return (" + wideType(width) + ")(" + result.first + ");}())";
                    return body;
                }
                if ((kind == "sliceStatic" || kind == "sliceDynamic") && type(operands[0]).width > 64)
                {
                    // start clamps at the source container (padding reads as
                    // zero), the ternary keeps the shift defined. The shift is
                    // word-decomposed (see the <=64-bit result path).
                    const auto srcContainer = wideContainer(type(operands[0]).width);
                    const auto start = kind == "sliceStatic" ? std::to_string(number("sliceStart")) : raw(1);
                    return "((" + start + ")>=" + std::to_string(srcContainer) + "?" + wideTrunc("0", width) + ":" +
                           wideTrunc("((" + raw(0) + ")>>((" + start + ")&~UINT64_C(63)))>>((" + start +
                                      ")&UINT64_C(63))",
                                  width) + ")";
                }
                if (kind == "sliceArray" && type(operands[0]).width > 64)
                {
                    // Multi-index block index in result-width units (same
                    // geometry as the scalar path's sliceArray branch).
                    const auto &baseShape = type(operands[0]).shape;
                    if (baseShape.empty() || operands.size() - 1 > baseShape.size())
                        throw std::runtime_error("CPU sliceArray base value is not a shaped logic value");
                    std::vector<std::string> indices;
                    indices.reserve(operands.size() - 1);
                    for (std::size_t j = 1; j < operands.size(); ++j)
                        indices.push_back("static_cast<std::uint64_t>(" + raw(j) + ")");
                    const auto srcContainer = wideContainer(type(operands[0]).width);
                    const auto start = "(" + linearIndexText(baseShape, operands.size() - 1, indices) + ")*" +
                                       std::to_string(width);
                    return "((" + start + ")>=" + std::to_string(srcContainer) + "?" + wideTrunc("0", width) + ":" +
                           wideTrunc("((" + raw(0) + ")>>((" + start + ")&~UINT64_C(63)))>>((" + start +
                                      ")&UINT64_C(63))",
                                  width) + ")";
                }
                throw std::runtime_error("CPU six-phase emit unsupported wide operation: " + std::string(kind) + " width=" + std::to_string(width));
            }
            if (kind == "expr")
                throw std::runtime_error("M5a后续切片: fused expr chains are not implemented");
            return scalarExpression(kind, result, operands.size(), raw,
                [&](std::size_t i) -> const Type & { return type(operands[i]); }, number);
        }

        std::optional<std::uint64_t> SixPhaseEmitter::scalarConstantValue(ValueId operand) const
        {
            if (!operand || operand.index >= producers_.size()) return {};
            const auto producer = producers_[operand.index];
            if (!producer) return {};
            const auto &op = model_.operations()[producer.index - 1];
            if (model_.text(op.opType) != "core.compute.constant") return {};
            const auto &operandType = type(operand);
            if (operandType.kind != TypeKind::Logic || operandType.domain != LogicDomain::TwoState ||
                operandType.width == 0 || operandType.width > 64)
                return {};
            const auto params = model_.parameters(op);
            const auto *text = parameter<std::string>(model_, params, "value");
            if (!text) text = parameter<std::string>(model_, params, "constValue");
            if (!text) return {};
            try
            {
                auto parsed = slang::SVInt::fromString(*text).resize(operandType.width);
                parsed.flattenUnknowns();
                return parsed.as<std::uint64_t>();
            }
            catch (const std::exception &)
            {
                return {};
            }
        }
        bool SixPhaseEmitter::isAllOnesConstant(ValueId operand, uint32_t width) const
        {
            if (width == 0) return false;
            if (width <= 64)
            {
                const auto value = scalarConstantValue(operand);
                return value && *value == (width == 64 ? ~UINT64_C(0) : (UINT64_C(1) << width) - 1);
            }
            if (!operand || operand.index >= producers_.size()) return false;
            const auto producer = producers_[operand.index];
            if (!producer) return false;
            const auto &op = model_.operations()[producer.index - 1];
            if (model_.text(op.opType) != "core.compute.constant") return false;
            const auto params = model_.parameters(op);
            const auto *text = parameter<std::string>(model_, params, "value");
            if (!text) text = parameter<std::string>(model_, params, "constValue");
            if (!text) return {};
            std::string_view literalText = *text;
            if (literalText == "'1") literalText = "-1"; // initLiteral's unsized fill normalization
            try
            {
                auto parsed = slang::SVInt::fromString(literalText).resize(width);
                parsed.flattenUnknowns();
                return parsed.countOnes() == width;
            }
            catch (const std::exception &)
            {
                return false;
            }
        }

        // ----- Guards / activation -----
        // UINT64_C(0x…) literal for emitted bit masks.
        static std::string u64Const(uint64_t value)
        {
            std::ostringstream text;
            text << "UINT64_C(0x" << std::hex << value << ')';
            return text.str();
        }
        // OR of the given event act bits. eventActStore is byte-packed: bit
        // act%8 of byte act/8 (layout-named-stores EventAct store) — the same
        // packing emitEdgeDet and the timeslot triggers use.
        std::string SixPhaseEmitter::actBitsGuard(std::span<const int64_t> acts) const
        {
            std::string guard;
            for (const auto act : acts)
            {
                if (act < 0) throw std::runtime_error("CPU six-phase emit negative event act");
                if (!guard.empty()) guard += "||";
                guard += "((eventActStore[" + std::to_string(act / 8) + "]>>" + std::to_string(act % 8) + ")&1)";
            }
            return guard;
        }
        // OR of the op's event_acts bits; "true" when event-free.
        std::string SixPhaseEmitter::actGuard(const SimOp &op) const
        {
            const auto acts = readCpuPhaseEventActs(model_, op);
            if (acts.empty()) return "true";
            return actBitsGuard(acts);
        }
        // V2-M3 boundary rule: inside a sink supernode body the scanRange
        // call-site gate already covers every op — SinkEvent ops all carry
        // the supernode signature (structural, cpu.st.merge-general-supernodes)
        // and SinkEscape ops are event-free — so the per-op event guard is
        // redundant and dropped. Non-sink bodies mix event and event-free
        // ops and keep the per-op guard; P_mem and the P_event/P_output op
        // lists (current == ~0u) always guard.
        std::string SixPhaseEmitter::bodyGuard(const SimOp &op, uint32_t current) const
        {
            if (current != ~0u &&
                *tree_.partitions[order_[current].index - 1].attrs.supernodeCategory != CpuSupernodeCategory::NonSink)
                return "true";
            return actGuard(op);
        }
        std::string SixPhaseEmitter::callCondition(ValueId condition) const
        {
            const auto &target = type(condition);
            // Wide conditions are clean _BitInt values: reduce-or is != 0.
            return target.width > 64 ? "((" + read(condition) + ")!=0)" : read(condition);
        }
        // fanout 合写累积变量名（数组名 + 词号；数组名本身是合法标识符）。
        static std::string flagAccumVar(std::string_view array, uint32_t word)
        {
            return "cpu_flagAcc_" + std::string(array) + "_" + std::to_string(word);
        }
        // 函数尾写回：每个被动过的词一条 `array[word] |= acc;`。
        void SixPhaseEmitter::flagAccumWriteBack(std::ostream &out) const
        {
            for (const auto &[array, words] : flagAccumTouched_)
                for (const uint32_t word : words)
                    out << array << '[' << word << "]|=" << flagAccumVar(array, word) << ";\n";
        }
        void SixPhaseEmitter::flagAccumDecls(std::ostream &out) const
        {
            for (const auto &[array, words] : flagAccumTouched_)
                for (const uint32_t word : words)
                    out << "std::uint64_t " << flagAccumVar(array, word) << "=0;\n";
        }
        void SixPhaseEmitter::activateOrdinals(std::ostream &out, std::span<const uint32_t> ordinals,
                                               std::string_view array) const
        {
            // V3-M1: batched per-word mask ORs into the ActiveFlags bit words
            // (ordinals need not be sorted).
            std::map<uint32_t, uint64_t> words;
            for (const auto ordinal : ordinals) words[ordinal / 64] |= 1ull << (ordinal % 64);
            for (const auto &[word, mask] : words)
            {
                if (flagAccumActive_)
                {
                    // stage-07 fanout 合写：函数级局部累积，函数尾一次写回。
                    flagAccumTouched_[std::string(array)].insert(word);
                    out << flagAccumVar(array, word) << "|=" << u64Const(mask) << ";\n";
                }
                else
                {
                    out << array << '[' << word << "]|=" << u64Const(mask) << ";\n";
                }
            }
        }
        void SixPhaseEmitter::activateFanout(std::ostream &out, std::span<const uint32_t> ordinals,
                                             uint32_t current) const
        {
            // Same-round split: a successor later in the supernode scan fires this
            // round (nonSinkActiveFlag); an earlier/self successor waits for the next
            // round (nonSinkActiveFlagNext). current == ~0 (outside P_general) queues
            // every target into nonSinkActiveFlagNext.
            const auto dataActive = model_.text(activeStore_->fields[0].name);
            const auto dataNext = model_.text(activeStore_->fields[1].name);
            std::vector<uint32_t> now, later;
            for (const auto ordinal : ordinals)
                (current != ~0u && ordinal > current ? now : later).push_back(ordinal);
            activateOrdinals(out, now, dataActive);
            activateOrdinals(out, later, dataNext);
        }
        void SixPhaseEmitter::activateOrdinalsCond(std::ostream &out, std::span<const uint32_t> ordinals,
                                                   std::string_view array, std::string_view cond) const
        {
            std::map<uint32_t, uint64_t> words;
            for (const auto ordinal : ordinals) words[ordinal / 64] |= 1ull << (ordinal % 64);
            for (const auto &[word, mask] : words)
            {
                if (flagAccumActive_)
                {
                    flagAccumTouched_[std::string(array)].insert(word);
                    out << flagAccumVar(array, word) << "|=-(std::uint64_t)" << cond << '&' << u64Const(mask) << ";\n";
                }
                else
                {
                    out << array << '[' << word << "]|=-(std::uint64_t)" << cond << '&' << u64Const(mask) << ";\n";
                }
            }
        }
        // Emit the output-gate dirty flag when this change point feeds the
        // output phase (the set membership is decided at emit time, so other
        // writes pay nothing at runtime).
        static void flagOutputDirty(std::ostream &out, bool feeds)
        {
            if (feeds) out << "cpu_anyOutChange_=true;\n";
        }
        void SixPhaseEmitter::activateFanoutCond(std::ostream &out, std::span<const uint32_t> ordinals,
                                                 uint32_t current, std::string_view cond) const
        {
            const auto dataActive = model_.text(activeStore_->fields[0].name);
            const auto dataNext = model_.text(activeStore_->fields[1].name);
            std::vector<uint32_t> now, later;
            for (const auto ordinal : ordinals)
                (current != ~0u && ordinal > current ? now : later).push_back(ordinal);
            activateOrdinalsCond(out, now, dataActive, cond);
            activateOrdinalsCond(out, later, dataNext, cond);
        }
        void SixPhaseEmitter::publishBoundary(std::ostream &out, ValueId result, const std::string &expr,
                                              uint32_t current, bool arrayValue) const
        {
            const auto &fields = boundaryFieldsByValue_[result.index];
            if (fields.empty()) { declareLocal(out, result, expr); return; }
            const auto &resultType = type(result);
            std::optional<std::string> constantAggregate;
            if (!resultType.shape.empty()) constantAggregate = constantArrayInitializer(result);
            const std::string constantName = "cpu_const_array_" + std::to_string(result.index);
            if (constantAggregate)
                out << "static constexpr " << cppValueType(resultType) << ' ' << constantName << '='
                    << *constantAggregate << ";\n";
            const bool shared = fields.size() > 1;
            if (shared) out << "{const auto &cpu_shared_value=" << expr << ";\n";
            const std::string source = shared ? "cpu_shared_value" : expr;
            for (const auto *field : fields)
            {
                const auto slot = boundaryRef(field);
                const auto smallHook = smallEnable_.fieldBits.find(field);
                const auto largeHook = largeEnable_.fieldBits.find(field);
                const bool hasHook = smallHook != smallEnable_.fieldBits.end() ||
                                     largeHook != largeEnable_.fieldBits.end();
                const bool notify = field == fields.front();
                if (!resultType.shape.empty())
                {
                    const auto rows = shapeElementCount(resultType.shape);
                    const auto elementWidth = typeElementWidth(resultType);
                    if (!rows || !elementWidth)
                        throw std::runtime_error("CPU six-phase emit shaped boundary value has an invalid shape");
                    const Type element = arrayElementType(resultType);
                    const std::string rowExpr = constantAggregate
                        ? rowAccessText(constantName, resultType.shape, "cpu_j")
                        : arrayValue
                            ? rowAccessText("cpu_value", resultType.shape, "cpu_j")
                            : normalize(rowSliceText("cpu_value", "cpu_j", *elementWidth, resultType.width), element);
                    out << "{const auto &cpu_value=" << (constantAggregate ? constantName : source) << ";\n";
                    if (!hasHook && (!notify || supernodeFanout_[result.index].empty()))
                    {
                        out << "for(std::size_t cpu_j=0;cpu_j<" << *rows << ";++cpu_j)"
                            << rowAccessText(slot, resultType.shape, "cpu_j") << '=' << rowExpr
                            << ";\n}\n";
                        continue;
                    }
                    out << "bool cpu_chg=false;\n"
                        << "for(std::size_t cpu_j=0;cpu_j<" << *rows << ";++cpu_j){const auto cpu_e=" << rowExpr
                        << ";auto &cpu_c=" << rowAccessText(slot, resultType.shape, "cpu_j")
                        << ";if(cpu_c!=cpu_e){cpu_c=cpu_e;cpu_chg=true;}}\n"
                        << "if(cpu_chg){\n";
                    if (notify)
                    {
                        flagOutputDirty(out, outputReadValues_.contains(result.index));
                        activateFanout(out, supernodeFanout_[result.index], current);
                    }
                    out << "}}\n";
                    continue;
                }
                if (!hasHook && (!notify || supernodeFanout_[result.index].empty()))
                {
                    out << slot << '=' << source << ";\n";
                    continue;
                }
                if (boundaryStringSlot_.contains(field))
                {
                    out << "{const auto &cpu_value=" << source << ";if(" << slot << "!=cpu_value){"
                        << slot << "=cpu_value;";
                    if (notify)
                    {
                        flagOutputDirty(out, outputReadValues_.contains(result.index));
                        activateFanout(out, supernodeFanout_[result.index], current);
                    }
                    if (smallHook != smallEnable_.fieldBits.end())
                        out << enableSync(smallEnable_, smallHook->second, "cpu_value");
                    if (largeHook != largeEnable_.fieldBits.end())
                        out << enableSync(largeEnable_, largeHook->second, "cpu_value");
                    out << "}}\n";
                    continue;
                }
                out << "{const auto &cpu_value=" << source << ";const bool cpu_chg=" << slot
                    << "!=cpu_value;" << slot << "=cpu_value;\n";
                if (smallHook != smallEnable_.fieldBits.end())
                    out << enableSync(smallEnable_, smallHook->second, "cpu_value");
                if (largeHook != largeEnable_.fieldBits.end())
                    out << enableSync(largeEnable_, largeHook->second, "cpu_value");
                if (notify)
                {
                    flagOutputDirty(out, outputReadValues_.contains(result.index));
                    activateFanoutCond(out, supernodeFanout_[result.index], current, "cpu_chg");
                }
                out << "}\n";
            }
            if (shared) out << "}\n";
        }

        // ----- Per-op emission -----
        void SixPhaseEmitter::emitCompute(std::ostream &out, const SimOp &op, uint32_t current) const
        {
            const auto name = model_.text(op.opType);
            if (name == "core.system.task") { emitSystemTask(out, op, current); return; }
            if (name == "core.dpi.call") { emitDpiCall(out, op, current); return; }
            if (name == "core.state.write" || name == "core.state.writeMask" ||
                name == "core.state.latchWrite")
            {
                // A write on a shaped (array) state is a whole-array write and
                // follows the array write path; scalar write/latchWrite is the
                // register RMW.
                const auto refs = model_.objectRefs(op);
                const bool shapedTarget = (name == "core.state.write" ||
                                           name == "core.state.writeMask") && !refs.empty() &&
                    refs[0].kind == ObjectKind::State && refs[0].index >= 1 &&
                    refs[0].index <= model_.states().size() &&
                    !stateType(StateId{refs[0].index, 0}).shape.empty();
                if (shapedTarget) { emitGeneralMemWrite(out, op, current); return; }
                emitRegWrite(out, op, current); return;
            }
            if (name == "core.state.writeArray" || name == "core.state.writeArrayMask" ||
                name == "core.state.fill")
            { emitGeneralMemWrite(out, op, current); return; }
            if (model_.results(op).size() != 1)
                throw std::runtime_error("CPU six-phase emit unsupported result arity: " + std::string(name));
            const auto result = model_.results(op)[0];
            // Constants fold into their use sites; a constant boundary field is
            // preloaded at init() and never republished.
            if (staticScalars_.contains(result.index) || staticStrings_.contains(result.index)) return;
            if (name == "core.input.read")
            {
                // The natural read text is the input port's boundary field; only
                // a cross-supernode result republishes it into its own boundary
                // field (expression(), not read(): read(result) would resolve to
                // the destination field itself and self-assign).
                if (boundaryByValue_[result.index]) publishBoundary(out, result, expression(op), current);
                return;
            }
            const auto &resultType = type(result);
            if (!resultType.shape.empty())
                if (const auto direct = directShapedExpression(op))
                {
                    // A shaped boundary value is physically an array. Keep
                    // the direct row-wise expression all the way through the
                    // compare-store so it never crosses the packed _BitInt
                    // bridge just to be unpacked again by publishBoundary.
                    if (boundaryByValue_[result.index])
                    {
                        publishBoundary(out, result, *direct, current, true);
                        return;
                    }
                    if (crossingLocals_[result.index])
                        out << localRef(result) << '=' << *direct << ";\n";
                    else
                    {
                        out << "const " << cppValueType(resultType) << " cpu_v" << result.index << '='
                            << *direct << ";\n";
                        activeLocals_[result.index] = 1;
                    }
                    return;
                }
            // Wide two-state compute ops need no dedicated path: they are
            // plain _BitInt expressions and flow through the generic
            // normalize + publishBoundary/declareLocal tail below (the
            // pointer-ABI word helpers and fused change-detection templates
            // of the array storage era are gone). Stage-06: shaped (array)
            // values are flat total-width vectors and take the same tail —
            // the per-row store form only reappears inside publishBoundary.
            const auto expr = normalize(expression(op), resultType);
            if (boundaryByValue_[result.index]) { publishBoundary(out, result, expr, current); return; }
            declareLocal(out, result, expr);
        }
        void SixPhaseEmitter::emitEdgeDet(std::ostream &out, const SimOp &op) const
        {
            const auto params = model_.parameters(op);
            const auto *act = parameter<int64_t>(model_, params, "act");
            const auto *edge = parameter<std::string>(model_, params, "edge");
            const auto *prev = parameter<int64_t>(model_, params, "prev");
            if (!act || !edge || !prev || *act < 0 || *prev < 0 || *act != *prev)
                throw std::runtime_error("CPU six-phase emit malformed edgeDet parameters");
            if (*edge != "posedge" && *edge != "negedge" && *edge != "both")
                throw std::runtime_error("CPU six-phase emit unknown edgeDet edge: " + *edge);
            const auto operands = model_.operands(op);
            if (operands.size() != 1)
                throw std::runtime_error("CPU six-phase emit edgeDet requires one event operand");
            const auto event = operands.front();
            const auto &eventType = type(event);
            if (static_cast<std::size_t>(*act) >= prevByAct_.size() || !prevByAct_[*act])
                throw std::runtime_error("CPU six-phase emit edgeDet act has no prevEvent field");
            const auto prevSlot = "prevEventStore." + std::string(model_.text(prevByAct_[*act]->name));
            const std::string cur = read(event);
            // The IR contract permits only an unshaped 1-bit event. CPU emit
            // currently implements its two-state form.
            std::string hit;
            if (eventType.kind == TypeKind::Logic && eventType.width == 1 &&
                eventType.shape.empty() && eventType.domain == LogicDomain::TwoState)
            {
                const auto curW = "static_cast<std::uint64_t>(" + cur + ")";
                const auto prevW = "static_cast<std::uint64_t>(" + prevSlot + ")";
                if (*edge == "posedge") hit = "grhsim_event_posedge(" + curW + "," + prevW + ")";
                else if (*edge == "negedge") hit = "grhsim_event_negedge(" + curW + "," + prevW + ")";
                else hit = "(grhsim_event_posedge(" + curW + "," + prevW + ")||grhsim_event_negedge(" + curW + "," + prevW + "))";
            }
            else throw std::runtime_error("CPU six-phase emit edgeDet event must be unshaped 1-bit two-state logic");
            out << "if(" << hit << "){\n"
                << "eventActStore[" << *act / 8 << "]|=std::uint8_t(1u<<" << *act % 8 << ");\n";
            // V2 (M2): OR the act's activation words (bit i == ordinal i, the
            // non-sink supernodes holding an op with this act) straight into
            // nonSinkActiveFlag — eventActiveFlag and the dual gate are gone.
            // V3-M1: the activation words and nonSinkActiveFlag share the same
            // bit-per-supernode layout, so the activation is a plain word OR.
            if (const auto it = activationByAct_.find(static_cast<uint32_t>(*act));
                it != activationByAct_.end() && std::any_of(it->second->supernodeWords.begin(), it->second->supernodeWords.end(),
                                                            [](std::uint64_t word) { return word != 0; }))
            {
                const auto &words = it->second->supernodeWords;
                const auto dataActive = model_.text(activeStore_->fields[0].name);
                out << "{static constexpr std::uint64_t cpu_bm[]={";
                for (const auto word : words) out << u64Const(word) << ',';
                out << "};\nfor(std::size_t cpu_i=0;cpu_i<" << words.size() << ";++cpu_i)" << dataActive << "[cpu_i]|=cpu_bm[cpu_i];\n}\n";
            }
            // prev updates whether or not the edge hit (plan §72), so a missed
            // direction never replays and the next opposite edge is seen fresh.
            out << "}\n" << prevSlot << '=' << cur << ";\n";
        }
        void SixPhaseEmitter::emitRegWrite(std::ostream &out, const SimOp &op, uint32_t current) const
        {
            const auto operands = model_.operands(op);
            const auto refs = model_.objectRefs(op);
            const auto name = model_.text(op.opType);
            const bool masked = name == "core.state.writeMask" || name == "core.state.latchWrite" ||
                                (name == "core.state.write" && operands.size() == 3);
            if (refs.empty() || refs[0].kind != ObjectKind::State ||
                operands.size() != (masked ? 3u : 2u))
                throw std::runtime_error("CPU six-phase emit malformed state write");
            const std::string guard = bodyGuard(op, current);
            const auto *field = refs[0].index < regFieldByState_.size() ? regFieldByState_[refs[0].index] : nullptr;
            if (!field) throw std::runtime_error("CPU six-phase emit state write target is not a regLatch state");
            const StateId target{refs[0].index, 0};
            const auto &targetType = stateType(target);
            // write/latchWrite here are scalar-only: a whole-array write goes
            // through emitGeneralMemWrite.
            if (!targetType.shape.empty())
                throw std::runtime_error("CPU six-phase emit scalar state write on an array state");
            const std::string nextSlot = "smallSinkStoreNext." + std::string(model_.text(field->name));
            const std::string curSlot = "smallSinkStore." + std::string(model_.text(field->name));
            // Merge base is smallSinkStoreNext (read-modify-write across same-round
            // writers); the fanout compare runs against the currently visible
            // smallSinkStore value so readers fire only on a true change.
            out << "if((" << guard << ")&&"
                << readAs(operands[0], CpuBoundaryUse::SmallSinkEnable) << "){\n";
            const std::string mask = masked ? read(operands[2]) : literal("-1", targetType);
            if (isScalarLogic(targetType))
            {
                const std::string merged = normalize("(static_cast<std::uint64_t>(" + nextSlot + ")&~static_cast<std::uint64_t>(" + mask +
                    "))|(static_cast<std::uint64_t>(" + read(operands[1]) + ")&static_cast<std::uint64_t>(" + mask + "))", targetType);
                out << "{const auto cpu_merged=" << merged << ";if(" << nextSlot << "!=cpu_merged){if(" << curSlot << "!=cpu_merged){\n";
                flagOutputDirty(out, outputReadStates_.contains(refs[0].index));
                if (!smallFinalChange_.contains(target.index))
                    activateOrdinals(out, stateFanout_[refs[0].index], model_.text(activeStore_->fields[1].name));
                out << "GRHSIM_PERF_COUNT(touchedStateShadowCount);\n";
                out << "}" << nextSlot << "=cpu_merged;}}\n";
            }
            else if (targetType.kind == TypeKind::Logic && targetType.domain == LogicDomain::TwoState)
            {
                // grhsim_merge_words_masked natively: (base & ~mask) | (data & mask).
                const std::string merged = normalize("(" + nextSlot + "&~(" + mask + "))|(" +
                                                         read(operands[1]) + "&(" + mask + "))",
                                                     targetType);
                out << "{const auto cpu_merged=" << merged << ";if(" << nextSlot << "!=cpu_merged){if(" << curSlot << "!=cpu_merged){\n";
                flagOutputDirty(out, outputReadStates_.contains(refs[0].index));
                if (!smallFinalChange_.contains(target.index))
                    activateOrdinals(out, stateFanout_[refs[0].index], model_.text(activeStore_->fields[1].name));
                out << "GRHSIM_PERF_COUNT(touchedStateShadowCount);\n";
                out << "}" << nextSlot << "=cpu_merged;}}\n";
            }
            else throw std::runtime_error("CPU six-phase emit state write target type is not two-state logic");
            out << "}\n";
        }
        std::vector<ValueId> SixPhaseEmitter::memEnableOperands(const SimOp &op) const
        {
            const auto name = model_.text(op.opType);
            const auto operands = model_.operands(op);
            // write [en, data], writeMask/latchWrite [en, data, mask];
            // writeArray [en, i0..ik-1, data], writeArrayMask [en, ...,
            // data, mask], fill [en, data] — enable is always first.
            if ((name == "core.state.write" || name == "core.state.writeMask" ||
                 name == "core.state.latchWrite") && operands.size() >= 2)
                return {operands[0]};
            if ((name == "core.state.writeArray" || name == "core.state.writeArrayMask") &&
                operands.size() >= 3) return {operands[0]};
            if (name == "core.state.fill" && operands.size() == 2) return {operands[0]};
            return {};
        }
        std::string SixPhaseEmitter::memEnableGuard(ValueId enable) const
        {
            if (memEnableBitmap_)
                if (const auto it = largeEnable_.bitOfValue.find(enable.index);
                    it != largeEnable_.bitOfValue.end())
                    return "((largeSinkEnableBoundaryStore.enabledWords[" + std::to_string(it->second / 64) + "]>>" +
                           std::to_string(it->second % 64) + ")&UINT64_C(1))";
            return readAs(enable, CpuBoundaryUse::LargeSinkEnable);
        }
        std::string SixPhaseEmitter::enableSync(const EnableBitmap &bitmap, uint32_t bit,
                                                const std::string &valueText) const
        {
            const std::string word = std::string(bitmap.storeName) + ".enabledWords[" +
                                     std::to_string(bit / 64) + ']';
            const std::string mask = u64Const(1ull << (bit % 64));
            return word + "=(" + valueText + ")?(" + word + "|" + mask + "):(" + word + "&~" + mask + ");\n";
        }
        void SixPhaseEmitter::emitMemWrite(std::ostream &out, const SimOp &op, const CpuMemWritePlanEntry &plan,
                                           bool actHoisted, std::set<uint32_t> *burstReaders, bool forceOnChange,
                                           bool enableHoisted) const
        {
            const auto name = model_.text(op.opType);
            const auto operands = model_.operands(op);
            const auto refs = model_.objectRefs(op);
            if (refs.empty() || refs[0].kind != ObjectKind::State)
                throw std::runtime_error("CPU six-phase emit mem write requires a state target");
            const StateId target{refs[0].index, 0};
            const auto &array = stateType(target);
            if (array.kind != TypeKind::Logic)
                throw std::runtime_error("CPU six-phase emit large-sink write target is not logic");
            const Type element = array.shape.empty() ? array : arrayElementType(array);
            const auto rowCountOpt = array.shape.empty() ? std::optional<uint64_t>(1)
                                                         : shapeElementCount(array.shape);
            if (!rowCountOpt)
                throw std::runtime_error("CPU six-phase emit mem write target has an invalid shape");
            const uint64_t rowCount = *rowCountOpt;
            const auto *field = target.index < memFieldByState_.size() ? memFieldByState_[target.index] : nullptr;
            if (!field) throw std::runtime_error("CPU six-phase emit mem write target has no largeSinkStore field");
            const std::string mem = "largeSinkStore." + std::string(model_.text(field->name));
            // Row-major strides over the state shape (element-row units).
            std::vector<uint64_t> strides(array.shape.size(), 1);
            for (std::size_t d = array.shape.size(); d-- > 1;)
                strides[d - 1] = strides[d] * static_cast<uint64_t>(array.shape[d]);
            // Event-gated writes read the act bits (P_event owns edge detection);
            // eventFree writes enter every round and converge via cell change
            // detection (plan §123). Boundary-backed 1-bit enables consult the
            // dense enable boundary bitmap (memEnableGuard) instead of
            // re-reading the scattered boundary field.
            const std::string guard = actGuard(op);
            const auto dataNext = model_.text(activeStore_->fields[1].name);
            // Write-occurs activation (--mem-write-activate on-write) drops the
            // per-cell compare; event-free writes re-enter every round and must
            // keep it, otherwise their readers never quiesce.
            const bool finalChange = largeFinalChange_.contains(target.index);
            const bool onWrite = memWriteOnWrite_ && !forceOnChange && !finalChange;
            // Reader activation for one actually-changed row: a static-row reader
            // fires only on an exact address overlap; a dynamic-row reader fires
            // on any change (plan §132-135). A statically known write row folds
            // the overlap test at emit time.
            const auto activateReaders = [&](const std::string &rowText, std::optional<uint64_t> constRow) {
                flagOutputDirty(out, outputReadStates_.contains(refs[0].index));
                if (finalChange) return;
                for (const auto &reader : plan.readers)
                {
                    const auto ordinal = reader.owner.index < ordinalOf_.size() ? ordinalOf_[reader.owner.index] : ~0u;
                    if (ordinal == ~0u)
                        throw std::runtime_error("CPU six-phase emit mem reader has no supernode ordinal");
                    if (reader.staticRow && constRow && *reader.staticRow != *constRow) continue;
                    if (reader.staticRow && !constRow) out << "if(" << rowText << "==" << *reader.staticRow << ')';
                    out << dataNext << '[' << ordinal / 64 << "]|=" << u64Const(1ull << (ordinal % 64)) << ";\n";
                }
            };
            // On-write mode with a burst collector: reader ordinals are queued
            // for one hoisted activation burst per enable-word run instead of
            // per-cell sets. The emit-time row overlap fold still applies; the
            // runtime row test is dropped (conservative over-activation).
            const auto collectReaders = [&](std::optional<uint64_t> constRow) {
                flagOutputDirty(out, outputReadStates_.contains(refs[0].index));
                for (const auto &reader : plan.readers)
                {
                    const auto ordinal = reader.owner.index < ordinalOf_.size() ? ordinalOf_[reader.owner.index] : ~0u;
                    if (ordinal == ~0u)
                        throw std::runtime_error("CPU six-phase emit mem reader has no supernode ordinal");
                    if (reader.staticRow && constRow && *reader.staticRow != *constRow) continue;
                    burstReaders->insert(ordinal);
                }
            };
            // One cell write with the plan §123 change detection: no change -> no
            // write, no reader activation. On-write mode: unconditional store,
            // readers activate on write-occurs.
            const auto writeCell = [&](const std::string &rowText, std::optional<uint64_t> constRow,
                                       const std::string &data, const std::string *mask) {
                const std::string cell = rowAccessText(mem, array.shape, rowText);
                const auto rememberCell = [&] {
                    if (finalChange)
                        out << "cpu_largeBefore" << target.index << ".try_emplace("
                            << rowText << ',' << cell << ");\n";
                };
                if (isScalarLogic(element))
                {
                    const std::string next = mask
                        ? normalize("(static_cast<std::uint64_t>(" + cell + ")&~static_cast<std::uint64_t>(" + *mask +
                                    "))|(static_cast<std::uint64_t>(" + data + ")&static_cast<std::uint64_t>(" + *mask + "))", element)
                        : normalize(data, element);
                    if (onWrite)
                    {
                        out << cell << "=" << next << ";\n"
                            << "GRHSIM_PERF_COUNT(touchedWriteCount);\n";
                        if (burstReaders) collectReaders(constRow);
                        else activateReaders(rowText, constRow);
                        return;
                    }
                    out << "{auto &cpu_cell=" << cell << ";const auto cpu_next=" << next << ";if(cpu_cell!=cpu_next){\n";
                    rememberCell();
                    out << "cpu_cell=cpu_next;\nGRHSIM_PERF_COUNT(touchedWriteCount);\n";
                    activateReaders(rowText, constRow);
                    out << "}}\n";
                    return;
                }
                if (element.kind == TypeKind::Logic && element.domain == LogicDomain::TwoState)
                {
                    // grhsim_apply_masked_words_inplace natively: merged =
                    // (cell & ~mask) | (data & mask), store on change.
                    if (onWrite)
                    {
                        if (mask)
                            out << cell << "=" << normalize("(" + cell + "&~(" + *mask + "))|(" + data + "&(" + *mask + "))",
                                                            element)
                                << ";\n";
                        else
                            out << cell << '=' << data << ";\n";
                        out << "GRHSIM_PERF_COUNT(touchedWriteCount);\n";
                        if (burstReaders) collectReaders(constRow);
                        else activateReaders(rowText, constRow);
                        return;
                    }
                    if (mask)
                        out << "{const auto cpu_merged=" << normalize("(" + cell + "&~(" + *mask + "))|(" + data +
                                                                       "&(" + *mask + "))",
                                                                       element)
                            << ";if(" << cell << "!=cpu_merged){\n";
                    else
                        out << "if(" << cell << "!=" << data << "){\n";
                    rememberCell();
                    out << cell << '=' << (mask ? "cpu_merged" : data) << ";\n";
                    out << "GRHSIM_PERF_COUNT(touchedWriteCount);\n";
                    activateReaders(rowText, constRow);
                    // The masked branch opened an extra cpu_merged block.
                    out << (mask ? "}}\n" : "}\n");
                    return;
                }
                throw std::runtime_error("CPU six-phase emit mem write element type is not two-state logic");
            };
            // Linearizes the index operands into the flat row number. An
            // all-constant index vector folds to a static row (bounds-checked
            // against rowCount; out of range = dead write); any dynamic index
            // yields the stride-sum text. A single dynamic index keeps the
            // legacy raw-text emission shape.
            const auto rowOf = [&](std::span<const ValueId> indices, std::string &rowText,
                                   std::optional<uint64_t> &constRow) {
                uint64_t folded = 0;
                bool allConstant = !indices.empty();
                for (std::size_t j = 0; j < indices.size() && allConstant; ++j)
                {
                    const auto constant = scalarConstantValue(indices[j]);
                    // Fold overflow stays dynamic: the emitted stride sum wraps
                    // exactly like the runtime size_t evaluation would.
                    if (!constant || *constant > (std::numeric_limits<uint64_t>::max() - folded) / strides[j])
                        allConstant = false;
                    else
                        folded += *constant * strides[j];
                }
                if (allConstant)
                {
                    rowText = std::to_string(folded);
                    if (folded < rowCount) constRow = folded;
                    else return false; // statically out of bounds: dead write
                    return true;
                }
                if (indices.size() == 1)
                {
                    rowText = read(indices[0]);
                    // A k=1 index into a rank>=2 state addresses a row block:
                    // scale by the outer-dimension stride (rank 1 keeps the
                    // legacy raw-index emission shape).
                    if (strides[0] != 1)
                        rowText = "static_cast<std::size_t>(" + rowText + ")*" +
                                  std::to_string(strides[0]) + "u";
                    return true;
                }
                std::vector<std::string> terms;
                terms.reserve(indices.size());
                for (const auto index : indices)
                    terms.push_back("static_cast<std::size_t>(" + read(index) + ")");
                rowText = linearIndexText(array.shape, array.shape.size(), terms);
                return true;
            };
            if (guard != "true" && !actHoisted) out << "if(" << guard << "){\n";
            if (name == "core.state.writeArray" || name == "core.state.writeArrayMask")
            {
                // [en, i0..ik-1, data, (mask)]: k == rank writes one element row
                // (the legacy single-row shape); k < rank writes the
                // contiguous sub-array row block [row, row + prod(shape[k:])).
                const auto layout = cpuIndexedWriteLayout(model_, op, array);
                if (!layout.valid) throw std::runtime_error("CPU array write has invalid arity or geometry");
                const bool masked = layout.masked;
                const std::size_t indexCount = layout.indexCount;
                const std::string dataText = read(operands[1 + indexCount]);
                const std::string maskText = masked ? read(operands[2 + indexCount]) : std::string();
                std::string rowText; std::optional<uint64_t> constRow;
                if (!rowOf(operands.subspan(1, indexCount), rowText, constRow))
                    out << "// cpu_mem_write_dead row=" << rowText << "\n";
                else if (indexCount == array.shape.size())
                {
                    const bool needGuard = !enableHoisted || !constRow;
                    if (needGuard)
                    {
                        out << "if(";
                        if (!enableHoisted) out << memEnableGuard(operands[0]);
                        if (!constRow)
                        {
                            if (!enableHoisted) out << "&&";
                            out << "static_cast<std::size_t>(" << rowText << ")<" << rowCount;
                        }
                        out << "){\n";
                    }
                    writeCell(rowText, constRow, dataText, masked ? &maskText : nullptr);
                    if (needGuard) out << "}\n";
                }
                else
                {
                    const auto blockRows =
                        shapeElementCount(std::span<const int64_t>(array.shape).subspan(indexCount));
                    if (!blockRows)
                        throw std::runtime_error("CPU array write sub-array shape is invalid");
                    if (constRow && *constRow + *blockRows > rowCount)
                        out << "// cpu_mem_write_dead row=" << rowText << "\n";
                    else
                    {
                        // Sub-array write: per-row slices of the packed
                        // data/mask over the block. A dynamic base writes the
                        // block only when wholly in bounds — a partial block
                        // drops whole (conservative runtime protection; index
                        // legality is the verifier's contract).
                        const auto accessWidth = static_cast<uint32_t>(element.width * *blockRows);
                        const std::string dataSlice = rowSliceText(dataText, "cpu_j", element.width, accessWidth);
                        const std::string maskSlice = masked ?
                            rowSliceText(maskText, "cpu_j", element.width, accessWidth) : std::string();
                        const bool needGuard = !enableHoisted || !constRow;
                        if (needGuard)
                        {
                            out << "if(";
                            if (!enableHoisted) out << memEnableGuard(operands[0]);
                            if (!constRow)
                            {
                                if (!enableHoisted) out << "&&";
                                out << "static_cast<std::size_t>(" << rowText << ")+" << *blockRows << "<=" << rowCount;
                            }
                            out << "){\n";
                        }
                        out << "for(std::size_t cpu_j=0;cpu_j<" << *blockRows << ";++cpu_j){\n";
                        writeCell(rowText + "+cpu_j", std::nullopt, dataSlice,
                                  masked ? &maskSlice : nullptr);
                        out << "}\n";
                        if (needGuard) out << "}\n";
                    }
                }
            }
            else if (name == "core.state.write" || name == "core.state.writeMask" ||
                     name == "core.state.latchWrite")
            {
                // Whole-array write (stage-06: absorbs the retired whole-array
                // assign): data and mask are full-width flat values, sliced
                // per row. A constant all-ones mask folds to plain data
                // stores.
                const bool masked = name == "core.state.writeMask" ||
                                    name == "core.state.latchWrite" || operands.size() == 3;
                if (operands.size() != (masked ? 3u : 2u))
                    throw std::runtime_error("CPU state write has invalid arity");
                const std::string data = read(operands[1]);
                const bool fullMask = !masked || isAllOnesConstant(operands[2], array.width);
                const std::string maskText = fullMask ? std::string() : read(operands[2]);
                const std::string dataSlice = rowSliceText(data, "cpu_row", element.width, array.width);
                const std::string maskSlice =
                    fullMask ? std::string() : rowSliceText(maskText, "cpu_row", element.width, array.width);
                if (!enableHoisted) out << "if(" << memEnableGuard(operands[0]) << "){\n";
                out << "for(std::size_t cpu_row=0;cpu_row<" << rowCount << ";++cpu_row){\n";
                writeCell("cpu_row", std::nullopt, dataSlice, fullMask ? nullptr : &maskSlice);
                out << "}\n";
                if (!enableHoisted) out << "}\n";
            }
            else if (name == "core.state.fill")
            {
                if (operands.size() != 2) throw std::runtime_error("CPU memory fill has invalid arity");
                // Broadcast (element-width data) or packed (full-width data,
                // sliced per row) fill — the legacy isPackedFill semantics.
                const std::string data = read(operands[1]);
                const auto &dataType = type(operands[1]);
                const bool packedFill = dataType.kind == TypeKind::Logic &&
                                        dataType.width == array.width && array.width != element.width;
                if (!enableHoisted) out << "if(" << memEnableGuard(operands[0]) << "){\n";
                out << "for(std::size_t cpu_row=0;cpu_row<" << rowCount << ";++cpu_row){\n";
                if (!packedFill)
                    writeCell("cpu_row", std::nullopt, data, nullptr);
                else
                    writeCell("cpu_row", std::nullopt, rowSliceText(data, "cpu_row", element.width, array.width),
                              nullptr);
                out << "}\n";
                if (!enableHoisted) out << "}\n";
            }
            else throw std::runtime_error("CPU six-phase emit unsupported mem write op: " + std::string(name));
            if (guard != "true" && !actHoisted) out << "}\n";
        }
        // General-phase regLatch-class array writes (M5d-6, stage-06 op
        // family write/writeArray/fill on a shaped state): the write lives
        // inside its General supernode and commits NBA-style into
        // smallSinkStoreNext. The merge base is the NEXT row (read-modify-write
        // across same-round writers, in op order), while the fanout compare
        // runs against the currently visible smallSinkStore row so stateFanout
        // readers fire (next round, after P_publish) only on a true change —
        // the same activation contract as emitRegWrite, per row.
        void SixPhaseEmitter::emitGeneralMemWrite(std::ostream &out, const SimOp &op, uint32_t current) const
        {
            const auto name = model_.text(op.opType);
            const auto operands = model_.operands(op);
            const auto refs = model_.objectRefs(op);
            if (refs.empty() || refs[0].kind != ObjectKind::State)
                throw std::runtime_error("CPU six-phase emit general mem write requires a state target");
            const StateId target{refs[0].index, 0};
            const auto &array = stateType(target);
            if (array.kind != TypeKind::Logic || array.shape.empty())
                throw std::runtime_error("CPU six-phase emit general mem write target is not an array");
            const Type element = arrayElementType(array);
            const auto rowCountOpt = shapeElementCount(array.shape);
            if (!rowCountOpt)
                throw std::runtime_error("CPU six-phase emit general mem write target has an invalid shape");
            const uint64_t rowCount = *rowCountOpt;
            const auto *field = target.index < regFieldByState_.size() ? regFieldByState_[target.index] : nullptr;
            if (!field)
                throw std::runtime_error("CPU six-phase emit general mem write target is not a "
                                         "regLatch-class state");
            const std::string nextArray = "smallSinkStoreNext." + std::string(model_.text(field->name));
            const std::string curArray = "smallSinkStore." + std::string(model_.text(field->name));
            const std::string guard = bodyGuard(op, current);
            // Row-major strides over the state shape (element-row units).
            std::vector<uint64_t> strides(array.shape.size(), 1);
            for (std::size_t d = array.shape.size(); d-- > 1;)
                strides[d - 1] = strides[d] * static_cast<uint64_t>(array.shape[d]);
            // State fanout always queues into nonSinkActiveFlagNext.
            const auto activateState = [&] {
                flagOutputDirty(out, outputReadStates_.contains(refs[0].index));
                if (!smallFinalChange_.contains(target.index))
                    activateOrdinals(out, stateFanout_[refs[0].index], model_.text(activeStore_->fields[1].name));
                out << "GRHSIM_PERF_COUNT(touchedStateShadowCount);\n";
            };
            const auto writeCell = [&](const std::string &rowText, std::optional<uint64_t> constRow,
                                       const std::string &data, const std::string *mask) {
                (void)constRow;
                const std::string nextCell = rowAccessText(nextArray, array.shape, rowText);
                const std::string curCell = rowAccessText(curArray, array.shape, rowText);
                if (isScalarLogic(element))
                {
                    const std::string merged = mask
                        ? normalize("(static_cast<std::uint64_t>(" + nextCell + ")&~static_cast<std::uint64_t>(" + *mask +
                                    "))|(static_cast<std::uint64_t>(" + data + ")&static_cast<std::uint64_t>(" + *mask + "))",
                                    element)
                        : normalize(data, element);
                    out << "{const auto cpu_merged=" << merged << ";if(" << nextCell << "!=cpu_merged){if("
                        << curCell << "!=cpu_merged){\n";
                    activateState();
                    out << "}" << nextCell << "=cpu_merged;}}\n";
                    return;
                }
                if (element.kind == TypeKind::Logic && element.domain == LogicDomain::TwoState)
                {
                    // grhsim_merge_words_masked natively (see emitRegWrite).
                    if (mask)
                        out << "{const auto cpu_merged=" << normalize("(" + nextCell + "&~(" + *mask + "))|(" + data +
                                                                       "&(" + *mask + "))",
                                                                       element)
                            << ";if(" << nextCell << "!=cpu_merged){if(" << curCell
                            << "!=cpu_merged){\n";
                    else
                        out << "{if(" << nextCell << "!=" << data << "){if(" << curCell << "!=" << data << "){\n";
                    activateState();
                    if (mask) out << "}" << nextCell << "=cpu_merged;}}\n";
                    else out << "}" << nextCell << '=' << data << ";}}\n";
                    return;
                }
                throw std::runtime_error("CPU six-phase emit general mem write element type is not two-state logic");
            };
            // Same linearization contract as the P_mem path (see emitMemWrite):
            // all-constant indices fold to a static row; a single dynamic
            // index keeps the raw-text emission shape.
            const auto rowOf = [&](std::span<const ValueId> indices, std::string &rowText,
                                   std::optional<uint64_t> &constRow) {
                uint64_t folded = 0;
                bool allConstant = !indices.empty();
                for (std::size_t j = 0; j < indices.size() && allConstant; ++j)
                {
                    const auto constant = scalarConstantValue(indices[j]);
                    if (!constant || *constant > (std::numeric_limits<uint64_t>::max() - folded) / strides[j])
                        allConstant = false;
                    else
                        folded += *constant * strides[j];
                }
                if (allConstant)
                {
                    rowText = std::to_string(folded);
                    if (folded < rowCount) constRow = folded;
                    else return false; // statically out of bounds: dead write
                    return true;
                }
                if (indices.size() == 1)
                {
                    rowText = read(indices[0]);
                    // A k=1 index into a rank>=2 state addresses a row block:
                    // scale by the outer-dimension stride (rank 1 keeps the
                    // legacy raw-index emission shape).
                    if (strides[0] != 1)
                        rowText = "static_cast<std::size_t>(" + rowText + ")*" +
                                  std::to_string(strides[0]) + "u";
                    return true;
                }
                std::vector<std::string> terms;
                terms.reserve(indices.size());
                for (const auto index : indices)
                    terms.push_back("static_cast<std::size_t>(" + read(index) + ")");
                rowText = linearIndexText(array.shape, array.shape.size(), terms);
                return true;
            };
            if (guard != "true") out << "if(" << guard << "){\n";
            if (name == "core.state.writeArray" || name == "core.state.writeArrayMask")
            {
                // [en, i0..ik-1, data, (mask)]: k == rank writes one element
                // row; k < rank RMWs the contiguous sub-array row block.
                const auto layout = cpuIndexedWriteLayout(model_, op, array);
                if (!layout.valid) throw std::runtime_error("CPU array write has invalid arity or geometry");
                const bool masked = layout.masked;
                const std::size_t indexCount = layout.indexCount;
                const std::string dataText = read(operands[1 + indexCount]);
                const std::string maskText = masked ? read(operands[2 + indexCount]) : std::string();
                std::string rowText; std::optional<uint64_t> constRow;
                if (!rowOf(operands.subspan(1, indexCount), rowText, constRow))
                    out << "// cpu_mem_write_dead row=" << rowText << "\n";
                else if (indexCount == array.shape.size())
                {
                    out << "if(" << readAs(operands[0], CpuBoundaryUse::SmallSinkEnable);
                    if (!constRow) out << "&&static_cast<std::size_t>(" << rowText << ")<" << rowCount;
                    out << "){\n";
                    writeCell(rowText, constRow, dataText, masked ? &maskText : nullptr);
                    out << "}\n";
                }
                else
                {
                    const auto blockRows =
                        shapeElementCount(std::span<const int64_t>(array.shape).subspan(indexCount));
                    if (!blockRows)
                        throw std::runtime_error("CPU array write sub-array shape is invalid");
                    if (constRow && *constRow + *blockRows > rowCount)
                        out << "// cpu_mem_write_dead row=" << rowText << "\n";
                    else
                    {
                        // Sub-array write: per-row slices of the packed
                        // data/mask over the block. A dynamic base applies the
                        // block only when wholly in bounds (a partial block
                        // drops whole — runtime protection; index legality is
                        // the verifier's contract).
                        const auto accessWidth = static_cast<uint32_t>(element.width * *blockRows);
                        const std::string dataSlice = rowSliceText(dataText, "cpu_j", element.width, accessWidth);
                        const std::string maskSlice = masked ?
                            rowSliceText(maskText, "cpu_j", element.width, accessWidth) : std::string();
                        out << "if(" << readAs(operands[0], CpuBoundaryUse::SmallSinkEnable);
                        if (!constRow)
                            out << "&&static_cast<std::size_t>(" << rowText << ")+" << *blockRows << "<=" << rowCount;
                        out << "){\n";
                        out << "for(std::size_t cpu_j=0;cpu_j<" << *blockRows << ";++cpu_j){\n";
                        writeCell(rowText + "+cpu_j", std::nullopt, dataSlice,
                                  masked ? &maskSlice : nullptr);
                        out << "}\n}\n";
                    }
                }
            }
            else if (name == "core.state.write" || name == "core.state.writeMask")
            {
                // Whole-array write (stage-06): full-width flat data/mask,
                // sliced per row; a constant all-ones mask folds to plain
                // per-row stores.
                const bool masked = name == "core.state.writeMask" || operands.size() == 3;
                if (operands.size() != (masked ? 3u : 2u))
                    throw std::runtime_error("CPU state write has invalid arity");
                const std::string data = read(operands[1]);
                const bool fullMask = !masked || isAllOnesConstant(operands[2], array.width);
                const std::string maskText = fullMask ? std::string() : read(operands[2]);
                const std::string dataSlice = rowSliceText(data, "cpu_row", element.width, array.width);
                const std::string maskSlice =
                    fullMask ? std::string() : rowSliceText(maskText, "cpu_row", element.width, array.width);
                out << "if(" << readAs(operands[0], CpuBoundaryUse::SmallSinkEnable) << "){\n"
                    << "for(std::size_t cpu_row=0;cpu_row<" << rowCount << ";++cpu_row){\n";
                writeCell("cpu_row", std::nullopt, dataSlice, fullMask ? nullptr : &maskSlice);
                out << "}\n}\n";
            }
            else if (name == "core.state.fill")
            {
                if (operands.size() != 2) throw std::runtime_error("CPU memory fill has invalid arity");
                // Broadcast (element-width data) or packed (full-width data,
                // sliced per row) fill.
                const std::string data = read(operands[1]);
                const auto &dataType = type(operands[1]);
                const bool packedFill = dataType.kind == TypeKind::Logic &&
                                        dataType.width == array.width && array.width != element.width;
                out << "if(" << readAs(operands[0], CpuBoundaryUse::SmallSinkEnable) << "){\n"
                    << "for(std::size_t cpu_row=0;cpu_row<" << rowCount << ";++cpu_row){\n";
                if (!packedFill)
                    writeCell("cpu_row", std::nullopt, data, nullptr);
                else
                    writeCell("cpu_row", std::nullopt, rowSliceText(data, "cpu_row", element.width, array.width),
                              nullptr);
                out << "}\n}\n";
            }
            else throw std::runtime_error("CPU six-phase emit unsupported general mem write op: " + std::string(name));
            if (guard != "true") out << "}\n";
        }
        void SixPhaseEmitter::emitSystemTask(std::ostream &out, const SimOp &op, uint32_t current) const
        {
            const auto operands = model_.operands(op);
            if (operands.empty()) throw std::runtime_error("CPU system task requires a call condition operand");
            // Event guard from the act bits (P_event owns edge detection);
            // event-free tasks run whenever their supernode fires. Sink
            // bodies drop it (bodyGuard): the call-site gate already covers
            // the signature.
            std::string cond = callCondition(operands[0]);
            const std::string guard = bodyGuard(op, current);
            if (guard != "true") cond += "&&(" + guard + ")";
            cond += sideCallExtras(op);
            out << "if(" << cond << "){\n";
            systemTaskBody(out, op);
            out << "}\n";
        }
        void SixPhaseEmitter::emitOutputTask(std::ostream &out, const SimOp &op) const
        {
            const auto operands = model_.operands(op);
            if (operands.empty()) throw std::runtime_error("CPU timeslot task requires a call condition operand");
            // Event-driven timeslot task: the eval-sticky flag (set by P_event)
            // is consumed and cleared here; the callCond clone (P_output) still
            // gates the report. Event-free tasks carry their history-change
            // guard in the cloned cone (migrate-timeslot-tasks).
            if (const auto *flag = parameter<int64_t>(model_, model_.parameters(op), "timeslotFlag"))
            {
                if (*flag < 0) throw std::runtime_error("CPU six-phase emit negative timeslot flag");
                out << "if(timeslotTriggerFlag[" << *flag << "]){timeslotTriggerFlag[" << *flag << "]=0;\n";
                out << "if(" << callCondition(operands[0]) << "){\n";
                systemTaskBody(out, op);
                out << "}\n}\n";
                return;
            }
            out << "if(" << callCondition(operands[0]) << "){\n";
            systemTaskBody(out, op);
            out << "}\n";
        }
        std::string SixPhaseEmitter::sideCallExtras(const SimOp &op) const
        {
            const auto params = model_.parameters(op);
            const auto *proc = parameter<std::string>(model_, params, "proc_kind");
            const auto *timed = parameter<bool>(model_, params, "has_timing");
            std::string extras;
            if (proc && *proc == "initial" && (!timed || !*timed)) extras += "&&cpu_first_eval";
            if (const auto once = onceTasks_.find(op.id.index); once != onceTasks_.end())
                extras += "&&!cpu_system_done[" + std::to_string(once->second) + ']';
            return extras;
        }
        void SixPhaseEmitter::systemTaskBody(std::ostream &out, const SimOp &op) const
        {
            const auto params = model_.parameters(op);
            const auto *nameParam = parameter<std::string>(model_, params, "name");
            if (!nameParam) throw std::runtime_error("CPU system task requires a name parameter");
            const auto operands = model_.operands(op);
            const auto args = operands.subspan(1);
            out << "const std::array<grhsim_task_arg," << args.size() << "> cpu_args{{";
            for (std::size_t i = 0; i < args.size(); ++i)
            {
                if (i) out << ',';
                const auto &argType = type(args[i]);
                if (argType.kind == TypeKind::Logic && argType.domain == LogicDomain::TwoState && argType.width > 64)
                {
                    // Wide values are _BitInt objects: the scalar overload
                    // would silently truncate to u64, so unpack words here.
                    out << "grhsim_make_task_arg_wide(" << read(args[i]);
                    out << ',' << argType.width << ',' << (argType.isSigned ? "true" : "false");
                }
                else
                {
                    out << "grhsim_make_task_arg(" << read(args[i]);
                    if (argType.kind == TypeKind::Logic)
                        out << ',' << argType.width << ',' << (argType.isSigned ? "true" : "false");
                }
                out << ')';
            }
            out << "}};cpu_system_task(\"";
            for (const char ch : *nameParam)
            {
                if (ch == '"' || ch == '\\') out << '\\';
                out << ch;
            }
            out << "\",cpu_args);\n";
            if (const auto once = onceTasks_.find(op.id.index); once != onceTasks_.end())
                out << "cpu_system_done[" << once->second << "]=true;\n";
        }
        void SixPhaseEmitter::emitDpiCall(std::ostream &out, const SimOp &op, uint32_t current) const
        {
            const auto &function = model_.functions()[model_.objectRefs(op)[0].index - 1];
            const auto arguments = model_.arguments(function);
            const auto operands = model_.operands(op), results = model_.results(op);
            std::size_t inputCount = 0, outputCount = 0;
            for (const auto &arg : arguments)
            {
                inputCount += arg.direction == DpiDirection::Input;
                outputCount += arg.direction == DpiDirection::Output;
            }
            std::size_t input = 1, inoutInput = 1 + inputCount, output = function.returnType ? 1 : 0;
            std::size_t inoutOutput = output + outputCount;
            // Result locals are declared before the guard so same-supernode
            // consumers can read them; a guard-missed call leaves them zeroed
            // (the IR gives no defined value to a consumer of a skipped call).
            // Frame-spilled results (M5d-7) skip the declaration: the driver
            // value-initializes the frame field.
            for (const auto result : results)
                if (!boundaryByValue_[result.index] && !crossingLocals_[result.index])
                {
                    out << cppType(type(result)) << " cpu_v" << result.index << "{};\n";
                    activeLocals_[result.index] = 1;
                }
            std::vector<std::pair<ValueId, std::string>> produced;
            std::string cond = callCondition(operands[0]);
            const std::string guard = bodyGuard(op, current);
            if (guard != "true") cond += "&&(" + guard + ")";
            cond += sideCallExtras(op);
            out << "if(" << cond << "){\n";
            std::string call = "::" + identifier(model_.text(function.symbol)) + "(";
            for (std::size_t i = 0; i < arguments.size(); ++i)
            {
                if (i) call += ',';
                const auto &arg = arguments[i]; const auto &target = model_.types()[arg.type.index - 1];
                if (arg.direction == DpiDirection::Input)
                {
                    const auto source = read(operands[input++]);
                    call += target.kind == TypeKind::String ? "(" + source + ").c_str()" : normalize(source, target);
                }
                else
                {
                    const auto temporary = "cpu_dpi_arg_" + std::to_string(i);
                    out << dpiType(arg.type) << ' ' << temporary;
                    if (arg.direction == DpiDirection::Inout) out << '=' << normalize(read(operands[inoutInput++]), target);
                    else out << "{}";
                    out << ";\n"; call += '&' + temporary;
                    produced.push_back({results[arg.direction == DpiDirection::Output ? output++ : inoutOutput++], temporary});
                }
            }
            call += ')';
            if (function.returnType)
            {
                out << "auto cpu_dpi_return=" << call << ";\n";
                publishDpiResult(out, results[0], "cpu_dpi_return", current);
            }
            else out << call << ";\n";
            for (const auto &[result, temporary] : produced) publishDpiResult(out, result, temporary, current);
            out << "}\n";
        }
        void SixPhaseEmitter::publishDpiResult(std::ostream &out, ValueId result, const std::string &temporary,
                                               uint32_t current) const
        {
            const auto &target = type(result);
            out << "{\n";
            auto source = temporary;
            if (target.kind == TypeKind::Logic && target.width > 64)
                // Mask the C side's result back to the semantic width (was
                // grhsim_trunc_words on the word array).
                out << temporary << "&=" << wideMask(target.width) << ";\n";
            else if (target.kind == TypeKind::Logic)
            {
                source = "cpu_dpi_normalized";
                out << "auto " << source << '=' << normalize(temporary, target) << ";\n";
            }
            if (boundaryByValue_[result.index]) publishBoundary(out, result, source, current);
            else out << localRef(result) << "=std::move(" << source << ");\n";
            out << "}\n";
        }
        std::string SixPhaseEmitter::dpiType(TypeId id) const
        {
            if (!id) return "void";
            const auto &type = model_.types()[id.index - 1];
            if (!type.shape.empty())
                throw std::runtime_error("CPU DPI aggregate parameter/return ABI is not implemented (P1)");
            if (type.kind == TypeKind::Logic && type.width == 1) return "std::uint8_t";
            return cppType(type);
        }
        std::string SixPhaseEmitter::dpiDeclaration(const ExternFunction &function) const
        {
            if (model_.text(function.declRef) != "core.dpi")
                throw std::runtime_error("CPU external function is not a DPI declaration");
            std::string result = "extern \"C\" " + dpiType(function.returnType) + " " + identifier(model_.text(function.symbol)) + "(";
            bool first = true;
            for (const auto &arg : model_.arguments(function))
            {
                if (!first) result += ',';
                first = false;
                const auto &type = model_.types()[arg.type.index - 1];
                const auto base = dpiType(arg.type);
                if (arg.direction != DpiDirection::Input) result += base + "*";
                else if (type.kind == TypeKind::String) result += "const char*";
                else if (type.kind == TypeKind::Logic && type.width > 64) result += "const " + base + "&";
                else result += base;
            }
            return result + ");";
        }
        void SixPhaseEmitter::emitOutputWrite(std::ostream &out, const SimOp &op) const
        {
            const auto operands = model_.operands(op);
            const auto refs = model_.objectRefs(op);
            if (operands.size() != 1 || refs.size() != 1 || refs[0].kind != ObjectKind::Output)
                throw std::runtime_error("CPU six-phase emit malformed output write");
            const auto &output = model_.outputs()[refs[0].index - 1];
            const auto member = "this->" + identifier(model_.text(output.name));
            const auto &outputType = model_.types()[output.type.index - 1];
            if (outputType.shape.empty())
            {
                out << member << '=' << read(operands[0]) << ";\n";
                return;
            }
            const auto rows = shapeElementCount(outputType.shape);
            const auto elementWidth = typeElementWidth(outputType);
            if (!rows || !elementWidth)
                throw std::runtime_error("CPU six-phase emit shaped output port has an invalid shape");
            const Type element = arrayElementType(outputType);
            out << "{const auto cpu_packed=" << read(operands[0]) << ";\n"
                << "for(std::size_t cpu_j=0;cpu_j<" << *rows << ";++cpu_j)"
                << rowAccessText(member, outputType.shape, "cpu_j") << '='
                << normalize(rowSliceText("cpu_packed", "cpu_j", *elementWidth,
                                          outputType.width), element) << ";}\n";
        }
        void SixPhaseEmitter::commitStagedOutputWrites(std::ostream &out) const
        {
            // Timeslot history write-backs commit after every P_output op ran, so
            // the in-body ne comparisons all observed the previous eval's values.
            // P_publish already equalized smallSinkStore/smallSinkStoreNext, so the
            // commit lands in both to keep the round-start invariant.
            for (const auto &staged : stagedOutputWrites_)
            {
                const auto &target = stateType(staged.field->state);
                const std::string name(model_.text(staged.field->name));
                const bool small = staged.field->state.index < regFieldByState_.size() &&
                                   regFieldByState_[staged.field->state.index];
                const std::string slot = std::string(small ? "smallSinkStore." : "largeSinkStore.") + name;
                const std::string syncNext = small ? "smallSinkStoreNext." + name + "=cpu_merged;\n"
                                                   : std::string();
                out << "if(" << staged.enable << "){\n";
                if (isScalarLogic(target))
                {
                    out << "{const auto cpu_merged=" << normalize("(static_cast<std::uint64_t>(" + slot +
                            ")&~static_cast<std::uint64_t>(" + staged.mask + "))|(static_cast<std::uint64_t>(" + staged.data +
                            ")&static_cast<std::uint64_t>(" + staged.mask + "))", target) << ";\n"
                        << "if(" << slot << "!=cpu_merged){GRHSIM_PERF_COUNT(touchedStateShadowCount);\n"
                        << slot << "=cpu_merged;\n" << syncNext << "}}\n";
                }
                else if (target.kind == TypeKind::Logic && target.domain == LogicDomain::TwoState)
                {
                    out << "{const auto cpu_merged=" << normalize("(" + slot + "&~(" + staged.mask + "))|(" +
                                                                      staged.data + "&(" + staged.mask + "))", target) << ";\n"
                        << "if(" << slot << "!=cpu_merged){GRHSIM_PERF_COUNT(touchedStateShadowCount);\n"
                        << slot << "=cpu_merged;\n" << syncNext << "}}\n";
                }
                else throw std::runtime_error("CPU six-phase emit output latchWrite target type is not two-state logic");
                out << "}\n";
            }
        }

        // ----- Validation -----
        void SixPhaseEmitter::validate() const
        {
            for (const auto &type : model_.types())
                if (type.kind == TypeKind::Logic && type.domain != LogicDomain::TwoState)
                    throw std::runtime_error(type.shape.empty()
                        ? "CPU six-phase emit requires two-state logic types"
                        : "CPU six-phase emit does not support four-state aggregate types");
            for (const auto &state : model_.states())
                if (containsString(model_.types()[state.type.index - 1]))
                    throw std::runtime_error("CPU six-phase emit string state initialization and commit are not implemented");
            std::set<std::string> names{class_, "init", "eval", "dumpState", "set_runtime_profile_enabled",
                "dump_runtime_profile", "configure_waveform", "perf_counters", "PerfCounters", "pInput", "pEvent",
                "pGeneral", "pLargeSink", "pPublish", "pOutput",
                "smallSinkStore", "smallSinkStoreNext", "largeSinkStore", "inputStore", "nonSinkBoundaryStore",
                "smallSinkEnableBoundaryStore", "smallSinkPayloadBoundaryStore", "largeSinkEnableBoundaryStore",
                "largeSinkPayloadBoundaryStore", "prevEventStore", "eventActStore", "timeslotTriggerFlag",
                "cpu_rng", "perf_", "SmallSinkStore", "LargeSinkStore", "InputStore",
                "NonSinkBoundaryStore", "SmallSinkEnableBoundaryStore", "SmallSinkPayloadBoundaryStore",
                "LargeSinkEnableBoundaryStore", "LargeSinkPayloadBoundaryStore", "PrevEventStore", "boundaryStrings"};
            // M5d-7 chunk members and spill frame types share the class scope;
            // register them with duplicate detection so a pathological op name
            // (e.g. one that makes sn_12_c0 collide with a chunk name) fails
            // validation instead of silently breaking the generated code.
            const auto claim = [&names](const std::string &name) {
                if (!names.insert(name).second)
                    throw std::runtime_error("CPU generated member name collides: " + name);
            };
            for (const auto &[state, readers] : largeFinalChange_)
                claim("cpu_largeBefore" + std::to_string(state));
            for (uint32_t i = 0; i < initChunks_.size(); ++i) claim("cpu_init_" + std::to_string(i));
            for (uint32_t i = 0; i < eventChunks_.size(); ++i) claim("pEvent_c" + std::to_string(i));
            for (uint32_t i = 0; i < scanChunks_.size(); ++i) claim("pGeneral_c" + std::to_string(i));
            for (uint32_t i = 0; i < memChunks_.size(); ++i) claim("pLargeSink_c" + std::to_string(i));
            for (uint32_t i = 0; i < outputChunks_.size(); ++i) claim("pOutput_c" + std::to_string(i));
            for (uint32_t i = 0; i < dumpChunks_.size(); ++i) claim("cpu_dump_" + std::to_string(i));
            if (!eventFrame_.empty()) claim("EventFrame");
            if (!outputFrame_.empty()) claim("OutputFrame");
            if (hasSystemTasks_)
                for (const auto *name : {"cpu_first_eval", "cpu_system_done", "cpu_system_task"}) names.insert(name);
            for (const auto &field : activeStore_->fields) names.insert(std::string(model_.text(field.name)));
            for (uint32_t ordinal = 0; ordinal < supernodeCount_; ++ordinal)
            {
                claim(supernodeName(ordinal));
                const auto &attrs = tree_.partitions[order_[ordinal].index - 1].attrs;
                if (attrs.helperChunks.size() < 2) continue;
                if (!supernodeFrames_[ordinal].empty()) claim("SnFrame" + std::to_string(ordinal));
                for (std::size_t i = 0; i < attrs.helperChunks.size(); ++i)
                    claim(supernodeName(ordinal) + "__c" + std::to_string(i));
            }
            const auto checkPortType = [&](TypeId id) {
                const auto &type = model_.types()[id.index - 1];
                if (type.kind != TypeKind::Logic)
                    throw std::runtime_error("CPU six-phase emit ports must be two-state logic values");
            };
            for (const auto &input : model_.inputs())
            {
                checkPortType(input.type);
                validatePort(model_.text(input.name), names);
            }
            for (const auto &output : model_.outputs())
            {
                checkPortType(output.type);
                validatePort(model_.text(output.name), names);
            }
            std::map<std::string, std::string> declarations;
            for (const auto &function : model_.functions())
            {
                const auto symbol = identifier(model_.text(function.symbol));
                const auto declaration = dpiDeclaration(function);
                const auto [it, inserted] = declarations.emplace(symbol, declaration);
                if (!inserted && it->second != declaration)
                    throw std::runtime_error("CPU DPI imports have conflicting C signatures: " + symbol);
            }
            validateInit();
            for (const auto &op : model_.operations())
            {
                const auto opName = model_.text(op.opType);
                if (opName == "core.system.task") validateSystemTask(op);
                else if (opName == "core.system.function") validateSystemFunction(op);
                else if (opName == "core.dpi.call") validateDpiCall(op);
            }
            // Dry-run every body generator so write() cannot fail mid-file.
            struct DiscardBuffer : std::streambuf
            {
                std::streamsize xsputn(const char *, std::streamsize count) override { return count; }
                int_type overflow(int_type ch) override { return traits_type::not_eof(ch); }
            } buffer;
            std::ostream discard(&buffer);
            header(discard);
            for (const auto &unit : tuPlan_.units) unitCpp(discard, unit);
        }
        void SixPhaseEmitter::validatePort(std::string_view name, std::set<std::string> &names) const
        {
            const auto cpp = identifier(name);
            if (!names.insert(cpp).second)
                throw std::runtime_error("CPU port name collides with generated member: " + cpp);
        }
        void SixPhaseEmitter::validateSystemTask(const SimOp &op) const
        {
            if (!model_.results(op).empty()) throw std::runtime_error("CPU system task must not produce values");
            const auto params = model_.parameters(op);
            const auto *name = parameter<std::string>(model_, params, "name");
            static const std::set<std::string_view> supported{
                "display", "write", "strobe", "monitor", "fdisplay", "fwrite", "info", "warning", "error", "fatal", "finish", "stop"};
            if (!name || !supported.contains(*name))
                throw std::runtime_error("CPU system task is not implemented: " + (name ? *name : std::string("<missing>")));
            const auto *proc = parameter<std::string>(model_, params, "proc_kind");
            if (proc && *proc == "final")
                throw std::runtime_error("CPU final-process task execution is not implemented");
            for (auto operand : model_.operands(op).subspan(1))
                // stage-07: a shaped logic argument is read as its flat
                // total-width integer (packed-first doctrine; SV prints a
                // packed array as one flat value), only non-logic shaped
                // arguments remain unsupported.
                if (!type(operand).shape.empty() && type(operand).kind != TypeKind::Logic)
                    throw std::runtime_error("CPU system task array arguments are not implemented");
        }
        void SixPhaseEmitter::validateSystemFunction(const SimOp &op) const
        {
            const auto *name = parameter<std::string>(model_, model_.parameters(op), "name");
            const auto results = model_.results(op);
            if (!name || *name != "random" || !model_.operands(op).empty() || results.size() != 1 ||
                !model_.objectRefs(op).empty() || !isScalarLogic(type(results[0])) ||
                type(results[0]).width > 32)
                throw std::runtime_error("CPU system function is not implemented: " +
                                         (name ? *name : std::string("<missing>")));
        }
        void SixPhaseEmitter::validateDpiCall(const SimOp &op) const
        {
            const auto refs = model_.objectRefs(op);
            if (refs.size() != 1 || refs[0].kind != ObjectKind::Function)
                throw std::runtime_error("CPU DPI call requires one function reference");
            const auto &function = model_.functions()[refs[0].index - 1];
            std::vector<TypeId> inputs, outputs;
            if (function.returnType) outputs.push_back(function.returnType);
            for (const auto &arg : model_.arguments(function))
                if (arg.direction == DpiDirection::Input) inputs.push_back(arg.type);
                else if (arg.direction == DpiDirection::Output) outputs.push_back(arg.type);
            for (const auto &arg : model_.arguments(function))
                if (arg.direction == DpiDirection::Inout) { inputs.push_back(arg.type); outputs.push_back(arg.type); }
            const auto operands = model_.operands(op), results = model_.results(op);
            if (operands.size() != 1 + inputs.size() || results.size() != outputs.size())
                throw std::runtime_error("CPU DPI call arity disagrees with its signature");
            const auto checkType = [&](ValueId value, TypeId expected) {
                const auto &source = type(value), &target = model_.types()[expected.index - 1];
                if (source.kind != target.kind || source.width != target.width || source.domain != target.domain ||
                    source.shape != target.shape)
                    throw std::runtime_error("CPU DPI call value type disagrees with its signature");
            };
            for (std::size_t i = 0; i < inputs.size(); ++i) checkType(operands[i + 1], inputs[i]);
            for (std::size_t i = 0; i < outputs.size(); ++i) checkType(results[i], outputs[i]);
        }

        void SixPhaseEmitter::validateInit() const
        {
            for (const auto &record : model_.initRecords())
            {
                const auto &target = stateType(record.state);
                const bool array = !target.shape.empty();
                if (!array && model_.steps(record).size() != 1)
                    throw std::runtime_error("CPU scalar state requires exactly one full initializer");
                if (!array) continue;
                // Row intervals are linear row numbers over the flat
                // (row-major) shape.
                const auto rowCount = shapeElementCount(target.shape);
                if (!rowCount)
                    throw std::runtime_error("CPU six-phase emit array state has an invalid shape");
                const Type element = arrayElementType(target);
                std::vector<std::pair<uint64_t, uint64_t>> covered;
                for (const auto &step : model_.steps(record))
                {
                    const auto kind = model_.text(step.kind);
                    const auto params = model_.parameters(step);
                    if (kind == "core.init.const")
                    {
                        covered.emplace_back(0, *rowCount);
                        continue;
                    }
                    if (kind != "core.init.fill" && kind != "core.init.readmem")
                        throw std::runtime_error("CPU six-phase emit unsupported array initializer: " + std::string(kind));
                    const auto *start = parameter<int64_t>(model_, params, "start"), *count = parameter<int64_t>(model_, params, "count");
                    if ((start && (*start < 0 || uint64_t(*start) > *rowCount)) || (count && *count < 0))
                        throw std::runtime_error("CPU array initializer range is invalid");
                    const auto first = start ? uint64_t(*start) : 0;
                    const auto size = count ? uint64_t(*count) : *rowCount - first;
                    if (size > *rowCount - first) throw std::runtime_error("CPU array initializer range exceeds state");
                    if (kind == "core.init.fill") covered.emplace_back(first, first + size);
                    else for (const auto &[row, data] : readmemRows(step, element, first, first + size)) covered.emplace_back(row, row + 1);
                }
                std::sort(covered.begin(), covered.end());
                uint64_t end = 0;
                for (const auto &[first, last] : covered)
                {
                    if (first > end) break;
                    end = std::max(end, last);
                }
                if (end != *rowCount)
                    throw std::runtime_error("CPU array initializer leaves uncovered rows: " + std::string(model_.text(model_.states()[record.state.index - 1].name)));
            }
        }

        // ----- init -----
        bool SixPhaseEmitter::initZeroElidable(const InitStep &step) const
        {
            if (!initElidableBuilt_)
            {
                initElidableBuilt_ = true;
                std::vector<char> dirty(model_.states().size() + 1, 0);
                for (const auto &record : model_.initRecords())
                    for (const auto &init : model_.steps(record))
                    {
                        const auto &target = stateType(record.state);
                        const bool array = !target.shape.empty();
                        const Type element = array ? arrayElementType(target) : target;
                        bool zero = false;
                        if (element.kind == TypeKind::Logic && element.domain == LogicDomain::TwoState)
                        {
                            const auto kind = model_.text(init.kind);
                            const auto params = model_.parameters(init);
                            if (kind == "core.init.const")
                            {
                                if (!array)
                                {
                                    if (const auto *text = parameter<std::string>(model_, params, "value"))
                                        zero = initLiteral(*text, element) == literal("0", element);
                                }
                                else if (const auto *text = parameter<std::string>(model_, params, "value"))
                                    zero = initLiteral(*text, target) == literal("0", target);
                            }
                            else if (kind == "core.init.fill")
                                if (const auto *text = parameter<std::string>(model_, params, "value"))
                                    zero = initLiteral(*text, element) == literal("0", element);
                        }
                        // init() value-initializes every store first, so a zero-valued
                        // step restates already-zero bytes and can be dropped.
                        if (zero && !dirty[record.state.index]) initElidable_.insert(&init);
                        if (!zero) dirty[record.state.index] = 1;
                    }
            }
            return initElidable_.contains(&step);
        }
        const SixPhaseEmitter::InitRows &SixPhaseEmitter::readmemRows(const InitStep &step, const Type &element,
                                                                      uint64_t first, uint64_t end) const
        {
            if (const auto found = readmemRows_.find(&step); found != readmemRows_.end()) return found->second;
            const auto params = model_.parameters(step);
            const auto *file = parameter<std::string>(model_, params, "file"), *format = parameter<std::string>(model_, params, "format");
            if (!file || file->empty() || !format || (*format != "hex" && *format != "bin"))
                throw std::runtime_error("CPU readmem requires file and hex/bin format");
            std::ifstream stream(*file, std::ios::binary);
            if (!stream) throw std::runtime_error("CPU readmem failed to open: " + *file);
            const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
            if (stream.bad()) throw std::runtime_error("CPU readmem failed to read: " + *file);
            std::vector<std::string> tokens;
            std::string error;
            if (!emit::tokenizeReadmemText(text, tokens, error)) throw std::runtime_error(*file + ": " + error);
            InitRows rows;
            uint64_t row = first;
            for (const auto &token : tokens)
            {
                if (token.front() == '@')
                {
                    const auto address = emit::parseReadmemAddress(std::string_view(token).substr(1));
                    if (!address) throw std::runtime_error("CPU readmem invalid address: " + token + " in " + *file);
                    row = *address;
                    continue;
                }
                bool hasDigit = false;
                for (const char ch : token)
                {
                    if (ch == '_') continue;
                    const bool valid = ch == '0' || ch == '1' || ch == 'x' || ch == 'X' || ch == 'z' || ch == 'Z' || ch == '?' ||
                        (*format == "hex" && ((ch >= '2' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')));
                    if (!valid) throw std::runtime_error("CPU readmem invalid data: " + token + " in " + *file);
                    hasDigit = true;
                }
                if (!hasDigit) throw std::runtime_error("CPU readmem empty data token in " + *file);
                if (row >= first && row < end)
                    rows.emplace_back(row, literal(std::to_string(element.width) + (*format == "hex" ? "'h" : "'b") + token, element));
                if (row != std::numeric_limits<uint64_t>::max()) ++row;
            }
            return readmemRows_.emplace(&step, std::move(rows)).first->second;
        }
        void SixPhaseEmitter::initStep(std::ostream &out, StateId id, const InitStep &step) const
        {
            const auto &target = stateType(id);
            const bool array = !target.shape.empty();
            const Type element = array ? arrayElementType(target) : target;
            if (element.kind != TypeKind::Logic || element.domain != LogicDomain::TwoState)
                throw std::runtime_error("CPU initializer currently requires two-state logic or an array of two-state logic");
            const auto kind = model_.text(step.kind);
            const auto params = model_.parameters(step);
            // The store follows the state's classification (M5d-6): regLatch
            // covers scalars and regLatch-class arrays, mem the mem-class
            // arrays. Initializers write the current buffer; initGlue syncs
            // smallSinkStoreNext afterwards.
            const bool inRegLatch = id.index < regFieldByState_.size() && regFieldByState_[id.index];
            const auto *field = inRegLatch ? regFieldByState_[id.index]
                                           : (id.index < memFieldByState_.size() ? memFieldByState_[id.index] : nullptr);
            if (!field) throw std::runtime_error("CPU six-phase emit state has no named-store field");
            const std::string slot = (inRegLatch ? "smallSinkStore." : "largeSinkStore.") + std::string(model_.text(field->name));
            const bool elide = initZeroElidable(step);
            if (!elide) out << "{\n";
            if (!array)
            {
                if (kind == "core.init.const")
                {
                    const auto *text = parameter<std::string>(model_, params, "value");
                    if (!text) throw std::runtime_error("CPU scalar initializer requires value literal");
                    if (!elide) out << slot << '=' << initLiteral(*text, element) << ";\n";
                }
                else if (kind == "core.init.random")
                {
                    const auto *seed = parameter<int64_t>(model_, params, "seed");
                    if (seed) out << "std::uint64_t rng=UINT64_C(" << static_cast<uint64_t>(*seed) << ");\n";
                    randomInit(out, element, slot, seed ? "rng" : "cpu_rng");
                }
                else throw std::runtime_error("CPU scalar initializer must be const or random");
                if (!elide) out << "}\n";
                return;
            }

            // Array initializers address row-major elements of the nested store.
            const auto rowCountOpt = shapeElementCount(target.shape);
            if (!rowCountOpt)
                throw std::runtime_error("CPU six-phase emit array state has an invalid shape");
            const uint64_t rowCount = *rowCountOpt;
            if (kind == "core.init.const")
            {
                const auto *text = parameter<std::string>(model_, params, "value");
                if (!text)
                    throw std::runtime_error("CPU array const initializer requires one total-width literal");
                if (!elide)
                {
                    out << "const auto cpu_value=" << initLiteral(*text, target) << ";\n"
                        << "for(std::size_t cpu_row=0;cpu_row<" << rowCount << ";++cpu_row)"
                        << rowAccessText(slot, target.shape, "cpu_row") << '='
                        << normalize(rowSliceText("cpu_value", "cpu_row", element.width, target.width), element)
                        << ";\n";
                }
            }
            else if (kind == "core.init.fill" || kind == "core.init.readmem")
            {
                const auto *start = parameter<int64_t>(model_, params, "start"), *count = parameter<int64_t>(model_, params, "count");
                if ((start && (*start < 0 || uint64_t(*start) > rowCount)) || (count && *count < 0))
                    throw std::runtime_error("CPU array initializer range is invalid");
                const auto first = start ? uint64_t(*start) : 0;
                const auto size = count ? uint64_t(*count) : rowCount - first;
                if (size > rowCount - first) throw std::runtime_error("CPU array initializer range exceeds state");
                const auto end = first + size;
                const auto rowBytes = storageBytes(element);
                if (kind == "core.init.fill")
                {
                    const auto *text = parameter<std::string>(model_, params, "value");
                    const auto *random = parameter<bool>(model_, params, "random");
                    if ((!text && !(random && *random)) || (text && random))
                        throw std::runtime_error("CPU array fill requires exactly one of value or random=true");
                    const bool zero = !elide && text && (initLiteral(*text, element) == literal("0", element));
                    if (!elide)
                    {
                        if (zero)
                            out << "std::memset(reinterpret_cast<char*>(&" << slot << ")+" << first * rowBytes
                                << ",0," << size * rowBytes << ");\n";
                        else
                        {
                            if (text) out << "static const auto data=" << initLiteral(*text, element) << ";\n";
                            out << "for(std::size_t row=" << first << ";row<" << end << ";++row){\n"
                                << "auto &dst=" << rowAccessText(slot, target.shape, "row") << ";\n";
                            if (text) out << "dst=data;\n";
                            else randomInit(out, element, "dst", "cpu_rng");
                            out << "}\n";
                        }
                    }
                }
                else
                {
                    const auto &rows = readmemRows(step, element, first, end);
                    if (!rows.empty())
                    {
                        out << "static const std::size_t rows[]={";
                        for (const auto &[row, data] : rows) out << row << ',';
                        out << "};\nstatic const " << cppType(element) << " data[]={\n";
                        for (const auto &[row, data] : rows) out << data << ",\n";
                        out << "};\nfor(std::size_t i=0;i<" << rows.size() << ";++i)"
                            << rowAccessText(slot, target.shape, "rows[i]") << "=data[i];\n";
                    }
                }
            }
            else throw std::runtime_error("CPU six-phase emit unsupported array initializer: " + std::string(kind));
            if (!elide) out << "}\n";
        }

        // ----- File bodies -----
        std::string SixPhaseEmitter::supernodeName(uint32_t ordinal) const
        {
            std::string name = "sn_" + std::to_string(ordinal);
            const auto &ops = supernodeOps_[ordinal];
            if (!ops.empty())
            {
                const auto &op = model_.operations()[ops.front().index - 1];
                if (op.name.valid()) name += "_" + identifier(model_.text(op.name));
            }
            return name;
        }
        void SixPhaseEmitter::header(std::ostream &out) const
        {
            out << "#pragma once\n#include \"" << prefix_ << "_runtime.hpp\"\n#include <cstdio>\n#include <cstring>\n#include <stdexcept>\n#include <type_traits>\n";
            if (!largeFinalChange_.empty()) out << "#include <unordered_map>\n";
            if (waveform_) out << "#include <memory>\n";
            if (memWriteOnWrite_)
                out << "// emit option: --mem-write-activate on-write (write-occurs reader activation)\n";
            // Wide (>64-bit) two-state values are C23 unsigned _BitInt objects:
            // clang is required (gcc's C++ frontend rejects _BitInt), and the
            // raw-byte paths (FST word dump, whole-store memcpy, dumpState
            // hash) rely on the little-endian u64-word layout with 8-byte
            // size rounding and alignment.
            out << "#ifndef __BITINT_MAXWIDTH__\n#error \"C23 _BitInt support is required (compile the model with clang >= 19)\"\n#endif\n"
                << "static_assert(sizeof(unsigned _BitInt(65))==16 && alignof(unsigned _BitInt(65))==8 &&\n"
                << "    sizeof(unsigned _BitInt(128))==16 && alignof(unsigned _BitInt(128))==8,\n"
                << "    \"unexpected _BitInt object layout: the generated model requires 8-byte-rounded little-endian words\");\n";
            // Optional perf/waveform build knobs (XS difftest hooks): default
            // off; counters are only compiled in a WOLVRIX_GRHSIM_PERF build.
            out << "#ifndef WOLVRIX_GRHSIM_PERF\n#define WOLVRIX_GRHSIM_PERF 0\n#endif\n"
                << "#ifndef WOLVRIX_GRHSIM_WAVEFORM\n#define WOLVRIX_GRHSIM_WAVEFORM 0\n#endif\n"
                << "#if WOLVRIX_GRHSIM_PERF\n#define GRHSIM_PERF_COUNT(field) (++perf_.field)\n"
                << "#else\n#define GRHSIM_PERF_COUNT(field) ((void)0)\n#endif\n";
            // M5d-7 multi-TU: every unit includes this header, so the shared
            // pieces live here — the dump value printers (the port section
            // matches the trace shim: bool -> 0/1, integral -> zero-padded
            // hex, arrays MS word first). DPI import declarations stay out of
            // the public header (testbenches define their own extern "C"
            // copies); each unit .cpp redeclares the imports it references.
            out << "namespace " << prefix_ << "_dump {\n"
                << "inline void grhsim_dump_value(std::FILE *stream,bool value){std::fprintf(stream,\"%u\",value?1u:0u);}\n"
                << "inline void grhsim_dump_wide(std::FILE *stream,const std::uint64_t *words,std::size_t count){for(std::size_t i=count;i-->0;)std::fprintf(stream,\"%016llx\",static_cast<unsigned long long>(words[i]));}\n"
                << "template<typename T> inline void grhsim_dump_value(std::FILE *stream,const T &value){\n"
                << "if constexpr(std::is_same_v<T,double>){std::uint64_t bits=0;std::memcpy(&bits,&value,sizeof(bits));std::fprintf(stream,\"%016llx\",static_cast<unsigned long long>(bits));}\n"
                << "else if constexpr(std::is_same_v<T,float>){std::uint32_t bits=0;std::memcpy(&bits,&value,sizeof(bits));std::fprintf(stream,\"%08x\",bits);}\n"
                << "else if constexpr(std::is_same_v<T,std::string>){std::fprintf(stream,\"%s\",value.c_str());}\n"
                << "else if constexpr(sizeof(T)>8){grhsim_dump_wide(stream,reinterpret_cast<const std::uint64_t*>(&value),sizeof(T)/8);}\n"
                << "else{using U=std::make_unsigned_t<T>;std::fprintf(stream,\"%0*llx\",static_cast<int>(sizeof(T))*2,static_cast<unsigned long long>(static_cast<U>(value)));}\n"
                << "}\n"
                << "template<typename T,std::size_t N> inline void grhsim_dump_value(std::FILE *stream,const std::array<T,N> &value){for(std::size_t i=N;i-->0;)grhsim_dump_value(stream,value[i]);}\n"
                << "}\n";
            // Contract §1.2: the interface port members open the class body (the
            // first public block's leading run of member declarations).
            out << "class " << class_ << " {\npublic:\n";
            for (const auto &input : model_.inputs()) out << cppValueType(model_.types()[input.type.index - 1]) << ' ' << identifier(model_.text(input.name)) << "{};\n";
            for (const auto &output : model_.outputs()) out << cppValueType(model_.types()[output.type.index - 1]) << ' ' << identifier(model_.text(output.name)) << "{};\n";
            out << "\n" << class_ << "()=default;\nvoid init();\nvoid eval();\nvoid dumpState(std::FILE *stream) const;\n"
                << "void set_runtime_profile_enabled(bool){}\nvoid dump_runtime_profile() const {}\n";
            if (waveform_)
                out << "void configure_waveform(bool enabled){configure_waveform(enabled,\"grhsim.fst\");}\n"
                    << "void configure_waveform(bool enabled,const char *path);\n"
                    << "[[nodiscard]] bool waveform_enabled() const{return waveform_enabled_;}\n";
            else
                out << "void configure_waveform(bool enabled){configure_waveform(enabled,\"grhsim.fst\");}\n"
                    << "void configure_waveform(bool enabled,const char *path){(void)path;\n"
                    << "#if WOLVRIX_GRHSIM_WAVEFORM\n"
                    << "if(enabled){static bool cpu_wave_warned=false;if(!cpu_wave_warned){cpu_wave_warned=true;std::fprintf(stderr,\"[grhsim] waveform capture was not enabled at emit time (--waveform declared-symbols)\\n\");}}\n"
                    << "#else\n(void)enabled;\n#endif\n"
                    << "}\n";
            out << "#if WOLVRIX_GRHSIM_PERF\n"
                << "struct PerfCounters{std::uint64_t evalCount=0,round1Count=0,round2Count=0,totalRoundCount=0,"
                << "computeBatchExecCount=0,commitBatchExecCount=0,touchedStateShadowCount=0,touchedWriteCount=0;};\n"
                << "PerfCounters perf_counters() const{return perf_;}\n#endif\n";
            out << "private:\n";
            if (waveform_)
            {
                out << "struct WaveEntry{const void *ptr;std::uint32_t bytes;std::uint32_t width;std::uint32_t prevOff;};\n"
                    << "bool waveform_enabled_=false,waveform_initialized_=false;\n"
                    << "std::uint64_t waveform_time_=0;\n"
                    << "std::string waveform_path_;\n"
                    << "std::unique_ptr<grhsim_fst_writer> waveform_writer_;\n"
                    << "std::vector<fstHandle> waveform_handles_;\n"
                    << "std::vector<WaveEntry> wave_entries_;\n"
                    << "std::array<std::uint64_t," << std::max<uint64_t>(wavePrevWords_, 1) << "> waveform_prev_{};\n"
                    << "void wave_add(grhsim_fst_writer &cpu_w,const char *cpu_name,std::uint32_t cpu_width,"
                    << "const void *cpu_ptr,std::uint32_t cpu_bytes,std::uint32_t cpu_prev){\n"
                    << "waveform_handles_.push_back(cpu_w.register_logic(cpu_name,cpu_width));\n"
                    << "wave_entries_.push_back({cpu_ptr,cpu_bytes,cpu_width,cpu_prev});}\n"
                    << "void ensure_waveform_open();\nvoid dump_waveform();\n";
                for (uint32_t chunk = 0; chunk < waveChunkCount(); ++chunk)
                    out << "void wave_setup_" << chunk << "(grhsim_fst_writer &);\n";
            }
            out << "#if WOLVRIX_GRHSIM_PERF\nPerfCounters perf_{};\n#endif\n";
            if (hasSystemTasks_)
                out << "bool cpu_first_eval=true;\nstd::array<bool," << onceTasks_.size() << "> cpu_system_done{};\n"
                    << "void cpu_system_task(std::string_view,std::span<const grhsim_task_arg>);\n";
            storeStruct(out, "SmallSinkStore", *smallSinkStore_);
            out << "SmallSinkStore smallSinkStore;\nSmallSinkStore smallSinkStoreNext;\n";
            storeStruct(out, "LargeSinkStore", *largeSinkStore_);
            out << "LargeSinkStore largeSinkStore;\n";
            // Only repeated event-free writers need old values for final
            // change detection. Save changed cells sparsely, never copy a RAM.
            for (const auto &[state, readers] : largeFinalChange_)
            {
                const auto &type = stateType(StateId{state, 0});
                out << "std::unordered_map<std::size_t,"
                    << cppType(type.shape.empty() ? type : arrayElementType(type))
                    << "> cpu_largeBefore" << state << ";\n";
            }
            constexpr std::array<std::string_view, 6> boundaryTypes{
                "InputStore", "NonSinkBoundaryStore", "SmallSinkEnableBoundaryStore",
                "SmallSinkPayloadBoundaryStore", "LargeSinkEnableBoundaryStore",
                "LargeSinkPayloadBoundaryStore"};
            constexpr std::array<std::string_view, 6> boundaryNames{
                "inputStore", "nonSinkBoundaryStore", "smallSinkEnableBoundaryStore",
                "smallSinkPayloadBoundaryStore", "largeSinkEnableBoundaryStore",
                "largeSinkPayloadBoundaryStore"};
            for (std::size_t i = 0; i < boundaryStores_.size(); ++i)
            {
                storeStruct(out, boundaryTypes[i], *boundaryStores_[i]);
                out << boundaryTypes[i] << ' ' << boundaryNames[i] << ";\n";
            }
            if (boundaryStringCount_)
                out << "std::array<std::string," << boundaryStringCount_ << "> boundaryStrings; // hoisted string boundary fields\n";
            storeStruct(out, "PrevEventStore", *prevEventStore_);
            out << "PrevEventStore prevEventStore;\n";
            // eventAct is byte-packed (bit act%8 of byte act/8 per field);
            // timeslot flags are one byte per task, index == the flag index.
            out << "std::array<std::uint8_t," << std::max<uint64_t>(eventActStore_->sizeBytes, 1) << "> eventActStore{};\n";
            out << "std::array<std::uint8_t," << std::max<uint64_t>(timeslotStore_->sizeBytes, 1) << "> timeslotTriggerFlag{};\n";
            for (const auto &field : activeStore_->fields)
                out << cppStoreType(field.type) << ' ' << model_.text(field.name) << "{}; // supernodes=" << field.aux << '\n';
            out << "std::uint64_t cpu_rng=UINT64_C(0x6a09e667f3bcc909);\n";
            out << "bool cpu_anyOutChange_=true; // output gate: dirty through the first eval\n";
            if (randomSampleCount_)
                out << "std::array<std::uint64_t," << randomSampleCount_ << "> cpu_random_values{};\n"
                    << "std::array<bool," << randomSampleCount_ << "> cpu_random_sampled{};\n";
            out << "void pInput();\nvoid pEvent();\nvoid pGeneral();\nvoid pLargeSink();\nbool pPublish();\nvoid pOutput();\n";
            // M5d-7 multi-TU: spill frame structs (cross-chunk locals) and the
            // chunk member declarations. Frame structs nest inside the class;
            // chunk functions live in the unit .cpp the C8 plan assigned.
            for (uint32_t ordinal = 0; ordinal < supernodeCount_; ++ordinal)
                if (!supernodeFrames_[ordinal].empty())
                    frameStruct(out, "SnFrame" + std::to_string(ordinal), supernodeFrames_[ordinal]);
            if (!eventFrame_.empty()) frameStruct(out, "EventFrame", eventFrame_);
            if (!outputFrame_.empty()) frameStruct(out, "OutputFrame", outputFrame_);
            for (uint32_t ordinal = 0; ordinal < supernodeCount_; ++ordinal)
            {
                out << "void " << supernodeName(ordinal) << "();\n";
                const auto &attrs = tree_.partitions[order_[ordinal].index - 1].attrs;
                if (attrs.helperChunks.size() < 2) continue;
                for (std::size_t i = 0; i < attrs.helperChunks.size(); ++i)
                {
                    out << "void " << supernodeName(ordinal) << "__c" << i << '(';
                    if (!supernodeFrames_[ordinal].empty()) out << "SnFrame" << ordinal << " &";
                    out << ");\n";
                }
            }
            for (uint32_t i = 0; i < initChunks_.size(); ++i) out << "void cpu_init_" << i << "();\n";
            for (uint32_t i = 0; i < eventChunks_.size(); ++i)
            {
                out << "void pEvent_c" << i << '(';
                if (!eventFrame_.empty()) out << "EventFrame &";
                out << ");\n";
            }
            for (uint32_t i = 0; i < scanChunks_.size(); ++i) out << "void pGeneral_c" << i << "();\n";
            for (uint32_t i = 0; i < memChunks_.size(); ++i) out << "void pLargeSink_c" << i << "();\n";
            for (uint32_t i = 0; i < outputChunks_.size(); ++i)
            {
                out << "void pOutput_c" << i << '(';
                if (!outputFrame_.empty()) out << "OutputFrame &";
                out << ");\n";
            }
            for (uint32_t i = 0; i < dumpChunks_.size(); ++i) out << "void cpu_dump_" << i << "(std::FILE *) const;\n";
            out << "};\n";
        }
        // M5d-7: a store can be reset with memset when every field is
        // trivially copyable (std::string fields forbid it).
        bool SixPhaseEmitter::storeTriviallyResettable(const CpuNamedStore &store) const
        {
            const auto trivial = [&](CpuTypeId id) {
                const CpuType *type = &layout_.types[id.index - 1];
                while (type->kind == CpuTypeKind::Array) type = &layout_.types[type->elementType.index - 1];
                return type->kind != CpuTypeKind::String;
            };
            for (const auto &field : store.fields)
                if (!trivial(field.type) && !boundaryStringSlot_.contains(&field)) return false;
            return true;
        }
        void SixPhaseEmitter::storeStruct(std::ostream &out, std::string_view structName,
                                          const CpuNamedStore &store) const
        {
            // M5d-7: no per-field initializers — a value-initialized
            // 100k-field aggregate forces the compiler to materialize a giant
            // ctor; init() memsets the stores instead (see initGlue).
            out << "struct " << structName << "{\n";
            for (const auto &field : store.fields)
            {
                // Hoisted string boundary fields live in boundaryStrings.
                if (boundaryStringSlot_.contains(&field)) continue;
                out << cppStoreType(field.type) << ' ' << model_.text(field.name) << ";";
                switch (store.kind)
                {
                case CpuNamedStoreKind::SmallSink:
                case CpuNamedStoreKind::LargeSink:
                    if (field.state) out << " // state=" << model_.text(model_.states()[field.state.index - 1].name);
                    break;
                case CpuNamedStoreKind::Input:
                case CpuNamedStoreKind::NonSinkBoundary:
                case CpuNamedStoreKind::SmallSinkEnableBoundary:
                case CpuNamedStoreKind::SmallSinkPayloadBoundary:
                case CpuNamedStoreKind::LargeSinkEnableBoundary:
                case CpuNamedStoreKind::LargeSinkPayloadBoundary:
                    if (field.value)
                    {
                        const auto &value = model_.values()[field.value.index - 1];
                        out << " // value=";
                        if (value.name.valid()) out << model_.text(value.name);
                        else out << 'v' << field.value.index;
                    }
                    else if (field.aux < model_.inputs().size())
                        out << " // input=" << model_.text(model_.inputs()[field.aux].name);
                    break;
                case CpuNamedStoreKind::PrevEvent:
                    out << " // act=" << field.aux;
                    if (const auto it = detByAct_.find(field.aux); it != detByAct_.end()) out << " edge=" << it->second.edge;
                    break;
                default: break;
                }
                out << '\n';
            }
            const auto words = store.kind == CpuNamedStoreKind::SmallSinkEnableBoundary
                ? smallEnable_.words() : store.kind == CpuNamedStoreKind::LargeSinkEnableBoundary
                ? largeEnable_.words() : 0;
            if (words) out << "std::array<std::uint64_t," << words << "> enabledWords;\n";
            out << "};\n";
        }
        void SixPhaseEmitter::unitCpp(std::ostream &out, const CpuTranslationUnit &unit) const
        {
            out << "#include \"" << prefix_ << ".hpp\"\n";
            out << "// TU " << unit.name << ": " << unit.chunks.size() << " chunks, ~" << unit.estimatedLines
                << " estimated lines (C8 plan)\n";
            // DPI import declarations are per-unit (never in the public
            // header): only the imports this unit's chunks reference.
            {
                std::set<uint32_t> used;
                const auto scan = [&](const std::vector<OpId> &ops) {
                    for (const auto opId : ops)
                    {
                        const auto &op = model_.operations()[opId.index - 1];
                        if (model_.text(op.opType) != "core.dpi.call") continue;
                        used.insert(model_.objectRefs(op)[0].index);
                    }
                };
                for (const auto &chunk : unit.chunks)
                {
                    if (chunk.kind == CpuEmitChunkKind::Supernode)
                    {
                        // V3-M2: a split supernode's chunk is wrapper-only and
                        // references no DPI; its parts scan their own slices.
                        if (splitSupernodes_.contains(chunk.offset)) continue;
                        scan(supernodeOps_[chunk.offset]);
                    }
                    else if (chunk.kind == CpuEmitChunkKind::SupernodePart)
                    {
                        const auto &attrs = tree_.partitions[order_[chunk.offset].index - 1].attrs;
                        const auto ranges = cpuSupernodePartRanges(model_, tree_, order_[chunk.offset],
                                                                   tuPlan_.unitMaxEstimatedLines);
                        const auto range = ranges[chunk.count].first;
                        for (uint32_t helper = range.offset; helper < range.offset + range.count; ++helper)
                        {
                            const auto opRange = attrs.helperChunks[helper];
                            scan(std::vector<OpId>(supernodeOps_[chunk.offset].begin() + opRange.offset,
                                                   supernodeOps_[chunk.offset].begin() + opRange.offset + opRange.count));
                        }
                    }
                    else if (chunk.kind == CpuEmitChunkKind::Output)
                        scan(std::vector<OpId>(outputOps_.begin() + chunk.offset,
                                               outputOps_.begin() + chunk.offset + chunk.count));
                    else if (chunk.kind == CpuEmitChunkKind::Event)
                        scan(std::vector<OpId>(eventOps_.begin() + chunk.offset,
                                               eventOps_.begin() + chunk.offset + chunk.count));
                }
                if (!used.empty())
                {
                    std::set<std::string> declared;
                    for (const auto &function : model_.functions())
                        if (used.contains(function.id.index) &&
                            declared.insert(identifier(model_.text(function.symbol))).second)
                            out << dpiDeclaration(function) << '\n';
                }
            }
            const auto chunkId = [&](const CpuEmitChunk &chunk) {
                const auto key = (uint64_t(static_cast<unsigned>(chunk.kind)) << 32) | chunk.offset;
                return chunkIds_.at(key);
            };
            for (const auto &chunk : unit.chunks)
                switch (chunk.kind)
                {
                case CpuEmitChunkKind::Core: coreChunk(out); break;
                case CpuEmitChunkKind::Init: initChunkFn(out, chunkId(chunk), chunk); break;
                case CpuEmitChunkKind::Event: eventChunkFn(out, chunkId(chunk), chunk); break;
                case CpuEmitChunkKind::GeneralScan: scanChunkFn(out, chunkId(chunk), chunk); break;
                case CpuEmitChunkKind::Supernode:
                    if (splitSupernodes_.contains(chunk.offset)) supernodeWrapper(out, chunk.offset);
                    else supernodeChunkFns(out, chunk.offset);
                    break;
                case CpuEmitChunkKind::SupernodePart:
                {
                    // Part index -> helper range, recomputed with the same
                    // deterministic packing C8 used (validated at setup).
                    const auto ranges = cpuSupernodePartRanges(model_, tree_, order_[chunk.offset],
                                                               tuPlan_.unitMaxEstimatedLines);
                    const auto range = ranges[chunk.count].first;
                    supernodeHelperRange(out, chunk.offset, range.offset, range.offset + range.count);
                    break;
                }
                case CpuEmitChunkKind::LargeSink: memChunkFn(out, chunkId(chunk), chunk); break;
                case CpuEmitChunkKind::Output: outputChunkFn(out, chunkId(chunk), chunk); break;
                case CpuEmitChunkKind::Dump: dumpChunkFn(out, chunkId(chunk), chunk); break;
                }
        }
        void SixPhaseEmitter::coreChunk(std::ostream &out) const
        {
            // The fixed small core: init() driver, phase drivers, eval, the
            // dumpState driver and the system-task driver. Everything sizable
            // lives in the chunk functions these call.
            initGlue(out);
            pInputBody(out);
            pEventBody(out);
            pGeneralBody(out);
            pLargeSinkBody(out);
            pPublishBody(out);
            pOutputBody(out);
            evalBody(out);
            dumpStateBody(out);
            if (waveform_) waveformGlue(out);
            systemTaskDriver(out);
        }
        void SixPhaseEmitter::supernodeBody(std::ostream &out, uint32_t ordinal) const
        {
            const auto previousContext = readContext_;
            readContext_ = *tree_.partitions[order_[ordinal].index - 1].attrs.supernodeCategory ==
                                   CpuSupernodeCategory::NonSink
                               ? CpuBoundaryUse::NonSink : CpuBoundaryUse::SmallSinkPayload;
            std::fill(activeLocals_.begin(), activeLocals_.end(), 0);
            // fanout 合写：函数体先进缓冲，旗标写累积到局部变量，尾端一次写回。
            std::ostringstream body;
            flagAccumActive_ = true;
            flagAccumTouched_.clear();
            for (const auto opId : supernodeOps_[ordinal])
                emitCompute(body, model_.operations()[opId.index - 1], ordinal);
            flagAccumActive_ = false;
            out << "void " << class_ << "::" << supernodeName(ordinal) << "(){\n";
            flagAccumDecls(out);
            out << body.str();
            flagAccumWriteBack(out);
            out << "}\n";
            readContext_ = previousContext;
        }
        // V3-M2: the sn_<ordinal> driver (wrapper) alone — emitted by the
        // Supernode chunk of a split supernode; the __c<i> helpers live in
        // the SupernodePart chunks' units.
        void SixPhaseEmitter::supernodeWrapper(std::ostream &out, uint32_t ordinal) const
        {
            const auto &attrs = tree_.partitions[order_[ordinal].index - 1].attrs;
            const auto &frame = supernodeFrames_[ordinal];
            out << "void " << class_ << "::" << supernodeName(ordinal) << "(){\n";
            if (!frame.empty())
                out << "SnFrame" << ordinal << " cpu_f"
                    << (frameNeedsZeroInit(frame) ? "{}" : "") << ";\n";
            for (std::size_t i = 0; i < attrs.helperChunks.size(); ++i)
                out << supernodeName(ordinal) << "__c" << i << '(' << (frame.empty() ? "" : "cpu_f") << ");\n";
            out << "}\n";
        }
        // The __c<begin..end-1> helper definitions of a chunked supernode —
        // emitted by SupernodePart chunks (a part slice) or by an unsplit
        // Supernode chunk (the whole range).
        void SixPhaseEmitter::supernodeHelperRange(std::ostream &out, uint32_t ordinal, uint32_t begin,
                                                   uint32_t end) const
        {
            const auto &attrs = tree_.partitions[order_[ordinal].index - 1].attrs;
            const auto previousContext = readContext_;
            readContext_ = *attrs.supernodeCategory == CpuSupernodeCategory::NonSink
                               ? CpuBoundaryUse::NonSink : CpuBoundaryUse::SmallSinkPayload;
            const auto &frame = supernodeFrames_[ordinal];
            for (std::size_t i = begin; i < end; ++i)
            {
                const auto range = attrs.helperChunks[i];
                // fanout 合写：同 supernodeBody —— 体先进缓冲，尾端一次写回。
                std::ostringstream body;
                flagAccumActive_ = true;
                flagAccumTouched_.clear();
                std::fill(activeLocals_.begin(), activeLocals_.end(), 0);
                activateCrossing(frame);
                for (uint32_t i2 = 0; i2 < range.count; ++i2)
                {
                    const auto opId = supernodeOps_[ordinal][range.offset + i2];
                    emitCompute(body, model_.operations()[opId.index - 1], ordinal);
                }
                deactivateCrossing(frame);
                flagAccumActive_ = false;
                out << "void " << class_ << "::" << supernodeName(ordinal) << "__c" << i << '(';
                if (!frame.empty()) out << "SnFrame" << ordinal << " &cpu_f";
                out << "){\n";
                flagAccumDecls(out);
                out << body.str();
                flagAccumWriteBack(out);
                out << "}\n";
            }
            readContext_ = previousContext;
        }
        void SixPhaseEmitter::supernodeChunkFns(std::ostream &out, uint32_t ordinal) const
        {
            const auto &attrs = tree_.partitions[order_[ordinal].index - 1].attrs;
            if (attrs.helperChunks.size() < 2) { supernodeBody(out, ordinal); return; }
            // Chunked supernode (M5d-7): the driver value-initializes the spill
            // frame on its stack and calls the C6 helper chunk members in
            // order; cross-chunk locals live in the frame (cpu_f.v<index>).
            // Unsplit supernodes emit wrapper and helpers into one unit here;
            // split supernodes (V3-M2) get the wrapper from the Supernode
            // chunk and each helper range from its SupernodePart chunk.
            supernodeWrapper(out, ordinal);
            supernodeHelperRange(out, ordinal, 0, attrs.helperChunks.size());
        }
        void SixPhaseEmitter::emitOpListRange(std::ostream &out, const std::vector<OpId> &ops, uint32_t offset,
                                              uint32_t count, const std::vector<uint32_t> &frame,
                                              bool outputPhase) const
        {
            std::fill(activeLocals_.begin(), activeLocals_.end(), 0);
            activateCrossing(frame);
            for (uint32_t i = 0; i < count; ++i)
            {
                const auto &op = model_.operations()[ops[offset + i].index - 1];
                const auto name = model_.text(op.opType);
                if (!outputPhase && name == "core.event.edgeDet") { emitEdgeDet(out, op); continue; }
                if (outputPhase && name == "core.output.write") { emitOutputWrite(out, op); continue; }
                // latchWrite is pre-staged (precomputeStagedOutputWrites); the
                // driver commits it after every output chunk ran.
                if (outputPhase && name == "core.state.latchWrite") continue;
                if (outputPhase && name == "core.system.task") { emitOutputTask(out, op); continue; }
                emitCompute(out, op, ~0u);
            }
            deactivateCrossing(frame);
        }
        void SixPhaseEmitter::initChunkFn(std::ostream &out, uint32_t id, const CpuEmitChunk &chunk) const
        {
            out << "void " << class_ << "::cpu_init_" << id << "(){\n";
            for (uint64_t position = chunk.offset; position < uint64_t(chunk.offset) + chunk.count; ++position)
                initStreamItem(out, position);
            out << "}\n";
        }
        void SixPhaseEmitter::initStreamItem(std::ostream &out, uint64_t position) const
        {
            if (position < initSteps_.size())
            {
                const auto &[state, step] = initSteps_[position];
                initStep(out, state, *step);
                return;
            }
            position -= initSteps_.size();
            if (position < constBoundaryFields_.size())
            {
                const auto *field = constBoundaryFields_[position];
                const auto &init = constBoundaryInit_.find(field)->second;
                // A shaped (array) constant's boundary field is the flat
                // per-row array: unpack the packed literal per row.
                if (layout_.types[field->type.index - 1].kind == CpuTypeKind::Array)
                {
                    const auto &valueType = type(field->value);
                    const auto rows = shapeElementCount(valueType.shape);
                    const auto elementWidth = typeElementWidth(valueType);
                    if (valueType.shape.empty() || !rows || !elementWidth)
                        throw std::runtime_error("CPU six-phase emit shaped constant boundary value has an "
                                                 "invalid shape");
                    const Type element = arrayElementType(valueType);
                    out << "{const auto cpu_value=" << init << ";\n"
                        << "for(std::size_t cpu_j=0;cpu_j<" << *rows << ";++cpu_j)"
                        << rowAccessText(boundaryRef(field), valueType.shape, "cpu_j")
                        << "="
                        << normalize(rowSliceText("cpu_value", "cpu_j", *elementWidth, valueType.width), element)
                        << ";\n}\n";
                    return;
                }
                out << boundaryRef(field) << '=' << init << ";\n";
                return;
            }
            position -= constBoundaryFields_.size();
            if (position < detActs_.size())
            {
                const auto &det = detByAct_.at(detActs_[position]);
                out << "prevEventStore." << model_.text(prevByAct_[det.act]->name) << '='
                    << initLiteral(det.prevInit, type(det.event)) << ";\n";
                return;
            }
            out << "smallSinkStoreNext=smallSinkStore;\n";
        }
        void SixPhaseEmitter::eventChunkFn(std::ostream &out, uint32_t id, const CpuEmitChunk &chunk) const
        {
            out << "void " << class_ << "::pEvent_c" << id << '(';
            if (!eventFrame_.empty()) out << "EventFrame &cpu_f";
            out << "){\n";
            emitOpListRange(out, eventOps_, chunk.offset, chunk.count, eventFrame_, false);
            out << "}\n";
        }
        void SixPhaseEmitter::scanRange(std::ostream &out, uint32_t begin, uint32_t end) const
        {
            // V2 (M2) firing rules by supernode category: a non-sink
            // supernode fires on nonSinkActiveFlag (cleared on fire); a sink
            // event cluster fires on its eventActStore signature (the
            // signature IS the gate — no flag involved); a sink escape
            // supernode fires unconditionally every round.
            // V3-M1: the ActiveFlags arrays are u64 bit words. The non-sink
            // prefix scans them with an outer per-word skip (64 silent
            // supernodes collapse into one test); each bit check inside reads
            // the LIVE word, so a same-round fanout to a later ordinal in the
            // same word is still picked up — the ordinal>current rule in
            // activateFanout makes every same-round target lie ahead in scan
            // order. The bit clear before firing preserves clear-on-fire.
            // A1: a guard-subdivided SinkEvent supernode additionally ANDs one
            // read of its shared write enable into the signature gate; the
            // scan functions emit no op bodies, so stale function-local value
            // state must not leak into that read.
            std::fill(activeLocals_.begin(), activeLocals_.end(), 0);
            const auto dataActive = model_.text(activeStore_->fields[0].name);
            uint32_t ordinal = begin;
            const uint32_t nonSinkEnd = std::min(end, nonSinkCount_);
            while (ordinal < nonSinkEnd)
            {
                const uint32_t word = ordinal / 64;
                const uint32_t wordEnd = std::min((word + 1) * 64, nonSinkEnd);
                const uint32_t width = wordEnd - ordinal;
                const uint64_t wordMask = width == 64 ? ~0ull : ((1ull << width) - 1) << (ordinal % 64);
                out << "if(" << dataActive << '[' << word << ']';
                if (wordMask != ~0ull) out << '&' << u64Const(wordMask);
                out << "){\n";
                for (; ordinal < wordEnd; ++ordinal)
                {
                    const auto bitMask = u64Const(1ull << (ordinal % 64));
                    out << "if(" << dataActive << '[' << word << "]&" << bitMask << "){" << dataActive << '[' << word << "]&=~" << bitMask << ";\n"
                        << "GRHSIM_PERF_COUNT(computeBatchExecCount);\n"
                        << supernodeName(ordinal) << "();\n}\n";
                }
                out << "}\n";
            }
            const auto emitSinkCall = [&](uint32_t sinkOrdinal, bool actHoisted) {
                const auto &attrs = tree_.partitions[order_[sinkOrdinal].index - 1].attrs;
                switch (*attrs.supernodeCategory)
                {
                case CpuSupernodeCategory::SmallSinkEscape:
                    out << "GRHSIM_PERF_COUNT(computeBatchExecCount);\n"
                        << supernodeName(sinkOrdinal) << "();\n";
                    break;
                case CpuSupernodeCategory::SmallSinkEvent:
                    if (!actHoisted || attrs.enableGuard)
                    {
                        out << "if(";
                        if (!actHoisted) out << actBitsGuard(*attrs.eventActs);
                        if (attrs.enableGuard)
                        {
                            if (!actHoisted) out << "&&";
                            out << readAs(ValueId{static_cast<uint32_t>(*attrs.enableGuard), 0},
                                          CpuBoundaryUse::SmallSinkEnable);
                        }
                        out << "){\n";
                    }
                    out << "GRHSIM_PERF_COUNT(computeBatchExecCount);\n"
                        << supernodeName(sinkOrdinal) << "();\n";
                    if (!actHoisted || attrs.enableGuard) out << "}\n";
                    break;
                case CpuSupernodeCategory::NonSink:
                    throw std::runtime_error("CPU six-phase emit non-sink supernode beyond the ordinal prefix");
                }
            };
            while (ordinal < end)
            {
                if (smallSinkEnableBits_[ordinal].empty())
                {
                    emitSinkCall(ordinal++, false);
                    continue;
                }
                const auto &attrs = tree_.partitions[order_[ordinal].index - 1].attrs;
                uint32_t runEnd = ordinal + 1;
                while (runEnd < end && !smallSinkEnableBits_[runEnd].empty() &&
                       smallSinkPriority_[runEnd] == smallSinkPriority_[ordinal] &&
                       tree_.partitions[order_[runEnd].index - 1].attrs.eventActs == attrs.eventActs)
                    ++runEnd;
                std::map<uint32_t, uint64_t> wordMasks;
                for (uint32_t i = ordinal; i < runEnd; ++i)
                    for (const auto bit : smallSinkEnableBits_[i])
                        wordMasks[bit / 64] |= 1ull << (bit % 64);
                if (!attrs.eventActs->empty()) out << "if(" << actBitsGuard(*attrs.eventActs) << "){\n";
                out << "if((";
                bool first = true;
                for (const auto &[word, mask] : wordMasks)
                {
                    if (!first) out << '|';
                    first = false;
                    out << "(smallSinkEnableBoundaryStore.enabledWords[" << word << ']'
                        << '&' << u64Const(mask) << ')';
                }
                out << ")!=UINT64_C(0)){\n";
                for (uint32_t i = ordinal; i < runEnd; ++i) emitSinkCall(i, true);
                out << "}\n";
                if (!attrs.eventActs->empty()) out << "}\n";
                ordinal = runEnd;
            }
        }
        void SixPhaseEmitter::scanChunkFn(std::ostream &out, uint32_t id, const CpuEmitChunk &chunk) const
        {
            out << "void " << class_ << "::pGeneral_c" << id << "(){\n";
            scanRange(out, chunk.offset, chunk.offset + chunk.count);
            out << "}\n";
        }
        void SixPhaseEmitter::memChunkFn(std::ostream &out, uint32_t id, const CpuEmitChunk &chunk) const
        {
            const auto previousContext = readContext_;
            readContext_ = CpuBoundaryUse::LargeSinkPayload;
            out << "void " << class_ << "::pLargeSink_c" << id << "(){\n";
            if (schedule_.memWritePlan)
            {
                const auto &plan = *schedule_.memWritePlan;
                // Priority buckets are outermost; inside one priority, act
                // clustering hoists the guard and enable-word runs skip empty
                // write groups. The plan is ordered from lower to higher
                // priority so later writes overwrite earlier writes directly.
                // 1. Act clustering (Q2b): maximal runs of entries sharing the
                //    same eventActStore guard get one hoisted test instead of a
                //    per-port re-read. Ports only read eventActStore (P_event
                //    owns the writes within a round), so merging the tests is
                //    exact; the plan priority order is untouched.
                // 2. Enable-bitmap run grouping (inside each act run): maximal
                //    runs of entries whose every enable is shadowed get one
                //    aggregate word-OR skip. Runs never reorder entries — an
                //    unbitmapped entry (constant-1 enable, state/mem-backed
                //    enable, ...) simply ends the run.
                uint32_t i = 0;
                while (i < chunk.count)
                {
                    const auto &entry = plan[chunk.offset + i];
                    const auto &entryOp = model_.operations()[entry.writeOp.index - 1];
                    const std::string guard = actGuard(entryOp);
                    uint32_t end = i + 1;
                    while (end < chunk.count && plan[chunk.offset + end].priority == entry.priority &&
                           actGuard(model_.operations()[plan[chunk.offset + end].writeOp.index - 1]) == guard)
                        ++end;
                    const bool hoisted = guard != "true";
                    // Event-free runs re-enter every round: on-write activation
                    // would never quiesce, so they keep the on-change compare.
                    const bool eventFree = guard == "true";
                    if (eventFree && memWriteOnWrite_)
                        out << "// mem-write-activate fallback: event-free run keeps on-change detection\n";
                    if (hoisted) out << "if(" << guard << "){\n";
                    uint32_t k = i;
                    while (k < end)
                    {
                        // Consecutive aggregate writes often share one scalar
                        // enable (one preserved array port fans out to many
                        // rows).  Keep the event/priority order, but hoist the
                        // exact bitmap-bit test around the whole run so the
                        // generated code does not repeat the same branch for
                        // every row write.  The outer word guard remains a
                        // cheap skip when all bits in this run are clear.
                        const auto entryBit = [&](uint32_t position)
                            -> std::optional<std::pair<ValueId, uint32_t>> {
                            const auto enables = memEnableOperands(
                                model_.operations()[plan[chunk.offset + position].writeOp.index - 1]);
                            if (enables.size() != 1) return std::nullopt;
                            const auto it = largeEnable_.bitOfValue.find(enables.front().index);
                            if (it == largeEnable_.bitOfValue.end()) return std::nullopt;
                            return std::pair<ValueId, uint32_t>{enables.front(), it->second};
                        };
                        const auto firstBit = entryBit(k);
                        if (!firstBit)
                        {
                            emitMemWrite(out, model_.operations()[plan[chunk.offset + k].writeOp.index - 1],
                                         plan[chunk.offset + k], hoisted, nullptr, eventFree);
                            ++k;
                            continue;
                        }
                        std::set<uint32_t> words;
                        uint32_t runEnd = k + 1;
                        while (runEnd < end)
                        {
                            if (!entryBit(runEnd)) break;
                            ++runEnd;
                        }
                        for (uint32_t position = k; position < runEnd; ++position)
                            words.insert(entryBit(position)->second / 64);
                        const bool wordGuard = runEnd - k >= 2;
                        if (wordGuard)
                        {
                            out << "if((";
                            bool first = true;
                            for (const auto word : words)
                            {
                                if (!first) out << '|';
                                first = false;
                                out << "largeSinkEnableBoundaryStore.enabledWords[" << word << ']';
                            }
                            out << ")!=UINT64_C(0)){\n";
                        }
                        uint32_t groupBegin = k;
                        while (groupBegin < runEnd)
                        {
                            const auto groupBit = entryBit(groupBegin);
                            const auto groupEnable = groupBit->first;
                            uint32_t groupEnd = groupBegin + 1;
                            while (groupEnd < runEnd && entryBit(groupEnd)->second == groupBit->second)
                                ++groupEnd;
                            out << "if(" << memEnableGuard(groupEnable) << "){\n";
                            std::set<uint32_t> burst;
                            for (uint32_t position = groupBegin; position < groupEnd; ++position)
                                emitMemWrite(out,
                                             model_.operations()[plan[chunk.offset + position].writeOp.index - 1],
                                             plan[chunk.offset + position], hoisted,
                                             (memWriteOnWrite_ && !eventFree) ? &burst : nullptr,
                                             eventFree, true);
                            if (!burst.empty())
                            {
                                const auto dataNext = model_.text(activeStore_->fields[1].name);
                                for (const auto ordinal : burst)
                                    out << dataNext << '[' << ordinal / 64 << "]|="
                                        << u64Const(1ull << (ordinal % 64)) << ";\n";
                            }
                            out << "}\n";
                            groupBegin = groupEnd;
                        }
                        if (wordGuard) out << "}\n";
                        k = runEnd;
                    }
                    if (hoisted) out << "}\n";
                    i = end;
                }
            }
            out << "}\n";
            readContext_ = previousContext;
        }
        void SixPhaseEmitter::outputChunkFn(std::ostream &out, uint32_t id, const CpuEmitChunk &chunk) const
        {
            out << "void " << class_ << "::pOutput_c" << id << '(';
            if (!outputFrame_.empty()) out << "OutputFrame &cpu_f";
            out << "){\n";
            emitOpListRange(out, outputOps_, chunk.offset, chunk.count, outputFrame_, true);
            out << "}\n";
        }
        void SixPhaseEmitter::dumpChunkFn(std::ostream &out, uint32_t id, const CpuEmitChunk &chunk) const
        {
            out << "void " << class_ << "::cpu_dump_" << id << "(std::FILE *stream) const{\n";
            for (uint64_t position = chunk.offset; position < uint64_t(chunk.offset) + chunk.count; ++position)
                dumpStreamItem(out, position);
            out << "}\n";
        }
        void SixPhaseEmitter::dumpStreamItem(std::ostream &out, uint64_t position) const
        {
            // Canonical dump item order (C8 contract): input ports, output
            // ports, then the named-store fields in store order.
            const auto dumpPrinter = prefix_ + "_dump::grhsim_dump_value";
            // Wide _BitInt objects print through their u64-word image (the
            // generic grhsim_dump_value overload cannot name _BitInt).
            const auto wideLine = [&](const std::string &label, const std::string &ref, uint64_t width) {
                out << "std::fprintf(stream,\"" << label << "=\");" << prefix_
                    << "_dump::grhsim_dump_wide(stream,reinterpret_cast<const std::uint64_t*>(&(" << ref << ")),"
                    << (width + 63) / 64 << ");std::fputc('\\n',stream);\n";
            };
            const auto portLine = [&](const std::string &name, TypeId typeId) {
                const auto &type = model_.types()[typeId.index - 1];
                if (type.kind == TypeKind::Logic && type.domain == LogicDomain::TwoState && type.width > 64)
                {
                    wideLine(name, "this->" + identifier(name), type.width);
                    return;
                }
                out << "std::fprintf(stream,\"" << name << "=\");" << dumpPrinter << "(stream,this->" << identifier(name)
                    << ");std::fputc('\\n',stream);\n";
            };
            if (position < model_.inputs().size())
            {
                portLine(std::string(model_.text(model_.inputs()[position].name)), model_.inputs()[position].type);
                return;
            }
            position -= model_.inputs().size();
            if (position < model_.outputs().size())
            {
                portLine(std::string(model_.text(model_.outputs()[position].name)), model_.outputs()[position].type);
                return;
            }
            position -= model_.outputs().size();
            const std::array<const CpuNamedStore *, 12> dumpStores{
                smallSinkStore_, largeSinkStore_, boundaryStores_[0], boundaryStores_[1],
                boundaryStores_[2], boundaryStores_[3], boundaryStores_[4], boundaryStores_[5],
                prevEventStore_, eventActStore_, timeslotStore_, activeStore_};
            constexpr std::array<std::string_view, 6> boundaryNames{
                "inputStore", "nonSinkBoundaryStore", "smallSinkEnableBoundaryStore",
                "smallSinkPayloadBoundaryStore", "largeSinkEnableBoundaryStore",
                "largeSinkPayloadBoundaryStore"};
            for (const auto *store : dumpStores)
            {
                if (position >= store->fields.size())
                {
                    position -= store->fields.size();
                    continue;
                }
                const auto &field = store->fields[position];
                if (store == smallSinkStore_ || store == largeSinkStore_)
                {
                    // Large arrays dump as an fnv1a hash in any store
                    // (regLatch-class arrays joined the regLatch store in
                    // M5d-6).
                    const auto storeName = store == smallSinkStore_ ? "smallSinkStore" : "largeSinkStore";
                    const auto &type = layout_.types[field.type.index - 1];
                    if (type.kind == CpuTypeKind::Array && type.count > 64)
                    {
                        out << "std::fprintf(stream,\"" << storeName << "." << model_.text(field.name) << "=\");"
                            << "{std::uint64_t cpu_h=UINT64_C(14695981039346656037);const auto *cpu_p=reinterpret_cast<const unsigned char*>("
                            << storeName << "." << model_.text(field.name)
                            << ".data());for(std::size_t cpu_i=0;cpu_i<sizeof(" << storeName << "."
                            << model_.text(field.name)
                            << ");++cpu_i)cpu_h=(cpu_h^cpu_p[cpu_i])*UINT64_C(1099511628211);std::fprintf(stream,\"fnv1a:%016llx\",static_cast<unsigned long long>(cpu_h));}"
                            << "std::fputc('\\n',stream);\n";
                    }
                    else dumpStateField(out, storeName, field);
                    return;
                }
                const auto boundaryIt = std::find(boundaryStores_.begin(), boundaryStores_.end(), store);
                if (boundaryIt != boundaryStores_.end())
                {
                    const auto storeName = boundaryNames[boundaryIt - boundaryStores_.begin()];
                    const auto &fieldType = layout_.types[field.type.index - 1];
                    if (fieldType.kind == CpuTypeKind::UInt && fieldType.width > 64)
                    {
                        wideLine(std::string(storeName) + "." + std::string(model_.text(field.name)), boundaryRef(&field),
                                 fieldType.width);
                        return;
                    }
                    out << "std::fprintf(stream,\"" << storeName << '.' << model_.text(field.name) << "=\");" << prefix_
                        << "_dump::grhsim_dump_value(stream," << boundaryRef(&field)
                        << ");std::fputc('\\n',stream);\n";
                    return;
                }
                if (store == prevEventStore_)
                {
                    dumpStateField(out, "prevEventStore", field);
                    return;
                }
                if (store == eventActStore_)
                {
                    out << "std::fprintf(stream,\"eventActStore." << model_.text(field.name)
                        << "=%u\",static_cast<unsigned>((eventActStore[" << field.offset << "]>>"
                        << (field.aux % 8) << ")&1));std::fputc('\\n',stream);\n";
                    return;
                }
                if (store == timeslotStore_)
                {
                    out << "std::fprintf(stream,\"timeslotTriggerFlag." << model_.text(field.name)
                        << "=%u\",static_cast<unsigned>(timeslotTriggerFlag[" << field.aux
                        << "]));std::fputc('\\n',stream);\n";
                    return;
                }
                out << "std::fprintf(stream,\"" << model_.text(field.name) << "=\");" << prefix_
                    << "_dump::grhsim_dump_value(stream,"
                    << model_.text(field.name) << ");std::fputc('\\n',stream);\n";
                return;
            }
            throw std::runtime_error("CPU six-phase emit dump chunk position is out of range");
        }
        void SixPhaseEmitter::pInputBody(std::ostream &out) const
        {
            // Load changed input ports into their boundary fields and flag the
            // input.read fanout. Pure-event inputs carry no inputFanout row, so
            // their write lands without raising nonSinkActiveFlag (spec §3.3).
            const auto dataActive = model_.text(activeStore_->fields[0].name);
            out << "void " << class_ << "::pInput(){\n";
            activateOrdinals(out, std::vector<uint32_t>(randomSupernodes_.begin(), randomSupernodes_.end()), dataActive);
            for (const auto &input : model_.inputs())
            {
                const auto *field = input.id.index < boundaryByInput_.size() ? boundaryByInput_[input.id.index] : nullptr;
                if (!field) throw std::runtime_error("CPU six-phase emit input port has no boundary field");
                const auto slot = boundaryRef(field);
                const auto member = "this->" + identifier(model_.text(input.name));
                const auto &inputType = model_.types()[input.type.index - 1];
                if (!inputType.shape.empty())
                {
                    // Shaped ports and inputStore fields have the same nested
                    // geometry; compare cells without copying the port.
                    const auto rows = shapeElementCount(inputType.shape);
                    const auto elementWidth = typeElementWidth(inputType);
                    if (!rows || !elementWidth)
                        throw std::runtime_error("CPU six-phase emit shaped input port has an invalid shape");
                    const std::string rowExpr = rowAccessText("cpu_value", inputType.shape, "cpu_j");
                    out << "{const auto &cpu_value=" << member << ";bool cpu_chg=false;\n"
                        << "for(std::size_t cpu_j=0;cpu_j<" << *rows << ";++cpu_j){const auto cpu_e=" << rowExpr
                        << ";auto &cpu_c=" << rowAccessText(slot, inputType.shape, "cpu_j")
                        << ";if(cpu_c!=cpu_e){cpu_c=cpu_e;cpu_chg=true;}}\n"
                        << "if(cpu_chg){";
                    flagOutputDirty(out, outputReadInputs_.contains(input.id.index));
                    out << "\n";
                    activateOrdinals(out, inputPortFanout_[input.id.index - 1], dataActive);
                    out << "}}\n";
                    continue;
                }
                out << "if(" << slot << "!=" << member << "){" << slot << '=' << member << ";";
                flagOutputDirty(out, outputReadInputs_.contains(input.id.index));
                out << "\n";
                if (const auto it = smallEnable_.inputBits.find(input.id.index);
                    it != smallEnable_.inputBits.end())
                    out << enableSync(smallEnable_, it->second, member);
                if (const auto it = largeEnable_.inputBits.find(input.id.index);
                    it != largeEnable_.inputBits.end())
                    out << enableSync(largeEnable_, it->second, member);
                activateOrdinals(out, inputPortFanout_[input.id.index - 1], dataActive);
                out << "}\n";
            }
            out << "}\n";
        }
        void SixPhaseEmitter::pEventBody(std::ostream &out) const
        {
            out << "void " << class_ << "::pEvent(){\n"
                << "eventActStore.fill(0);\n";
            // M5d-7: the cone+edgeDet op list runs in the pEvent_c<k> chunk
            // members (EventFrame spills cross-chunk locals).
            if (!eventFrame_.empty()) out << "EventFrame cpu_f{};\n";
            for (uint32_t i = 0; i < eventChunks_.size(); ++i)
                out << "pEvent_c" << i << '(' << (eventFrame_.empty() ? "" : "cpu_f") << ");\n";
            // act -> timeslot trigger mapping (spec §3.4; eval-level sticky).
            for (uint32_t act = 0; act < triggersByAct_.size(); ++act)
                for (const auto flag : triggersByAct_[act])
                    out << "if((eventActStore[" << act / 8 << "]>>" << (act % 8) << ")&1)timeslotTriggerFlag[" << flag << "]=1;\n";
            out << "}\n";
        }
        void SixPhaseEmitter::pGeneralBody(std::ostream &out) const
        {
            out << "void " << class_ << "::pGeneral(){\n";
            for (uint32_t i = 0; i < scanChunks_.size(); ++i) out << "pGeneral_c" << i << "();\n";
            out << "}\n";
        }
        void SixPhaseEmitter::pLargeSinkBody(std::ostream &out) const
        {
            out << "void " << class_ << "::pLargeSink(){\n";
            for (uint32_t i = 0; i < memChunks_.size(); ++i) out << "pLargeSink_c" << i << "();\n";
            for (const auto &[state, readers] : largeFinalChange_)
            {
                const auto &type = stateType(StateId{state, 0});
                const auto cell = rowAccessText("largeSinkStore." + std::string(model_.text(memFieldByState_[state]->name)),
                                               type.shape, "cpu_row");
                out << "for(const auto &[cpu_row,cpu_old]:cpu_largeBefore" << state
                    << "){if(" << cell << "!=cpu_old){\n";
                for (const auto &reader : readers)
                {
                    const auto ordinal = ordinalOf_[reader.owner.index];
                    if (reader.staticRow) out << "if(cpu_row==" << *reader.staticRow << ')';
                    out << model_.text(activeStore_->fields[1].name) << '[' << ordinal / 64
                        << "]|=" << u64Const(1ull << (ordinal % 64)) << ";\n";
                }
                out << "}}\ncpu_largeBefore" << state << ".clear();\n";
            }
            out << "}\n";
        }
        void SixPhaseEmitter::pPublishBody(std::ostream &out) const
        {
            const auto dataActive = model_.text(activeStore_->fields[0].name);
            const auto dataNext = model_.text(activeStore_->fields[1].name);
            out << "bool " << class_ << "::pPublish(){\n"
                << "GRHSIM_PERF_COUNT(commitBatchExecCount);\n";
            for (const auto state : smallFinalChange_)
            {
                const auto name = model_.text(regFieldByState_[state]->name);
                out << "if(smallSinkStoreNext." << name << "!=smallSinkStore." << name << "){\n";
                activateOrdinals(out, stateFanout_[state], dataNext);
                out << "}\n";
            }
            out << "bool cpu_fixed=true;\nfor(std::size_t cpu_i=0;cpu_i<" << dataNext << ".size();++cpu_i)if(" << dataNext << "[cpu_i]){cpu_fixed=false;break;}\n"
                << "for(std::size_t cpu_i=0;cpu_i<" << dataNext << ".size();++cpu_i){" << dataActive << "[cpu_i]|=" << dataNext << "[cpu_i];" << dataNext << "[cpu_i]=0;}\n"
                << "std::memcpy(&smallSinkStore,&smallSinkStoreNext,sizeof(smallSinkStore));\n"
                << "return cpu_fixed;\n}\n";
        }
        void SixPhaseEmitter::pOutputBody(std::ostream &out) const
        {
            // M5d-7: the output cone runs in the pOutput_c<k> chunk members
            // (OutputFrame spills cross-chunk locals); the staged latchWrite
            // commit trails every chunk so its ne comparisons all observed the
            // previous eval's values.
            out << "void " << class_ << "::pOutput(){\n";
            // Output-activity gate: skip the cone sweep when no output-read
            // state/boundary/input changed this eval (outputs hold).  The
            // staged latchWrite commit always runs.
            if (outputGate_)
            {
                out << "const bool cpu_run=cpu_anyOutChange_;\ncpu_anyOutChange_=false;\nif(cpu_run){\n";
                if (!outputFrame_.empty()) out << "OutputFrame cpu_f{};\n";
                for (uint32_t i = 0; i < outputChunks_.size(); ++i)
                    out << "pOutput_c" << i << '(' << (outputFrame_.empty() ? "" : "cpu_f") << ");\n";
                out << "}\n";
            }
            else
            {
                if (!outputFrame_.empty()) out << "OutputFrame cpu_f{};\n";
                for (uint32_t i = 0; i < outputChunks_.size(); ++i)
                    out << "pOutput_c" << i << '(' << (outputFrame_.empty() ? "" : "cpu_f") << ");\n";
            }
            commitStagedOutputWrites(out);
            out << "}\n";
        }
        void SixPhaseEmitter::evalBody(std::ostream &out) const
        {
            out << "void " << class_ << "::eval(){\n"
                << "GRHSIM_PERF_COUNT(evalCount);\n"
                << (randomFunctions_.empty() ? "" : "cpu_random_sampled.fill(false);\n")
                << "pInput();\n"
                << "bool cpu_converged=false;\n"
                << "for(std::uint32_t cpu_round=1;cpu_round<=100000;++cpu_round){\n"
                << "GRHSIM_PERF_COUNT(totalRoundCount);\n"
                << "pEvent();\npGeneral();\npLargeSink();\n"
                << "if(pPublish()){cpu_converged=true;\n"
                << "if(cpu_round==1){GRHSIM_PERF_COUNT(round1Count);}else if(cpu_round==2){GRHSIM_PERF_COUNT(round2Count);}\n"
                << "break;}\n"
                << "}\n"
                << "if(!cpu_converged)throw std::runtime_error(\"CPU model did not converge\");\n"
                << "pOutput();\n";
            if (waveform_)
                out << "// FST: one record per eval at the eval boundary (difftest waveform_tick is a no-op).\n"
                    << "if(waveform_enabled_)dump_waveform();\n";
            if (hasSystemTasks_) out << "cpu_first_eval=false;\n";
            out << "}\n";
        }
        void SixPhaseEmitter::initGlue(std::ostream &out) const
        {
            out << "void " << class_ << "::init(){\n";
            if (waveform_)
                out << "waveform_initialized_=false;waveform_time_=0;waveform_prev_.fill(0);\n";
            for (const auto &input : model_.inputs())
                out << "this->" << identifier(model_.text(input.name)) << "={};\n";
            for (const auto &output : model_.outputs())
                out << "this->" << identifier(model_.text(output.name)) << "={};\n";
            // M5d-7: memset the (trivially copyable) stores — a value-init of
            // a 100k-field aggregate makes the compiler materialize a giant
            // ctor (measured: clang -O1 never finishes on the XS boundary
            // store). A string-carrying boundary store keeps the value-init
            // form (memset would be UB on std::string).
            const auto resetStore = [&](std::string_view member, std::string_view structName,
                                        const CpuNamedStore &store) {
                if (storeTriviallyResettable(store))
                    out << "std::memset(&" << member << ",0,sizeof(" << member << "));\n";
                else
                    out << member << '=' << structName << "{};\n";
            };
            resetStore("smallSinkStore", "SmallSinkStore", *smallSinkStore_);
            resetStore("smallSinkStoreNext", "SmallSinkStore", *smallSinkStore_);
            resetStore("largeSinkStore", "LargeSinkStore", *largeSinkStore_);
            for (const auto &[state, readers] : largeFinalChange_)
                out << "cpu_largeBefore" << state << ".clear();\n";
            constexpr std::array<std::string_view, 6> boundaryTypes{
                "InputStore", "NonSinkBoundaryStore", "SmallSinkEnableBoundaryStore",
                "SmallSinkPayloadBoundaryStore", "LargeSinkEnableBoundaryStore",
                "LargeSinkPayloadBoundaryStore"};
            constexpr std::array<std::string_view, 6> boundaryNames{
                "inputStore", "nonSinkBoundaryStore", "smallSinkEnableBoundaryStore",
                "smallSinkPayloadBoundaryStore", "largeSinkEnableBoundaryStore",
                "largeSinkPayloadBoundaryStore"};
            for (std::size_t i = 0; i < boundaryStores_.size(); ++i)
                resetStore(boundaryNames[i], boundaryTypes[i], *boundaryStores_[i]);
            resetStore("prevEventStore", "PrevEventStore", *prevEventStore_);
            out << "eventActStore.fill(0);\ntimeslotTriggerFlag.fill(0);\n";
            const auto dataActive = model_.text(activeStore_->fields[0].name);
            const auto dataNext = model_.text(activeStore_->fields[1].name);
            // V3-M1: set exactly the non-sink bits [0, nonSinkCount_) — sink
            // ordinals and the tail padding bits stay 0. Writers never target
            // them (schedule fanout covers non-sink targets only), so the
            // word-level convergence check and merge stay exact.
            if (nonSinkCount_ == 0)
            {
                out << dataActive << ".fill(0);\n";
            }
            else
            {
                out << dataActive << ".fill(UINT64_C(0xffffffffffffffff));\n";
                if (nonSinkCount_ % 64)
                    out << dataActive << '[' << (nonSinkCount_ - 1) / 64 << "]=" << u64Const((1ull << (nonSinkCount_ % 64)) - 1) << ";\n";
                for (uint32_t word = (nonSinkCount_ + 63) / 64; word < activeWords_; ++word)
                    out << dataActive << '[' << word << "]=0;\n";
            }
            out << dataNext << ".fill(0);\n"
                << "cpu_rng=UINT64_C(0x6a09e667f3bcc909);\n";
            if (!randomFunctions_.empty()) out << "cpu_random_values.fill(0);\ncpu_random_sampled.fill(false);\n";
            // M5d-7: the init stream (steps, constant-boundary preloads,
            // prevEvent inits, smallSinkStoreNext sync) runs in the
            // cpu_init_<k> chunk members.
            for (uint32_t i = 0; i < initChunks_.size(); ++i) out << "cpu_init_" << i << "();\n";
            // Build both enable bitmaps after constant boundary preloads.
            const auto initEnableBitmap = [&](const EnableBitmap &bitmap) {
                if (bitmap.fields.empty()) return;
                std::map<uint32_t, std::vector<std::pair<uint32_t, const CpuStoreField *>>> byWord;
                for (uint32_t bit = 0; bit < bitmap.fields.size(); ++bit)
                    byWord[bit / 64].emplace_back(bit % 64, bitmap.fields[bit]);
                for (const auto &[word, entries] : byWord)
                {
                    // Keep cold initialization bounded even with 100k enables.
                    // Without noinline, LLVM merges the chunks back into the
                    // giant init function and MachineCombiner takes minutes.
                    constexpr uint32_t wordsPerChunk = 8;
                    if (word % wordsPerChunk == 0)
                        out << "[this]() __attribute__((noinline)) {\n";
                    out << bitmap.storeName << ".enabledWords[" << word << "]=";
                    bool first = true;
                    for (const auto &[off, field] : entries)
                    {
                        if (!first) out << '|';
                        first = false;
                        out << '(' << boundaryRef(field) << '?' << u64Const(1ull << off) << ":UINT64_C(0))";
                    }
                    out << ";\n";
                    if (word % wordsPerChunk == wordsPerChunk - 1 || word + 1 == byWord.size())
                        out << "}();\n";
                }
            };
            initEnableBitmap(smallEnable_);
            initEnableBitmap(largeEnable_);
            out << "}\n";
        }
        void SixPhaseEmitter::dumpStateBody(std::ostream &out) const
        {
            out << "void " << class_ << "::dumpState(std::FILE *stream) const{\n"
                << "if(!stream)return;\n";
            for (uint32_t i = 0; i < dumpChunks_.size(); ++i) out << "cpu_dump_" << i << "(stream);\n";
            out << "}\n";
        }
        void SixPhaseEmitter::dumpStateField(std::ostream &out, std::string_view store, const CpuStoreField &field) const
        {
            const auto &cpuType = layout_.types[field.type.index - 1];
            if (cpuType.kind == CpuTypeKind::UInt && cpuType.width > 64)
            {
                // Wide _BitInt values print through their u64-word image,
                // most-significant word first (identical text to the old
                // std::array<uint64_t,N> dump).
                out << "std::fprintf(stream,\"" << store << '.' << model_.text(field.name) << "=\");" << prefix_
                    << "_dump::grhsim_dump_wide(stream,reinterpret_cast<const std::uint64_t*>(&" << store << '.'
                    << model_.text(field.name) << ")," << (cpuType.width + 63) / 64 << ");std::fputc('\\n',stream);\n";
                return;
            }
            out << "std::fprintf(stream,\"" << store << '.' << model_.text(field.name) << "=\");" << prefix_
                << "_dump::grhsim_dump_value(stream,"
                << store << '.' << model_.text(field.name) << ");std::fputc('\\n',stream);\n";
        }
        void SixPhaseEmitter::collectWaveformSignals()
        {
            // Shape rule: a declared symbol is dumpable when its port/state/
            // value reads back as flat two-state logic — scalars of any width
            // (wide scalars are _BitInt objects, dumped through their u64-word
            // image), Reals (dumped as their 64-bit pattern) and shaped
            // (array) symbols. Strings, four-state values and symbols folded
            // into a supernode-local are skipped. Stage-06 §8 Q3: a shaped
            // state/boundary/port symbol registers one FST var PER ELEMENT
            // (name[i0][i1]..., row-major; GTKWave folds the bracket names
            // into an array tree) over the flat row-array storage — the
            // per-element addressing lifts the old element-width restriction
            // (wide elements dump through their u64-word image like wide
            // scalars). An array whose physical byte volume exceeds
            // kWaveMaxBytes is skipped whole (no partial dump).
            constexpr uint64_t kWaveMaxBytes = 512;
            // Flat-storage dumpability: {width, bytes} of the single var.
            const auto flatShape = [&](const Type &type) -> std::optional<std::pair<uint32_t, uint64_t>> {
                switch (type.kind)
                {
                case TypeKind::Logic:
                    if (type.domain == LogicDomain::FourState) return std::nullopt;
                    return std::pair{std::max(type.width, 1u), storageBytes(type)};
                case TypeKind::Real: return std::pair{64u, uint64_t(8)};
                default: return std::nullopt;
                }
            };
            // Per-element dumpability of a shaped logic type over the flat
            // row-array storage: {elementWidth, rows, elementBytes}.
            struct ArrayWave
            {
                uint32_t elementWidth;
                uint64_t rows;
                uint64_t elementBytes;
            };
            const auto arrayShape = [&](const Type &type) -> std::optional<ArrayWave> {
                if (type.kind != TypeKind::Logic || type.shape.empty() ||
                    type.domain == LogicDomain::FourState)
                    return std::nullopt;
                const auto elementWidth = typeElementWidth(type);
                const auto rows = shapeElementCount(type.shape);
                if (!elementWidth || !rows) return std::nullopt;
                const uint64_t elementBytes = storageBytes(arrayElementType(type));
                if (*rows * elementBytes > kWaveMaxBytes) return std::nullopt;
                return ArrayWave{*elementWidth, *rows, elementBytes};
            };
            std::unordered_map<std::string_view, StateId> stateByName;
            for (const auto &state : model_.states()) stateByName.try_emplace(model_.text(state.name), state.id);
            std::unordered_map<std::string_view, ValueId> valueByName;
            for (const auto &value : model_.values())
                if (value.name.valid()) valueByName.try_emplace(model_.text(value.name), value.id);
            std::unordered_map<std::string_view, TypeId> portTypes;
            for (const auto &input : model_.inputs()) portTypes.try_emplace(model_.text(input.name), input.type);
            for (const auto &output : model_.outputs()) portTypes.try_emplace(model_.text(output.name), output.type);
            std::unordered_set<std::string_view> seen;
            seen.reserve(model_.declaredSymbols().size());
            uint64_t prevWords = 0;
            for (const auto symbolId : model_.declaredSymbols())
            {
                const std::string_view symbol = model_.text(symbolId);
                if (symbol.empty() || !seen.insert(symbol).second) continue;
                const Type *objectType = nullptr;
                std::string baseRef;
                bool rowArray = false; // the storage is the per-row flat array
                if (const auto it = portTypes.find(symbol); it != portTypes.end())
                {
                    objectType = &model_.types()[it->second.index - 1];
                    baseRef = "this->" + identifier(symbol);
                    rowArray = !objectType->shape.empty();
                }
                else if (const auto it = stateByName.find(symbol); it != stateByName.end())
                {
                    const StateId state = it->second;
                    const auto *field = state.index < regFieldByState_.size() ? regFieldByState_[state.index] : nullptr;
                    if (!field) continue; // mem-class state (memory contents stay out of the FST)
                    objectType = &stateType(state);
                    baseRef = "smallSinkStore." + std::string(model_.text(field->name));
                    rowArray = true;
                }
                else if (const auto it = valueByName.find(symbol); it != valueByName.end())
                {
                    const ValueId value = it->second;
                    const auto *field = value.index < boundaryByValue_.size() ? boundaryByValue_[value.index] : nullptr;
                    if (!field) continue; // folded into a supernode-local value
                    objectType = &type(value);
                    baseRef = boundaryRef(field);
                    rowArray = true;
                }
                else
                    continue; // no live object (optimized-away wire)
                if (!objectType->shape.empty() && rowArray)
                {
                    // Per-element registration (stage-06 §8 Q3).
                    const auto array = arrayShape(*objectType);
                    if (!array) continue;
                    for (uint64_t row = 0; row < array->rows; ++row)
                    {
                        WaveSignal signal;
                        signal.name = std::string(symbol);
                        std::vector<uint64_t> indices(objectType->shape.size());
                        uint64_t rem = row;
                        for (std::size_t d = indices.size(); d-- > 0;)
                        {
                            const uint64_t dim = static_cast<uint64_t>(objectType->shape[d]);
                            indices[d] = rem % dim;
                            rem /= dim;
                        }
                        for (const uint64_t index : indices)
                            signal.name += '[' + std::to_string(index) + ']';
                        signal.ref = baseRef;
                        for (const uint64_t index : indices)
                            signal.ref += '[' + std::to_string(index) + ']';
                        signal.width = array->elementWidth;
                        signal.bytes = array->elementBytes;
                        signal.prevOff = static_cast<uint32_t>(prevWords);
                        prevWords += std::max<uint64_t>(1, (signal.bytes + 7) / 8);
                        waveSignals_.push_back(std::move(signal));
                    }
                    continue;
                }
                const auto flat = flatShape(*objectType);
                if (!flat) continue;
                WaveSignal signal;
                signal.name = symbol;
                signal.ref = std::move(baseRef);
                signal.width = flat->first;
                signal.bytes = flat->second;
                signal.prevOff = static_cast<uint32_t>(prevWords);
                prevWords += std::max<uint64_t>(1, (signal.bytes + 7) / 8);
                waveSignals_.push_back(std::move(signal));
            }
            wavePrevWords_ = prevWords;
        }
        void SixPhaseEmitter::waveSetupFile(std::ostream &out, uint32_t fileIndex) const
        {
            out << "#include \"" << prefix_ << ".hpp\"\n";
            const uint32_t firstChunk = fileIndex * kWaveChunksPerFile;
            const uint32_t lastChunk = std::min(firstChunk + kWaveChunksPerFile, waveChunkCount());
            for (uint32_t chunk = firstChunk; chunk < lastChunk; ++chunk)
            {
                out << "void " << class_ << "::wave_setup_" << chunk << "(grhsim_fst_writer &cpu_w){\n";
                const uint64_t begin = uint64_t(chunk) * kWaveSignalsPerChunk;
                const uint64_t end = std::min<uint64_t>(begin + kWaveSignalsPerChunk, waveSignals_.size());
                for (uint64_t i = begin; i < end; ++i)
                {
                    const auto &signal = waveSignals_[i];
                    out << "wave_add(cpu_w,\"" << escapeWaveName(signal.name) << "\"," << signal.width << ",&" << signal.ref
                        << ",static_cast<std::uint32_t>(sizeof(" << signal.ref << "))," << signal.prevOff << ");\n";
                }
                out << "}\n";
            }
        }
        void SixPhaseEmitter::waveformGlue(std::ostream &out) const
        {
            out << "void " << class_ << "::configure_waveform(bool enabled,const char *path){\n"
                << "if(path&&path[0])waveform_path_=path;\n"
                << "waveform_enabled_=enabled;\n"
                << "if(!waveform_enabled_){\n"
                << "if(waveform_writer_){waveform_writer_->close();waveform_writer_.reset();}\n"
                << "waveform_handles_.clear();wave_entries_.clear();waveform_initialized_=false;}\n"
                << "}\n";
            out << "void " << class_ << "::ensure_waveform_open(){\n"
                << "if(!waveform_enabled_||waveform_writer_)return;\n"
                << "auto cpu_w=std::make_unique<grhsim_fst_writer>();\n"
                << "const char *cpu_path=waveform_path_.empty()?\"grhsim.fst\":waveform_path_.c_str();\n"
                << "if(!cpu_w->open(cpu_path,\"" << identifier(model_.text(model_.name())) << "\"))return;\n"
                << "waveform_handles_.reserve(" << waveSignals_.size() << ");wave_entries_.reserve(" << waveSignals_.size()
                << ");\n";
            for (uint32_t chunk = 0; chunk < waveChunkCount(); ++chunk) out << "wave_setup_" << chunk << "(*cpu_w);\n";
            out << "waveform_writer_=std::move(cpu_w);\n}\n";
            out << "void " << class_ << "::dump_waveform(){\n"
                << "ensure_waveform_open();\n"
                << "if(!waveform_writer_)return;\n"
                << "const bool cpu_force=!waveform_initialized_;\n"
                << "waveform_writer_->emit_time(waveform_time_++);\n"
                << "for(std::size_t cpu_i=0;cpu_i<wave_entries_.size();++cpu_i){\n"
                << "const WaveEntry &cpu_e=wave_entries_[cpu_i];\n"
                << "std::uint64_t *cpu_prev=&waveform_prev_[cpu_e.prevOff];\n"
                << "if(!cpu_force&&!std::memcmp(cpu_prev,cpu_e.ptr,cpu_e.bytes))continue;\n"
                << "std::memcpy(cpu_prev,cpu_e.ptr,cpu_e.bytes);\n"
                << "if(cpu_e.bytes<=8){std::uint64_t cpu_v=0;std::memcpy(&cpu_v,cpu_e.ptr,cpu_e.bytes);\n"
                << "waveform_writer_->emit_logic_u64(waveform_handles_[cpu_i],cpu_e.width,cpu_v);}\n"
                << "else waveform_writer_->emit_logic_words(waveform_handles_[cpu_i],cpu_e.width,"
                << "reinterpret_cast<const std::uint64_t*>(cpu_e.ptr));\n"
                << "}\n"
                << "waveform_initialized_=true;\n"
                << "}\n";
        }
        std::string SixPhaseEmitter::escapeWaveName(std::string_view name)
        {
            std::string result;
            result.reserve(name.size());
            for (const char c : name)
            {
                if (c == '\\' || c == '"') result += '\\';
                result += c;
            }
            return result;
        }
        void SixPhaseEmitter::systemTaskDriver(std::ostream &out) const
        {
            if (!hasSystemTasks_) return;
            // $strobe needs no deferral in the six-phase model: timeslot tasks
            // already live in P_output (the eval's time-slot end).
            out << "void " << class_ << "::cpu_system_task(std::string_view name,std::span<const grhsim_task_arg> args){\n";
            out << R"CPP(
std::ostream *stream=(name=="warning"||name=="error"||name=="fatal")?&std::cerr:&std::cout;
if(name=="fwrite"||name=="fdisplay"){
    if(args.empty())throw std::runtime_error("CPU file task is missing its handle");
    const auto handle=grhsim_task_arg_u64(args.front());args=args.subspan(1);
    if(handle==1||handle==UINT64_C(0x80000001))stream=&std::cout;
    else if(handle==2||handle==UINT64_C(0x80000002))stream=&std::cerr;
    else throw std::runtime_error("CPU system task file handle is not implemented");
}
const bool terminal=name=="fatal"||name=="finish"||name=="stop";
int exitCode=name=="fatal"?1:0;
if(terminal&&!args.empty()&&args.front().kind==grhsim_task_arg_kind::Logic){
    exitCode=static_cast<int>(grhsim_task_arg_u64(args.front()));args=args.subspan(1);
}
auto text=grhsim_format_task_message(args);
if(name=="info"||name=="warning"||name=="error"||name=="fatal")text="["+std::string(name)+"] "+text;
if(!terminal||!text.empty()){
    *stream<<text;
    if(name!="write"&&name!="fwrite")*stream<<'\n';
}
if(terminal){
    std::cout.flush();std::cerr.flush();std::exit(exitCode);
}
}
)CPP";
        }
        PassResult SixPhaseEmitter::write(const std::filesystem::path &directory)
        {
            if (std::filesystem::exists(directory) && !std::filesystem::is_empty(directory))
                throw std::runtime_error("CPU emit output directory must be empty: " + directory.string());
            std::filesystem::create_directories(directory);
            std::vector<std::string> artifacts, sources;
            const auto file = [&](const std::string &name, const auto &callback) {
                const auto path = directory / name;
                std::ofstream out(path, std::ios::binary);
                if (!out) throw std::runtime_error("cannot create CPU artifact: " + path.string());
                if (name == "Makefile")
                {
                    // Recipe lines rely on literal leading tabs; never re-indent.
                    callback(out);
                    out.flush();
                }
                else
                {
                    IndentBuffer indent(out.rdbuf());
                    std::ostream formatted(&indent);
                    callback(formatted);
                    formatted.flush();
                    if (!formatted) throw std::runtime_error("cannot write CPU artifact: " + path.string());
                }
                if (!out) throw std::runtime_error("cannot write CPU artifact: " + path.string());
                artifacts.push_back(path.string());
            };
            file(prefix_ + "_runtime.hpp", [&](auto &out) { emit::writeGrhSimRuntime(out, {.waveform = waveform_, .systemTasks = hasSystemTasks_}); });
            file(prefix_ + ".hpp", [&](auto &out) { header(out); });
            // M5d-7: one .cpp per planned translation unit; the generated
            // Makefile lists them all, so `make -j` compiles the units in
            // parallel and archives one static library.
            for (const auto &unit : tuPlan_.units)
            {
                const auto source = prefix_ + "_" + unit.name + ".cpp";
                sources.push_back(source);
                file(source, [&](auto &out) { unitCpp(out, unit); });
            }
            // Waveform setup tables live outside the C8 TU plan (the plan is
            // emit-option independent): one extra .cpp per kWaveChunksPerFile
            // setup chunks.
            if (waveform_)
            {
                const uint32_t fileCount = (waveChunkCount() + kWaveChunksPerFile - 1) / kWaveChunksPerFile;
                for (uint32_t fileIndex = 0; fileIndex < fileCount; ++fileIndex)
                {
                    const auto source = prefix_ + "_wave_" + std::to_string(fileIndex) + ".cpp";
                    sources.push_back(source);
                    file(source, [&](auto &out) { waveSetupFile(out, fileIndex); });
                }
            }
            file("Makefile", [&](auto &out) {
                // Generated code uses C23 _BitInt for wide values: clang is
                // required (gcc's C++ frontend does not support it). make's
                // built-in CXX default is g++, and ?= does not override a
                // built-in default — replace only the default origin so an
                // env or command-line CXX still wins.
                out << "ifeq ($(origin CXX),default)\nCXX := $(shell command -v clang++ || echo c++)\nendif\nAR ?= ar\n"
                    // stage-07: -fno-strict-aliasing is REQUIRED for
                    // compile-time sanity: millions of distinct struct-member
                    // accesses send clang's type-based alias analysis
                    // quadratic (measured on XS aggregate: single TUs spin in
                    // TypeBasedAAResult::alias for 45min+; the flag cuts them
                    // to seconds). The generated code never type-puns, so the
                    // flag costs nothing semantically.
                    << "CXXFLAGS ?= -std=c++20 -O3 -fno-strict-aliasing\n";
                if (waveform_)
                    out << "CC ?= cc\nCFLAGS ?= -O2 -D_GNU_SOURCE\nLIBFST_SRC_DIR := " << libfstSourceDir() << '\n';
                out << "SOURCES :=";
                for (const auto &source : sources) out << ' ' << source;
                out << "\nOBJECTS := $(SOURCES:.cpp=.o)\n";
                if (waveform_) out << "FST_OBJECTS := fstapi.o fastlz.o lz4.o\n";
                out << "LIB := lib" << prefix_ << ".a\n";
                // stage-07: precompile the (huge) model header once — every TU
                // used to reparse ~160MB of store struct declarations (measured
                // ~2.3s/TU on XiangShan; PCH saves ~0.9s/TU after load).
                out << "PCH := " << prefix_ << ".hpp.pch\n";
                out << "all: $(LIB)\n"
                    << "$(LIB): $(OBJECTS)" << (waveform_ ? " $(FST_OBJECTS)" : "") << "\n\t$(AR) rcs $@ $^\n"
                    << "$(PCH): " << prefix_ << ".hpp " << prefix_ << "_runtime.hpp\n"
                    << "\t$(CXX) $(CXXFLAGS)" << (waveform_ ? " -I$(LIBFST_SRC_DIR)" : "") << " -x c++-header $< -o $@\n"
                    << "%.o: %.cpp $(PCH)\n"
                    << "\t$(CXX) $(CXXFLAGS)" << (waveform_ ? " -I$(LIBFST_SRC_DIR)" : "") << " -include-pch $(PCH) -c $< -o $@\n";
                if (waveform_)
                    for (const char *unit : {"fstapi", "fastlz", "lz4"})
                        out << unit << ".o: $(LIBFST_SRC_DIR)/" << unit << ".c\n"
                            << "\t$(CC) $(CFLAGS) -I$(LIBFST_SRC_DIR) -c $< -o $@\n";
                out << ".PHONY: all\n";
            });
            return {true, false, std::move(artifacts)};
        }

        class EmitPhaseCppPass final : public Pass
        {
        public:
            explicit EmitPhaseCppPass(std::filesystem::path path, bool waveform, bool memEnableBitmap,
                                      bool memWriteOnWrite)
                : Pass("cpu.st.emit-cpp", PassKind::Emit), path_(std::move(path)), waveform_(waveform),
                  memEnableBitmap_(memEnableBitmap), memWriteOnWrite_(memWriteOnWrite) {}
            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                return emitSixPhaseCpuCpp(model, path_, diagnostics, waveform_, memEnableBitmap_, memWriteOnWrite_);
            }
        private:
            std::filesystem::path path_;
            bool waveform_ = false;
            bool memEnableBitmap_ = true;
            bool memWriteOnWrite_ = false;
        };
    }

    PassResult emitSixPhaseCpuCpp(const GrhSimModel &model, const std::filesystem::path &directory,
                                  wolvrix::lib::diag::Diagnostics &diagnostics, bool waveform, bool memEnableBitmap,
                                  bool memWriteOnWrite)
    {
        if (!verifyGrhSimModel(model, defaultDialectRegistry(), diagnostics)) return {false, false, {}};
        const auto *mapping = model.cpuMapping();
        if (!mapping || mapping->stage != CpuMappingStage::TranslationUnits || !mapping->translationUnits)
        {
            diagnostics.error("CPU six-phase emit requires a TranslationUnits-stage cpu mapping "
                              "(run cpu.st.plan-translation-units)", "cpu.st.emit-cpp");
            return {false, false, {}};
        }
        if (!mapping->dataLayout || !mapping->dataLayout->namedStores || !mapping->schedule)
        {
            diagnostics.error("CPU six-phase emit requires the named-store layout and phase schedule", "cpu.st.emit-cpp");
            return {false, false, {}};
        }
#if !WOLVRIX_HAVE_LIBFST
        if (waveform)
        {
            diagnostics.error("waveform emission requested, but wolvrix was built without libfst support",
                              "cpu.st.emit-cpp");
            return {false, false, {}};
        }
#endif
        try
        {
            SixPhaseEmitter emitter(model, waveform, memEnableBitmap, memWriteOnWrite);
            emitter.validate();
            return emitter.write(directory);
        }
        catch (const std::exception &error)
        { diagnostics.error(error.what(), "cpu.st.emit-cpp"); return {false, false, {}}; }
    }

    void registerCpuPhaseEmitPasses(PassRegistry &registry)
    {
        std::string error;
        if (!registry.registerPass("cpu.st.emit-cpp", PassKind::Emit,
            [](std::span<const std::string_view> args, std::string &error) -> std::unique_ptr<Pass> {
                constexpr std::string_view usage =
                    "expected --output <empty-directory> [--waveform <off|declared-symbols>] [--perf <off|eval>] "
                    "[--mem-enable-bitmap <on|off>] [--mem-write-activate <on-change|on-write>]";
                if (args.empty() || args.size() % 2) { error = std::string(usage); return {}; }
                std::filesystem::path output;
                bool waveform = false;
                bool memEnableBitmap = true;
                bool memWriteOnWrite = true;
                for (std::size_t i = 0; i < args.size(); i += 2)
                {
                    const auto key = args[i];
                    const auto value = args[i + 1];
                    if (key == "--output" && output.empty() && !value.empty()) { output = value; continue; }
                    if (key == "--waveform" && (value == "off" || value == "declared-symbols"))
                    {
                        waveform = value == "declared-symbols";
                        continue;
                    }
                    // Counters are compiled in by WOLVRIX_GRHSIM_PERF at model
                    // build time; the option is accepted for CLI compatibility.
                    if (key == "--perf" && (value == "off" || value == "eval")) continue;
                    if (key == "--mem-enable-bitmap" && (value == "on" || value == "off"))
                    {
                        memEnableBitmap = value == "on";
                        continue;
                    }
                    if (key == "--mem-write-activate" && (value == "on-change" || value == "on-write"))
                    {
                        memWriteOnWrite = value == "on-write";
                        continue;
                    }
                    error = std::string(usage); return {};
                }
                if (output.empty()) { error = std::string(usage); return {}; }
                return std::make_unique<EmitPhaseCppPass>(std::move(output), waveform, memEnableBitmap,
                                                          memWriteOnWrite);
            }, error)) throw std::logic_error(error);
    }
}
