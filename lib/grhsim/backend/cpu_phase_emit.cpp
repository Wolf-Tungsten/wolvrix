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
            explicit SixPhaseEmitter(const GrhSimModel &model, bool waveform = false)
                : model_(model), mapping_(*model.cpuMapping()), tree_(mapping_.partitionTree),
                  layout_(*mapping_.dataLayout), schedule_(*mapping_.schedule), stores_(*layout_.namedStores),
                  tuPlan_(*mapping_.translationUnits),
                  prefix_("grhsim_" + identifier(model.text(model.name()))),
                  class_("GrhSIM_" + identifier(model.text(model.name()))),
                  waveform_(waveform),
                  ordinalOf_(tree_.partitions.size() + 1, ~0u),
                  regFieldByState_(model.states().size() + 1),
                  memFieldByState_(model.states().size() + 1),
                  boundaryByValue_(model.values().size() + 1),
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
                    case CpuNamedStoreKind::RegLatch: regLatchStore_ = &store; break;
                    case CpuNamedStoreKind::Mem: memStore_ = &store; break;
                    case CpuNamedStoreKind::Boundary: boundaryStore_ = &store; break;
                    case CpuNamedStoreKind::PrevEvent: prevEventStore_ = &store; break;
                    case CpuNamedStoreKind::EventAct: eventActStore_ = &store; break;
                    case CpuNamedStoreKind::TimeslotTrigger: timeslotStore_ = &store; break;
                    case CpuNamedStoreKind::ActiveFlags: activeStore_ = &store; break;
                    }
                if (!regLatchStore_ || !memStore_ || !boundaryStore_ || !prevEventStore_ || !eventActStore_ ||
                    !timeslotStore_ || !activeStore_)
                    throw std::runtime_error("CPU six-phase emit requires all seven named stores");

                order_ = generalSupernodeOrder(tree_);
                supernodeCount_ = static_cast<uint32_t>(order_.size());
                for (uint32_t i = 0; i < supernodeCount_; ++i) ordinalOf_[order_[i].index] = i;
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

                // V2 (M2): the P_event activation map (act -> non-sink
                // supernodes holding an op with that act) replaces the pre-v2
                // influence bitmaps; emitEdgeDet ORs the fired acts' words
                // straight into dataActiveFlag (eventActiveFlag is gone).
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

                for (const auto &field : regLatchStore_->fields) regFieldByState_[field.state.index] = &field;
                for (const auto &field : memStore_->fields) memFieldByState_[field.state.index] = &field;
                for (const auto &field : boundaryStore_->fields)
                {
                    if (field.value) boundaryByValue_[field.value.index] = &field;
                    else if (field.aux < model_.inputs().size()) boundaryByInput_[field.aux + 1] = &field;
                    // M5d-7: string-typed boundary fields live in the separate
                    // boundaryStrings array so the store struct stays trivially
                    // copyable (memset reset in init); a 684k-field struct
                    // value-initializing one std::string member never
                    // finishes -O1 compilation (measured on XS).
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
                for (const auto &field : boundaryStore_->fields)
                {
                    if (!field.value) continue;
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
                        case CpuEmitChunkKind::Mem: chunkIds_[key] = memChunks_.size(); memChunks_.push_back(chunk); break;
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
                }
                // Init stream metadata (the cpu_init_<k> chunks iterate it by
                // position): flattened steps, then the constant-boundary
                // preloads in store offset order, then the prevEvent inits in
                // act order, then the regLatchStoreNext sync.
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

            const CpuNamedStore *regLatchStore_ = nullptr;
            const CpuNamedStore *memStore_ = nullptr;
            const CpuNamedStore *boundaryStore_ = nullptr;
            const CpuNamedStore *prevEventStore_ = nullptr;
            const CpuNamedStore *eventActStore_ = nullptr;
            const CpuNamedStore *timeslotStore_ = nullptr;
            const CpuNamedStore *activeStore_ = nullptr;

            std::vector<PartitionId> order_;
            std::vector<uint32_t> ordinalOf_;
            uint32_t supernodeCount_ = 0;
            std::vector<std::vector<OpId>> supernodeOps_;
            std::vector<OpId> eventOps_, outputOps_;

            std::vector<const CpuStoreField *> regFieldByState_, memFieldByState_;
            std::vector<const CpuStoreField *> boundaryByValue_, boundaryByInput_, prevByAct_;
            std::map<uint32_t, DetInfo> detByAct_;
            std::map<uint32_t, const CpuEventActivation *> activationByAct_;
            std::vector<std::vector<uint32_t>> triggersByAct_;
            uint32_t maxAct_ = 0, maxTimeslotFlag_ = 0;

            // ----- M5d-7 multi-TU emit plan consumption -----
            uint32_t coreChunks_ = 0;
            std::vector<CpuEmitChunk> initChunks_, eventChunks_, scanChunks_, supernodeChunks_,
                memChunks_, outputChunks_, dumpChunks_;
            // (kind, offset) -> the chunk's global per-kind index, which is the
            // chunk function suffix (cpu_init_<i>, pEvent_c<i>, ...).
            std::unordered_map<uint64_t, uint32_t> chunkIds_;
            // Init stream: flattened (state, step) pairs, then constant-boundary
            // preloads (store offset order), then prevEvent inits (act order),
            // then the regLatchStoreNext sync line.
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
            std::unordered_map<uint32_t, uint32_t> onceTasks_;
            std::unordered_map<uint32_t, uint32_t> randomFunctions_;
            std::set<uint32_t> randomSupernodes_;
            uint32_t randomSampleCount_ = 0;

            // Per-function value locals: values with a live cpu_v<index> local in
            // the function currently being emitted.
            mutable std::vector<char> activeLocals_;
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
            std::string cppStoreType(CpuTypeId id) const;
            uint64_t storageBytes(const Type &type) const;

            // ----- FST waveform (declared-symbols mode) -----
            // One dumpable declared symbol: ports read from the interface
            // members, registers/wires from their store fields. ref is the
            // member-access expression (its address feeds the change detect).
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

            // ----- Guards / activation -----
            // OR of the op's event_acts bits in eventActStore; "true" when event-free.
            std::string actBitsGuard(std::span<const int64_t> acts) const;
            std::string actGuard(const SimOp &op) const;
            std::string callCondition(ValueId condition) const;
            // Fanout activation statements. When a firing supernode ordinal is
            // given, later ordinals raise dataActiveFlag directly (same-round
            // successor firing) and earlier/self ordinals queue into
            // dataActiveFlagNext; otherwise every target raises `array`.
            void activateOrdinals(std::ostream &out, std::span<const uint32_t> ordinals,
                                  std::string_view array) const;
            void activateFanout(std::ostream &out, std::span<const uint32_t> ordinals,
                                uint32_t current) const;
            // Compare-store of a computed boundary value; on a real change the
            // supernodeFanout ordinals fire (same-round split by current).
            void publishBoundary(std::ostream &out, ValueId result, const std::string &expr,
                                 uint32_t current) const;

            // ----- Per-op emission -----
            void emitCompute(std::ostream &out, const SimOp &op, uint32_t current) const;
            void emitEdgeDet(std::ostream &out, const SimOp &op) const;
            void emitRegWrite(std::ostream &out, const SimOp &op, uint32_t current) const;
            void emitMemWrite(std::ostream &out, const SimOp &op, const CpuMemWritePlanEntry &plan) const;
            // General-phase regLatch-class array writes (M5d-6): NBA merge
            // into regLatchStoreNext inside the owning supernode.
            void emitGeneralMemWrite(std::ostream &out, const SimOp &op, uint32_t current) const;
            void emitSystemTask(std::ostream &out, const SimOp &op) const;
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
            // Boundary field storage reference: boundaryValueStore.<name>, or
            // boundaryStrings[<i>] for hoisted string fields.
            std::string boundaryRef(const CpuStoreField *field) const;
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
            void pMemBody(std::ostream &out) const;
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
            // fields and regLatchStore/memStore are stable across a chunk
            // sequence). memRead results are NOT freely re-readable: the
            // re-expression re-reads the address operand, whose local may be
            // dead — so memRead results spill like compute values. The commit
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
            return "boundaryValueStore." + std::string(model_.text(field->name));
        }
        void SixPhaseEmitter::frameStruct(std::ostream &out, std::string_view name,
                                          const std::vector<uint32_t> &fields) const
        {
            out << "struct " << name << "{\n";
            for (const auto index : fields)
                out << cppType(type(ValueId{index, 0})) << " v" << index << "{};\n";
            out << "};\n";
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
                if (!field)
                    throw std::runtime_error("CPU six-phase emit output latchWrite target is not a regLatch state");
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
            return type.kind == TypeKind::String || (type.kind == TypeKind::Array &&
                containsString(model_.types()[type.elementType.index - 1]));
        }
        bool SixPhaseEmitter::isScalarLogic(const Type &type) const
        {
            return type.kind == TypeKind::Logic && type.domain == LogicDomain::TwoState &&
                type.width > 0 && type.width <= 64;
        }
        std::string SixPhaseEmitter::cppType(const Type &type) const
        {
            if (type.kind == TypeKind::Array)
                return "std::array<" + cppType(model_.types()[type.elementType.index - 1]) + "," + std::to_string(type.count) + ">";
            if (type.kind == TypeKind::String) return "std::string";
            if (type.kind == TypeKind::Real) return "double";
            if (type.kind != TypeKind::Logic)
                throw std::runtime_error("CPU six-phase emit non-logic scalar storage is not implemented");
            if (type.domain != LogicDomain::TwoState || type.width == 0 || type.width > 64)
            {
                if (type.domain != LogicDomain::TwoState || type.width == 0)
                    throw std::runtime_error("CPU six-phase emit scalar logic width/domain is not implemented");
                return "std::array<std::uint64_t," + std::to_string((type.width + 63u) / 64u) + ">";
            }
            if (type.width == 1 && !type.isSigned) return "bool";
            return "std::" + std::string(type.isSigned ? "int" : "uint") +
                   std::to_string(type.width <= 8 ? 8 : type.width <= 16 ? 16 : type.width <= 32 ? 32 : 64) + "_t";
        }
        std::string SixPhaseEmitter::cppStoreType(CpuTypeId id) const
        {
            const auto &type = layout_.types[id.index - 1];
            switch (type.kind)
            {
            case CpuTypeKind::Bool: return "bool";
            case CpuTypeKind::UInt:
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
            if (type.kind == TypeKind::Array)
                return storageBytes(model_.types()[type.elementType.index - 1]) * type.count;
            if (type.kind != TypeKind::Logic || type.width == 0 || type.domain != LogicDomain::TwoState)
                throw std::runtime_error("CPU six-phase emit requires two-state logic storage");
            return type.width > 64 ? ((uint64_t(type.width) + 63u) / 64u) * 8u :
                   type.width <= 8 ? 1 : type.width <= 16 ? 2 : type.width <= 32 ? 4 : 8;
        }
        std::string SixPhaseEmitter::normalize(std::string expression, const Type &type) const
        {
            if (type.kind == TypeKind::String || type.kind == TypeKind::Real) return expression;
            if (type.kind == TypeKind::Logic && type.domain == LogicDomain::TwoState && type.width > 64) return expression;
            return "static_cast<" + cppType(type) + ">(" + (type.isSigned ? "grhsim_sign_extend_i64(" : "grhsim_trunc_u64(") +
                   expression + "," + std::to_string(type.width) + "))";
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
                const auto words = (type.width + 63u) / 64u;
                std::string result = "std::array<std::uint64_t," + std::to_string(words) + ">{";
                const auto *raw = parsed.getRawPtr();
                for (uint32_t i = 0; i < words; ++i)
                    result += (i ? "," : "") + std::string("UINT64_C(") + std::to_string(raw[i]) + ")";
                return result + "}";
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
                out << "for(std::size_t w=0;w<" << (type.width + 63u) / 64u << ";++w) " << destination
                    << "[w]=grhsim_splitmix64_next(" << rng << ");\n"
                    << "grhsim_trunc_words(" << destination << ',' << type.width << ");\n";
            }
        }

        std::string SixPhaseEmitter::read(ValueId value) const
        {
            if (const auto it = staticScalars_.find(value.index); it != staticScalars_.end()) return it->second;
            if (type(value).kind == TypeKind::String)
                if (const auto it = staticStrings_.find(value.index); it != staticStrings_.end()) return it->second;
            // Frame-spilled (M5d-7) values resolve through the chunk driver's
            // frame in every chunk; plain locals only live in one function.
            if (crossingLocals_[value.index]) return localRef(value);
            if (activeLocals_[value.index]) return localRef(value);
            if (const auto *field = boundaryByValue_[value.index])
                return boundaryRef(field);
            const auto producer = producers_[value.index];
            if (!producer)
                throw std::runtime_error("CPU six-phase emit value has no producer: v" + std::to_string(value.index));
            const auto &op = model_.operations()[producer.index - 1];
            const auto name = model_.text(op.opType);
            if (name == "core.input.read" || name == "core.state.read" || name == "core.state.memRead" ||
                name == "core.compute.constant")
                return expression(op);
            throw std::runtime_error("CPU six-phase emit value is not readable: v" + std::to_string(value.index) +
                                     " produced by " + std::string(name) + " (op " +
                                     std::to_string(producer.index) + ")");
        }
        void SixPhaseEmitter::declareLocal(std::ostream &out, ValueId result, const std::string &expr) const
        {
            // Frame-spilled values assign their pre-declared frame field; the
            // driver value-initializes the frame before the first chunk runs.
            if (crossingLocals_[result.index])
            {
                out << localRef(result) << '=' << expr << ";\n";
                return;
            }
            out << "const " << cppType(type(result)) << " cpu_v" << result.index << '=' << expr << ";\n";
            activeLocals_[result.index] = 1;
        }

        template <typename Raw, typename TypeOf, typename Number>
        std::string SixPhaseEmitter::scalarExpression(std::string_view kind, const Type &result, std::size_t arity,
                                                      const Raw &raw, const TypeOf &typeOf, const Number &number) const
        {
            const auto width = result.width;
            const auto cast = [&](std::size_t i, uint32_t width) {
                const auto expr = raw(i); const auto &source = typeOf(i);
                return "grhsim_cast_u64(" + expr + "," + std::to_string(source.width) + "," + std::to_string(width) +
                       "," + (source.isSigned ? "true" : "false") + ")";
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
                    return "grhsim_mux_u64(" + raw(0) + "," + cast(1, width) + "," + cast(2, width) + ")";
                return "(" + raw(0) + "?" + cast(1, width) + ":" + cast(2, width) + ")";
            }
            if (kind == "prioritySelect")
            {
                const std::size_t count = (arity - 1) / 2;
                if (count < 3 || count > 64)
                    throw std::runtime_error("CPU six-phase emit prioritySelect condition count is out of range");
                std::string expr = cast(2 * count, width);
                for (std::size_t i = count; i-- > 0;)
                    expr = "grhsim_mux_u64(" + raw(i) + "," + cast(count + i, width) + "," + expr + ")";
                return expr;
            }
            if (kind == "bitSelect") {
                if (width == 1 && !result.isSigned)
                    return "((" + raw(0) + "&" + raw(1) + ")|((" + raw(0) + "^1)&" + raw(2) + "))";
                return "((" + cast(0, width) + "&" + cast(1, width) + ")|(~" + cast(0, width) +
                       "&" + cast(2, width) + "))";
            }
            if (kind == "shl" || kind == "lshr" || kind == "ashr")
                return "grhsim_" + std::string(kind) + "_u64(" + cast(0, width) + ",grhsim_index_words(" + raw(1) + "," + std::to_string(width) + ")," + std::to_string(width) + ")";
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
                    std::string prefix = "([&](){";
                    std::array<std::string, 2> pointers;
                    for (std::size_t i = 0; i < 2; ++i)
                    {
                        if (typeOf(i).width > 64) pointers[i] = "(" + raw(i) + ").data()";
                        else
                        {
                            const auto local = "cpu_cmp_" + std::to_string(i);
                            prefix += "const std::uint64_t " + local + "=static_cast<std::uint64_t>(" + raw(i) + ");";
                            pointers[i] = "&" + local;
                        }
                    }
                    return prefix + "return grhsim_compare_extended_words(" + pointers[0] + "," +
                        std::to_string((typeOf(0).width + 63u) / 64u) + "," + std::to_string(typeOf(0).width) + "," +
                        pointers[1] + "," + std::to_string((typeOf(1).width + 63u) / 64u) + "," +
                        std::to_string(typeOf(1).width) + "," +
                        (typeOf(0).isSigned && typeOf(1).isSigned ? "true" : "false") + ")" +
                        std::string(it->second) + "0;}())";
                }
                return "(grhsim_compare_" + std::string(typeOf(0).isSigned && typeOf(1).isSigned ? "signed" : "unsigned") +
                       "_u64(" + cast(0, compareWidth) + "," + cast(1, compareWidth) + "," + std::to_string(compareWidth) + ")" +
                       std::string(it->second) + "0)";
            }
            static const std::map<std::string_view, std::string_view> reduces{
                {"reduceAnd", "and"}, {"reduceNand", "nand"}, {"reduceOr", "or"}, {"reduceNor", "nor"},
                {"reduceXor", "xor"}, {"reduceXnor", "xnor"}};
            if (auto it = reduces.find(kind); it != reduces.end())
            {
                const auto operandWidth = typeOf(0).width;
                if (operandWidth > 64)
                    return "grhsim_reduce_" + std::string(it->second) + "_words(" + raw(0) + "," + std::to_string(operandWidth) + ")";
                return "grhsim_reduce_" + std::string(it->second) + "_u64(" + raw(0) + "," + std::to_string(operandWidth) + ")";
            }
            if (kind == "sliceStatic" || kind == "sliceDynamic" || kind == "sliceArray")
            {
                if (typeOf(0).width > 64)
                {
                    const auto srcWords = (typeOf(0).width + 63u) / 64u;
                    std::string start = kind == "sliceStatic" ? std::to_string(number("sliceStart")) : cast(1, typeOf(1).width);
                    if (kind == "sliceArray") start = "(" + start + ")*" + std::to_string(width);
                    return "grhsim_slice_words_u64<" + std::to_string(srcWords) + ">( " + raw(0) + "," + start + "," + std::to_string(width) + ")";
                }
                std::string start = kind == "sliceStatic" ? std::to_string(number("sliceStart")) : cast(1, typeOf(1).width);
                if (kind == "sliceArray")
                    start = "((" + start + ")>=64/" + std::to_string(width) + "+1?64:(" + start + ")*" + std::to_string(width) + ")";
                return "grhsim_slice_dynamic_u64(grhsim_trunc_u64(" + raw(0) + "," + std::to_string(typeOf(0).width) +
                       ")," + start + "," + std::to_string(width) + ")";
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
                    expr = "grhsim_concat_u64(" + expr + "," + std::to_string(total) + "," + raw(index) + "," +
                           std::to_string(typeOf(index).width) + ")";
                    total += typeOf(index).width;
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
                return "boundaryValueStore." + std::string(model_.text(boundaryByInput_[refs[0].index]->name));
            }
            if (name == "core.state.read")
            {
                const auto refs = model_.objectRefs(op);
                if (refs.size() != 1 || refs[0].kind != ObjectKind::State)
                    throw std::runtime_error("CPU six-phase emit state read requires one state reference");
                if (refs[0].index < regFieldByState_.size() && regFieldByState_[refs[0].index])
                    return "regLatchStore." + std::string(model_.text(regFieldByState_[refs[0].index]->name));
                if (refs[0].index < memFieldByState_.size() && memFieldByState_[refs[0].index])
                    return "memStore." + std::string(model_.text(memFieldByState_[refs[0].index]->name));
                throw std::runtime_error("CPU six-phase emit state read has no named-store field");
            }
            if (name == "core.state.memRead")
            {
                const auto refs = model_.objectRefs(op);
                if (refs.size() != 1 || refs[0].kind != ObjectKind::State)
                    throw std::runtime_error("CPU memory read requires one array state reference");
                if (operands.size() != 1)
                    throw std::runtime_error("CPU memory read requires one index operand");
                // mem-class arrays live in the single memStore; regLatch-class
                // arrays (M5d-6) read the currently visible regLatch row (NBA:
                // same-round writes are not visible until P_publish).
                if (refs[0].index < memFieldByState_.size() && memFieldByState_[refs[0].index])
                    return "memStore." + std::string(model_.text(memFieldByState_[refs[0].index]->name)) +
                           "[static_cast<std::size_t>(" + raw(0) + ")]";
                if (refs[0].index < regFieldByState_.size() && regFieldByState_[refs[0].index])
                    return "regLatchStore." + std::string(model_.text(regFieldByState_[refs[0].index]->name)) +
                           "[static_cast<std::size_t>(" + raw(0) + ")]";
                throw std::runtime_error("CPU six-phase emit mem read has no named-store field");
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
                       ");cpu_random_sampled[" + std::to_string(slot) + "]=true;}return cpu_random_values[" +
                       std::to_string(slot) + "]; }())";
            }
            if (!name.starts_with("core.compute.")) throw std::runtime_error("CPU six-phase emit unsupported operation: " + std::string(name));
            const auto kind = name.substr(13);
            if (width > 64)
            {
                const auto words = std::to_string((width + 63u) / 64u);
                if (kind == "assign") return raw(0);
                if (kind == "and" || kind == "or" || kind == "xor" || kind == "xnor")
                    return "grhsim_" + std::string(kind) + "_words(" + raw(0) + "," + raw(1) + "," + std::to_string(width) + ")";
                if (kind == "not") return "grhsim_not_words(" + raw(0) + "," + std::to_string(width) + ")";
                if (kind == "mux") return "grhsim_mux_words(" + raw(0) + "," + raw(1) + "," + raw(2) + "," + std::to_string(width) + ")";
                if (kind == "shl" || kind == "lshr" || kind == "ashr")
                    return "grhsim_" + std::string(kind) + "_words(" + raw(0) + "," + raw(1) + "," + std::to_string(width) + ")";
                if (kind == "add" || kind == "sub")
                    return "grhsim_" + std::string(kind) + "_words(" + raw(0) + "," + raw(1) + "," + std::to_string(width) + ")";
                if (kind == "replicate")
                    return "grhsim_replicate_words<" + words + "," + std::to_string((type(operands[0]).width + 63u) / 64u) + ">( " + raw(0) + "," +
                           std::to_string(type(operands[0]).width) + "," + std::to_string(number("rep")) + "," + std::to_string(width) + ")";
                if (kind == "concat" && operands.size() >= 2)
                {
                    std::string joined = raw(0); uint64_t joinedWidth = type(operands[0]).width;
                    for (std::size_t i = 1; i < operands.size(); ++i)
                    {
                        const auto rhsWidth = type(operands[i]).width;
                        if (joinedWidth <= 64 && rhsWidth <= 64 && joinedWidth + rhsWidth > 64)
                            joined = "grhsim_concat_scalar_scalar_wide<" + words + ">( " + joined + "," + std::to_string(joinedWidth) + "," + raw(i) + "," +
                                     std::to_string(rhsWidth) + "," + std::to_string(std::min<uint32_t>(width, joinedWidth + rhsWidth)) + ")";
                        else if (joinedWidth <= 64 && rhsWidth <= 64)
                            joined = "grhsim_concat_u64(" + joined + "," + std::to_string(joinedWidth) + "," + raw(i) + "," +
                                     std::to_string(rhsWidth) + ")";
                        else if (rhsWidth <= 64)
                            joined = "grhsim_concat_wide_scalar<" + words + ">( " + joined + "," + std::to_string(joinedWidth) + "," +
                                     raw(i) + "," + std::to_string(rhsWidth) + "," + std::to_string(std::min<uint32_t>(width, joinedWidth + rhsWidth)) + ")";
                        else if (joinedWidth <= 64)
                            joined = "grhsim_concat_scalar_wide<" + words + "," + std::to_string((rhsWidth + 63u) / 64u) + ">( " + joined + "," +
                                     std::to_string(joinedWidth) + "," + raw(i) + "," + std::to_string(rhsWidth) + "," +
                                     std::to_string(std::min<uint32_t>(width, joinedWidth + rhsWidth)) + ")";
                        else
                            joined = "grhsim_concat_words<" + words + ">( " + joined + "," + std::to_string(joinedWidth) + "," + raw(i) + "," +
                                     std::to_string(rhsWidth) + "," + std::to_string(std::min<uint32_t>(width, joinedWidth + rhsWidth)) + ")";
                        joinedWidth += type(operands[i]).width;
                    }
                    return joined;
                }
                if ((kind == "sliceStatic" || kind == "sliceDynamic") && type(operands[0]).width > 64)
                {
                    const auto srcWords = (type(operands[0]).width + 63u) / 64u;
                    const auto start = kind == "sliceStatic" ? std::to_string(number("sliceStart")) : raw(1);
                    return "grhsim_slice_words<" + words + "," + std::to_string(srcWords) + ">( " + raw(0) + "," + start + "," + std::to_string(width) + ")";
                }
                if (kind == "sliceArray" && type(operands[0]).width > 64)
                {
                    const auto srcWords = (type(operands[0]).width + 63u) / 64u;
                    return "grhsim_slice_words<" + words + "," + std::to_string(srcWords) + ">( " + raw(0) + ",(" + raw(1) + ")*" +
                           std::to_string(width) + "," + std::to_string(width) + ")";
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

        // ----- Guards / activation -----
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
        std::string SixPhaseEmitter::callCondition(ValueId condition) const
        {
            const auto &target = type(condition);
            return target.width > 64 ? "grhsim_reduce_or_words(" + read(condition) + ',' +
                std::to_string(target.width) + ')' : read(condition);
        }
        void SixPhaseEmitter::activateOrdinals(std::ostream &out, std::span<const uint32_t> ordinals,
                                               std::string_view array) const
        {
            for (const auto ordinal : ordinals) out << array << '[' << ordinal << "]=1;\n";
        }
        void SixPhaseEmitter::activateFanout(std::ostream &out, std::span<const uint32_t> ordinals,
                                             uint32_t current) const
        {
            // Same-round split: a successor later in the supernode scan fires this
            // round (dataActiveFlag); an earlier/self successor waits for the next
            // round (dataActiveFlagNext). current == ~0 (outside P_general) queues
            // every target into dataActiveFlagNext.
            const auto dataActive = model_.text(activeStore_->fields[0].name);
            const auto dataNext = model_.text(activeStore_->fields[1].name);
            for (const auto ordinal : ordinals)
                out << (ordinal != ~0u && ordinal > current ? dataActive : dataNext) << '[' << ordinal << "]=1;\n";
        }
        void SixPhaseEmitter::publishBoundary(std::ostream &out, ValueId result, const std::string &expr,
                                              uint32_t current) const
        {
            const auto *field = boundaryByValue_[result.index];
            if (!field) { declareLocal(out, result, expr); return; }
            const auto slot = boundaryRef(field);
            out << "{const auto cpu_value=" << expr << ";if(" << slot << "!=cpu_value){" << slot << "=cpu_value;\n";
            activateFanout(out, supernodeFanout_[result.index], current);
            out << "}}\n";
        }

        // ----- Per-op emission -----
        void SixPhaseEmitter::emitCompute(std::ostream &out, const SimOp &op, uint32_t current) const
        {
            const auto name = model_.text(op.opType);
            if (name == "core.system.task") { emitSystemTask(out, op); return; }
            if (name == "core.dpi.call") { emitDpiCall(out, op, current); return; }
            if (name == "core.state.regWrite" || name == "core.state.latchWrite") { emitRegWrite(out, op, current); return; }
            if (name == "core.state.memWrite" || name == "core.state.memFill" ||
                name == "core.state.memAssign" || name == "core.state.memWriteSeq")
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
            if (resultType.kind == TypeKind::Logic && resultType.domain == LogicDomain::TwoState && resultType.width > 64 &&
                name.starts_with("core.compute."))
            {
                const auto kind = name.substr(std::string_view("core.compute.").size());
                const auto operands = model_.operands(op);
                const auto words = (resultType.width + 63u) / 64u;
                const auto *field = boundaryByValue_[result.index];
                std::string dst;
                if (field) dst = boundaryRef(field);
                else
                {
                    dst = localRef(result);
                    if (!crossingLocals_[result.index])
                    {
                        out << cppType(resultType) << ' ' << dst << ";\n";
                        activeLocals_[result.index] = 1;
                    }
                }
                if (kind == "concat")
                {
                    out << "{\n";
                    std::string build = dst;
                    if (field)
                    {
                        out << cppType(resultType) << " cpu_concat{};\n";
                        build = "cpu_concat";
                    }
                    else out << dst << ".fill(0);\n";
                    uint64_t offset = 0;
                    for (std::size_t i = operands.size(); i > 0 && offset < resultType.width; --i)
                    {
                        const auto operand = operands[i - 1];
                        const auto width = std::min<uint64_t>(type(operand).width, resultType.width - offset);
                        out << (type(operand).width > 64 ? "grhsim_insert_words" : "grhsim_insert_scalar_words")
                            << '(' << build << ',' << offset << ',' << read(operand) << ',' << width << ");\n";
                        offset += width;
                    }
                    if (field)
                    {
                        out << "if(" << dst << "!=cpu_concat){" << dst << "=cpu_concat;\n";
                        activateFanout(out, supernodeFanout_[result.index], current);
                        out << "}\n";
                    }
                    out << "}\n";
                    return;
                }
                if (kind == "replicate")
                {
                    const auto operand = operands[0];
                    const auto &sourceType = type(operand);
                    const auto sourceWords = (sourceType.width + 63u) / 64u;
                    const auto *rep = parameter<int64_t>(model_, model_.parameters(op), "rep");
                    if (!rep || *rep < 0) throw std::runtime_error("missing or negative CPU replication parameter");
                    const auto call = (sourceType.width > 64 ?
                        "cpu_replicate_words_changed<" + std::to_string(words) + "," + std::to_string(sourceWords) + ">( " :
                        "cpu_replicate_words_changed<" + std::to_string(words) + ">( ") + read(operand) + "," +
                        std::to_string(sourceType.width) + "," + std::to_string(*rep) + "," +
                        std::to_string(resultType.width) + "," + dst + ")";
                    if (field)
                    {
                        out << "if(" << call << "){\n";
                        activateFanout(out, supernodeFanout_[result.index], current);
                        out << "}\n";
                    }
                    else out << "(void)" << call << ";\n";
                    return;
                }
                const bool pointerOperation = kind == "and" || kind == "or" || kind == "xor" || kind == "not" ||
                    kind == "shl" || kind == "lshr" || kind == "ashr" || kind == "add" || kind == "sub";
                if (pointerOperation)
                {
                    out << "{\n";
                    std::set<uint32_t> scalars;
                    const bool binary = kind == "and" || kind == "or" || kind == "xor" || kind == "add" || kind == "sub";
                    for (std::size_t i = 0; i < (binary ? 2u : 1u); ++i)
                    {
                        const auto operand = operands[i];
                        if (type(operand).width <= 64 && scalars.insert(operand.index).second)
                            out << "const std::uint64_t cpu_operand_" << operand.index << "=grhsim_trunc_u64("
                                << read(operand) << ',' << type(operand).width << ");\n";
                    }
                    const auto ptr = [&](ValueId valueId, const std::string &expr) {
                        return type(valueId).width > 64 ? "(" + expr + ").data()" : "&cpu_operand_" + std::to_string(valueId.index);
                    };
                    if (kind == "and" || kind == "or" || kind == "xor" || kind == "not")
                    {
                        const bool unary = kind == "not";
                        const char operation = unary ? '~' : kind == "and" ? '&' : kind == "or" ? '|' : '^';
                        // cpu_bitwise_words_changed takes an explicit (nullptr,0) rhs
                        // for '~'; the plain grhsim_not_words drops those two slots.
                        const auto args = [&](std::ostream &stream, bool nullRhs) {
                            stream << ptr(operands[0], read(operands[0])) << ',' << ((type(operands[0]).width + 63u) / 64u) << ',';
                            if (unary) { if (nullRhs) stream << "nullptr,0,"; }
                            else stream << ptr(operands[1], read(operands[1])) << ',' << ((type(operands[1]).width + 63u) / 64u) << ',';
                            stream << resultType.width << ',' << dst << ".data()," << words;
                        };
                        if (field)
                        {
                            out << "if(cpu_bitwise_words_changed<'" << operation << "'>(";
                            args(out, true);
                            out << ")){\n";
                            activateFanout(out, supernodeFanout_[result.index], current);
                            out << "}\n";
                        }
                        else
                        {
                            out << (unary ? "grhsim_not_words(" : "grhsim_" + std::string(kind) + "_words(");
                            args(out, false);
                            out << ");\n";
                        }
                        out << "}\n";
                        return;
                    }
                    if (kind == "add" || kind == "sub")
                    {
                        const auto args = [&](std::ostream &stream) {
                            stream << ptr(operands[0], read(operands[0])) << ',' << ((type(operands[0]).width + 63u) / 64u) << ','
                                << ptr(operands[1], read(operands[1])) << ',' << ((type(operands[1]).width + 63u) / 64u) << ','
                                << resultType.width << ',' << dst << ".data()," << words;
                        };
                        if (field)
                        {
                            out << (kind == "add" ? "if(cpu_arithmetic_words_changed<'+'>(" : "if(cpu_arithmetic_words_changed<'-'>(");
                            args(out);
                            out << ")){\n";
                            activateFanout(out, supernodeFanout_[result.index], current);
                            out << "}\n";
                        }
                        else
                        {
                            out << (kind == "add" ? "grhsim_add_words(" : "grhsim_sub_words(");
                            args(out);
                            out << ");\n";
                        }
                        out << "}\n";
                        return;
                    }
                    // shl/lshr/ashr
                    const auto args = [&](std::ostream &stream) {
                        stream << ptr(operands[0], read(operands[0])) << ',' << ((type(operands[0]).width + 63u) / 64u)
                            << ",grhsim_index_words(" << read(operands[1]) << ',' << resultType.width << ")," << resultType.width << ','
                            << dst << ".data()," << words;
                    };
                    if (field)
                    {
                        out << "if(cpu_shift_words_changed<'"
                            << (kind == "shl" ? 'L' : kind == "lshr" ? 'R' : 'A') << "'>(";
                        args(out);
                        out << ")){\n";
                        activateFanout(out, supernodeFanout_[result.index], current);
                        out << "}\n";
                    }
                    else
                    {
                        out << "grhsim_" << kind << "_words(";
                        args(out);
                        out << ");\n";
                    }
                    out << "}\n";
                    return;
                }
                // Remaining wide ops (mux/slice/div/mod/mul/...) use the
                // value-returning runtime helpers with a compare-store on top.
                const auto expr = expression(op);
                if (field)
                {
                    out << "{const auto cpu_value=" << expr << ";if(" << dst << "!=cpu_value){" << dst << "=cpu_value;\n";
                    activateFanout(out, supernodeFanout_[result.index], current);
                    out << "}}\n";
                }
                else out << dst << '=' << expr << ";\n";
                return;
            }
            // Whole-array values (a memAssign source read) skip normalize():
            // they move as std::array units between the mem store, the boundary
            // store, and locals.
            if (resultType.kind == TypeKind::Array)
            {
                const auto expr = expression(op);
                if (boundaryByValue_[result.index]) { publishBoundary(out, result, expr, current); return; }
                declareLocal(out, result, expr);
                return;
            }
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
            // Two-state full-width edge semantics (runtime helpers): posedge =
            // any-bit-nonzero now vs all-zero before, negedge the mirror.
            std::string hit;
            if (eventType.kind == TypeKind::Logic && eventType.domain == LogicDomain::TwoState && eventType.width > 64)
            {
                const auto width = std::to_string(eventType.width);
                if (*edge == "posedge") hit = "grhsim_event_posedge_words(" + cur + "," + prevSlot + "," + width + ")";
                else if (*edge == "negedge") hit = "grhsim_event_negedge_words(" + cur + "," + prevSlot + "," + width + ")";
                else hit = "(grhsim_event_posedge_words(" + cur + "," + prevSlot + "," + width + ")||grhsim_event_negedge_words(" + cur + "," + prevSlot + "," + width + "))";
            }
            else if (isScalarLogic(eventType))
            {
                const auto curW = "static_cast<std::uint64_t>(" + cur + ")";
                const auto prevW = "static_cast<std::uint64_t>(" + prevSlot + ")";
                if (*edge == "posedge") hit = "grhsim_event_posedge(" + curW + "," + prevW + ")";
                else if (*edge == "negedge") hit = "grhsim_event_negedge(" + curW + "," + prevW + ")";
                else hit = "(grhsim_event_posedge(" + curW + "," + prevW + ")||grhsim_event_negedge(" + curW + "," + prevW + "))";
            }
            else throw std::runtime_error("CPU six-phase emit edgeDet event must be two-state logic");
            out << "if(" << hit << "){\n"
                << "eventActStore[" << *act / 8 << "]|=std::uint8_t(1u<<" << *act % 8 << ");\n";
            // V2 (M2): OR the act's activation words (bit i == ordinal i, the
            // non-sink supernodes holding an op with this act) straight into
            // dataActiveFlag — eventActiveFlag and the dual gate are gone.
            if (const auto it = activationByAct_.find(static_cast<uint32_t>(*act));
                it != activationByAct_.end() && std::any_of(it->second->supernodeWords.begin(), it->second->supernodeWords.end(),
                                                            [](std::uint64_t word) { return word != 0; }))
            {
                const auto &words = it->second->supernodeWords;
                const auto dataActive = model_.text(activeStore_->fields[0].name);
                out << "{static constexpr std::uint64_t cpu_bm[]={";
                for (const auto word : words) out << "UINT64_C(0x" << std::hex << word << std::dec << "),";
                out << "};\nfor(std::size_t cpu_i=0;cpu_i<" << words.size() << ";++cpu_i){std::uint64_t cpu_bits=cpu_bm[cpu_i];"
                    << "while(cpu_bits){const unsigned cpu_b=static_cast<unsigned>(__builtin_ctzll(cpu_bits));cpu_bits&=cpu_bits-1;"
                    << dataActive << "[cpu_i*64+cpu_b]=1;}}\n}\n";
            }
            // prev updates whether or not the edge hit (plan §72), so a missed
            // direction never replays and the next opposite edge is seen fresh.
            out << "}\n" << prevSlot << '=' << cur << ";\n";
        }
        void SixPhaseEmitter::emitRegWrite(std::ostream &out, const SimOp &op, uint32_t current) const
        {
            const auto operands = model_.operands(op);
            const auto refs = model_.objectRefs(op);
            if (refs.empty() || refs[0].kind != ObjectKind::State || operands.size() != 3)
                throw std::runtime_error("CPU six-phase emit malformed reg/latch write");
            const std::string guard = actGuard(op);
            const auto *field = refs[0].index < regFieldByState_.size() ? regFieldByState_[refs[0].index] : nullptr;
            if (!field) throw std::runtime_error("CPU six-phase emit reg/latch write target is not a regLatch state");
            const StateId target{refs[0].index, 0};
            const auto &targetType = stateType(target);
            const std::string nextSlot = "regLatchStoreNext." + std::string(model_.text(field->name));
            const std::string curSlot = "regLatchStore." + std::string(model_.text(field->name));
            (void)current;
            // Merge base is regLatchStoreNext (read-modify-write across same-round
            // writers); the fanout compare runs against the currently visible
            // regLatchStore value so readers fire only on a true change.
            out << "if((" << guard << ")&&" << read(operands[0]) << "){\n";
            if (isScalarLogic(targetType))
            {
                const std::string merged = normalize("(static_cast<std::uint64_t>(" + nextSlot + ")&~static_cast<std::uint64_t>(" + read(operands[2]) +
                    "))|(static_cast<std::uint64_t>(" + read(operands[1]) + ")&static_cast<std::uint64_t>(" + read(operands[2]) + "))", targetType);
                out << "{const auto cpu_merged=" << merged << ";if(" << nextSlot << "!=cpu_merged){if(" << curSlot << "!=cpu_merged){\n";
                activateOrdinals(out, stateFanout_[refs[0].index], model_.text(activeStore_->fields[1].name));
                out << "GRHSIM_PERF_COUNT(touchedStateShadowCount);\n";
                out << "}" << nextSlot << "=cpu_merged;}}\n";
            }
            else if (targetType.kind == TypeKind::Logic && targetType.domain == LogicDomain::TwoState)
            {
                out << "{const auto cpu_merged=grhsim_merge_words_masked(" << nextSlot << ',' << read(operands[1]) << ','
                    << read(operands[2]) << ',' << targetType.width << ");\n"
                    << "if(" << nextSlot << "!=cpu_merged){if(" << curSlot << "!=cpu_merged){\n";
                activateOrdinals(out, stateFanout_[refs[0].index], model_.text(activeStore_->fields[1].name));
                out << "GRHSIM_PERF_COUNT(touchedStateShadowCount);\n";
                out << "}" << nextSlot << "=cpu_merged;}}\n";
            }
            else throw std::runtime_error("CPU six-phase emit reg/latch write target type is not two-state logic");
            out << "}\n";
        }
        void SixPhaseEmitter::emitMemWrite(std::ostream &out, const SimOp &op, const CpuMemWritePlanEntry &plan) const
        {
            const auto name = model_.text(op.opType);
            const auto operands = model_.operands(op);
            const auto refs = model_.objectRefs(op);
            if (refs.empty() || refs[0].kind != ObjectKind::State)
                throw std::runtime_error("CPU six-phase emit mem write requires a state target");
            const StateId target{refs[0].index, 0};
            const auto &array = stateType(target);
            if (array.kind != TypeKind::Array)
                throw std::runtime_error("CPU six-phase emit mem write target is not an array");
            const auto &element = model_.types()[array.elementType.index - 1];
            const auto *field = target.index < memFieldByState_.size() ? memFieldByState_[target.index] : nullptr;
            if (!field) throw std::runtime_error("CPU six-phase emit mem write target has no memStore field");
            const std::string mem = "memStore." + std::string(model_.text(field->name));
            // Event-gated writes read the act bits (P_event owns edge detection);
            // eventFree writes enter every round and converge via cell change
            // detection (plan §123).
            const std::string guard = actGuard(op);
            const auto dataNext = model_.text(activeStore_->fields[1].name);
            // Reader activation for one actually-changed row: a static-row reader
            // fires only on an exact address overlap; a dynamic-row reader fires
            // on any change (plan §132-135). A statically known write row folds
            // the overlap test at emit time.
            const auto activateReaders = [&](const std::string &rowText, std::optional<uint64_t> constRow) {
                for (const auto &reader : plan.readers)
                {
                    const auto ordinal = reader.owner.index < ordinalOf_.size() ? ordinalOf_[reader.owner.index] : ~0u;
                    if (ordinal == ~0u)
                        throw std::runtime_error("CPU six-phase emit mem reader has no supernode ordinal");
                    if (reader.staticRow && constRow && *reader.staticRow != *constRow) continue;
                    if (reader.staticRow && !constRow) out << "if(" << rowText << "==" << *reader.staticRow << ')';
                    out << dataNext << '[' << ordinal << "]=1;\n";
                }
            };
            // One cell write with the plan §123 change detection: no change -> no
            // write, no reader activation.
            const auto writeCell = [&](const std::string &rowText, std::optional<uint64_t> constRow,
                                       const std::string &data, const std::string *mask) {
                const std::string cell = mem + "[" + rowText + "]";
                if (isScalarLogic(element))
                {
                    const std::string next = mask
                        ? normalize("(static_cast<std::uint64_t>(" + cell + ")&~static_cast<std::uint64_t>(" + *mask +
                                    "))|(static_cast<std::uint64_t>(" + data + ")&static_cast<std::uint64_t>(" + *mask + "))", element)
                        : normalize(data, element);
                    out << "{auto &cpu_cell=" << cell << ";const auto cpu_next=" << next << ";if(cpu_cell!=cpu_next){cpu_cell=cpu_next;\n"
                        << "GRHSIM_PERF_COUNT(touchedWriteCount);\n";
                    activateReaders(rowText, constRow);
                    out << "}}\n";
                    return;
                }
                if (element.kind == TypeKind::Logic && element.domain == LogicDomain::TwoState)
                {
                    if (mask)
                        out << "if(grhsim_apply_masked_words_inplace(" << cell << ',' << data << ',' << *mask << ',' << element.width << ")){\n";
                    else
                        out << "if(" << cell << "!=" << data << "){" << cell << '=' << data << ";\n";
                    out << "GRHSIM_PERF_COUNT(touchedWriteCount);\n";
                    activateReaders(rowText, constRow);
                    out << "}\n";
                    return;
                }
                throw std::runtime_error("CPU six-phase emit mem write element type is not two-state logic");
            };
            const auto rowOf = [&](ValueId address, std::string &rowText, std::optional<uint64_t> &constRow) {
                rowText = read(address);
                if (const auto constantRow = scalarConstantValue(address))
                {
                    rowText = std::to_string(*constantRow);
                    if (*constantRow < array.count) constRow = *constantRow;
                    else return false; // statically out of bounds: dead write
                }
                return true;
            };
            if (guard != "true") out << "if(" << guard << "){\n";
            if (name == "core.state.memWrite")
            {
                if (operands.size() != 4) throw std::runtime_error("CPU memory write has invalid arity");
                const std::string maskText = read(operands[3]);
                std::string rowText; std::optional<uint64_t> constRow;
                if (!rowOf(operands[1], rowText, constRow))
                    out << "// cpu_mem_write_dead row=" << read(operands[1]) << "\n";
                else
                {
                    out << "if(" << read(operands[0]);
                    if (!constRow) out << "&&static_cast<std::size_t>(" << rowText << ")<" << array.count;
                    out << "){\n";
                    writeCell(rowText, constRow, read(operands[2]), &maskText);
                    out << "}\n";
                }
            }
            else if (name == "core.state.memFill" || name == "core.state.memAssign")
            {
                if (operands.size() != 2) throw std::runtime_error("CPU memory fill/assign has invalid arity");
                const bool assign = name == "core.state.memAssign";
                // memAssign's data is a whole-array value indexed per row.
                const std::string data = read(operands[1]);
                // A whole packed-array write (ingest lowers packed-aggregate
                // whole writes to kMemoryFillPort) arrives as a memFill whose
                // data is the packed row-major value (element.width * count
                // bits); slice it per row — the legacy emit's isPackedFill
                // semantics. Element-width data keeps the plain broadcast fill.
                const auto &dataType = type(operands[1]);
                const uint64_t packedWidth = static_cast<uint64_t>(element.width) * array.count;
                const bool packedFill = !assign && dataType.kind == TypeKind::Logic &&
                                        element.kind == TypeKind::Logic &&
                                        dataType.width == packedWidth && packedWidth != element.width;
                out << "if(" << read(operands[0]) << "){\n"
                    << "for(std::size_t cpu_row=0;cpu_row<" << array.count << ";++cpu_row){\n";
                if (!packedFill)
                    writeCell("cpu_row", std::nullopt, assign ? data + "[cpu_row]" : data, nullptr);
                else if (dataType.width <= 64)
                    writeCell("cpu_row", std::nullopt,
                              "grhsim_slice_dynamic_u64(static_cast<std::uint64_t>(" + data +
                                  "),cpu_row*" + std::to_string(element.width) + "u," +
                                  std::to_string(element.width) + "u)",
                              nullptr);
                else if (isScalarLogic(element))
                    // Wide packed data, scalar rows: slice straight to a word.
                    writeCell("cpu_row", std::nullopt,
                              "grhsim_slice_words_u64<" + std::to_string((dataType.width + 63u) / 64u) +
                                  ">((" + data + "),cpu_row*" + std::to_string(element.width) + "u," +
                                  std::to_string(element.width) + "u)",
                              nullptr);
                else
                {
                    const auto elementWords = (element.width + 63u) / 64u;
                    out << "{std::array<std::uint64_t," << elementWords << "> cpu_fill_slice;\n"
                        << "grhsim_slice_words((" << data << ").data()," << (packedWidth + 63u) / 64u
                        << ",cpu_row*" << element.width << "u," << element.width << ",cpu_fill_slice.data(),"
                        << elementWords << ");\n";
                    writeCell("cpu_row", std::nullopt, "cpu_fill_slice", nullptr);
                    out << "}\n";
                }
                out << "}\n}\n";
            }
            else if (name == "core.state.memWriteSeq")
            {
                if (operands.empty() || operands.size() % 3 != 0)
                    throw std::runtime_error("CPU sequential memory write operands are not triples");
                // Port order is the priority order: the last triple wins an
                // address collision (core.md memWriteSeq), which sequential
                // in-place application reproduces exactly.
                for (std::size_t i = 0; i < operands.size(); i += 3)
                {
                    std::string rowText; std::optional<uint64_t> constRow;
                    if (!rowOf(operands[i + 1], rowText, constRow))
                    {
                        out << "// cpu_mem_write_dead row=" << read(operands[i + 1]) << "\n";
                        continue;
                    }
                    out << "if(" << read(operands[i]);
                    if (!constRow) out << "&&static_cast<std::size_t>(" << rowText << ")<" << array.count;
                    out << "){\n";
                    writeCell(rowText, constRow, read(operands[i + 2]), nullptr);
                    out << "}\n";
                }
            }
            else throw std::runtime_error("CPU six-phase emit unsupported mem write op: " + std::string(name));
            if (guard != "true") out << "}\n";
        }
        // General-phase regLatch-class array writes (M5d-6): the write lives
        // inside its General supernode and commits NBA-style into
        // regLatchStoreNext. The merge base is the NEXT row (read-modify-write
        // across same-round writers, in op order), while the fanout compare
        // runs against the currently visible regLatchStore row so stateFanout
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
            if (array.kind != TypeKind::Array)
                throw std::runtime_error("CPU six-phase emit general mem write target is not an array");
            const auto &element = model_.types()[array.elementType.index - 1];
            const auto *field = target.index < regFieldByState_.size() ? regFieldByState_[target.index] : nullptr;
            if (!field)
                throw std::runtime_error("CPU six-phase emit general mem write target is not a "
                                         "regLatch-class state");
            const std::string nextArray = "regLatchStoreNext." + std::string(model_.text(field->name));
            const std::string curArray = "regLatchStore." + std::string(model_.text(field->name));
            const std::string guard = actGuard(op);
            (void)current; // state fanout always queues into dataActiveFlagNext
            const auto activateState = [&] {
                activateOrdinals(out, stateFanout_[refs[0].index], model_.text(activeStore_->fields[1].name));
                out << "GRHSIM_PERF_COUNT(touchedStateShadowCount);\n";
            };
            const auto writeCell = [&](const std::string &rowText, std::optional<uint64_t> constRow,
                                       const std::string &data, const std::string *mask) {
                (void)constRow;
                const std::string nextCell = nextArray + "[" + rowText + "]";
                const std::string curCell = curArray + "[" + rowText + "]";
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
                    if (mask)
                        out << "{const auto cpu_merged=grhsim_merge_words_masked(" << nextCell << ',' << data << ','
                            << *mask << ',' << element.width << ");if(" << nextCell << "!=cpu_merged){if(" << curCell
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
            const auto rowOf = [&](ValueId address, std::string &rowText, std::optional<uint64_t> &constRow) {
                rowText = read(address);
                if (const auto constantRow = scalarConstantValue(address))
                {
                    rowText = std::to_string(*constantRow);
                    if (*constantRow < array.count) constRow = *constantRow;
                    else return false; // statically out of bounds: dead write
                }
                return true;
            };
            if (guard != "true") out << "if(" << guard << "){\n";
            if (name == "core.state.memWrite")
            {
                if (operands.size() != 4) throw std::runtime_error("CPU memory write has invalid arity");
                const std::string maskText = read(operands[3]);
                std::string rowText; std::optional<uint64_t> constRow;
                if (!rowOf(operands[1], rowText, constRow))
                    out << "// cpu_mem_write_dead row=" << read(operands[1]) << "\n";
                else
                {
                    out << "if(" << read(operands[0]);
                    if (!constRow) out << "&&static_cast<std::size_t>(" << rowText << ")<" << array.count;
                    out << "){\n";
                    writeCell(rowText, constRow, read(operands[2]), &maskText);
                    out << "}\n";
                }
            }
            else if (name == "core.state.memFill" || name == "core.state.memAssign")
            {
                if (operands.size() != 2) throw std::runtime_error("CPU memory fill/assign has invalid arity");
                const bool assign = name == "core.state.memAssign";
                // memAssign's data is a whole-array value indexed per row; a
                // whole packed-array memFill carries the packed row-major
                // value (element.width * count bits) and is sliced per row.
                const std::string data = read(operands[1]);
                const auto &dataType = type(operands[1]);
                const uint64_t packedWidth = static_cast<uint64_t>(element.width) * array.count;
                const bool packedFill = !assign && dataType.kind == TypeKind::Logic &&
                                        element.kind == TypeKind::Logic &&
                                        dataType.width == packedWidth && packedWidth != element.width;
                out << "if(" << read(operands[0]) << "){\n"
                    << "for(std::size_t cpu_row=0;cpu_row<" << array.count << ";++cpu_row){\n";
                if (!packedFill)
                    writeCell("cpu_row", std::nullopt, assign ? data + "[cpu_row]" : data, nullptr);
                else if (dataType.width <= 64)
                    writeCell("cpu_row", std::nullopt,
                              "grhsim_slice_dynamic_u64(static_cast<std::uint64_t>(" + data +
                                  "),cpu_row*" + std::to_string(element.width) + "u," +
                                  std::to_string(element.width) + "u)",
                              nullptr);
                else if (isScalarLogic(element))
                    writeCell("cpu_row", std::nullopt,
                              "grhsim_slice_words_u64<" + std::to_string((dataType.width + 63u) / 64u) +
                                  ">((" + data + "),cpu_row*" + std::to_string(element.width) + "u," +
                                  std::to_string(element.width) + "u)",
                              nullptr);
                else
                {
                    const auto elementWords = (element.width + 63u) / 64u;
                    out << "{std::array<std::uint64_t," << elementWords << "> cpu_fill_slice;\n"
                        << "grhsim_slice_words((" << data << ").data()," << (packedWidth + 63u) / 64u
                        << ",cpu_row*" << element.width << "u," << element.width << ",cpu_fill_slice.data(),"
                        << elementWords << ");\n";
                    writeCell("cpu_row", std::nullopt, "cpu_fill_slice", nullptr);
                    out << "}\n";
                }
                out << "}\n}\n";
            }
            else if (name == "core.state.memWriteSeq")
            {
                if (operands.empty() || operands.size() % 3 != 0)
                    throw std::runtime_error("CPU sequential memory write operands are not triples");
                // Port order is the priority order: the last triple wins an
                // address collision, reproduced exactly by sequential
                // next-buffer application (each triple RMWs the NEXT row).
                for (std::size_t i = 0; i < operands.size(); i += 3)
                {
                    std::string rowText; std::optional<uint64_t> constRow;
                    if (!rowOf(operands[i + 1], rowText, constRow))
                    {
                        out << "// cpu_mem_write_dead row=" << read(operands[i + 1]) << "\n";
                        continue;
                    }
                    out << "if(" << read(operands[i]);
                    if (!constRow) out << "&&static_cast<std::size_t>(" << rowText << ")<" << array.count;
                    out << "){\n";
                    writeCell(rowText, constRow, read(operands[i + 2]), nullptr);
                    out << "}\n";
                }
            }
            else throw std::runtime_error("CPU six-phase emit unsupported general mem write op: " + std::string(name));
            if (guard != "true") out << "}\n";
        }
        void SixPhaseEmitter::emitSystemTask(std::ostream &out, const SimOp &op) const
        {
            const auto operands = model_.operands(op);
            if (operands.empty()) throw std::runtime_error("CPU system task requires a call condition operand");
            // Event guard from the act bits (P_event owns edge detection);
            // event-free tasks run whenever their supernode fires.
            std::string cond = callCondition(operands[0]);
            const std::string guard = actGuard(op);
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
                out << "grhsim_make_task_arg(" << read(args[i]);
                if (type(args[i]).kind == TypeKind::Logic)
                    out << ',' << type(args[i]).width << ',' << (type(args[i]).isSigned ? "true" : "false");
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
            const std::string guard = actGuard(op);
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
                out << "grhsim_trunc_words(" << temporary << ',' << target.width << ");\n";
            else if (target.kind == TypeKind::Logic)
            {
                source = "cpu_dpi_normalized";
                out << "auto " << source << '=' << normalize(temporary, target) << ";\n";
            }
            if (const auto *field = boundaryByValue_[result.index])
            {
                const auto slot = boundaryRef(field);
                out << "if(" << slot << "!=" << source << "){" << slot << "=std::move(" << source << ");\n";
                activateFanout(out, supernodeFanout_[result.index], current);
                out << "}\n";
            }
            else out << localRef(result) << "=std::move(" << source << ");\n";
            out << "}\n";
        }
        std::string SixPhaseEmitter::dpiType(TypeId id) const
        {
            if (!id) return "void";
            const auto &type = model_.types()[id.index - 1];
            if (type.kind == TypeKind::Array)
                throw std::runtime_error("CPU DPI unpacked array ABI is not implemented");
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
            out << "this->" << identifier(model_.text(output.name)) << '=' << read(operands[0]) << ";\n";
        }
        void SixPhaseEmitter::commitStagedOutputWrites(std::ostream &out) const
        {
            // Timeslot history write-backs commit after every P_output op ran, so
            // the in-body ne comparisons all observed the previous eval's values.
            // P_publish already equalized regLatchStore/regLatchStoreNext, so the
            // commit lands in both to keep the round-start invariant.
            for (const auto &staged : stagedOutputWrites_)
            {
                const auto &target = stateType(staged.field->state);
                const std::string name(model_.text(staged.field->name));
                out << "if(" << staged.enable << "){\n";
                if (isScalarLogic(target))
                {
                    out << "{const auto cpu_merged=" << normalize("(static_cast<std::uint64_t>(regLatchStore." + name +
                            ")&~static_cast<std::uint64_t>(" + staged.mask + "))|(static_cast<std::uint64_t>(" + staged.data +
                            ")&static_cast<std::uint64_t>(" + staged.mask + "))", target) << ";\n"
                        << "if(regLatchStore." << name << "!=cpu_merged){GRHSIM_PERF_COUNT(touchedStateShadowCount);\n"
                        << "regLatchStore." << name << "=cpu_merged;\nregLatchStoreNext." << name << "=cpu_merged;\n}}\n";
                }
                else if (target.kind == TypeKind::Logic && target.domain == LogicDomain::TwoState)
                {
                    out << "{const auto cpu_merged=grhsim_merge_words_masked(regLatchStore." << name << ',' << staged.data << ','
                        << staged.mask << ',' << target.width << ");\n"
                        << "if(regLatchStore." << name << "!=cpu_merged){GRHSIM_PERF_COUNT(touchedStateShadowCount);\n"
                        << "regLatchStore." << name << "=cpu_merged;\nregLatchStoreNext." << name << "=cpu_merged;\n}}\n";
                }
                else throw std::runtime_error("CPU six-phase emit output latchWrite target type is not two-state logic");
                out << "}\n";
            }
        }

        // ----- Validation -----
        void SixPhaseEmitter::validate() const
        {
            for (const auto &type : model_.types())
            {
                if (type.kind == TypeKind::Logic && type.domain != LogicDomain::TwoState)
                    throw std::runtime_error("CPU six-phase emit requires two-state logic types");
                if (type.kind == TypeKind::Array && containsString(type))
                    throw std::runtime_error("CPU six-phase emit arrays of string handles are not implemented");
            }
            for (const auto &state : model_.states())
                if (containsString(model_.types()[state.type.index - 1]))
                    throw std::runtime_error("CPU six-phase emit string state initialization and commit are not implemented");
            std::set<std::string> names{class_, "init", "eval", "dumpState", "set_runtime_profile_enabled",
                "dump_runtime_profile", "configure_waveform", "perf_counters", "PerfCounters", "pInput", "pEvent",
                "pGeneral", "pMem", "pPublish", "pOutput",
                "regLatchStore", "regLatchStoreNext", "memStore", "boundaryValueStore", "prevEventStore",
                "eventActStore", "timeslotTriggerFlag", "cpu_rng", "perf_", "RegLatchStore", "MemStore",
                "BoundaryValueStore", "PrevEventStore", "boundaryStrings"};
            // M5d-7 chunk members and spill frame types share the class scope;
            // register them with duplicate detection so a pathological op name
            // (e.g. one that makes sn_12_c0 collide with a chunk name) fails
            // validation instead of silently breaking the generated code.
            const auto claim = [&names](const std::string &name) {
                if (!names.insert(name).second)
                    throw std::runtime_error("CPU generated member name collides: " + name);
            };
            for (uint32_t i = 0; i < initChunks_.size(); ++i) claim("cpu_init_" + std::to_string(i));
            for (uint32_t i = 0; i < eventChunks_.size(); ++i) claim("pEvent_c" + std::to_string(i));
            for (uint32_t i = 0; i < scanChunks_.size(); ++i) claim("pGeneral_c" + std::to_string(i));
            for (uint32_t i = 0; i < memChunks_.size(); ++i) claim("pMem_c" + std::to_string(i));
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
                if (type(operand).kind == TypeKind::Array)
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
                if (source.kind != target.kind || source.width != target.width || source.domain != target.domain)
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
                if (target.kind != TypeKind::Array && model_.steps(record).size() != 1)
                    throw std::runtime_error("CPU scalar state requires exactly one full initializer");
                if (target.kind != TypeKind::Array) continue;
                const auto &element = model_.types()[target.elementType.index - 1];
                std::vector<std::pair<uint64_t, uint64_t>> covered;
                for (const auto &step : model_.steps(record))
                {
                    const auto kind = model_.text(step.kind);
                    const auto params = model_.parameters(step);
                    if (kind == "core.init.const")
                    {
                        covered.emplace_back(0, target.count);
                        continue;
                    }
                    if (kind != "core.init.fill" && kind != "core.init.readmem")
                        throw std::runtime_error("CPU six-phase emit unsupported array initializer: " + std::string(kind));
                    const auto *start = parameter<int64_t>(model_, params, "start"), *count = parameter<int64_t>(model_, params, "count");
                    if ((start && (*start < 0 || uint64_t(*start) > target.count)) || (count && *count < 0))
                        throw std::runtime_error("CPU array initializer range is invalid");
                    const auto first = start ? uint64_t(*start) : 0;
                    const auto size = count ? uint64_t(*count) : target.count - first;
                    if (size > target.count - first) throw std::runtime_error("CPU array initializer range exceeds state");
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
                if (end != target.count)
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
                        const bool array = target.kind == TypeKind::Array;
                        const auto &element = array ? model_.types()[target.elementType.index - 1] : target;
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
                                else if (const auto *values = parameter<std::vector<std::string>>(model_, params, "value"))
                                {
                                    zero = true;
                                    for (const auto &text : *values)
                                        if (initLiteral(text, element) != literal("0", element)) { zero = false; break; }
                                }
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
            const bool array = target.kind == TypeKind::Array;
            const auto &element = array ? model_.types()[target.elementType.index - 1] : target;
            if (element.kind != TypeKind::Logic || element.domain != LogicDomain::TwoState)
                throw std::runtime_error("CPU initializer currently requires two-state logic or an array of two-state logic");
            const auto kind = model_.text(step.kind);
            const auto params = model_.parameters(step);
            // The store follows the state's classification (M5d-6): regLatch
            // covers scalars and regLatch-class arrays, mem the mem-class
            // arrays. Initializers write the current buffer; initGlue syncs
            // regLatchStoreNext afterwards.
            const bool inRegLatch = id.index < regFieldByState_.size() && regFieldByState_[id.index];
            const auto *field = inRegLatch ? regFieldByState_[id.index]
                                           : (id.index < memFieldByState_.size() ? memFieldByState_[id.index] : nullptr);
            if (!field) throw std::runtime_error("CPU six-phase emit state has no named-store field");
            const std::string slot = (inRegLatch ? "regLatchStore." : "memStore.") + std::string(model_.text(field->name));
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

            if (kind == "core.init.const")
            {
                const auto *values = parameter<std::vector<std::string>>(model_, params, "value");
                if (!values || values->size() != target.count)
                    throw std::runtime_error("CPU array const initializer requires exactly count element literals");
                if (!elide && !values->empty())
                {
                    out << "static const " << cppType(element) << " data[]={\n";
                    for (const auto &text : *values) out << initLiteral(text, element) << ",\n";
                    out << "};\nstd::memcpy(" << slot << ".data(),data,sizeof(data));\n";
                }
            }
            else if (kind == "core.init.fill" || kind == "core.init.readmem")
            {
                const auto *start = parameter<int64_t>(model_, params, "start"), *count = parameter<int64_t>(model_, params, "count");
                if ((start && (*start < 0 || uint64_t(*start) > target.count)) || (count && *count < 0))
                    throw std::runtime_error("CPU array initializer range is invalid");
                const auto first = start ? uint64_t(*start) : 0;
                const auto size = count ? uint64_t(*count) : target.count - first;
                if (size > target.count - first) throw std::runtime_error("CPU array initializer range exceeds state");
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
                            out << "std::memset(" << slot << ".data()+" << first * rowBytes << ",0," << size * rowBytes << ");\n";
                        else
                        {
                            if (text) out << "static const auto data=" << initLiteral(*text, element) << ";\n";
                            out << "for(std::size_t row=" << first << ";row<" << end << ";++row){\n"
                                << "auto &dst=" << slot << "[row];\n";
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
                        out << "};\nfor(std::size_t i=0;i<" << rows.size() << ";++i)" << slot << "[rows[i]]=data[i];\n";
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
            if (waveform_) out << "#include <memory>\n";
            // Optional perf/waveform build knobs (XS difftest hooks): default
            // off; counters are only compiled in a WOLVRIX_GRHSIM_PERF build.
            out << "#ifndef WOLVRIX_GRHSIM_PERF\n#define WOLVRIX_GRHSIM_PERF 0\n#endif\n"
                << "#ifndef WOLVRIX_GRHSIM_WAVEFORM\n#define WOLVRIX_GRHSIM_WAVEFORM 0\n#endif\n"
                << "#if WOLVRIX_GRHSIM_PERF\n#define GRHSIM_PERF_COUNT(field) (++perf_.field)\n"
                << "#else\n#define GRHSIM_PERF_COUNT(field) ((void)0)\n#endif\n";
            // Pointer-based wide-value change helpers (caller-provided out
            // buffers, same ABI shape as the legacy emitter's header copies).
            out << R"CPP(template<char Operation>
inline bool cpu_bitwise_words_changed(const std::uint64_t *lhs, std::size_t lhsWords,
    const std::uint64_t *rhs, std::size_t rhsWords, std::size_t width,
    std::uint64_t *out, std::size_t outWords)
{
    static_assert(Operation=='&' || Operation=='|' || Operation=='^' || Operation=='~');
    bool changed=false;
    for(std::size_t i=0;i<outWords;++i){
        const std::uint64_t a=i<lhsWords?lhs[i]:UINT64_C(0);
        std::uint64_t word;
        if constexpr(Operation=='~') word=~a;
        else {
            const std::uint64_t b=i<rhsWords?rhs[i]:UINT64_C(0);
            if constexpr(Operation=='&') word=a&b;
            else if constexpr(Operation=='|') word=a|b;
            else word=a^b;
        }
        if(i>=width/64) word=i==width/64 ? word&grhsim_mask(width%64) : UINT64_C(0);
        changed|=out[i]!=word;
        out[i]=word;
    }
    return changed;
}
template<char Operation>
inline bool cpu_arithmetic_words_changed(const std::uint64_t *lhs, std::size_t lhsWords,
    const std::uint64_t *rhs, std::size_t rhsWords, std::size_t width,
    std::uint64_t *out, std::size_t outWords)
{
    static_assert(Operation=='+' || Operation=='-');
    bool changed=false;
    std::uint64_t carry=0;
    for(std::size_t i=0;i<outWords;++i){
        const std::uint64_t a=i<lhsWords?lhs[i]:UINT64_C(0);
        const std::uint64_t b=i<rhsWords?rhs[i]:UINT64_C(0);
        std::uint64_t word;
        if constexpr(Operation=='+'){
            const unsigned __int128 sum=static_cast<unsigned __int128>(a)+b+carry;
            word=static_cast<std::uint64_t>(sum);carry=sum>>64;
        }else{
            const std::uint64_t subtrahend=b+carry;
            carry=(subtrahend<b || a<subtrahend)?1:0;
            word=a-subtrahend;
        }
        if(i>=width/64) word=i==width/64 ? word&grhsim_mask(width%64) : UINT64_C(0);
        changed|=out[i]!=word;
        out[i]=word;
    }
    return changed;
}
template<char Operation>
inline bool cpu_shift_words_changed(const std::uint64_t *value, std::size_t valueWords,
    std::size_t amount, std::size_t width, std::uint64_t *out, std::size_t outWords)
{
    static_assert(Operation=='L' || Operation=='R' || Operation=='A');
    bool changed=false;
    const bool sign=Operation=='A' && grhsim_sign_bit_words(value,valueWords,width);
    const std::size_t wordShift=amount/64,bitShift=amount%64;
    for(std::size_t step=0;step<outWords;++step){
        const std::size_t i=Operation=='L'?outWords-1-step:step;
        std::uint64_t word=0;
        if(amount<width){
            if constexpr(Operation=='L'){
                if(i>=wordShift){
                    const std::size_t src=i-wordShift;
                    if(src<valueWords) word=value[src]<<bitShift;
                    if(bitShift && src>0 && src-1<valueWords) word|=value[src-1]>>(64-bitShift);
                }
            }else{
                const std::size_t src=i+wordShift;
                if(src<valueWords) word=value[src]>>bitShift;
                if(bitShift && src+1<valueWords) word|=value[src+1]<<(64-bitShift);
            }
        }
        if(sign){
            const std::size_t start=amount>=width?0:width-amount;
            if(i>=start/64) word|=i==start/64 ? ~grhsim_mask(start%64) : ~UINT64_C(0);
        }
        if(i>=width/64) word=i==width/64 ? word&grhsim_mask(width%64) : UINT64_C(0);
        changed|=out[i]!=word;
        out[i]=word;
    }
    return changed;
}
)CPP";
            out << R"CPP(template<std::size_t DestN,std::size_t SrcN>
inline bool cpu_replicate_words_changed(const std::array<std::uint64_t,SrcN> &source,
    std::size_t elemWidth,std::size_t rep,std::size_t totalWidth,
    std::array<std::uint64_t,DestN> &out)
{
    bool changed=false;
    for(std::size_t repeat=0;repeat<rep;++repeat){
        const std::size_t destLsb=repeat*elemWidth;
        if(destLsb>=totalWidth) break;
        const std::size_t width=std::min(elemWidth,totalWidth-destLsb);
        const std::size_t sourceWords=(width+63u)/64u;
        for(std::size_t i=0;i<sourceWords && i<SrcN;++i){
            const std::size_t wordWidth=(i+1u==sourceWords)?width-i*64u:64u;
            const std::uint64_t sourceWord=source[i]&grhsim_mask(wordWidth);
            const std::size_t bit=destLsb+i*64u;
            const std::size_t word=bit/64u;
            const std::size_t shift=bit&63u;
            if(word<DestN){
                const std::size_t first=std::min(wordWidth,64u-shift);
                const std::uint64_t mask=grhsim_mask(first)<<shift;
                const std::uint64_t value=(sourceWord&grhsim_mask(first))<<shift;
                const std::uint64_t next=(out[word]&~mask)|value;
                changed|=out[word]!=next;out[word]=next;
                if(first<wordWidth && word+1u<DestN){
                    const std::size_t second=wordWidth-first;
                    const std::uint64_t nextWord=(out[word+1u]&~grhsim_mask(second))|(sourceWord>>first&grhsim_mask(second));
                    changed|=out[word+1u]!=nextWord;out[word+1u]=nextWord;
                }
            }
        }
    }
    const std::size_t liveWidth=elemWidth==0||rep==0?0:rep>totalWidth/elemWidth?totalWidth:rep*elemWidth;
    const std::size_t firstDead=liveWidth/64u;
    if(liveWidth&63u){
        if(firstDead<DestN){
            const std::uint64_t next=out[firstDead]&grhsim_mask(liveWidth&63u);
            changed|=out[firstDead]!=next;out[firstDead]=next;
            for(std::size_t i=firstDead+1u;i<DestN;++i){changed|=out[i]!=UINT64_C(0);out[i]=UINT64_C(0);}
        }
    }
    else if(firstDead<DestN){
        for(std::size_t i=firstDead;i<DestN;++i){changed|=out[i]!=UINT64_C(0);out[i]=UINT64_C(0);}
    }
    return changed;
}
template<std::size_t DestN,class Scalar>
inline bool cpu_replicate_words_changed(Scalar source,std::size_t elemWidth,std::size_t rep,
    std::size_t totalWidth,std::array<std::uint64_t,DestN> &out)
{
    const std::array<std::uint64_t,1> words{{static_cast<std::uint64_t>(source)}};
    return cpu_replicate_words_changed<DestN,1>(words,elemWidth,rep,totalWidth,out);
}
)CPP";
            out << "template<std::size_t N,std::size_t R> inline std::array<std::uint64_t,N> grhsim_concat_wide_scalar(const std::array<std::uint64_t,R>& lhs,std::size_t lhsWidth,std::uint64_t rhs,std::size_t rhsWidth,std::size_t totalWidth){std::array<std::uint64_t,N> out{};grhsim_insert_scalar_words(out,0,rhs,rhsWidth);grhsim_insert_words(out,rhsWidth,lhs,std::min(lhsWidth,totalWidth-rhsWidth));grhsim_trunc_words(out,totalWidth);return out;}\n";
            out << "template<std::size_t N> inline std::array<std::uint64_t,N> grhsim_concat_wide_scalar(std::uint64_t lhs,std::size_t lhsWidth,std::uint64_t rhs,std::size_t rhsWidth,std::size_t totalWidth){std::array<std::uint64_t,N> out{};grhsim_insert_scalar_words(out,0,rhs,rhsWidth);grhsim_insert_scalar_words(out,rhsWidth,lhs,std::min(lhsWidth,totalWidth-rhsWidth));grhsim_trunc_words(out,totalWidth);return out;}\n";
            out << "template<std::size_t N> inline std::array<std::uint64_t,N> grhsim_concat_scalar_scalar_wide(std::uint64_t lhs,std::size_t lhsWidth,std::uint64_t rhs,std::size_t rhsWidth,std::size_t totalWidth){std::array<std::uint64_t,N> out{};grhsim_insert_scalar_words(out,0,rhs,rhsWidth);grhsim_insert_scalar_words(out,rhsWidth,lhs,std::min(lhsWidth,totalWidth-rhsWidth));grhsim_trunc_words(out,totalWidth);return out;}\n";
            out << "template<std::size_t N,std::size_t R> inline std::array<std::uint64_t,N> grhsim_concat_scalar_wide(std::uint64_t lhs,std::size_t lhsWidth,const std::array<std::uint64_t,R>& rhs,std::size_t rhsWidth,std::size_t totalWidth){std::array<std::uint64_t,N> out{};grhsim_insert_words(out,0,rhs,std::min(rhsWidth,totalWidth));if(rhsWidth<totalWidth)grhsim_insert_scalar_words(out,rhsWidth,lhs,std::min(lhsWidth,totalWidth-rhsWidth));grhsim_trunc_words(out,totalWidth);return out;}\n";
            // M5d-7 multi-TU: every unit includes this header, so the shared
            // pieces live here — the dump value printers (the port section
            // matches the trace shim: bool -> 0/1, integral -> zero-padded
            // hex, arrays MS word first). DPI import declarations stay out of
            // the public header (testbenches define their own extern "C"
            // copies); each unit .cpp redeclares the imports it references.
            out << "namespace " << prefix_ << "_dump {\n"
                << "inline void grhsim_dump_value(std::FILE *stream,bool value){std::fprintf(stream,\"%u\",value?1u:0u);}\n"
                << "template<typename T> inline void grhsim_dump_value(std::FILE *stream,const T &value){\n"
                << "if constexpr(std::is_same_v<T,double>){std::uint64_t bits=0;std::memcpy(&bits,&value,sizeof(bits));std::fprintf(stream,\"%016llx\",static_cast<unsigned long long>(bits));}\n"
                << "else if constexpr(std::is_same_v<T,float>){std::uint32_t bits=0;std::memcpy(&bits,&value,sizeof(bits));std::fprintf(stream,\"%08x\",bits);}\n"
                << "else if constexpr(std::is_same_v<T,std::string>){std::fprintf(stream,\"%s\",value.c_str());}\n"
                << "else{using U=std::make_unsigned_t<T>;std::fprintf(stream,\"%0*llx\",static_cast<int>(sizeof(T))*2,static_cast<unsigned long long>(static_cast<U>(value)));}\n"
                << "}\n"
                << "template<typename T,std::size_t N> inline void grhsim_dump_value(std::FILE *stream,const std::array<T,N> &value){for(std::size_t i=N;i-->0;)grhsim_dump_value(stream,value[i]);}\n"
                << "}\n";
            // Contract §1.2: the interface port members open the class body (the
            // first public block's leading run of member declarations).
            out << "class " << class_ << " {\npublic:\n";
            for (const auto &input : model_.inputs()) out << cppType(model_.types()[input.type.index - 1]) << ' ' << identifier(model_.text(input.name)) << "{};\n";
            for (const auto &output : model_.outputs()) out << cppType(model_.types()[output.type.index - 1]) << ' ' << identifier(model_.text(output.name)) << "{};\n";
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
            storeStruct(out, "RegLatchStore", *regLatchStore_);
            out << "RegLatchStore regLatchStore;\nRegLatchStore regLatchStoreNext;\n";
            storeStruct(out, "MemStore", *memStore_);
            out << "MemStore memStore;\n";
            storeStruct(out, "BoundaryValueStore", *boundaryStore_);
            out << "BoundaryValueStore boundaryValueStore;\n";
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
            if (randomSampleCount_)
                out << "std::array<std::uint64_t," << randomSampleCount_ << "> cpu_random_values{};\n"
                    << "std::array<bool," << randomSampleCount_ << "> cpu_random_sampled{};\n";
            out << "void pInput();\nvoid pEvent();\nvoid pGeneral();\nvoid pMem();\nbool pPublish();\nvoid pOutput();\n";
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
            for (uint32_t i = 0; i < memChunks_.size(); ++i) out << "void pMem_c" << i << "();\n";
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
                case CpuNamedStoreKind::RegLatch:
                case CpuNamedStoreKind::Mem:
                    if (field.state) out << " // state=" << model_.text(model_.states()[field.state.index - 1].name);
                    break;
                case CpuNamedStoreKind::Boundary:
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
                    if (chunk.kind == CpuEmitChunkKind::Supernode) scan(supernodeOps_[chunk.offset]);
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
                case CpuEmitChunkKind::Supernode: supernodeChunkFns(out, chunk.offset); break;
                case CpuEmitChunkKind::Mem: memChunkFn(out, chunkId(chunk), chunk); break;
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
            pMemBody(out);
            pPublishBody(out);
            pOutputBody(out);
            evalBody(out);
            dumpStateBody(out);
            if (waveform_) waveformGlue(out);
            systemTaskDriver(out);
        }
        void SixPhaseEmitter::supernodeBody(std::ostream &out, uint32_t ordinal) const
        {
            std::fill(activeLocals_.begin(), activeLocals_.end(), 0);
            out << "void " << class_ << "::" << supernodeName(ordinal) << "(){\n";
            for (const auto opId : supernodeOps_[ordinal])
                emitCompute(out, model_.operations()[opId.index - 1], ordinal);
            out << "}\n";
        }
        void SixPhaseEmitter::supernodeChunkFns(std::ostream &out, uint32_t ordinal) const
        {
            const auto &attrs = tree_.partitions[order_[ordinal].index - 1].attrs;
            const auto &frame = supernodeFrames_[ordinal];
            if (attrs.helperChunks.size() < 2) { supernodeBody(out, ordinal); return; }
            // Chunked supernode (M5d-7): the driver value-initializes the spill
            // frame on its stack and calls the C6 helper chunk members in
            // order; cross-chunk locals live in the frame (cpu_f.v<index>).
            out << "void " << class_ << "::" << supernodeName(ordinal) << "(){\n";
            if (!frame.empty()) out << "SnFrame" << ordinal << " cpu_f{};\n";
            for (std::size_t i = 0; i < attrs.helperChunks.size(); ++i)
                out << supernodeName(ordinal) << "__c" << i << '(' << (frame.empty() ? "" : "cpu_f") << ");\n";
            out << "}\n";
            for (std::size_t i = 0; i < attrs.helperChunks.size(); ++i)
            {
                const auto range = attrs.helperChunks[i];
                out << "void " << class_ << "::" << supernodeName(ordinal) << "__c" << i << '(';
                if (!frame.empty()) out << "SnFrame" << ordinal << " &cpu_f";
                out << "){\n";
                std::fill(activeLocals_.begin(), activeLocals_.end(), 0);
                activateCrossing(frame);
                for (uint32_t i2 = 0; i2 < range.count; ++i2)
                {
                    const auto opId = supernodeOps_[ordinal][range.offset + i2];
                    emitCompute(out, model_.operations()[opId.index - 1], ordinal);
                }
                deactivateCrossing(frame);
                out << "}\n";
            }
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
                out << boundaryRef(field) << '='
                    << constBoundaryInit_.find(field)->second << ";\n";
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
            out << "regLatchStoreNext=regLatchStore;\n";
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
            // supernode fires on dataActiveFlag (cleared on fire); a sink
            // event cluster fires on its eventActStore signature (the
            // signature IS the gate — no flag involved); a sink escape
            // supernode fires unconditionally every round.
            const auto dataActive = model_.text(activeStore_->fields[0].name);
            for (uint32_t ordinal = begin; ordinal < end; ++ordinal)
            {
                const auto &attrs = tree_.partitions[order_[ordinal].index - 1].attrs;
                switch (*attrs.supernodeCategory)
                {
                case CpuSupernodeCategory::SinkEscape:
                    out << "GRHSIM_PERF_COUNT(computeBatchExecCount);\n"
                        << supernodeName(ordinal) << "();\n";
                    break;
                case CpuSupernodeCategory::SinkEvent:
                    out << "if(" << actBitsGuard(*attrs.eventActs) << "){\n"
                        << "GRHSIM_PERF_COUNT(computeBatchExecCount);\n"
                        << supernodeName(ordinal) << "();\n}\n";
                    break;
                case CpuSupernodeCategory::NonSink:
                    out << "if(" << dataActive << '[' << ordinal << "]){" << dataActive << '[' << ordinal << "]=0;\n"
                        << "GRHSIM_PERF_COUNT(computeBatchExecCount);\n"
                        << supernodeName(ordinal) << "();\n}\n";
                    break;
                }
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
            out << "void " << class_ << "::pMem_c" << id << "(){\n";
            if (schedule_.memWritePlan)
                for (uint32_t i = 0; i < chunk.count; ++i)
                {
                    const auto &entry = (*schedule_.memWritePlan)[chunk.offset + i];
                    emitMemWrite(out, model_.operations()[entry.writeOp.index - 1], entry);
                }
            out << "}\n";
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
            const auto portLine = [&](const std::string &name) {
                out << "std::fprintf(stream,\"" << name << "=\");" << dumpPrinter << "(stream,this->" << identifier(name)
                    << ");std::fputc('\\n',stream);\n";
            };
            if (position < model_.inputs().size())
            {
                portLine(std::string(model_.text(model_.inputs()[position].name)));
                return;
            }
            position -= model_.inputs().size();
            if (position < model_.outputs().size())
            {
                portLine(std::string(model_.text(model_.outputs()[position].name)));
                return;
            }
            position -= model_.outputs().size();
            for (const auto *store : {regLatchStore_, memStore_, boundaryStore_, prevEventStore_,
                                      eventActStore_, timeslotStore_, activeStore_})
            {
                if (position >= store->fields.size())
                {
                    position -= store->fields.size();
                    continue;
                }
                const auto &field = store->fields[position];
                if (store == regLatchStore_ || store == memStore_)
                {
                    // Large arrays dump as an fnv1a hash in any store
                    // (regLatch-class arrays joined the regLatch store in
                    // M5d-6).
                    const auto storeName = store == regLatchStore_ ? "regLatchStore" : "memStore";
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
                if (store == boundaryStore_)
                {
                    // The dump label keeps the store field name (trace
                    // consumers parse it); hoisted strings print from
                    // boundaryStrings.
                    out << "std::fprintf(stream,\"boundaryValueStore." << model_.text(field.name) << "=\");" << prefix_
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
            // their write lands without raising dataActiveFlag (spec §3.3).
            const auto dataActive = model_.text(activeStore_->fields[0].name);
            out << "void " << class_ << "::pInput(){\n";
            for (const auto ordinal : randomSupernodes_) out << dataActive << '[' << ordinal << "]=1;\n";
            for (const auto &input : model_.inputs())
            {
                const auto *field = input.id.index < boundaryByInput_.size() ? boundaryByInput_[input.id.index] : nullptr;
                if (!field) throw std::runtime_error("CPU six-phase emit input port has no boundary field");
                const auto slot = "boundaryValueStore." + std::string(model_.text(field->name));
                const auto member = "this->" + identifier(model_.text(input.name));
                out << "if(" << slot << "!=" << member << "){" << slot << '=' << member << ";\n";
                for (const auto ordinal : inputPortFanout_[input.id.index - 1]) out << dataActive << '[' << ordinal << "]=1;\n";
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
        void SixPhaseEmitter::pMemBody(std::ostream &out) const
        {
            out << "void " << class_ << "::pMem(){\n";
            for (uint32_t i = 0; i < memChunks_.size(); ++i) out << "pMem_c" << i << "();\n";
            out << "}\n";
        }
        void SixPhaseEmitter::pPublishBody(std::ostream &out) const
        {
            const auto dataActive = model_.text(activeStore_->fields[0].name);
            const auto dataNext = model_.text(activeStore_->fields[1].name);
            out << "bool " << class_ << "::pPublish(){\n"
                << "GRHSIM_PERF_COUNT(commitBatchExecCount);\n"
                << "bool cpu_fixed=true;\nfor(std::size_t cpu_i=0;cpu_i<" << dataNext << ".size();++cpu_i)if(" << dataNext << "[cpu_i]){cpu_fixed=false;break;}\n"
                << "for(std::size_t cpu_i=0;cpu_i<" << dataNext << ".size();++cpu_i){" << dataActive << "[cpu_i]|=" << dataNext << "[cpu_i];" << dataNext << "[cpu_i]=0;}\n"
                << "std::memcpy(&regLatchStore,&regLatchStoreNext,sizeof(regLatchStore));\n"
                << "return cpu_fixed;\n}\n";
        }
        void SixPhaseEmitter::pOutputBody(std::ostream &out) const
        {
            // M5d-7: the output cone runs in the pOutput_c<k> chunk members
            // (OutputFrame spills cross-chunk locals); the staged latchWrite
            // commit trails every chunk so its ne comparisons all observed the
            // previous eval's values.
            out << "void " << class_ << "::pOutput(){\n";
            if (!outputFrame_.empty()) out << "OutputFrame cpu_f{};\n";
            for (uint32_t i = 0; i < outputChunks_.size(); ++i)
                out << "pOutput_c" << i << '(' << (outputFrame_.empty() ? "" : "cpu_f") << ");\n";
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
                << "pEvent();\npGeneral();\npMem();\n"
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
                out << "this->" << identifier(model_.text(input.name)) << '=' << cppType(model_.types()[input.type.index - 1]) << "{};\n";
            for (const auto &output : model_.outputs())
                out << "this->" << identifier(model_.text(output.name)) << '=' << cppType(model_.types()[output.type.index - 1]) << "{};\n";
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
            resetStore("regLatchStore", "RegLatchStore", *regLatchStore_);
            resetStore("regLatchStoreNext", "RegLatchStore", *regLatchStore_);
            resetStore("memStore", "MemStore", *memStore_);
            resetStore("boundaryValueStore", "BoundaryValueStore", *boundaryStore_);
            resetStore("prevEventStore", "PrevEventStore", *prevEventStore_);
            out << "eventActStore.fill(0);\ntimeslotTriggerFlag.fill(0);\n";
            const auto dataActive = model_.text(activeStore_->fields[0].name);
            const auto dataNext = model_.text(activeStore_->fields[1].name);
            // V2 (M2): the fill(1) also sets the sink ordinals, which no call
            // site ever reads (SinkEvent gates on its signature, SinkEscape
            // fires anyway) — clearing them one by one would just bloat init
            // on large models, so the stale bits stay (harmless, unread).
            out << dataActive << ".fill(1);\n" << dataNext << ".fill(0);\n"
                << "cpu_rng=UINT64_C(0x6a09e667f3bcc909);\n";
            if (!randomFunctions_.empty()) out << "cpu_random_values.fill(0);\ncpu_random_sampled.fill(false);\n";
            // M5d-7: the init stream (steps, constant-boundary preloads,
            // prevEvent inits, regLatchStoreNext sync) runs in the
            // cpu_init_<k> chunk members.
            for (uint32_t i = 0; i < initChunks_.size(); ++i) out << "cpu_init_" << i << "();\n";
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
            out << "std::fprintf(stream,\"" << store << '.' << model_.text(field.name) << "=\");" << prefix_
                << "_dump::grhsim_dump_value(stream,"
                << store << '.' << model_.text(field.name) << ");std::fputc('\\n',stream);\n";
        }
        void SixPhaseEmitter::collectWaveformSignals()
        {
            // Shape rule: a declared symbol is dumpable when its port/state/
            // value reads back as flat two-state logic — scalars of any width
            // (wide scalars are u64 word arrays in the layout), Reals (dumped
            // as their 64-bit pattern) and unpacked arrays with gap-free
            // element storage (element width 8/16/32/64). Strings, four-state
            // values and fields over kWaveMaxBytes (memory-like arrays, which
            // dumpState hashes for the same reason) are skipped, as are
            // symbols whose value was folded into a supernode-local.
            constexpr uint64_t kWaveMaxBytes = 512;
            const auto shape = [&](const Type &type) -> std::optional<std::pair<uint32_t, uint64_t>> {
                switch (type.kind)
                {
                case TypeKind::Logic:
                    if (type.domain == LogicDomain::FourState) return std::nullopt;
                    return std::pair{std::max(type.width, 1u), storageBytes(type)};
                case TypeKind::Real: return std::pair{64u, uint64_t(8)};
                case TypeKind::Array:
                {
                    const auto &element = model_.types()[type.elementType.index - 1];
                    if (element.kind != TypeKind::Logic || element.domain == LogicDomain::FourState)
                        return std::nullopt;
                    switch (element.width)
                    {
                    case 8:
                    case 16:
                    case 32:
                    case 64: break;
                    default: return std::nullopt;
                    }
                    const uint64_t bytes = storageBytes(type);
                    if (bytes > kWaveMaxBytes) return std::nullopt;
                    return std::pair{static_cast<uint32_t>(element.width * type.count), bytes};
                }
                default: return std::nullopt;
                }
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
                WaveSignal signal;
                signal.name = symbol;
                const Type *objectType = nullptr;
                if (const auto it = portTypes.find(symbol); it != portTypes.end())
                {
                    objectType = &model_.types()[it->second.index - 1];
                    signal.ref = "this->" + identifier(symbol);
                }
                else if (const auto it = stateByName.find(symbol); it != stateByName.end())
                {
                    const StateId state = it->second;
                    const auto *field = state.index < regFieldByState_.size() ? regFieldByState_[state.index] : nullptr;
                    if (!field) continue; // mem-class state (memory contents stay out of the FST)
                    objectType = &stateType(state);
                    signal.ref = "regLatchStore." + std::string(model_.text(field->name));
                }
                else if (const auto it = valueByName.find(symbol); it != valueByName.end())
                {
                    const ValueId value = it->second;
                    const auto *field = value.index < boundaryByValue_.size() ? boundaryByValue_[value.index] : nullptr;
                    if (!field) continue; // folded into a supernode-local value
                    objectType = &type(value);
                    signal.ref = "boundaryValueStore." + std::string(model_.text(field->name));
                }
                else
                    continue; // no live object (optimized-away wire)
                const auto signalShape = shape(*objectType);
                if (!signalShape) continue;
                signal.width = signalShape->first;
                signal.bytes = signalShape->second;
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
                out << "CXX ?= c++\nAR ?= ar\nCXXFLAGS ?= -std=c++20 -O3\n";
                if (waveform_)
                    out << "CC ?= cc\nCFLAGS ?= -O2 -D_GNU_SOURCE\nLIBFST_SRC_DIR := " << libfstSourceDir() << '\n';
                out << "SOURCES :=";
                for (const auto &source : sources) out << ' ' << source;
                out << "\nOBJECTS := $(SOURCES:.cpp=.o)\n";
                if (waveform_) out << "FST_OBJECTS := fstapi.o fastlz.o lz4.o\n";
                out << "LIB := lib" << prefix_ << ".a\nall: $(LIB)\n"
                    << "$(LIB): $(OBJECTS)" << (waveform_ ? " $(FST_OBJECTS)" : "") << "\n\t$(AR) rcs $@ $^\n"
                    << "%.o: %.cpp " << prefix_ << ".hpp " << prefix_ << "_runtime.hpp\n"
                    << "\t$(CXX) $(CXXFLAGS)" << (waveform_ ? " -I$(LIBFST_SRC_DIR)" : "") << " -c $< -o $@\n";
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
            explicit EmitPhaseCppPass(std::filesystem::path path, bool waveform)
                : Pass("cpu.st.emit-cpp", PassKind::Emit), path_(std::move(path)), waveform_(waveform) {}
            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                return emitSixPhaseCpuCpp(model, path_, diagnostics, waveform_);
            }
        private:
            std::filesystem::path path_;
            bool waveform_ = false;
        };
    }

    PassResult emitSixPhaseCpuCpp(const GrhSimModel &model, const std::filesystem::path &directory,
                                  wolvrix::lib::diag::Diagnostics &diagnostics, bool waveform)
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
            SixPhaseEmitter emitter(model, waveform);
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
                    "expected --output <empty-directory> [--waveform <off|declared-symbols>] [--perf <off|eval>]";
                if (args.empty() || args.size() % 2) { error = std::string(usage); return {}; }
                std::filesystem::path output;
                bool waveform = false;
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
                    error = std::string(usage); return {};
                }
                if (output.empty()) { error = std::string(usage); return {}; }
                return std::make_unique<EmitPhaseCppPass>(std::move(output), waveform);
            }, error)) throw std::logic_error(error);
    }
}
