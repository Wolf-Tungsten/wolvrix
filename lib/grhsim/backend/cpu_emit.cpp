#include "grhsim/backend/cpu_emit.hpp"

#include "emit/grhsim_runtime.hpp"
#include "emit/readmem.hpp"
#include "grhsim/backend/cpu.hpp"
#include "grhsim/dialect/registry.hpp"
#include "grhsim/ir/verifier.hpp"
#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wolvrix::lib::grhsim
{
    namespace
    {
        std::string identifier(std::string_view text)
        {
            std::string result;
            for (char ch : text)
                result += (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                          (ch >= '0' && ch <= '9') || ch == '_' ? ch : '_';
            if (result.empty() || (result.front() >= '0' && result.front() <= '9')) result.insert(0, "n_");
            return result;
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

        class Emitter
        {
        public:
            explicit Emitter(const GrhSimModel &model, bool dynamicStats = false, bool commitCompactWalk = false,
                             bool commitMemWalk = false)
                : model_(model), mapping_(*model.cpuMapping()), layout_(*mapping_.dataLayout), schedule_(*mapping_.schedule),
                  prefix_("grhsim_" + identifier(model.text(model.name()))), class_("GrhSIM_" + identifier(model.text(model.name()))),
                  wordOffsets_(mapping_.partitionTree.partitions.size() + 1), armOffsets_(wordOffsets_.size()), frameSizes_(wordOffsets_.size()),
                  activeOffsets_(wordOffsets_.size()), activeMasks_(wordOffsets_.size()), stateRanges_(model.states().size() + 1),
                  projected_(stateRanges_.size()), fanout_(model.values().size() + 1), producers_(model.values().size() + 1),
                  batchedHistories_(stateRanges_.size()),
                  directCommitStates_(stateRanges_.size()), privateByteHistories_(stateRanges_.size()),
                  historyAliases_(stateRanges_.size()), sharedHistoryEligible_(stateRanges_.size()),
                  commitDirectHistories_(stateRanges_.size()), dynamicStats_(dynamicStats),
                  commitCompactWalk_(commitCompactWalk), commitMemWalk_(commitMemWalk)
            {
                if (dynamicStats_)
                {
                    std::set<std::string> kinds;
                    for (const auto &op : model_.operations()) kinds.emplace(model_.text(op.opType));
                    for (const auto &kind : kinds) dynKinds_.emplace(kind, dynKinds_.size());
                    for (const auto &task : schedule_.numaNodes[0].cores[0].tasks)
                        dynTaskSpan_ = std::max(dynTaskSpan_, task.id.index + 1);
                }
                for (const auto &frame : layout_.localFrames) frameSizes_[frame.owner.index] = frame.size;
                if (layout_.helperReadCaches)
                    for (const auto &cache : *layout_.helperReadCaches)
                    {
                        helperReadCaches_.emplace(cache.firstOp.index, &cache.values);
                        helperReadCacheValues_ += cache.values.size();
                    }
                for (const auto &op : model_.operations())
                {
                    for (const auto result : model_.results(op))
                        producers_[result.index] = op.id;
                    if (model_.text(op.opType) == "core.compute.constant" && model_.results(op).size() == 1 &&
                        type(model_.results(op)[0]).kind == TypeKind::String)
                        staticStrings_.emplace(model_.results(op)[0].index, expression(op));
                    if (model_.text(op.opType) == "core.compute.constant" && model_.results(op).size() == 1 &&
                        isScalarLogic(type(model_.results(op)[0])))
                        staticScalars_.emplace(model_.results(op)[0].index, expression(op));
                    if (model_.text(op.opType) == "core.system.task")
                    {
                        hasSystemTasks_ = true;
                        const auto params = model_.parameters(op);
                        const auto *proc = parameter<std::string>(model_, params, "proc_kind");
                        const auto *timed = parameter<bool>(model_, params, "has_timing");
                        if (proc && *proc == "initial" && timed && *timed)
                            onceTasks_.emplace(op.id.index, onceTasks_.size());
                    }
                }
                localStrings_.resize(frameSizes_.size());
                for (const auto &entry : layout_.objects)
                    if (layout_.types[entry.slot.type.index - 1].kind == CpuTypeKind::String)
                        persistentStrings_.push_back({"cpu_objects.get()", entry.slot.offset});
                for (std::size_t i = 0; i < layout_.values.size(); ++i)
                {
                    const auto &slot = layout_.values[i];
                    if (layout_.types[slot.type.index - 1].kind == CpuTypeKind::String && !staticStrings_.contains(i + 1))
                    {
                        if (slot.kind == CpuStorageKind::Boundary)
                            persistentStrings_.push_back({"cpu_boundary.get()", slot.offset});
                        else localStrings_[slot.owner.index].push_back(slot.offset);
                    }
                }
                for (const auto &shadow : schedule_.inputShadows)
                    if (layout_.types[shadow.type.index - 1].kind == CpuTypeKind::String)
                        persistentStrings_.push_back({"cpu_inputs.data()", shadow.offset});
                for (const auto &slot : layout_.runtime)
                    if (slot.kind == CpuRuntimeKind::ActiveWord) wordOffsets_[slot.owner.index] = slot.offset;
                    else if (slot.kind == CpuRuntimeKind::DomainArm) armOffsets_[slot.owner.index] = slot.offset;
                for (const auto &partition : mapping_.partitionTree.partitions)
                    if (partition.attrs.activeId)
                    {
                        activeOffsets_[partition.id.index] = wordOffsets_[partition.parent.index];
                        activeMasks_[partition.id.index] = 1u << (*partition.attrs.activeId % 8);
                    }
                for (const auto &row : schedule_.computeSupernodeFanout) fanout_[row.source.index] = &row.targets;
                projected_ = schedule_.quiescenceProjection;
                if (projected_.size() < model_.states().size() + 1) projected_.resize(model_.states().size() + 1, false);
                planReadAliases();
                for (const auto &row : schedule_.commitStateFanout)
                {
                    auto &range = stateRanges_[row.source.index];
                    range.offset = static_cast<uint32_t>(stateTargets_.size());
                    std::map<uint32_t, uint32_t> masks;
                    for (auto target : row.targets.activate)
                        masks[activeOffsets_[target.index]] |= activeMasks_[target.index];
                    for (auto target : aliasConsumers_[row.source.index])
                        masks[activeOffsets_[target.index]] |= activeMasks_[target.index];
                    for (auto [offset, mask] : masks) stateTargets_.push_back({offset, mask, false});
                    for (auto target : row.targets.arm) stateTargets_.push_back({armOffsets_[target.index], 1, true});
                    range.count = static_cast<uint32_t>(stateTargets_.size()) - range.offset;
                }
                planMemoryCells();
            }

            void planReadAliases()
            {
                readAliases_.resize(model_.values().size() + 1);
                aliasConsumers_.resize(model_.states().size() + 1);
                computeOwners_.resize(model_.operations().size() + 1);
                const auto &tree = mapping_.partitionTree;
                for (const auto &task : schedule_.numaNodes[0].cores[0].tasks)
                    if (task.execution == CpuExecution::ActivityDrivenCompute)
                        for (auto word : tree.partitions[task.partition.index - 1].children)
                            for (auto unit : tree.partitions[word.index - 1].children)
                                for (auto node : tree.partitions[unit.index - 1].children)
                                    for (auto op : tree.partitions[node.index - 1].ops) computeOwners_[op.index] = unit;
                std::vector<bool> snapshot(readAliases_.size());
                const auto &projected = projected_;
                for (const auto &op : model_.operations())
                    if (!computeOwners_[op.id.index])
                        for (auto operand : model_.operands(op)) snapshot[operand.index] = true;
                for (const auto &partition : tree.partitions)
                    if (partition.attrs.eventGate)
                        for (auto event : partition.attrs.eventGate->events) snapshot[event.value.index] = true;
                for (const auto &op : model_.operations())
                    if (model_.text(op.opType) == "core.state.read" && computeOwners_[op.id.index])
                    {
                        const auto result = model_.results(op)[0];
                        const StateId source{model_.objectRefs(op)[0].index, 0};
                        // Commit operands must retain their pre-commit snapshot, even for a single writer.
                        if (projected[source.index] && !snapshot[result.index] && type(result).kind == TypeKind::Logic &&
                            model_.values()[result.index - 1].type == model_.states()[source.index - 1].type)
                            readAliases_[result.index] = source;
                    }
                for (const auto &op : model_.operations())
                    for (auto operand : model_.operands(op))
                        if (const auto source = readAliases_[operand.index])
                            aliasConsumers_[source.index].push_back(computeOwners_[op.id.index]);
                for (auto &consumers : aliasConsumers_)
                {
                    std::sort(consumers.begin(), consumers.end(), [](auto a, auto b) { return a.index < b.index; });
                    consumers.erase(std::unique(consumers.begin(), consumers.end()), consumers.end());
                }
            }

            void planMemoryCells()
            {
                memoryDirtyBases_.resize(model_.states().size() + 1);
                memoryRanges_.resize(memoryDirtyBases_.size());
                memoryReadIds_.resize(model_.operations().size() + 1);
                dirtyBytes_ = model_.states().size() + 1;
                std::vector<std::vector<OpId>> reads(memoryDirtyBases_.size());
                for (const auto &op : model_.operations())
                    if (model_.text(op.opType) == "core.state.memRead")
                        reads[model_.objectRefs(op)[0].index].push_back(op.id);
                for (const auto &state : model_.states())
                {
                    const auto &array = stateType(state.id);
                    if (array.kind != TypeKind::Array) continue;
                    memoryDirtyBases_[state.id.index] = dirtyBytes_;
                    dirtyBytes_ += array.count;
                    auto &range = memoryRanges_[state.id.index];
                    range.offset = memoryReaders_.size();
                    for (auto op : reads[state.id.index])
                    {
                        const auto owner = computeOwners_[op.index];
                        if (!owner) throw std::runtime_error("CPU memory reader must be compute-owned");
                        memoryReadIds_[op.index] = memoryReaders_.size() + 1;
                        memoryReaders_.push_back({activeOffsets_[owner.index], activeMasks_[owner.index], false});
                    }
                    range.count = memoryReaders_.size() - range.offset;
                }
            }

            void validate()
            {
                for (const auto &type : model_.types())
                {
                    if (type.kind == TypeKind::Logic &&
                        type.domain != LogicDomain::TwoState)
                        throw std::runtime_error("CPU C++ emit currently requires two-state logic types of at most 64 bits");
                    if (type.kind == TypeKind::Array && containsString(type))
                        throw std::runtime_error("CPU C++ emit arrays of string handles are not implemented");
                }
                for (const auto &state : model_.states())
                    if (containsString(model_.types()[state.type.index - 1]))
                        throw std::runtime_error("CPU C++ emit string state initialization and commit are not implemented");
                std::set<std::string> names{
                    class_, "init", "eval", "set_runtime_profile_enabled", "dump_runtime_profile", "cpu_at",
                    "cpu_objects", "cpu_shadow", "cpu_boundary", "cpu_inputs", "cpu_strings", "cpu_bind_strings",
                    "cpu_rng", "cpu_flags", "cpu_next_arms", "cpu_dirty", "Pending", "Target", "cpu_targets",
                    "cpu_pending", "cpu_stage", "cpu_write_scalar", "cpu_stage_bytes", "cpu_publish", "cpu_direct_again", "cpu_direct_state_changed", "cpu_direct_state_changed_one",
                    "cpu_bitwise_words_changed", "cpu_arithmetic_words_changed", "cpu_shift_words_changed", "cpu_replicate_words_changed", "cpu_active_word",
                    "CpuRuntimeProfile", "cpu_runtime_profile", "cpu_profile_enabled", "cpu_profile_data", "cpu_profile",
                    "cpu_profile_clock", "cpu_profile_eval_begin", "cpu_profile_phase_begin", "cpu_profile_tick",
                    "cpu_stage_cell", "cpu_write_cell", "cpu_stage_bytes_overwrite", "cpu_memory_readers", "cpu_read_offsets", "cpu_pflags", "cpu_armed", "cpu_consumed",
                    "cpu_rword", "cpu_rchanged", "cpu_rnext"};
                if (dynamicStats_)
                    for (const auto *name : {"cpu_dyn_wr", "cpu_dyn_ch", "cpu_dyn_silent", "cpu_dyn_sn_act", "cpu_dyn_sn_body",
                                             "cpu_dyn_sn_grp", "cpu_dyn_sn_chg", "cpu_dyn_cm_ent", "cpu_dyn_grp_pub", "cpu_dyn_grp_fire",
                                             "cpu_dyn_port_eval", "cpu_dyn_port_fire", "cpu_dyn_in_chk", "cpu_dyn_in_chg", "cpu_dyn_pub_calls",
                                             "cpu_dyn_pub_pending", "cpu_dyn_pub_changes", "cpu_dyn_cm_stable", "cpu_dyn_cm_inactive",
                                             "cpu_dyn_edge", "cpu_dyn_round_cur", "cpu_dyn_eval_pos", "cpu_dyn_eval_neg", "cpu_dyn_eval_other",
                                             "cpu_dyn_round_hist", "cpu_dyn_port_eval_pos", "cpu_dyn_port_eval_neg", "cpu_dyn_port_eval_r0",
                                             "cpu_dyn_port_eval_rN", "cpu_dyn_port_fire_pos", "cpu_dyn_port_fire_neg", "cpu_dyn_mw_gate_pos",
                                             "cpu_dyn_mw_gate_neg", "cpu_dyn_mw_fire_pos", "cpu_dyn_mw_fire_neg", "cpu_dyn_cm_ent_pos",
                                             "cpu_dyn_cm_ent_neg", "cpu_dyn_cm_ent_r0", "cpu_dyn_cm_ent_rN", "cpu_dyn_sn_act_pos",
                                             "cpu_dyn_sn_act_neg", "cpu_dyn_pub_pending_r0", "cpu_dyn_pub_pending_rN", "cpu_dyn_pub_changes_r0",
                                             "cpu_dyn_pub_changes_rN", "cpu_dyn_pub_pending_pos", "cpu_dyn_pub_pending_neg", "cpu_dyn_pub_changes_pos",
                                             "cpu_dyn_pub_changes_neg"})
                        names.insert(name);
                if (hasSystemTasks_)
                    for (const auto *name : {"cpu_first_eval", "cpu_system_done", "cpu_strobes", "cpu_system_task"}) names.insert(name);
                for (std::size_t i = 0; i < initChunkCount(); ++i) names.insert("cpu_init_" + std::to_string(i));
                for (const auto &task : schedule_.numaNodes[0].cores[0].tasks) names.insert("cpu_task_" + std::to_string(task.id.index));
                for (const auto &partition : mapping_.partitionTree.partitions)
                    for (std::size_t i = 0; i < partition.attrs.helperChunks.size(); ++i)
                        names.insert("cpu_helper_" + std::to_string(partition.id.index) + '_' + std::to_string(i));
                for (const auto &input : model_.inputs()) validatePort(model_.text(input.name), names);
                for (const auto &output : model_.outputs()) validatePort(model_.text(output.name), names);
                std::map<std::string, std::string> declarations;
                for (const auto &function : model_.functions())
                {
                    const auto symbol = identifier(model_.text(function.symbol));
                    const auto declaration = dpiDeclaration(function);
                    const auto [it, inserted] = declarations.emplace(symbol, declaration);
                    if (!inserted && it->second != declaration)
                        throw std::runtime_error("CPU DPI imports have conflicting C signatures: " + symbol);
                }
                for (const auto &op : model_.operations())
                {
                    const auto type = model_.text(op.opType);
                    if (type == "core.state.regWrite" || type == "core.state.latchWrite" ||
                        type == "core.state.memWrite" || type == "core.state.memWriteSeq" ||
                        type == "core.state.memFill" || type == "core.state.memAssign") continue;
                    if (type == "core.output.write") continue;
                    if (type == "core.system.task") { validateSystemTask(op); continue; }
                    if (type == "core.dpi.call") { validateDpiCall(op); continue; }
                    if (model_.results(op).size() != 1) throw std::runtime_error("CPU C++ emit unsupported result arity: " + std::string(type));
                    (void)expression(op);
                }
                struct DiscardBuffer : std::streambuf
                {
                    std::streamsize xsputn(const char *, std::streamsize count) override { return count; }
                    int_type overflow(int_type ch) override { return traits_type::not_eof(ch); }
                } buffer;
                std::ostream discard(&buffer);
                for (const auto &record : model_.initRecords())
                {
                    const auto &target = stateType(record.state);
                    if (target.kind != TypeKind::Array && model_.steps(record).size() != 1)
                        throw std::runtime_error("CPU scalar state requires exactly one full initializer");
                    std::vector<std::pair<uint64_t, uint64_t>> covered;
                    for (const auto &step : model_.steps(record)) initStep(discard, record.state, step, &covered);
                    if (target.kind != TypeKind::Array) continue;
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
                planDirectCommits();
                planSharedHistories();
                planComputeSharedHistories();
                planComputeEdgeGuards();
                planHistoryBatches();
                planCommitDirectHistories();
                planPortArms();
                planComputeQuiescence();
                planDirectSampling();
            }

            std::string historyBatchSummary() const
            {
                return "history_candidates=" + std::to_string(historyCandidates_) +
                    " history_private_rejected=" + std::to_string(historyPrivateRejected_) +
                    " history_layout_rejected=" + std::to_string(historyLayoutRejected_) +
                    " history_batch_states=" + std::to_string(historyBatchStates_) +
                    " history_batches=" + std::to_string(historyBatchCount_) +
                    " history_max_batch=" + std::to_string(historyMaxBatch_) +
                    " history_max_pattern=" + std::to_string(historyMaxPattern_) +
                    " history_shared_states=" + std::to_string(sharedHistoryCount_) +
                    " history_shared_tasks=" + std::to_string(sharedHistoryTasks_) +
                    " compute_history_aliases=" + std::to_string(computeSharedHistoryCount_) +
                    " compute_history_alias_units=" + std::to_string(computeSharedHistoryUnits_) +
                    " compute_guard_snapshots=" + std::to_string(computeGuardSnapshots_) +
                    " compute_guard_snapshot_uses=" + std::to_string(computeGuardUses_) +
                    " helper_read_cache_values=" + std::to_string(helperReadCacheValues_) +
                    " compute_quiescence_units=" + std::to_string(computeQuiescenceUnits_) +
                    " compute_quiescence_terms=" + std::to_string(computeQuiescenceTerms_) +
                    " edge_direction_units=" + std::to_string(edgeDirectionUnits_) +
                    " direct_sample_states=" + std::to_string(directSampleStateCount_) +
                    " direct_sample_units=" + std::to_string(directSampleUnitCount_) +
                    " direct_commit_states=" + std::to_string(directCommitCount_) +
                    " commit_direct_histories=" + std::to_string(commitDirectHistoryCount_) +
                    " port_arm_ports=" + std::to_string(portArmPortCount_) +
                    " port_arm_tasks=" + std::to_string(portArmTaskCount_) +
                    " port_arm_values=" + std::to_string(portArmValueCount_) +
                    " port_arm_words=" + std::to_string(portArmWordCount_) +
                    " state_read_aliases=" + std::to_string(std::count_if(readAliases_.begin(), readAliases_.end(), [](auto id) { return bool(id); })) +
                    " scalar_constants=" + std::to_string(staticScalars_.size()) +
                    " memory_cell_readers=" + std::to_string(memoryReaders_.size());
            }

            std::string packSummary() const
            {
                return "dispatch_packed_checks=" + std::to_string(dispatchPackedBytes_) +
                    " handoff_packed_slots=" + std::to_string(handoffPackedSlots_) +
                    " port_arm_walk_packed_words=" + std::to_string(pflagPackedWords_) +
                    " shared_edge_blocks=" + std::to_string(sharedEdgeBlocks_) +
                    " shared_edge_ports=" + std::to_string(sharedEdgePorts_) +
                    " gate_hoisted_runs=" + std::to_string(gateHoistedRuns_) +
                    " gate_hoisted_gates=" + std::to_string(gateHoistedGates_) +
                    " gate_merged_gates=" + std::to_string(gateMergedGates_) +
                    " gate_cold_hints=" + std::to_string(gateColdHints_) +
                    " commit_compact_groups=" + std::to_string(commitCompactGroups_) +
                    " commit_compact_ports=" + std::to_string(commitCompactPorts_) +
                    " mem_guard_hoist_runs=" + std::to_string(memGuardHoistRuns_) +
                    " mem_guard_hoist_sites=" + std::to_string(memGuardHoistSites_) +
                    " mem_enable_cache_values=" + std::to_string(memEnableCacheValues_) +
                    " mem_enable_cache_sites=" + std::to_string(memEnableCacheSites_);
            }

            PassResult write(const std::filesystem::path &directory)
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
                file(prefix_ + "_runtime.hpp", [&](auto &out) { emit::writeGrhSimRuntime(out, {.systemTasks = hasSystemTasks_}); });
                file(prefix_ + ".hpp", [&](auto &out) { header(out); });
                const auto main = prefix_ + ".cpp"; sources.push_back(main);
                file(main, [&](auto &out) { driver(out); });
                constexpr std::size_t initChunkSteps = 4096;
                std::size_t initChunk = 0;
                std::size_t initSteps = 0;
                for (const auto &record : model_.initRecords())
                    for (const auto &step : model_.steps(record))
                    {
                        if (initSteps % initChunkSteps == 0)
                        {
                            const auto source = prefix_ + "_init_" + std::to_string(initChunk) + ".cpp";
                            sources.push_back(source);
                            file(source, [&](auto &out) { initBody(out, initChunk, initSteps); });
                            ++initChunk;
                        }
                        ++initSteps;
                    }
                for (const auto &task : schedule_.numaNodes[0].cores[0].tasks)
                {
                    const auto source = prefix_ + "_task_" + std::to_string(task.id.index) + ".cpp";
                    sources.push_back(source); file(source, [&](auto &out) { taskBody(out, task); });
                }
                file("Makefile", [&](auto &out) {
                    out << "CXX ?= c++\nAR ?= ar\nCXXFLAGS ?= -std=c++20 -O3\nSOURCES :=";
                    for (const auto &source : sources) out << ' ' << source;
                    out << "\nOBJECTS := $(SOURCES:.cpp=.o)\nLIB := lib" << prefix_ << ".a\nall: $(LIB)\n"
                        << "$(LIB): $(OBJECTS)\n\t$(AR) rcs $@ $^\n%.o: %.cpp " << prefix_ << ".hpp " << prefix_
                        << "_runtime.hpp\n\t$(CXX) $(CXXFLAGS) -c $< -o $@\n.PHONY: all\n";
                });
                return {true, false, std::move(artifacts)};
            }

        private:
            struct PortArmTarget
            {
                std::uint32_t offset;
                std::uint32_t mask;
            };

            void planDirectCommits()
            {
                std::vector<uint32_t> references(model_.states().size() + 1), allowed(references.size()), writers(references.size());
                for (auto ref : model_.objectRefPool())
                    if (ref.kind == ObjectKind::State) ++references[ref.index];
                for (const auto &op : model_.operations())
                {
                    const auto name = model_.text(op.opType);
                    const auto refs = model_.objectRefs(op);
                    if (name == "core.state.read")
                        for (auto ref : refs) ++allowed[ref.index];
                    else if (name == "core.state.regWrite" || name == "core.state.latchWrite")
                    {
                        ++allowed[refs.front().index];
                        ++writers[refs.front().index];
                    }
                }
                for (const auto &state : model_.states())
                {
                    const auto &type = stateType(state.id);
                    if (writers[state.id.index] == 1 && references[state.id.index] == allowed[state.id.index] &&
                        type.kind == TypeKind::Logic && type.domain == LogicDomain::TwoState && type.width > 0 && type.width <= 64)
                    {
                        directCommitStates_[state.id.index] = true;
                        ++directCommitCount_;
                    }
                }
            }

            // A direct-commit write port whose enable/data/mask operands are
            // all boundary-resident scalar values may skip evaluation at an
            // edge when none of those operands changed since the port's last
            // evaluation: the unique writer keeps `current` stable, so the
            // recomputed value would equal it and neither the write nor the
            // fanout notification would happen. planPortArms assigns each
            // such op a bit in the per-model cpu_pflags activity bytes and
            // records, per operand value, the (word, mask) targets that its
            // compute-side change publish must arm.
            bool armableCommitPort(const SimOp &op, const std::vector<OpId> &producerOf) const
            {
                const auto name = model_.text(op.opType);
                if (name != "core.state.regWrite" && name != "core.state.latchWrite") return false;
                const auto refs = model_.objectRefs(op);
                const auto operands = model_.operands(op);
                if (refs.empty() || operands.size() < 3) return false;
                if (!directCommitStates_[refs.front().index]) return false;
                for (std::size_t i = 0; i < 3; ++i)
                {
                    const auto operand = operands[i];
                    if (readAliases_[operand.index]) return false;
                    const auto &operandType = type(operand);
                    if (operandType.kind != TypeKind::Logic || operandType.domain != LogicDomain::TwoState ||
                        operandType.width == 0 || operandType.width > 64) return false;
                    if (layout_.values[operand.index - 1].kind != CpuStorageKind::Boundary) return false;
                    const auto producerId = producerOf[operand.index];
                    if (!producerId.index) return false;
                    const auto producerName = model_.text(model_.operations()[producerId.index - 1].opType);
                    if (!producerName.starts_with("core.compute.") && producerName != "core.state.read" &&
                        producerName != "core.state.memRead" && producerName != "core.input.read") return false;
                }
                return true;
            }

            void planPortArms()
            {
                portArmWords_.assign(model_.operations().size() + 1, ~std::uint32_t(0));
                portArmBits_.assign(model_.operations().size() + 1, 0);
                portArmTargets_.assign(model_.values().size() + 1, {});
                std::vector<OpId> producerOf(model_.values().size() + 1);
                for (const auto &op : model_.operations())
                    for (auto result : model_.results(op)) producerOf[result.index] = op.id;
                const auto &tree = mapping_.partitionTree;
                for (const auto &task : schedule_.numaNodes[0].cores[0].tasks)
                {
                    if (task.execution == CpuExecution::ActivityDrivenCompute) continue;
                    std::uint32_t ordinal = 0;
                    bool any = false;
                    for (auto unit : tree.partitions[task.partition.index - 1].children)
                        for (auto opId : tree.partitions[unit.index - 1].ops)
                        {
                            const auto &op = model_.operations()[opId.index - 1];
                            if (!armableCommitPort(op, producerOf)) continue;
                            any = true;
                            const std::uint32_t word = portArmWordCount_ + ordinal / 8;
                            portArmWords_[opId.index] = word;
                            portArmBits_[opId.index] = static_cast<std::uint8_t>(ordinal % 8);
                            const auto operands = model_.operands(op);
                            for (std::size_t i = 0; i < 3; ++i)
                                portArmTargets_[operands[i].index].push_back({word, std::uint32_t(1u) << (ordinal % 8)});
                            ++ordinal;
                        }
                    if (any) ++portArmTaskCount_;
                    portArmWordCount_ += (ordinal + 7) / 8;
                    portArmPortCount_ += ordinal;
                }
                for (auto &targets : portArmTargets_)
                {
                    if (targets.empty()) continue;
                    std::map<std::uint32_t, std::uint32_t> merged;
                    for (const auto target : targets) merged[target.offset] |= target.mask;
                    std::vector<PortArmTarget> flat;
                    flat.reserve(merged.size());
                    for (const auto [offset, mask] : merged) flat.push_back({offset, mask});
                    targets = std::move(flat);
                    ++portArmValueCount_;
                }
            }

            const std::vector<PortArmTarget> *portArmTargets(ValueId value) const
            {
                if (value.index >= portArmTargets_.size() || portArmTargets_[value.index].empty()) return nullptr;
                return &portArmTargets_[value.index];
            }

            void armPorts(std::ostream &out, const std::vector<PortArmTarget> &targets, const std::string &condition) const
            {
                for (const auto &target : targets)
                    out << "cpu_pflags[" << target.offset << "] |= (static_cast<std::uint8_t>(-static_cast<std::uint8_t>("
                        << condition << ")) & " << target.mask << ");\n";
            }

            struct QuiescenceTerm { StateId history; ValueId event; };

            // Compute-side edge-quiescence skip: a unit whose every external
            // effect is edge-guarded (system/DPI calls and their embedded
            // history samples) is provably inert when hist == event holds for
            // every guard term at unit entry — all edge guards are false and
            // every sample is a current==next no-op. Frame and frame-local
            // computations are per-invocation temporaries. The body of such a
            // unit is wrapped in an entry check; scheduling is untouched.
            void planComputeQuiescence()
            {
                std::vector<uint32_t> references(model_.states().size() + 1);
                for (auto ref : model_.objectRefPool())
                    if (ref.kind == ObjectKind::State) ++references[ref.index];
                std::vector<std::vector<uint32_t>> preimage(model_.states().size() + 1);
                for (std::size_t index = 1; index < historyAliases_.size(); ++index)
                    if (historyAliases_[index]) preimage[historyAliases_[index].index].push_back(index);
                const auto &tree = mapping_.partitionTree;
                for (const auto &task : schedule_.numaNodes[0].cores[0].tasks)
                {
                    if (task.execution != CpuExecution::ActivityDrivenCompute) continue;
                    for (auto word : tree.partitions[task.partition.index - 1].children)
                    for (auto unit : tree.partitions[word.index - 1].children)
                    {
                        const auto &partition = tree.partitions[unit.index - 1];
                        std::vector<OpId> ops;
                        for (auto node : partition.children)
                            ops.insert(ops.end(), tree.partitions[node.index - 1].ops.begin(), tree.partitions[node.index - 1].ops.end());
                        std::set<uint32_t> produced;
                        for (auto id : ops)
                            for (auto result : model_.results(model_.operations()[id.index - 1])) produced.insert(result.index);
                        std::vector<QuiescenceTerm> terms;
                        std::map<uint32_t, uint32_t> termRefs;
                        bool eligible = true;
                        uint32_t edgeEvent = 0;
                        int edgeDirection = 0;
                        bool edgeUniform = true;
                        for (auto id : ops)
                        {
                            if (!eligible) break;
                            const auto &op = model_.operations()[id.index - 1];
                            const auto name = model_.text(op.opType);
                            const bool system = name == "core.system.task", dpi = name == "core.dpi.call";
                            if (system || dpi)
                            {
                                if (!model_.results(op).empty()) { eligible = false; break; }
                                const auto *edges = parameter<std::vector<std::string>>(model_, model_.parameters(op), "event_edges");
                                if (!edges || edges->empty()) { eligible = false; break; }
                                const auto events = model_.operands(op).last(edges->size());
                                const auto refs = model_.objectRefs(op);
                                const std::size_t historyBase = dpi ? 1 : 0;
                                if (refs.size() < historyBase + edges->size()) { eligible = false; break; }
                                for (std::size_t i = 0; i < edges->size() && eligible; ++i)
                                {
                                    if (refs[historyBase + i].kind != ObjectKind::State) { eligible = false; break; }
                                    const auto event = events[i];
                                    // The edge-direction fast path needs one shared
                                    // (event, direction) across every guarded op.
                                    {
                                        const int direction = (*edges)[i] == "posedge" ? 1 : (*edges)[i] == "negedge" ? -1 : 0;
                                        if (direction == 0) edgeUniform = false;
                                        else if (edgeDirection == 0)
                                        {
                                            edgeEvent = event.index;
                                            edgeDirection = direction;
                                        }
                                        else if (edgeEvent != event.index || edgeDirection != direction) edgeUniform = false;
                                    }
                                    // Same in-unit invariance test as planComputeEdgeGuards.
                                    if (produced.contains(event.index) ||
                                        (!readAliases_[event.index] && layout_.values[event.index - 1].kind != CpuStorageKind::Boundary))
                                    { eligible = false; break; }
                                    const auto alias = historyAliases_[refs[historyBase + i].index];
                                    const StateId history{alias ? alias.index : refs[historyBase + i].index, 0};
                                    if (batchedHistories_[history.index]) { eligible = false; break; }
                                    ++termRefs[history.index];
                                    const auto duplicate = std::any_of(terms.begin(), terms.end(), [&](const QuiescenceTerm &term) {
                                        return term.history.index == history.index && term.event.index == event.index; });
                                    if (!duplicate) terms.push_back({history, event});
                                }
                                continue;
                            }
                            const auto results = model_.results(op);
                            if (results.size() != 1) { eligible = false; break; }
                            const auto result = results[0];
                            if (readAliases_[result.index]) continue;
                            if (type(result).kind == TypeKind::String && staticStrings_.contains(result.index)) continue;
                            if (memoryReadIds_[op.id.index] || fanout_[result.index] || portArmTargets(result) ||
                                layout_.values[result.index - 1].kind != CpuStorageKind::PartitionLocal)
                            { eligible = false; break; }
                        }
                        if (!eligible || terms.empty()) continue;
                        // A guard history must be referenced only by this unit's
                        // own edge terms (including alias preimages): shared
                        // histories overwrite pending values in program order,
                        // so an external sampler would make the skipped sample
                        // observable.
                        for (const auto &[resolved, count] : termRefs)
                        {
                            uint32_t total = references[resolved];
                            for (auto index : preimage[resolved]) total += references[index];
                            if (total != count) { eligible = false; break; }
                        }
                        if (!eligible) continue;
                        computeQuiescenceTerms_ += terms.size();
                        ++computeQuiescenceUnits_;
                        if (edgeUniform && edgeDirection != 0)
                        {
                            computeEdgeDirection_[unit.index] = edgeDirection;
                            ++edgeDirectionUnits_;
                        }
                        computeQuiescence_[unit.index] = std::move(terms);
                    }
                }
            }

            // Direct in-place sampling for unit-private event histories. A history
            // state referenced only by one compute unit's own edge-history positions
            // has no state reader or activation fanout that could observe the
            // shadow/pending/publish round trip: every in-body read (edge guards,
            // quiescence check) sees the round-start value, and the deferred store
            // at the unit block end becomes visible at the same time publish would
            // have made it visible (the unit runs at most once per round and nothing
            // else reads the state). Certified conservatively from objectRef
            // reachability; anything doubtful keeps the staged write path.
            void planDirectSampling()
            {
                directSampleStates_.assign(model_.states().size() + 1, 0);
                const auto &tree = mapping_.partitionTree;
                std::vector<std::uint32_t> opUnit(model_.operations().size() + 1, 0);
                std::map<std::uint32_t, std::set<uint32_t>> unitProduced;
                for (const auto &task : schedule_.numaNodes[0].cores[0].tasks)
                {
                    if (task.execution != CpuExecution::ActivityDrivenCompute) continue;
                    for (auto word : tree.partitions[task.partition.index - 1].children)
                    for (auto unit : tree.partitions[word.index - 1].children)
                    {
                        const auto &partition = tree.partitions[unit.index - 1];
                        for (auto node : partition.children)
                            for (auto opId : tree.partitions[node.index - 1].ops)
                            {
                                opUnit[opId.index] = unit.index;
                                for (auto result : model_.results(model_.operations()[opId.index - 1]))
                                    unitProduced[unit.index].insert(result.index);
                            }
                    }
                }
                std::vector<std::uint32_t> references(model_.states().size() + 1, 0);
                for (auto ref : model_.objectRefPool())
                    if (ref.kind == ObjectKind::State) ++references[ref.index];
                std::vector<std::vector<uint32_t>> preimage(model_.states().size() + 1);
                for (std::size_t index = 1; index < historyAliases_.size(); ++index)
                    if (historyAliases_[index]) preimage[historyAliases_[index].index].push_back(index);

                struct SampleSite { uint32_t unit; ValueId event; bool direct; };
                std::vector<std::vector<SampleSite>> sites(model_.states().size() + 1);
                for (const auto &op : model_.operations())
                {
                    const std::uint32_t unit = opUnit[op.id.index];
                    if (!unit) continue;
                    const auto name = model_.text(op.opType);
                    const bool system = name == "core.system.task", dpi = name == "core.dpi.call";
                    if (!system && !dpi) continue;
                    const auto *edges = parameter<std::vector<std::string>>(model_, model_.parameters(op), "event_edges");
                    if (!edges || edges->empty()) continue;
                    const auto refs = model_.objectRefs(op);
                    const std::size_t historyBase = dpi ? 1 : 0;
                    if (refs.size() < historyBase + edges->size()) continue;
                    const auto events = model_.operands(op).last(edges->size());
                    for (std::size_t i = 0; i < edges->size(); ++i)
                    {
                        if (refs[historyBase + i].kind != ObjectKind::State) continue;
                        const auto raw = refs[historyBase + i].index;
                        const auto alias = historyAliases_[raw];
                        const std::uint32_t resolved = alias ? alias.index : raw;
                        sites[resolved].push_back({unit, events[i], resolved == raw});
                    }
                }
                for (std::size_t index = 1; index < sites.size(); ++index)
                {
                    const auto &list = sites[index];
                    if (list.empty() || historyAliases_[index] || batchedHistories_[index]) continue;
                    std::uint32_t total = references[index];
                    for (auto alias : preimage[index]) total += references[alias];
                    if (total != list.size()) continue;
                    const auto directIt = std::find_if(list.begin(), list.end(), [](const SampleSite &site) { return site.direct; });
                    const bool oneUnit = directIt != list.end() && std::all_of(list.begin(), list.end(),
                                                       [&](const SampleSite &site) { return site.unit == directIt->unit; });
                    // Repeated direct samples of one state inside the unit are idempotent
                    // when they share the event value; keep a single deferred entry.
                    const bool sameEvent = directIt != list.end() && std::all_of(list.begin(), list.end(),
                                                       [&](const SampleSite &site) { return site.event.index == directIt->event.index; });
                    if (!oneUnit || !sameEvent) continue;
                    const auto &type = stateType(StateId{static_cast<uint32_t>(index), 0});
                    if (!isScalarLogic(type) || type.width != 1) continue;
                    // The scheduler gives every event history a commit-fanout entry so a
                    // resampled value re-activates its readers. Certification already
                    // restricts readers to the sampling unit itself, which is seeded every
                    // round (roundSeeds), so a pure self-activation entry is unobservable.
                    const auto range = stateRanges_[index];
                    bool selfOnly = true;
                    for (std::uint32_t t = 0; t < range.count && selfOnly; ++t)
                    {
                        const auto &target = stateTargets_[range.offset + t];
                        if (target.arm || target.offset != activeOffsets_[directIt->unit] ||
                            target.mask != activeMasks_[directIt->unit])
                            selfOnly = false;
                    }
                    if (!selfOnly) continue;
                    const auto event = directIt->event;
                    if (unitProduced[directIt->unit].contains(event.index) ||
                        (!readAliases_[event.index] && layout_.values[event.index - 1].kind != CpuStorageKind::Boundary))
                        continue;
                    directSampleStates_[index] = 1;
                    directSampleUnits_[directIt->unit].push_back({StateId{static_cast<uint32_t>(index), 0}, event, projected_[index]});
                    ++directSampleStateCount_;
                }
                directSampleUnitCount_ = directSampleUnits_.size();
            }

            std::string quiescenceCheck(const std::vector<QuiescenceTerm> &terms) const
            {
                std::string check;
                for (const auto &term : terms)
                {
                    if (!check.empty()) check += " || ";
                    check += '(' + state(term.history) + "!=" + eventValue(term.event) + ')';
                }
                return check;
            }

            void planSharedHistories()
            {
                std::vector<uint32_t> references(model_.states().size() + 1), initializers(references.size());
                std::vector<std::string> constants(references.size());
                for (auto ref : model_.objectRefPool())
                    if (ref.kind == ObjectKind::State) ++references[ref.index];
                for (const auto &record : model_.initRecords())
                {
                    ++initializers[record.state.index];
                    const auto &type = stateType(record.state);
                    if (type.kind != TypeKind::Logic || type.width != 1 || type.isSigned ||
                        type.domain != LogicDomain::TwoState || model_.steps(record).size() != 1) continue;
                    const auto &step = model_.steps(record).front();
                    if (model_.text(step.kind) != "core.init.const") continue;
                    if (const auto *text = parameter<std::string>(model_, model_.parameters(step), "value"))
                        constants[record.state.index] = initLiteral(*text, type);
                }
                const auto &tree = mapping_.partitionTree;
                for (const auto &task : schedule_.numaNodes[0].cores[0].tasks)
                {
                    if (task.execution == CpuExecution::ActivityDrivenCompute) continue;
                    const auto &function = tree.partitions[task.partition.index - 1];
                    if (!tree.partitions[function.parent.index - 1].attrs.eventGate) continue;
                    const auto arm = armOffsets_[function.parent.index];
                    struct Sample { ValueId event; uint32_t references = 0; bool consistent = true; };
                    std::map<uint32_t, Sample> samples;
                    std::map<std::tuple<uint32_t, uint32_t, std::string>, StateId> representatives;
                    std::vector<std::pair<StateId, StateId>> aliases;
                    for (auto unit : function.children)
                        for (auto id : tree.partitions[unit.index - 1].ops)
                        {
                            const auto &op = model_.operations()[id.index - 1];
                            const auto name = model_.text(op.opType);
                            if (name != "core.state.regWrite" && name != "core.state.memWrite" &&
                                name != "core.state.memFill" && name != "core.state.memWriteSeq")
                                continue;
                            const auto *edges = parameter<std::vector<std::string>>(model_, model_.parameters(op), "event_edges");
                            if (!edges || edges->empty()) continue;
                            const auto events = model_.operands(op).last(edges->size());
                            const auto histories = model_.objectRefs(op).last(edges->size());
                            for (std::size_t i = 0; i < edges->size(); ++i)
                            {
                                const StateId history{histories[i].index, histories[i].generation};
                                auto &sample = samples.try_emplace(history.index, Sample{events[i]}).first->second;
                                ++sample.references;
                                sample.consistent &= sample.event == events[i] &&
                                    ((*edges)[i] == "posedge" || (*edges)[i] == "negedge");
                            }
                        }
                    for (const auto &[index, sample] : samples)
                    {
                        const StateId history{index, 0};
                        const auto range = stateRanges_[index];
                        // Every use must be an unconditional sample of the same
                        // invariant event in this task. Other histories in the
                        // task may be observed, written, or shared across tasks.
                        if (!sample.consistent || references[index] != sample.references || initializers[index] != 1 ||
                            constants[index].empty() || !projected_[index] || range.count != 1 ||
                            !stateTargets_[range.offset].arm || stateTargets_[range.offset].offset != arm ||
                            readAliases_[sample.event.index] ||
                            layout_.values[sample.event.index - 1].kind != CpuStorageKind::Boundary ||
                            model_.states()[index - 1].type != model_.values()[sample.event.index - 1].type)
                            continue;
                        sharedHistoryEligible_[index] = true;
                        const auto key = std::make_tuple(sample.event.index,
                            model_.states()[index - 1].type.index, constants[index]);
                        const auto [it, inserted] = representatives.emplace(key, history);
                        if (!inserted) aliases.emplace_back(history, it->second);
                    }
                    if (aliases.empty()) continue;
                    // Equal initial values and identical sampling sequences
                    // preserve visible/shadow equality by induction. Repeated
                    // samples of a representative remain in program order.
                    for (auto [history, representative] : aliases) historyAliases_[history.index] = representative;
                    sharedHistoryCount_ += aliases.size();
                    ++sharedHistoryTasks_;
                    sharedHistoryTaskIds_.insert(task.id.index);
                }
            }

            void planComputeSharedHistories()
            {
                std::vector<uint32_t> references(model_.states().size() + 1), initializers(references.size());
                std::vector<std::string> constants(references.size());
                for (auto ref : model_.objectRefPool())
                    if (ref.kind == ObjectKind::State) ++references[ref.index];
                for (const auto &record : model_.initRecords())
                {
                    ++initializers[record.state.index];
                    const auto &type = stateType(record.state);
                    if (type.kind != TypeKind::Logic || type.width != 1 || type.isSigned ||
                        type.domain != LogicDomain::TwoState || model_.steps(record).size() != 1) continue;
                    const auto &step = model_.steps(record).front();
                    if (model_.text(step.kind) != "core.init.const") continue;
                    if (const auto *text = parameter<std::string>(model_, model_.parameters(step), "value"))
                        constants[record.state.index] = initLiteral(*text, type);
                }
                const auto sameTargets = [&](Range a, Range b) {
                    if (a.count != b.count) return false;
                    for (std::size_t i = 0; i < a.count; ++i)
                    {
                        const auto &x = stateTargets_[a.offset + i], &y = stateTargets_[b.offset + i];
                        if (x.offset != y.offset || x.mask != y.mask || x.arm != y.arm) return false;
                    }
                    return true;
                };
                const auto &tree = mapping_.partitionTree;
                for (const auto &task : schedule_.numaNodes[0].cores[0].tasks)
                {
                    if (task.execution != CpuExecution::ActivityDrivenCompute) continue;
                    for (auto word : tree.partitions[task.partition.index - 1].children)
                    for (auto unit : tree.partitions[word.index - 1].children)
                    {
                        std::map<std::tuple<uint32_t, uint32_t, std::string>, StateId> representatives;
                        std::vector<std::pair<StateId, StateId>> aliases;
                        for (auto node : tree.partitions[unit.index - 1].children)
                        for (auto id : tree.partitions[node.index - 1].ops)
                        {
                            const auto &op = model_.operations()[id.index - 1];
                            const auto name = model_.text(op.opType);
                            std::size_t historyBase;
                            if (name == "core.dpi.call") historyBase = 1;
                            else if (name == "core.system.task") historyBase = 0;
                            else continue;
                            const auto *edges = parameter<std::vector<std::string>>(model_, model_.parameters(op), "event_edges");
                            if (!edges || edges->empty()) continue;
                            const auto operands = model_.operands(op);
                            const auto refs = model_.objectRefs(op);
                            if (operands.size() < edges->size() || refs.size() < historyBase + edges->size()) continue;
                            const auto events = operands.last(edges->size());
                            for (std::size_t i = 0; i < edges->size(); ++i)
                            {
                                if (refs[historyBase + i].kind != ObjectKind::State) continue;
                                const StateId history{refs[historyBase + i].index, refs[historyBase + i].generation};
                                const auto range = stateRanges_[history.index];
                                if (historyAliases_[history.index] || batchedHistories_[history.index] ||
                                    references[history.index] != 1 || initializers[history.index] != 1 ||
                                    constants[history.index].empty() || !projected_[history.index] || range.count < 1 ||
                                    model_.states()[history.index - 1].type != model_.values()[events[i].index - 1].type ||
                                    layout_.types[object(refs[historyBase + i]).type.index - 1].size != 1)
                                    continue;
                                const auto key = std::make_tuple(events[i].index,
                                    model_.states()[history.index - 1].type.index, constants[history.index]);
                                const auto [it, inserted] = representatives.emplace(key, history);
                                if (inserted) continue;
                                if (!sameTargets(range, stateRanges_[it->second.index])) continue;
                                aliases.emplace_back(history, it->second);
                            }
                        }
                        // One unit's samples run under the same active-word bit; guards read visible history.
                        for (auto [history, representative] : aliases) historyAliases_[history.index] = representative;
                        computeSharedHistoryCount_ += aliases.size();
                        computeSharedHistoryUnits_ += !aliases.empty();
                    }
                }
            }

            struct HistoryBatch
            {
                StateId first;
                uint64_t offset = 0;
                std::size_t count = 0;
                std::vector<ValueId> pattern;
            };

            struct ComputeGuardGroup
            {
                OpId first{};
                std::size_t historyBase = 0;
                std::string name;
                std::vector<OpId> ops;
            };

            void planComputeEdgeGuards()
            {
                const auto &tree = mapping_.partitionTree;
                std::size_t names = 0;
                for (const auto &task : schedule_.numaNodes[0].cores[0].tasks)
                {
                    if (task.execution != CpuExecution::ActivityDrivenCompute) continue;
                    for (auto word : tree.partitions[task.partition.index - 1].children)
                    for (auto unit : tree.partitions[word.index - 1].children)
                    {
                        const auto &partition = tree.partitions[unit.index - 1];
                        std::vector<OpId> ops;
                        for (auto node : partition.children)
                            ops.insert(ops.end(), tree.partitions[node.index - 1].ops.begin(), tree.partitions[node.index - 1].ops.end());
                        std::set<uint32_t> produced;
                        bool bail = false;
                        for (auto id : ops)
                        {
                            const auto &op = model_.operations()[id.index - 1];
                            const auto name = model_.text(op.opType);
                            // Visible state must stay invariant during the whole unit invocation.
                            if (name == "core.output.write" || (name == "core.dpi.call" && !model_.results(op).empty()))
                            { bail = true; break; }
                            for (auto result : model_.results(op)) produced.insert(result.index);
                        }
                        if (bail) continue;
                        using Key = std::vector<std::tuple<uint32_t, uint32_t, std::string>>;
                        std::map<Key, std::vector<OpId>> groups;
                        for (auto id : ops)
                        {
                            const auto &op = model_.operations()[id.index - 1];
                            const auto name = model_.text(op.opType);
                            std::size_t historyBase;
                            if (name == "core.dpi.call") historyBase = 1;
                            else if (name == "core.system.task") historyBase = 0;
                            else continue;
                            const auto *edges = parameter<std::vector<std::string>>(model_, model_.parameters(op), "event_edges");
                            if (!edges || edges->empty()) continue;
                            const auto events = model_.operands(op).last(edges->size());
                            const auto refs = model_.objectRefs(op);
                            if (refs.size() < historyBase + edges->size()) continue;
                            bool stable = true;
                            Key key;
                            for (std::size_t i = 0; i < edges->size() && stable; ++i)
                            {
                                if (refs[historyBase + i].kind != ObjectKind::State) { stable = false; break; }
                                const auto event = events[i];
                                // Only visible-state or boundary reads stay invariant during this unit invocation.
                                if (produced.contains(event.index) ||
                                    (!readAliases_[event.index] && layout_.values[event.index - 1].kind != CpuStorageKind::Boundary))
                                    stable = false;
                                const auto alias = historyAliases_[refs[historyBase + i].index];
                                key.emplace_back(event.index, alias ? alias.index : refs[historyBase + i].index, (*edges)[i]);
                            }
                            if (!stable) continue;
                            groups[std::move(key)].push_back(id);
                        }
                        for (auto &[key, grouped] : groups)
                        {
                            if (grouped.size() < 2) continue;
                            ComputeGuardGroup group;
                            group.first = grouped.front();
                            group.historyBase = model_.text(model_.operations()[grouped.front().index - 1].opType) == "core.dpi.call" ? 1 : 0;
                            group.name = "cpu_cevent_" + std::to_string(unit.index) + "_" + std::to_string(names++);
                            group.ops = std::move(grouped);
                            computeGuardUses_ += group.ops.size();
                            ++computeGuardSnapshots_;
                            computeGuardGroups_[unit.index].push_back(std::move(group));
                        }
                    }
                }
            }

            void planHistoryBatches()
            {
                std::vector<uint32_t> references(model_.states().size() + 1);
                for (auto ref : model_.objectRefPool())
                    if (ref.kind == ObjectKind::State) ++references[ref.index];
                struct Sample { StateId state; ValueId value; uint64_t offset; };
                const auto &tree = mapping_.partitionTree;
                for (const auto &task : schedule_.numaNodes[0].cores[0].tasks)
                {
                    if (task.execution == CpuExecution::ActivityDrivenCompute) continue;
                    const auto &function = tree.partitions[task.partition.index - 1];
                    if (!tree.partitions[function.parent.index - 1].attrs.eventGate) continue;
                    const auto arm = armOffsets_[function.parent.index];
                    std::vector<Sample> samples;
                    for (auto unit : function.children)
                        for (auto id : tree.partitions[unit.index - 1].ops)
                        {
                            const auto &op = model_.operations()[id.index - 1];
                            const auto *edges = parameter<std::vector<std::string>>(model_, model_.parameters(op), "event_edges");
                            if (!edges) continue;
                            const auto events = model_.operands(op).last(edges->size());
                            const auto refs = model_.objectRefs(op).last(edges->size());
                            for (std::size_t i = 0; i < events.size(); ++i)
                            {
                                ++historyCandidates_;
                                const StateId history{refs[i].index, refs[i].generation};
                                if (references[history.index] != 1) { ++historyPrivateRejected_; continue; }
                                const auto &type = stateType(history);
                                const auto &slot = object(refs[i]);
                                const auto range = stateRanges_[history.index];
                                privateByteHistories_[history.index] = type.kind == TypeKind::Logic && type.width == 1 &&
                                    !type.isSigned && type.domain == LogicDomain::TwoState && layout_.types[slot.type.index - 1].size == 1;
                                if (historyAliases_[history.index]) continue;
                                if (type.kind != TypeKind::Logic || type.width != 1 || type.domain != LogicDomain::TwoState ||
                                    model_.states()[history.index - 1].type != model_.values()[events[i].index - 1].type ||
                                    layout_.types[slot.type.index - 1].size != 1 || !projected_[history.index] || range.count != 1 ||
                                    !stateTargets_[range.offset].arm || stateTargets_[range.offset].offset != arm)
                                { ++historyLayoutRejected_; continue; }
                                samples.push_back({history, events[i], slot.offset});
                            }
                        }
                    std::sort(samples.begin(), samples.end(), [](const auto &a, const auto &b) { return a.offset < b.offset; });
                    for (std::size_t first = 0; first < samples.size();)
                    {
                        std::size_t end = first + 1;
                        while (end < samples.size() && samples[end].offset == samples[end - 1].offset + 1) ++end;
                        while (first < end)
                        {
                            std::size_t count = 0, period = 0;
                            for (std::size_t p = 1; p <= 8 && p <= (end - first) / 2; ++p)
                            {
                                std::size_t n = p;
                                while (first + n < end && samples[first + n].value == samples[first + n % p].value) ++n;
                                n -= n % p;
                                if (n > count) { count = n; period = p; }
                            }
                            if (count < 4) { ++first; continue; }
                            HistoryBatch batch{samples[first].state, samples[first].offset, count, {}};
                            for (std::size_t i = 0; i < period; ++i) batch.pattern.push_back(samples[first + i].value);
                            for (std::size_t i = 0; i < count; ++i) batchedHistories_[samples[first + i].state.index] = true;
                            historyBatches_[task.id.index].push_back(std::move(batch));
                            historyBatchStates_ += count; ++historyBatchCount_;
                            historyMaxBatch_ = std::max<uint64_t>(historyMaxBatch_, count);
                            historyMaxPattern_ = std::max<uint64_t>(historyMaxPattern_, period);
                            first += count;
                        }
                    }
                }
            }

            // Commit-side private 1-bit event histories whose only fanout is the
            // re-arm of their own domain retire their stage/pending/publish
            // round trip into deferred direct stores at the task end.
            // Certification: the byte is referenced by exactly one op (its own
            // write port), is neither aliased nor batched, and its commit-fanout
            // range is empty or just the self-domain arm. A history change can
            // never make an edge guard newly true (guards require hist==old &&
            // event==new), and every gate-value transition arms the domain
            // anyway, so dropping the history's re-arm/projection contribution
            // is unobservable. In-body guards keep reading the visible byte; the
            // deferred store lands at task end, the same moment publish would
            // have made it visible to the next round.
            void planCommitDirectHistories()
            {
                const auto &tree = mapping_.partitionTree;
                for (const auto &task : schedule_.numaNodes[0].cores[0].tasks)
                {
                    if (task.execution != CpuExecution::DomainGatedCommit) continue;
                    const auto &function = tree.partitions[task.partition.index - 1];
                    if (!tree.partitions[function.parent.index - 1].attrs.eventGate) continue;
                    const auto arm = armOffsets_[function.parent.index];
                    for (auto unit : function.children)
                        for (auto id : tree.partitions[unit.index - 1].ops)
                        {
                            const auto &op = model_.operations()[id.index - 1];
                            const auto *edges = parameter<std::vector<std::string>>(model_, model_.parameters(op), "event_edges");
                            if (!edges || edges->empty()) continue;
                            const auto refs = model_.objectRefs(op).last(edges->size());
                            for (std::size_t i = 0; i < edges->size(); ++i)
                            {
                                if (refs[i].kind != ObjectKind::State) continue;
                                const auto index = refs[i].index;
                                if (!privateByteHistories_[index] || historyAliases_[index] || batchedHistories_[index]) continue;
                                const auto range = stateRanges_[index];
                                bool selfOnly = true;
                                for (std::uint32_t t = 0; t < range.count && selfOnly; ++t)
                                {
                                    const auto &target = stateTargets_[range.offset + t];
                                    if (!target.arm || target.offset != arm || target.mask != 1) selfOnly = false;
                                }
                                if (!selfOnly) continue;
                                if (!commitDirectHistories_[index])
                                {
                                    commitDirectHistories_[index] = 1;
                                    ++commitDirectHistoryCount_;
                                }
                            }
                        }
                }
            }

            void flushDeferredHistoryStores(std::ostream &out) const
            {
                for (const auto &[target, expression] : deferredHistoryStores_)
                    out << at(stateType(target), arenaObjects(), object(ObjectRef::state(target)).offset) << '='
                        << normalize(std::move(expression), stateType(target)) << ";\n";
                deferredHistoryStores_.clear();
            }

            void sampleHistoryBatch(std::ostream &out, const HistoryBatch &batch) const
            {
                const auto range = stateRanges_[batch.first.index];
                // The batch fills every byte in the contiguous range, so avoid copying
                // the visible history into the shadow before the overwrite.
                out << "{ // cpu_history_batch states=" << batch.count << "\n"
                    << "auto *cpu_history=cpu_stage_bytes_overwrite(cpu_obj_,cpu_shadow_," << batch.first.index << ',' << batch.offset << ',' << batch.count
                    << ',' << range.offset << ',' << range.count << ",true);\n";
                if (batch.pattern.size() == 1)
                    out << "std::memset(cpu_history,static_cast<unsigned char>(" << value(batch.pattern.front()) << ")," << batch.count << ");\n";
                else
                {
                    out << "const unsigned char cpu_pattern[]={";
                    for (std::size_t i = 0; i < batch.pattern.size(); ++i)
                        out << (i ? "," : "") << "static_cast<unsigned char>(" << value(batch.pattern[i]) << ')';
                    out << "};\nfor(std::size_t i=0;i<" << batch.count << ";i+=" << batch.pattern.size()
                        << ")std::memcpy(cpu_history+i,cpu_pattern," << batch.pattern.size() << ");\n";
                }
                out << "}\n";
            }

            void validatePort(std::string_view name, std::set<std::string> &names)
            {
                const auto cpp = identifier(name);
                if (!names.insert(cpp).second)
                    throw std::runtime_error("CPU port name collides with generated member: " + cpp);
            }

            const Type &type(ValueId value) const { return model_.types()[model_.values()[value.index - 1].type.index - 1]; }
            const Type &stateType(StateId state) const { return model_.types()[model_.states()[state.index - 1].type.index - 1]; }
            bool containsString(const Type &type) const
            {
                return type.kind == TypeKind::String || (type.kind == TypeKind::Array &&
                    containsString(model_.types()[type.elementType.index - 1]));
            }
            std::string cppType(const Type &type) const
            {
                if (type.kind == TypeKind::Array)
                    return "std::array<" + cppType(model_.types()[type.elementType.index - 1]) + "," + std::to_string(type.count) + ">";
                if (type.kind == TypeKind::String)
                    return "std::string";
                if (type.kind == TypeKind::Real) return "double";
                if (type.kind != TypeKind::Logic)
                    throw std::runtime_error("CPU C++ emit non-logic scalar storage is not implemented");
                if (type.domain != LogicDomain::TwoState || type.width == 0 || type.width > 64)
                {
                    if (type.domain != LogicDomain::TwoState || type.width == 0)
                        throw std::runtime_error("CPU C++ emit scalar logic width/domain is not implemented");
                    return "std::array<std::uint64_t," + std::to_string((type.width + 63u) / 64u) + ">";
                }
                if (type.width == 1 && !type.isSigned) return "bool";
                return "std::" + std::string(type.isSigned ? "int" : "uint") +
                       std::to_string(type.width <= 8 ? 8 : type.width <= 16 ? 16 : type.width <= 32 ? 32 : 64) + "_t";
            }
            uint64_t storageBytes(const Type &type) const
            {
                if (type.kind == TypeKind::Array)
                    return storageBytes(model_.types()[type.elementType.index - 1]) * type.count;
                if (type.kind != TypeKind::Logic || type.width == 0 || type.domain != LogicDomain::TwoState)
                    throw std::runtime_error("CPU C++ emit requires two-state logic storage");
                return type.width > 64 ? ((uint64_t(type.width) + 63u) / 64u) * 8u :
                       type.width <= 8 ? 1 : type.width <= 16 ? 2 : type.width <= 32 ? 4 : 8;
            }
            std::string normalize(std::string expression, const Type &type) const
            {
                if (type.kind == TypeKind::String || type.kind == TypeKind::Real)
                    return expression;
                if (type.kind == TypeKind::Logic && type.domain == LogicDomain::TwoState && type.width > 64)
                    return expression;
                return "static_cast<" + cppType(type) + ">(" + (type.isSigned ? "grhsim_sign_extend_i64(" : "grhsim_trunc_u64(") +
                       expression + "," + std::to_string(type.width) + "))";
            }
            std::string at(const Type &type, std::string_view arena, uint64_t offset) const
            { return "cpu_at<" + cppType(type) + ">(" + std::string(arena) + "," + std::to_string(offset) + ")"; }
            // Task/helper bodies operate on restrict-qualified local copies of the
            // three buffer bases: the compiler otherwise reloads the unique_ptr
            // members after every store and cannot hoist buffer loads across
            // stores, which serializes the per-state commit inner loops.
            std::string_view arenaObjects() const { return localizeBuffers_ ? "cpu_obj_" : "cpu_objects.get()"; }
            std::string_view arenaBoundary() const { return localizeBuffers_ ? "cpu_bnd_" : "cpu_boundary.get()"; }
            std::string_view arenaShadow() const { return localizeBuffers_ ? "cpu_shadow_" : "cpu_shadow.get()"; }
            void emitBufferLocals(std::ostream &out) const
            {
                out << "[[maybe_unused]] std::byte *__restrict const cpu_obj_=cpu_objects.get();\n"
                    << "[[maybe_unused]] std::byte *__restrict const cpu_bnd_=cpu_boundary.get();\n"
                    << "[[maybe_unused]] std::byte *__restrict const cpu_shadow_=cpu_shadow.get();\n";
            }
            std::string value(ValueId value) const
            {
                if (activeValueCache_)
                    if (const auto found = activeValueCache_->find(value.index); found != activeValueCache_->end())
                        return found->second;
                if (!readAliases_.empty() && readAliases_[value.index]) return state(readAliases_[value.index]);
                if (const auto it = staticScalars_.find(value.index); it != staticScalars_.end()) return it->second;
                if (type(value).kind == TypeKind::String)
                    if (const auto it = staticStrings_.find(value.index); it != staticStrings_.end()) return it->second;
                const auto &slot = layout_.values[value.index - 1];
                return at(type(value), slot.kind == CpuStorageKind::Boundary ? arenaBoundary() : "cpu_local", slot.offset);
            }
            std::optional<StateId> stateReadSource(ValueId value) const
            {
                if (!value || value.index >= producers_.size()) return {};
                if (!readAliases_.empty() && readAliases_[value.index]) return readAliases_[value.index];
                const auto producer = producers_[value.index];
                if (!producer) return {};
                const auto &op = model_.operations()[producer.index - 1];
                const auto name = model_.text(op.opType);
                if (name == "core.state.read")
                {
                    const auto refs = model_.objectRefs(op);
                    if (refs.size() == 1 && refs[0].kind == ObjectKind::State)
                        return StateId{refs[0].index, 0};
                    return {};
                }
                // A slice/assignment preserves the immutable compute-phase
                // snapshot of its source state. Follow only these transparent
                // forms; arbitrary expressions may combine multiple states.
                if (name == "core.compute.sliceStatic" || name == "core.compute.sliceDynamic" ||
                    name == "core.compute.sliceArray" || name == "core.compute.assign")
                {
                    const auto operands = model_.operands(op);
                    if (!operands.empty()) return stateReadSource(operands.front());
                }
                return {};
            }
            const CpuDataSlot &object(ObjectRef ref) const
            {
                if (ref.kind == ObjectKind::State && historyAliases_[ref.index])
                    ref = ObjectRef::state(historyAliases_[ref.index]);
                const auto index = ref.kind == ObjectKind::Input ? ref.index - 1 : ref.kind == ObjectKind::Output ?
                    model_.inputs().size() + ref.index - 1 : model_.inputs().size() + model_.outputs().size() + ref.index - 1;
                return layout_.objects[index].slot;
            }
            std::string state(StateId state) const
            {
                if (activeStateCache_)
                    if (const auto found = activeStateCache_->find(state.index); found != activeStateCache_->end())
                        return found->second;
                return at(stateType(state), arenaObjects(), object(ObjectRef::state(state)).offset);
            }
            std::string literal(std::string_view text, const Type &type) const
            {
                auto parsed = [&]() {
                    try { return slang::SVInt::fromString(text).resize(type.width); }
                    catch (const std::exception &) { throw std::runtime_error("CPU C++ emit unsupported constant literal: " + std::string(text)); }
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
            std::string initLiteral(std::string_view text, const Type &type) const
            {
                // Lowering uses bare x for the two-state projection of an unspecified memory.
                if (text == "x" || text == "X" || text == "z" || text == "Z" ||
                    text == "'x" || text == "'X" || text == "'z" || text == "'Z" || text == "'0") text = "0";
                else if (text == "'1") text = "-1";
                return literal(text, type);
            }

            void randomInit(std::ostream &out, const Type &type, std::string_view destination, std::string_view rng) const
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

            using InitRows = std::vector<std::pair<uint64_t, std::string>>;
            mutable std::map<const InitStep *, InitRows> readmemRows_;

            const InitRows &readmemRows(const InitStep &step, const Type &element, uint64_t first, uint64_t end) const
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

            void initStep(std::ostream &out, StateId id, const InitStep &step,
                          std::vector<std::pair<uint64_t, uint64_t>> *covered = nullptr) const
            {
                const auto &target = stateType(id);
                const bool array = target.kind == TypeKind::Array;
                const auto &element = array ? model_.types()[target.elementType.index - 1] : target;
                if (element.kind != TypeKind::Logic || element.domain != LogicDomain::TwoState)
                    throw std::runtime_error("CPU initializer currently requires two-state logic or an array of two-state logic");
                const auto kind = model_.text(step.kind);
                const auto params = model_.parameters(step);
                const auto offset = object(ObjectRef::state(id)).offset;
                out << "{\n";
                if (!array)
                {
                    if (kind == "core.init.const")
                    {
                        const auto *text = parameter<std::string>(model_, params, "value");
                        if (!text) throw std::runtime_error("CPU scalar initializer requires value literal");
                        const auto expression = initLiteral(*text, element);
                        if (element.width <= 64) out << state(id) << '=' << expression << ";\n";
                        else out << "static const auto data=" << expression << ";\nstd::memcpy(cpu_objects.get()+" << offset << ",&data,sizeof(data));\n";
                    }
                    else if (kind == "core.init.random")
                    {
                        const auto *seed = parameter<int64_t>(model_, params, "seed");
                        if (seed) out << "std::uint64_t rng=UINT64_C(" << static_cast<uint64_t>(*seed) << ");\n";
                        randomInit(out, element, state(id), seed ? "rng" : "cpu_rng");
                    }
                    else throw std::runtime_error("CPU scalar initializer must be const or random");
                    out << "}\n";
                    return;
                }

                if (kind == "core.init.const")
                {
                    const auto *values = parameter<std::vector<std::string>>(model_, params, "value");
                    if (!values || values->size() != target.count)
                        throw std::runtime_error("CPU array const initializer requires exactly count element literals");
                    if (!values->empty())
                    {
                        out << "static const " << cppType(element) << " data[]={\n";
                        for (const auto &text : *values) out << initLiteral(text, element) << ",\n";
                        out << "};\nstd::memcpy(cpu_objects.get()+" << offset << ",data,sizeof(data));\n";
                    }
                    if (covered) covered->emplace_back(0, target.count);
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
                        const bool zero = text && (initLiteral(*text, element) == literal("0", element));
                        if (zero)
                            out << "std::memset(cpu_objects.get()+" << offset + first * rowBytes << ",0," << size * rowBytes << ");\n";
                        else
                        {
                            if (text) out << "static const auto data=" << initLiteral(*text, element) << ";\n";
                            out << "for(std::size_t row=" << first << ";row<" << end << ";++row){\n"
                                << "auto &dst=cpu_at<" << cppType(element) << ">(cpu_objects.get()," << offset << "+row*" << rowBytes << ");\n";
                            if (text) out << "std::memcpy(&dst,&data,sizeof(data));\n";
                            else randomInit(out, element, "dst", "cpu_rng");
                            out << "}\n";
                        }
                        if (covered) covered->emplace_back(first, end);
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
                            out << "};\nfor(std::size_t i=0;i<" << rows.size() << ";++i)std::memcpy(cpu_objects.get()+"
                                << offset << "+rows[i]*" << rowBytes << ",&data[i],sizeof(data[i]));\n";
                        }
                        if (covered) for (const auto &[row, data] : rows) covered->emplace_back(row, row + 1);
                    }
                }
                else throw std::runtime_error("CPU C++ emit unsupported array initializer: " + std::string(kind));
                out << "}\n";
            }
            std::string expression(const SimOp &op) const
            {
                const auto name = model_.text(op.opType); const auto operands = model_.operands(op);
                const auto &result = type(model_.results(op)[0]); const auto width = result.width;
                const auto raw = [&](std::size_t i) { if (i >= operands.size()) throw std::runtime_error("CPU op operand arity"); return value(operands[i]); };
                const auto cast = [&](std::size_t i, uint32_t width) {
                    const auto expr = raw(i); const auto &source = type(operands[i]);
                    return "grhsim_cast_u64(" + expr + "," + std::to_string(source.width) + "," + std::to_string(width) +
                           "," + (source.isSigned ? "true" : "false") + ")";
                };
                const auto number = [&](std::string_view key) {
                    const auto *n = parameter<int64_t>(model_, model_.parameters(op), key);
                    if (!n || *n < 0) throw std::runtime_error("missing or negative CPU slice/replication parameter");
                    return static_cast<uint64_t>(*n);
                };
                if (name == "core.input.read") return at(result, arenaObjects(), object(model_.objectRefs(op)[0]).offset);
                if (name == "core.state.read") return state({model_.objectRefs(op)[0].index, 0});
                if (name == "core.state.memRead")
                {
                    const auto refs = model_.objectRefs(op);
                    if (refs.size() != 1 || refs[0].kind != ObjectKind::State)
                        throw std::runtime_error("CPU memory read requires one array state reference");
                    const auto &array = stateType({refs[0].index, 0});
                    if (array.kind != TypeKind::Array)
                        throw std::runtime_error("CPU memory read target is not an array state");
                    const auto &element = model_.types()[array.elementType.index - 1];
                    if (operands.size() != 1)
                        throw std::runtime_error("CPU memory read requires one index operand");
                    return "cpu_at<" + cppType(element) + ">(" + std::string(arenaObjects()) + "," +
                           std::to_string(object(refs[0]).offset) + "+(" + raw(0) + ")*" +
                           std::to_string(storageBytes(element)) + ")";
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
                        throw std::runtime_error("CPU C++ emit non-logic constant is not implemented");
                    if (text) return literal(*text, result);
                    const auto *integer = parameter<int64_t>(model_, params, "value");
                    if (!integer) integer = parameter<int64_t>(model_, params, "constValue");
                    if (integer) return literal(std::to_string(*integer), result);
                    const auto *boolean = parameter<bool>(model_, params, "value");
                    if (!boolean) boolean = parameter<bool>(model_, params, "constValue");
                    if (boolean) return literal(*boolean ? "1" : "0", result);
                    throw std::runtime_error("CPU constant requires value literal");
                }
                if (!name.starts_with("core.compute.")) throw std::runtime_error("CPU C++ emit unsupported operation: " + std::string(name));
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
                    throw std::runtime_error("CPU C++ emit unsupported wide operation: " + std::string(kind) + " width=" + std::to_string(width));
                }
                if (kind == "assign") return cast(0, width);
                if ((kind == "and" || kind == "or") && operands.size() == 2 &&
                    width == 1 && !result.isSigned &&
                    type(operands[0]).width == 1 && !type(operands[0]).isSigned &&
                    type(operands[1]).width == 1 && !type(operands[1]).isSigned)
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
                    // A two-state scalar mux is a pure bit-select.  Use the
                    // branchless mask form so the hot compute path does not
                    // expose a data-dependent conditional branch for every
                    // mux result.  grhsim_mux_u64 preserves the SV condition
                    // rule (any non-zero condition selects the true arm),
                    // while the surrounding normalize() applies the result
                    // width and signedness exactly as before.
                    if (result.domain == LogicDomain::TwoState && width <= 64 && operands.size() == 3)
                        return "grhsim_mux_u64(" + raw(0) + "," + cast(1, width) + "," + cast(2, width) + ")";
                    return "(" + raw(0) + "?" + cast(1, width) + ":" + cast(2, width) + ")";
                }
                if (kind == "prioritySelect")
                {
                    // [c0..cN-1, a0..aN-1, default]: the first true condition wins,
                    // exactly the folded mux chain.  Emit as one right-nested
                    // branchless select expression: the intermediate slot stores
                    // and reloads of the link chain disappear while the per-link
                    // mask select and casts stay bit-identical.
                    const std::size_t count = (operands.size() - 1) / 2;
                    if (count < 3 || count > 64)
                        throw std::runtime_error("CPU C++ emit prioritySelect condition count is out of range");
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
                    return "grhsim_" + std::string(type(operands[0]).isSigned && type(operands[1]).isSigned ? "s" : "u") +
                           std::string(kind) + "_u64(" + cast(0, width) + "," + cast(1, width) + "," + std::to_string(width) + ")";
                static const std::map<std::string_view, std::string_view> compares{
                    {"eq", "=="}, {"ne", "!="}, {"caseEq", "=="}, {"caseNe", "!="},
                    {"wildcardEq", "=="}, {"wildcardNe", "!="}, {"lt", "<"}, {"le", "<="}, {"gt", ">"}, {"ge", ">="}};
                if (auto it = compares.find(kind); it != compares.end())
                {
                    const auto compareWidth = std::max(type(operands[0]).width, type(operands[1]).width);
                    if (compareWidth > 64)
                    {
                        std::string prefix = "([&](){";
                        std::array<std::string, 2> pointers;
                        for (std::size_t i = 0; i < 2; ++i)
                        {
                            if (type(operands[i]).width > 64) pointers[i] = "(" + raw(i) + ").data()";
                            else
                            {
                                const auto local = "cpu_cmp_" + std::to_string(i);
                                prefix += "const std::uint64_t " + local + "=static_cast<std::uint64_t>(" + raw(i) + ");";
                                pointers[i] = "&" + local;
                            }
                        }
                        return prefix + "return grhsim_compare_extended_words(" + pointers[0] + "," +
                            std::to_string((type(operands[0]).width + 63u) / 64u) + "," + std::to_string(type(operands[0]).width) + "," +
                            pointers[1] + "," + std::to_string((type(operands[1]).width + 63u) / 64u) + "," +
                            std::to_string(type(operands[1]).width) + "," +
                            (type(operands[0]).isSigned && type(operands[1]).isSigned ? "true" : "false") + ")" +
                            std::string(it->second) + "0;}())";
                    }
                    return "(grhsim_compare_" + std::string(type(operands[0]).isSigned && type(operands[1]).isSigned ? "signed" : "unsigned") +
                           "_u64(" + cast(0, compareWidth) + "," + cast(1, compareWidth) + "," + std::to_string(compareWidth) + ")" +
                           std::string(it->second) + "0)";
                }
                static const std::map<std::string_view, std::string_view> reduces{
                    {"reduceAnd", "and"}, {"reduceNand", "nand"}, {"reduceOr", "or"}, {"reduceNor", "nor"},
                    {"reduceXor", "xor"}, {"reduceXnor", "xnor"}};
                if (auto it = reduces.find(kind); it != reduces.end())
                {
                    const auto operandWidth = type(operands[0]).width;
                    if (operandWidth > 64)
                        return "grhsim_reduce_" + std::string(it->second) + "_words(" + raw(0) + "," + std::to_string(operandWidth) + ")";
                    return "grhsim_reduce_" + std::string(it->second) + "_u64(" + raw(0) + "," + std::to_string(operandWidth) + ")";
                }
                if (kind == "sliceStatic" || kind == "sliceDynamic" || kind == "sliceArray")
                {
                    if (type(operands[0]).width > 64)
                    {
                        const auto srcWords = (type(operands[0]).width + 63u) / 64u;
                        std::string start = kind == "sliceStatic" ? std::to_string(number("sliceStart")) : cast(1, type(operands[1]).width);
                        if (kind == "sliceArray") start = "(" + start + ")*" + std::to_string(width);
                        return "grhsim_slice_words_u64<" + std::to_string(srcWords) + ">( " + raw(0) + "," + start + "," + std::to_string(width) + ")";
                    }
                    std::string start = kind == "sliceStatic" ? std::to_string(number("sliceStart")) : cast(1, type(operands[1]).width);
                    if (kind == "sliceArray")
                        start = "(" + start + ">=64/" + std::to_string(width) + "+1?64:" + start + "*" + std::to_string(width) + ")";
                    return "grhsim_slice_dynamic_u64(grhsim_trunc_u64(" + raw(0) + "," + std::to_string(type(operands[0]).width) +
                           ")," + start + "," + std::to_string(width) + ")";
                }
                if (kind == "concat" || kind == "replicate")
                {
                    const auto &sourceType = type(operands[0]);
                    if (kind == "replicate" && sourceType.kind == TypeKind::Logic && sourceType.width == 1 &&
                        !sourceType.isSigned && sourceType.domain == LogicDomain::TwoState)
                    {
                        // {rep{bit}} broadcasts the bit: 0/-bit fills every lane in one
                        // subtract; normalize() applies the result width truncation.
                        return "(0-static_cast<std::uint64_t>(" + raw(0) + "))";
                    }
                    std::string expr = "UINT64_C(0)"; uint64_t total = 0;
                    const auto count = kind == "replicate" ? number("rep") : operands.size();
                    if (!count || count > 64) throw std::runtime_error("invalid CPU scalar concatenation/replication count");
                    for (uint64_t i = 0; i < count; ++i)
                    {
                        const auto index = kind == "replicate" ? 0 : i;
                        expr = "grhsim_concat_u64(" + expr + "," + std::to_string(total) + "," + raw(index) + "," +
                               std::to_string(type(operands[index]).width) + ")";
                        total += type(operands[index]).width;
                    }
                    return expr;
                }
                throw std::runtime_error("CPU C++ emit unsupported computation: " + std::string(kind));
            }

            void activate(std::ostream &out, const CpuActivationTargets &targets, bool next, PartitionId activeUnit = {},
                          const std::string &condition = {}) const
            {
                std::map<uint32_t, uint32_t> masks;
                uint32_t localMask = 0;
                for (auto target : targets.activate)
                {
                    const auto offset = activeOffsets_[target.index], mask = activeMasks_[target.index];
                    // As in legacy, later bits can run now; earlier/current bits remain queued.
                    if (activeUnit && offset == activeOffsets_[activeUnit.index] && mask > activeMasks_[activeUnit.index])
                        localMask |= mask;
                    else masks[offset] |= mask;
                }
                const auto gated = [&](uint32_t mask) {
                    return condition.empty() ? std::to_string(mask) :
                        "(static_cast<std::uint8_t>(-static_cast<std::uint8_t>(" + condition + ")) & " + std::to_string(mask) + ")";
                };
                if (localMask) out << "cpu_active_word |= " << gated(localMask) << ";\n";
                for (const auto &[offset, mask] : masks)
                    out << "cpu_flags[" << offset << "] |= " << gated(mask) << ";\n";
                for (auto target : targets.arm)
                    out << (next ? "cpu_next_arms[" : "cpu_flags[") << armOffsets_[target.index] << "] |= " << gated(1) << ";\n";
            }

            std::string dpiType(TypeId id) const
            {
                if (!id) return "void";
                const auto &type = model_.types()[id.index - 1];
                if (type.kind == TypeKind::Array)
                    throw std::runtime_error("CPU DPI unpacked array ABI is not implemented");
                if (type.kind == TypeKind::Logic && type.width == 1) return "bool";
                return cppType(type);
            }

            std::string dpiDeclaration(const ExternFunction &function) const
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

            std::string eventValue(ValueId event) const
            {
                if (activeEventCache_)
                {
                    const auto found = activeEventCache_->find(event.index);
                    if (found != activeEventCache_->end()) return found->second;
                }
                return value(event);
            }

            std::map<uint32_t, std::string> taskEventCache(const CpuScheduledTask &task) const
            {
                std::map<uint32_t, unsigned> counts;
                // Commit tasks are emitted as one function body. Compute tasks may
                // split operations into helper functions, whose event values are
                // deliberately left in their original form.
                if (task.execution == CpuExecution::ActivityDrivenCompute) return {};
                const auto &tree = mapping_.partitionTree;
                const auto taskPartition = tree.partitions[task.partition.index - 1];
                const auto countOp = [&](OpId opId) {
                    const auto &op = model_.operations()[opId.index - 1];
                    const auto *edges = parameter<std::vector<std::string>>(model_, model_.parameters(op), "event_edges");
                    if (!edges) return;
                    for (auto event : model_.operands(op).last(edges->size()))
                    {
                        if (event.index == 0 || type(event).kind != TypeKind::Logic || type(event).width != 1 ||
                            layout_.values[event.index - 1].kind != CpuStorageKind::Boundary)
                            continue;
                        ++counts[event.index];
                    }
                };
                for (auto unit : taskPartition.children)
                    for (auto opId : tree.partitions[unit.index - 1].ops) countOp(opId);
                std::map<uint32_t, std::string> cache;
                std::size_t index = 0;
                for (const auto &[event, count] : counts)
                    if (count >= 2) cache.emplace(event, "cpu_event_snapshot_" + std::to_string(index++));
                return cache;
            }

            std::string eventGuard(const SimOp &op, std::size_t historyBase) const
            {
                const auto *edges = parameter<std::vector<std::string>>(model_, model_.parameters(op), "event_edges");
                if (!edges || edges->empty()) return "true";
                const auto events = model_.operands(op).last(edges->size());
                const auto refs = model_.objectRefs(op);
                std::string guard = "false";
                for (std::size_t i = 0; i < edges->size(); ++i)
                    guard += " || (" + std::string((*edges)[i] == "posedge" ? "!" : "") +
                        state({refs[historyBase + i].index, 0}) + " && " +
                        ((*edges)[i] == "negedge" ? "!" : "") + eventValue(events[i]) + ")";
                return guard;
            }

            std::size_t validateCallEvents(const SimOp &op, std::size_t historyBase) const
            {
                const auto *edges = parameter<std::vector<std::string>>(model_, model_.parameters(op), "event_edges");
                const auto count = edges ? edges->size() : 0;
                const auto operands = model_.operands(op); const auto refs = model_.objectRefs(op);
                if (operands.size() < count + 1 || refs.size() != historyBase + count)
                    throw std::runtime_error("CPU external call condition/event/history arity mismatch");
                if (type(operands[0]).kind != TypeKind::Logic)
                    throw std::runtime_error("CPU external call condition must be logic");
                for (std::size_t i = 0; i < count; ++i)
                    if (type(operands[operands.size() - count + i]).kind != TypeKind::Logic ||
                        type(operands[operands.size() - count + i]).width != 1 ||
                        refs[historyBase + i].kind != ObjectKind::State ||
                        stateType({refs[historyBase + i].index, 0}).kind != TypeKind::Logic ||
                        stateType({refs[historyBase + i].index, 0}).width != 1 ||
                        ((*edges)[i] != "posedge" && (*edges)[i] != "negedge"))
                        throw std::runtime_error("CPU external call event/history must be one-bit edge state");
                return count;
            }

            std::string callCondition(ValueId condition) const
            {
                const auto &target = type(condition);
                return target.width > 64 ? "grhsim_reduce_or_words(" + value(condition) + ',' +
                    std::to_string(target.width) + ')' : value(condition);
            }

            void validateDpiCall(const SimOp &op) const
            {
                const auto eventCount = validateCallEvents(op, 1);
                const auto &function = model_.functions()[model_.objectRefs(op)[0].index - 1];
                std::vector<TypeId> inputs, outputs;
                if (function.returnType) outputs.push_back(function.returnType);
                for (const auto &arg : model_.arguments(function))
                    if (arg.direction == DpiDirection::Input) inputs.push_back(arg.type);
                    else if (arg.direction == DpiDirection::Output) outputs.push_back(arg.type);
                for (const auto &arg : model_.arguments(function))
                    if (arg.direction == DpiDirection::Inout) { inputs.push_back(arg.type); outputs.push_back(arg.type); }
                const auto operands = model_.operands(op), results = model_.results(op);
                if (operands.size() != 1 + inputs.size() + eventCount || results.size() != outputs.size())
                    throw std::runtime_error("CPU DPI call arity disagrees with its signature");
                const auto checkType = [&](ValueId value, TypeId expected) {
                    const auto &source = type(value), &target = model_.types()[expected.index - 1];
                    if (source.kind != target.kind || source.width != target.width || source.domain != target.domain)
                        throw std::runtime_error("CPU DPI call value type disagrees with its signature");
                };
                for (std::size_t i = 0; i < inputs.size(); ++i) checkType(operands[i + 1], inputs[i]);
                for (std::size_t i = 0; i < outputs.size(); ++i) checkType(results[i], outputs[i]);
            }

            void sampleEvents(std::ostream &out, const SimOp &op, std::size_t historyBase) const
            {
                const auto *edges = parameter<std::vector<std::string>>(model_, model_.parameters(op), "event_edges");
                if (!edges) return;
                const auto events = model_.operands(op).last(edges->size());
                const auto refs = model_.objectRefs(op);
                for (std::size_t i = 0; i < edges->size(); ++i)
                    stage(out, {refs[historyBase + i].index, 0}, eventValue(events[i]));
            }

            void publishDpiResult(std::ostream &out, ValueId result, const std::string &temporary, PartitionId activeUnit) const
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
                out << "if(" << value(result) << "!=" << source << "){\n";
                out << value(result) << "=std::move(" << source << ");\n";
                if (const auto *targets = fanout_[result.index]) activate(out, *targets, false, activeUnit);
                out << "}}\n";
            }

            // Call string for a resultless DPI call: validateDpiCall guarantees
            // that a call without results has no return value and no output or
            // inout arguments, so every formal is an input passed by value.
            std::string dpiCallExpression(const SimOp &op) const
            {
                const auto &function = model_.functions()[model_.objectRefs(op)[0].index - 1];
                const auto arguments = model_.arguments(function);
                const auto operands = model_.operands(op);
                std::string call = "::" + identifier(model_.text(function.symbol)) + "(";
                std::size_t input = 1;
                for (std::size_t i = 0; i < arguments.size(); ++i)
                {
                    if (i) call += ',';
                    const auto &arg = arguments[i]; const auto &target = model_.types()[arg.type.index - 1];
                    const auto source = value(operands[input++]);
                    call += target.kind == TypeKind::String ? "(" + source + ").c_str()" : normalize(source, target);
                }
                return call + ')';
            }

            void dpiCall(std::ostream &out, const SimOp &op, PartitionId activeUnit, const std::string &cachedGuard = {}) const
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
                std::vector<std::pair<ValueId, std::string>> produced;
                if (cachedGuard.empty()) out << "if(" << callCondition(operands[0]) << " && (" << eventGuard(op, 1) << ")){\n";
                else out << "if(" << cachedGuard << " && " << callCondition(operands[0]) << "){\n";
                std::string call = "::" + identifier(model_.text(function.symbol)) + "(";
                for (std::size_t i = 0; i < arguments.size(); ++i)
                {
                    if (i) call += ',';
                    const auto &arg = arguments[i]; const auto &target = model_.types()[arg.type.index - 1];
                    if (arg.direction == DpiDirection::Input)
                    {
                        const auto source = value(operands[input++]);
                        call += target.kind == TypeKind::String ? "(" + source + ").c_str()" : normalize(source, target);
                    }
                    else
                    {
                        const auto temporary = "cpu_dpi_arg_" + std::to_string(i);
                        out << dpiType(arg.type) << ' ' << temporary;
                        if (arg.direction == DpiDirection::Inout) out << '=' << normalize(value(operands[inoutInput++]), target);
                        else out << "{}";
                        out << ";\n"; call += '&' + temporary;
                        produced.push_back({results[arg.direction == DpiDirection::Output ? output++ : inoutOutput++], temporary});
                    }
                }
                call += ')';
                if (function.returnType)
                {
                    out << "auto cpu_dpi_return=" << call << ";\n";
                    publishDpiResult(out, results[0], "cpu_dpi_return", activeUnit);
                }
                else out << call << ";\n";
                for (const auto &[result, temporary] : produced) publishDpiResult(out, result, temporary, activeUnit);
                out << "}\n";
                sampleEvents(out, op, 1);
            }

            void validateSystemTask(const SimOp &op) const
            {
                validateCallEvents(op, 0);
                if (!model_.results(op).empty()) throw std::runtime_error("CPU system task must not produce values");
                const auto params = model_.parameters(op);
                const auto *name = parameter<std::string>(model_, params, "name");
                static const std::set<std::string_view> supported{
                    "display", "write", "strobe", "fdisplay", "fwrite", "info", "warning", "error", "fatal", "finish", "stop"};
                if (!name || !supported.contains(*name))
                    throw std::runtime_error("CPU system task is not implemented: " + (name ? *name : "<missing>"));
                const auto *proc = parameter<std::string>(model_, params, "proc_kind");
                if (proc && *proc == "final")
                    throw std::runtime_error("CPU final-process task execution is not implemented");
                const auto *edges = parameter<std::vector<std::string>>(model_, params, "event_edges");
                for (auto operand : model_.operands(op).subspan(1, model_.operands(op).size() - 1 - (edges ? edges->size() : 0)))
                    if (type(operand).kind == TypeKind::Array)
                        throw std::runtime_error("CPU system task array arguments are not implemented");
            }

            // Extra per-op gate conjuncts (initial/first-eval and once flags).
            std::string sideCallExtras(const SimOp &op) const
            {
                const auto params = model_.parameters(op);
                const auto *proc = parameter<std::string>(model_, params, "proc_kind");
                const auto *timed = parameter<bool>(model_, params, "has_timing");
                std::string extras;
                if (proc && *proc == "initial" && (!timed || !*timed)) extras += " && cpu_first_eval";
                if (const auto once = onceTasks_.find(op.id.index); once != onceTasks_.end())
                    extras += " && !cpu_system_done[" + std::to_string(once->second) + ']';
                return extras;
            }

            void systemTaskBody(std::ostream &out, const SimOp &op) const
            {
                const auto params = model_.parameters(op);
                const auto &name = *parameter<std::string>(model_, params, "name");
                const auto *edges = parameter<std::vector<std::string>>(model_, params, "event_edges");
                const auto operands = model_.operands(op);
                const auto args = operands.subspan(1, operands.size() - 1 - (edges ? edges->size() : 0));
                out << "const std::array<grhsim_task_arg," << args.size() << "> cpu_args{{";
                for (std::size_t i = 0; i < args.size(); ++i)
                {
                    if (i) out << ',';
                    out << "grhsim_make_task_arg(" << value(args[i]);
                    if (type(args[i]).kind == TypeKind::Logic)
                        out << ',' << type(args[i]).width << ',' << (type(args[i]).isSigned ? "true" : "false");
                    out << ')';
                }
                out << "}};cpu_system_task(\"" << name << "\",cpu_args);\n";
                if (const auto once = onceTasks_.find(op.id.index); once != onceTasks_.end())
                    out << "cpu_system_done[" << once->second << "]=true;\n";
            }

            void systemTask(std::ostream &out, const SimOp &op, const std::string &cachedGuard = {}) const
            {
                const auto operands = model_.operands(op);
                if (cachedGuard.empty()) out << "if(" << callCondition(operands[0]) << " && (" << eventGuard(op, 0) << ")";
                else out << "if(" << cachedGuard << " && " << callCondition(operands[0]);
                out << sideCallExtras(op) << "){\n";
                systemTaskBody(out, op);
                out << "}\n";
                sampleEvents(out, op, 0);
            }

            // Event-gated side-effect calls (system tasks and resultless DPI
            // calls) share per-unit edge-guard locals. A maximal run of such
            // gates on the same guard is restructured into
            // `if (guard) { if (cond_i) { body_i } ... }`: the shared edge test
            // is evaluated once per run instead of once per gate, and adjacent
            // gates with textually identical conditions merge into one block.
            // Only consecutive ops with no in-body history sampling are
            // eligible, so side-effect order and sampling placement are
            // preserved exactly. Gates whose arguments are all compile-time
            // constants are emitted as unlikely so the compiler sinks the
            // never-executed diagnostic bodies out of the sequential fetch
            // path; calls with runtime arguments (event streams that fire
            // often) keep the default layout.
            struct SideGateInfo
            {
                std::string guard;
                std::string condition;
                bool constantBody = false;
            };

            std::optional<SideGateInfo> sideCallGate(const SimOp &op, const std::map<uint32_t, std::string> &guards) const
            {
                const auto name = model_.text(op.opType);
                const bool system = name == "core.system.task", dpi = name == "core.dpi.call";
                if ((!system && !dpi) || !model_.results(op).empty()) return std::nullopt;
                const auto found = guards.find(op.id.index);
                if (found == guards.end() || found->second.empty()) return std::nullopt;
                const auto params = model_.parameters(op);
                const auto *edges = parameter<std::vector<std::string>>(model_, params, "event_edges");
                if (!edges || edges->empty()) return std::nullopt;
                const auto refs = model_.objectRefs(op);
                const std::size_t historyBase = dpi ? 1 : 0;
                if (refs.size() < historyBase + edges->size()) return std::nullopt;
                for (std::size_t i = 0; i < edges->size(); ++i)
                {
                    if (refs[historyBase + i].kind != ObjectKind::State) return std::nullopt;
                    const auto index = refs[historyBase + i].index;
                    if (!batchedHistories_[index] && !historyAliases_[index] && !directSampleStates_[index])
                        return std::nullopt;
                }
                const auto operands = model_.operands(op);
                bool constantBody = true;
                for (const auto arg : operands.subspan(1, operands.size() - 1 - edges->size()))
                {
                    const auto producer = producers_[arg.index];
                    if (!producer || model_.text(model_.operations()[producer.index - 1].opType) != "core.compute.constant")
                    {
                        constantBody = false;
                        break;
                    }
                }
                return SideGateInfo{found->second, callCondition(operands[0]) + sideCallExtras(op), constantBody};
            }

            void sideCallBody(std::ostream &out, const SimOp &op) const
            {
                if (model_.text(op.opType) == "core.system.task") { systemTaskBody(out, op); return; }
                out << dpiCallExpression(op) << ";\n";
            }

            void computeGroup(std::ostream &out, std::span<const OpId> ops, PartitionId unit,
                              const std::map<uint32_t, std::string> &guards = {}) const
            {
                if (dynamicStats_) out << "++cpu_dyn_sn_grp[" << unit.index << "];\n";
                // A compute helper observes a stable pre-commit snapshot. Cache
                // repeated scalar state reads (including transparent slices) once
                // per helper invocation so packed and unpacked readers do not
                // reload the same object slot for every derived value.
                std::map<uint32_t, uint32_t> stateUseCounts;
                for (auto id : ops)
                    for (auto operand : model_.operands(model_.operations()[id.index - 1]))
                        if (const auto source = stateReadSource(operand)) ++stateUseCounts[source->index];
                std::map<uint32_t, std::string> stateCaches;
                for (const auto &[index, count] : stateUseCounts)
                    if (count > 1)
                    {
                        const StateId source{index, 0};
                        const auto &sourceType = stateType(source);
                        if (sourceType.kind != TypeKind::Logic || sourceType.domain != LogicDomain::TwoState ||
                            sourceType.width == 0 || sourceType.width > 64) continue;
                        const auto name = "cpu_cached_state_" + std::to_string(index);
                        out << "const auto " << name << '=' << state(source) << ";\n";
                        stateCaches.emplace(index, name);
                    }
                activeStateCache_ = &stateCaches;
                std::map<uint32_t, std::string> valueCaches;
                if (!ops.empty())
                    if (const auto cache = helperReadCaches_.find(ops.front().index); cache != helperReadCaches_.end())
                        for (const auto source : *cache->second)
                        {
                            const auto name = "cpu_cached_value_" + std::to_string(source.index);
                            out << "const auto " << name << '=' << value(source) << ";\n";
                            valueCaches.emplace(source.index, name);
                        }
                activeValueCache_ = &valueCaches;
                struct ChangedGroup
                {
                    const CpuActivationTargets *targets = nullptr;
                    const std::vector<PortArmTarget> *ports = nullptr;
                };
                std::map<std::vector<uint32_t>, std::size_t> indices;
                std::vector<ChangedGroup> groups;
                std::map<uint32_t, std::size_t> groupByValue;
                for (auto id : ops)
                {
                    const auto &op = model_.operations()[id.index - 1];
                    const auto results = model_.results(op);
                    if (results.size() != 1 || model_.text(op.opType) == "core.dpi.call") continue;
                    const auto result = results[0];
                    if (staticScalars_.contains(result.index)) continue;
                    if (readAliases_[result.index] || type(result).kind != TypeKind::Logic) continue;
                    const auto *targets = fanout_[result.index];
                    const auto *ports = portArmTargets(result);
                    if (!targets && !ports) continue;
                    std::vector<uint32_t> key;
                    if (targets)
                    {
                        for (auto target : targets->activate) key.push_back(target.index);
                        key.push_back(0);
                        for (auto target : targets->arm) key.push_back(target.index);
                    }
                    key.push_back(~std::uint32_t(0));
                    if (ports)
                        for (const auto &target : *ports)
                        {
                            key.push_back(target.offset);
                            key.push_back(target.mask);
                        }
                    const auto [it, inserted] = indices.emplace(std::move(key), groups.size());
                    if (inserted) groups.push_back({targets, ports});
                    groupByValue.emplace(result.index, it->second);
                }
                for (std::size_t i = 0; i < groups.size(); ++i) out << "bool cpu_changed_" << i << "=false;\n";
                const auto emitOne = [&](OpId id) {
                    const auto &op = model_.operations()[id.index - 1];
                    const auto results = model_.results(op);
                    const auto found = results.size() == 1 ? groupByValue.find(results[0].index) : groupByValue.end();
                    const auto guard = guards.find(id.index);
                    compute(out, op, unit, found == groupByValue.end() ? std::string{} : "cpu_changed_" + std::to_string(found->second),
                        guard == guards.end() ? std::string{} : guard->second);
                };
                // Constants, aliased reads and static strings emit nothing, so they
                // must not split a gate run (they sit between the endpoint calls in
                // program order). compute() is a pure no-op for them.
                const auto emitsNothing = [&](OpId id) {
                    const auto &op = model_.operations()[id.index - 1];
                    const auto results = model_.results(op);
                    if (results.size() != 1) return false;
                    const auto result = results[0];
                    return staticScalars_.contains(result.index) || readAliases_[result.index] ||
                        (type(result).kind == TypeKind::String && staticStrings_.contains(result.index));
                };
                for (std::size_t pos = 0; pos < ops.size();)
                {
                    if (emitsNothing(ops[pos]))
                    {
                        ++pos;
                        continue;
                    }
                    const auto first = sideCallGate(model_.operations()[ops[pos].index - 1], guards);
                    if (!first)
                    {
                        emitOne(ops[pos]);
                        ++pos;
                        continue;
                    }
                    std::vector<std::pair<OpId, SideGateInfo>> run;
                    std::size_t end = pos;
                    while (end < ops.size())
                    {
                        if (emitsNothing(ops[end]))
                        {
                            ++end;
                            continue;
                        }
                        auto info = sideCallGate(model_.operations()[ops[end].index - 1], guards);
                        if (!info || info->guard != first->guard) break;
                        run.emplace_back(ops[end], std::move(*info));
                        ++end;
                    }
                    if (run.size() < 2)
                    {
                        const auto &only = run.front();
                        if (!only.second.constantBody) emitOne(only.first);
                        else
                        {
                            ++gateColdHints_;
                            out << "if(__builtin_expect(!!(" << only.second.guard << " && " << only.second.condition
                                << "),0)){ // cpu_cold_gate\n{\n";
                            sideCallBody(out, model_.operations()[only.first.index - 1]);
                            out << "}}\n";
                        }
                        pos = end;
                        continue;
                    }
                    ++gateHoistedRuns_;
                    gateHoistedGates_ += run.size();
                    out << "if(" << first->guard << "){ // cpu_gate_hoist gates=" << run.size() << "\n";
                    for (std::size_t k = 0; k < run.size();)
                    {
                        std::size_t merged = k + 1;
                        while (merged < run.size() && run[merged].second.condition == run[k].second.condition) ++merged;
                        const bool unlikely = std::all_of(run.begin() + k, run.begin() + merged,
                            [](const auto &entry) { return entry.second.constantBody; });
                        if (unlikely) gateColdHints_ += merged - k;
                        gateMergedGates_ += merged - k - 1;
                        out << "if(";
                        if (unlikely) out << "__builtin_expect(!!(" << run[k].second.condition << "),0)";
                        else out << run[k].second.condition;
                        if (merged - k > 1) out << "){ // cpu_gate_merge ops=" << merged - k << "\n";
                        else out << "){\n";
                        for (std::size_t j = k; j < merged; ++j)
                        {
                            // One scope per body: system-task bodies declare cpu_args.
                            out << "{\n";
                            sideCallBody(out, model_.operations()[run[j].first.index - 1]);
                            out << "}\n";
                        }
                        out << "}\n";
                        k = merged;
                    }
                    out << "}\n";
                    pos = end;
                }
                for (std::size_t i = 0; i < groups.size(); ++i)
                {
                    if (groups[i].targets) activate(out, *groups[i].targets, false, unit, "cpu_changed_" + std::to_string(i));
                    if (groups[i].ports) armPorts(out, *groups[i].ports, "cpu_changed_" + std::to_string(i));
                }
                if (dynamicStats_ && !groups.empty())
                {
                    out << "{const bool cpu_dyn_any=cpu_changed_0";
                    for (std::size_t i = 1; i < groups.size(); ++i) out << "|cpu_changed_" << i;
                    out << ";cpu_dyn_sn_chg[" << unit.index << "]+=cpu_dyn_any;++cpu_dyn_grp_pub;cpu_dyn_grp_fire+=cpu_dyn_any;}\n";
                }
                activeStateCache_ = nullptr;
                activeValueCache_ = nullptr;
            }

            void compute(std::ostream &out, const SimOp &op, PartitionId activeUnit, const std::string &changed = {},
                         const std::string &cachedGuard = {}) const
            {
                if (model_.text(op.opType) == "core.system.task")
                { systemTask(out, op, cachedGuard); return; }
                if (model_.text(op.opType) == "core.dpi.call")
                { dpiCall(out, op, activeUnit, cachedGuard); return; }
                if (model_.text(op.opType) == "core.output.write")
                {
                    const auto operand = model_.operands(op)[0]; const auto ref = model_.objectRefs(op)[0];
                    out << at(type(operand), arenaObjects(), object(ref).offset) << '=' << value(operand) << ";\n";
                    return;
                }
                const auto result = model_.results(op)[0];
                const auto &resultType = type(result);
                // All units and commit ports start active. Immutable operands
                // are available at every use, with no later change to publish.
                if (staticScalars_.contains(result.index)) return;
                if (readAliases_[result.index]) return;
                if (const auto read = memoryReadIds_[op.id.index])
                    out << "cpu_read_offsets[" << read - 1 << "]=" << object(model_.objectRefs(op)[0]).offset
                        << "+static_cast<std::size_t>(" << value(model_.operands(op)[0]) << ")*" << storageBytes(resultType) << ";\n";
                // Immutable strings are resolved at use sites, inside any existing call guard.
                // Every compute supernode is initially active; constants need no later fanout.
                if (resultType.kind == TypeKind::String && staticStrings_.contains(result.index)) return;
                if (resultType.kind == TypeKind::Logic && resultType.domain == LogicDomain::TwoState && resultType.width > 64)
                {
                    const auto name = model_.text(op.opType); const auto operands = model_.operands(op);
                    const auto words = (resultType.width + 63u) / 64u;
                    if (name == "core.compute.concat")
                    {
                        const auto *targets = fanout_[result.index];
                        out << "{\n";
                        if (targets) out << cppType(resultType) << " cpu_concat{};\n";
                        else out << "auto &cpu_concat=" << value(result) << ";cpu_concat.fill(0);\n";
                        // Inputs have distinct layout slots. Insert from the least significant end.
                        uint64_t offset = 0;
                        for (std::size_t i = operands.size(); i > 0 && offset < resultType.width; --i)
                        {
                            const auto operand = operands[i - 1];
                            const auto width = std::min<uint64_t>(type(operand).width, resultType.width - offset);
                            out << (type(operand).width > 64 ? "grhsim_insert_words" : "grhsim_insert_scalar_words")
                                << "(cpu_concat," << offset << ',' << value(operand) << ',' << width << ");\n";
                            offset += width;
                        }
                        if (targets)
                        {
                            if (!changed.empty())
                            {
                                if (dynamicStats_)
                                {
                                    const auto kind = dynKind(op);
                                    out << "{const bool cpu_dyn_c=(" << value(result) << "!=cpu_concat);++cpu_dyn_wr[" << kind
                                        << "];cpu_dyn_ch[" << kind << "]+=cpu_dyn_c;" << changed << "|=cpu_dyn_c;"
                                        << value(result) << "=cpu_concat;}\n";
                                }
                                else out << changed << "|=(" << value(result) << "!=cpu_concat);" << value(result) << "=cpu_concat;\n";
                            }
                            else if (dynamicStats_)
                            {
                                const auto kind = dynKind(op);
                                out << "++cpu_dyn_wr[" << kind << "];if(" << value(result) << "!=cpu_concat){++cpu_dyn_ch[" << kind
                                    << "];" << value(result) << "=cpu_concat;\n";
                                activate(out, *targets, false, activeUnit); out << "}\n";
                            }
                            else
                            {
                                out << "if(" << value(result) << "!=cpu_concat){" << value(result) << "=cpu_concat;\n";
                                activate(out, *targets, false, activeUnit); out << "}\n";
                            }
                        }
                        else if (dynBoundary(result)) out << "++cpu_dyn_silent[" << dynKind(op) << "];\n";
                        out << "}\n";
                        return;
                    }
                    if (name == "core.compute.replicate")
                    {
                        const auto operand = operands[0];
                        const auto &sourceType = type(operand);
                        const auto sourceWords = (sourceType.width + 63u) / 64u;
                        const auto *rep = parameter<int64_t>(model_, model_.parameters(op), "rep");
                        if (!rep || *rep < 0) throw std::runtime_error("missing or negative CPU replication parameter");
                        if (sourceType.width == 1 && !sourceType.isSigned && sourceType.domain == LogicDomain::TwoState)
                        {
                            // {rep{bit}} broadcast: every live lane equals the bit, so each
                            // result word is 0 or all-ones under the live-bit mask; dead
                            // padding lanes stay zero exactly like the word helper.
                            const auto kind = dynamicStats_ ? dynKind(op) : 0;
                            out << "{\nconst std::uint64_t cpu_rword=0-static_cast<std::uint64_t>(" << value(operand) << ");\n";
                            if (dynamicStats_) out << "++cpu_dyn_wr[" << kind << "];\n";
                            out << "bool cpu_rchanged=false;\n";
                            const std::uint64_t live = std::min<std::uint64_t>(static_cast<std::uint64_t>(*rep), resultType.width);
                            for (std::uint64_t word = 0; word < words; ++word)
                            {
                                const std::uint64_t liveBits = live > word * 64 ? std::min<std::uint64_t>(live - word * 64, 64) : 0;
                                out << "{const std::uint64_t cpu_rnext=cpu_rword&"
                                    << (liveBits == 64 ? "UINT64_MAX" : "((UINT64_C(1)<<" + std::to_string(liveBits) + ")-1)")
                                    << ";cpu_rchanged|=(" << value(result) << '[' << word << "]!=cpu_rnext);" << value(result) << '['
                                    << word << "]=cpu_rnext;}\n";
                            }
                            if (dynamicStats_) out << "cpu_dyn_ch[" << kind << "]+=cpu_rchanged;\n";
                            if (!changed.empty()) out << changed << "|=cpu_rchanged;\n";
                            else if (const auto *targets = fanout_[result.index])
                            {
                                out << "if(cpu_rchanged){\n";
                                activate(out, *targets, false, activeUnit); out << "}\n";
                            }
                            out << "}\n";
                            return;
                        }
                        const auto call = (sourceType.width > 64 ?
                            "cpu_replicate_words_changed<" + std::to_string(words) + "," + std::to_string(sourceWords) + ">( " :
                            "cpu_replicate_words_changed<" + std::to_string(words) + ">( ") + value(operand) + "," +
                            std::to_string(sourceType.width) + "," + std::to_string(*rep) + "," +
                            std::to_string(resultType.width) + "," + value(result) + ")";
                        if (!changed.empty())
                        {
                            if (dynamicStats_)
                            {
                                const auto kind = dynKind(op);
                                out << "{const bool cpu_dyn_c=" << call << ";++cpu_dyn_wr[" << kind << "];cpu_dyn_ch[" << kind
                                    << "]+=cpu_dyn_c;" << changed << "|=cpu_dyn_c;}\n";
                            }
                            else out << changed << "|=" << call << ";\n";
                        }
                        else if (const auto *targets = fanout_[result.index])
                        {
                            if (dynamicStats_)
                            {
                                const auto kind = dynKind(op);
                                out << "{const bool cpu_dyn_c=" << call << ";++cpu_dyn_wr[" << kind << "];cpu_dyn_ch[" << kind
                                    << "]+=cpu_dyn_c;if(cpu_dyn_c){\n";
                                activate(out, *targets, false, activeUnit); out << "}}\n";
                            }
                            else
                            {
                                out << "if(" << call << "){\n";
                                activate(out, *targets, false, activeUnit); out << "}\n";
                            }
                        }
                        else if (dynBoundary(result)) out << "++cpu_dyn_silent[" << dynKind(op) << "];(void)" << call << ";\n";
                        else
                            out << "(void)" << call << ";\n";
                        return;
                    }
                    const auto ptr = [&](ValueId valueId, const std::string &expr) {
                        return type(valueId).width > 64 ? "(" + expr + ").data()" : "&cpu_operand_" + std::to_string(valueId.index);
                    };
                    const bool pointerOperation = name == "core.compute.and" || name == "core.compute.or" ||
                        name == "core.compute.xor" || name == "core.compute.not" || name == "core.compute.shl" ||
                        name == "core.compute.lshr" || name == "core.compute.ashr" || name == "core.compute.add" || name == "core.compute.sub";
                    if (pointerOperation)
                    {
                        out << "{\n";
                        std::set<uint32_t> scalars;
                        const bool binary = name == "core.compute.and" || name == "core.compute.or" ||
                            name == "core.compute.xor" || name == "core.compute.add" || name == "core.compute.sub";
                        for (std::size_t i = 0; i < (binary ? 2u : 1u); ++i)
                        {
                            const auto operand = operands[i];
                            if (type(operand).width <= 64 && scalars.insert(operand.index).second)
                                out << "const std::uint64_t cpu_operand_" << operand.index << "=grhsim_trunc_u64("
                                    << value(operand) << ',' << type(operand).width << ");\n";
                        }
                    }
                    if (const auto *targets = fanout_[result.index]; targets &&
                        (name == "core.compute.and" || name == "core.compute.or" ||
                         name == "core.compute.xor" || name == "core.compute.not"))
                    {
                        const bool unary = name == "core.compute.not";
                        const char operation = unary ? '~' : name == "core.compute.and" ? '&' : name == "core.compute.or" ? '|' : '^';
                        const auto call = [&](std::ostream &stream)
                        {
                            stream << "cpu_bitwise_words_changed<'" << operation << "'>(" << ptr(operands[0], value(operands[0])) << ','
                                << ((type(operands[0]).width + 63u) / 64u) << ',';
                            if (unary) stream << "nullptr,0,";
                            else stream << ptr(operands[1], value(operands[1])) << ',' << ((type(operands[1]).width + 63u) / 64u) << ',';
                            stream << resultType.width << ',' << ptr(result, value(result)) << ',' << words << ')';
                        };
                        if (dynamicStats_)
                        {
                            const auto kind = dynKind(op);
                            out << "{const bool cpu_dyn_c="; call(out);
                            out << ";++cpu_dyn_wr[" << kind << "];cpu_dyn_ch[" << kind << "]+=cpu_dyn_c;";
                            if (changed.empty()) { out << "if(cpu_dyn_c){\n"; activate(out, *targets, false, activeUnit); out << "}}\n"; }
                            else out << changed << "|=cpu_dyn_c;}\n";
                        }
                        else
                        {
                            out << (changed.empty() ? "if(" : changed + "|="); call(out);
                            if (changed.empty()) { out << "){\n"; activate(out, *targets, false, activeUnit); out << "}\n"; }
                            else out << ";\n";
                        }
                        out << "}\n";
                        return;
                    }
                    if (name == "core.compute.and" || name == "core.compute.or" || name == "core.compute.xor")
                    {
                        if (dynBoundary(result)) out << "++cpu_dyn_silent[" << dynKind(op) << "];\n";
                        out << "grhsim_" << name.substr(std::string_view("core.compute.").size()) << "_words(" << ptr(operands[0], value(operands[0])) << ','
                            << ((type(operands[0]).width + 63u) / 64u) << ',' << ptr(operands[1], value(operands[1])) << ','
                            << ((type(operands[1]).width + 63u) / 64u) << ',' << resultType.width << ',' << ptr(result, value(result)) << ',' << words << ");\n";
                        out << "}\n";
                        return;
                    }
                    if (name == "core.compute.not")
                    {
                        if (dynBoundary(result)) out << "++cpu_dyn_silent[" << dynKind(op) << "];\n";
                        out << "grhsim_not_words(" << ptr(operands[0], value(operands[0])) << ',' << ((type(operands[0]).width + 63u) / 64u) << ','
                            << resultType.width << ',' << ptr(result, value(result)) << ',' << words << ");\n";
                        out << "}\n";
                        return;
                    }
                    if (name == "core.compute.shl" || name == "core.compute.lshr" || name == "core.compute.ashr")
                    {
                        const auto *targets = fanout_[result.index];
                        const auto call = [&](std::ostream &stream)
                        {
                            stream << (targets ? "cpu_shift_words_changed<'" : "grhsim_")
                                << (targets ? (name == "core.compute.shl" ? "L" : name == "core.compute.lshr" ? "R" : "A")
                                    : name == "core.compute.shl" ? "shl" : name == "core.compute.lshr" ? "lshr" : "ashr")
                                << (targets ? "'>(" : "_words(")
                                << ptr(operands[0], value(operands[0])) << ','
                                << ((type(operands[0]).width + 63u) / 64u) << ",grhsim_index_words(" << value(operands[1]) << ',' << resultType.width << ")," << resultType.width << ','
                                << ptr(result, value(result)) << ',' << words << ')';
                        };
                        if (dynamicStats_ && targets)
                        {
                            const auto kind = dynKind(op);
                            out << "{const bool cpu_dyn_c="; call(out);
                            out << ";++cpu_dyn_wr[" << kind << "];cpu_dyn_ch[" << kind << "]+=cpu_dyn_c;";
                            if (changed.empty()) { out << "if(cpu_dyn_c){\n"; activate(out, *targets, false, activeUnit); out << "}}\n"; }
                            else out << changed << "|=cpu_dyn_c;}\n";
                        }
                        else
                        {
                            if (!targets && dynBoundary(result)) out << "++cpu_dyn_silent[" << dynKind(op) << "];\n";
                            out << (targets ? (changed.empty() ? "if(" : changed + "|=") : ""); call(out);
                            if (targets && changed.empty()) { out << "){\n"; activate(out, *targets, false, activeUnit); out << "}\n"; }
                            else out << ";\n";
                        }
                        out << "}\n";
                        return;
                    }
                    if (name == "core.compute.add" || name == "core.compute.sub")
                    {
                        const auto *targets = fanout_[result.index];
                        const auto call = [&](std::ostream &stream)
                        {
                            stream << (targets ? (name == "core.compute.add" ? "cpu_arithmetic_words_changed<'+'>(" : "cpu_arithmetic_words_changed<'-'>(")
                                    : (name == "core.compute.add" ? "grhsim_add_words(" : "grhsim_sub_words("))
                                << ptr(operands[0], value(operands[0])) << ','
                                << ((type(operands[0]).width + 63u) / 64u) << ',' << ptr(operands[1], value(operands[1])) << ','
                                << ((type(operands[1]).width + 63u) / 64u) << ',' << resultType.width << ',' << ptr(result, value(result)) << ',' << words << ')';
                        };
                        if (dynamicStats_ && targets)
                        {
                            const auto kind = dynKind(op);
                            out << "{const bool cpu_dyn_c="; call(out);
                            out << ";++cpu_dyn_wr[" << kind << "];cpu_dyn_ch[" << kind << "]+=cpu_dyn_c;";
                            if (changed.empty()) { out << "if(cpu_dyn_c){\n"; activate(out, *targets, false, activeUnit); out << "}}\n"; }
                            else out << changed << "|=cpu_dyn_c;}\n";
                        }
                        else
                        {
                            if (!targets && dynBoundary(result)) out << "++cpu_dyn_silent[" << dynKind(op) << "];\n";
                            out << (targets ? (changed.empty() ? "if(" : changed + "|=") : ""); call(out);
                            if (targets && changed.empty()) { out << "){\n"; activate(out, *targets, false, activeUnit); out << "}\n"; }
                            else out << ";\n";
                        }
                        out << "}\n";
                        return;
                    }
                }
                const auto expr = normalize(expression(op), resultType);
                if (!changed.empty())
                {
                    if (dynamicStats_)
                    {
                        const auto kind = dynKind(op);
                        out << "{const auto cpu_value=" << expr << ";const bool cpu_dyn_c=(" << value(result)
                            << "!=cpu_value);++cpu_dyn_wr[" << kind << "];cpu_dyn_ch[" << kind << "]+=cpu_dyn_c;"
                            << changed << "|=cpu_dyn_c;" << value(result) << "=cpu_value;}\n";
                    }
                    else
                        out << "{const auto cpu_value=" << expr << ';' << changed << "|=(" << value(result) << "!=cpu_value);"
                            << value(result) << "=cpu_value;}\n";
                }
                else if (const auto *targets = fanout_[result.index])
                {
                    if (dynamicStats_)
                    {
                        const auto kind = dynKind(op);
                        out << "{ const auto cpu_value=" << expr << ";++cpu_dyn_wr[" << kind << "];if(" << value(result)
                            << "!=cpu_value){++cpu_dyn_ch[" << kind << "];\n" << value(result) << "=cpu_value;\n";
                        activate(out, *targets, false, activeUnit); out << "}}\n";
                    }
                    else
                    {
                        out << "{ const auto cpu_value=" << expr << "; if(" << value(result) << "!=cpu_value){\n"
                            << value(result) << "=cpu_value;\n";
                        activate(out, *targets, false, activeUnit); out << "}}\n";
                    }
                }
                else if (dynBoundary(result))
                    out << "++cpu_dyn_silent[" << dynKind(op) << "];" << value(result) << '=' << expr << ";\n";
                else out << value(result) << '=' << expr << ";\n";
            }

            bool isScalarLogic(const Type &type) const
            {
                return type.kind == TypeKind::Logic && type.domain == LogicDomain::TwoState &&
                    type.width > 0 && type.width <= 64;
            }

            void stage(std::ostream &out, StateId target, std::string expression) const
            {
                if (batchedHistories_[target.index] || historyAliases_[target.index]) return;
                // Unit-private histories are sampled directly at the unit block end.
                if (directSampleStates_[target.index]) return;
                // Commit-private histories defer to a direct store at the task end.
                if (commitDirectHistories_[target.index])
                {
                    deferredHistoryStores_.emplace_back(target, std::move(expression));
                    return;
                }
                const auto range = stateRanges_[target.index]; const auto &type = stateType(target);
                const bool scalar = isScalarLogic(type);
                out << (scalar ? "cpu_write_scalar<" : "cpu_stage<") << cppType(type) << ">(cpu_obj_,cpu_shadow_," << target.index << ',' << object(ObjectRef::state(target)).offset
                    << ',' << range.offset << ',' << range.count << ',' << (projected_[target.index] ? "true" : "false")
                    << (scalar ? "," : ")=") << normalize(std::move(expression), type) << (scalar ? ");\n" : ";\n");
            }

            // History sampling of a memWrite op, emitted separately when the write
            // part moved into a guard-hoisted run block (cpu_mem_guard_hoist).
            void memWriteStages(std::ostream &out, const SimOp &op) const
            {
                const auto refs = model_.objectRefs(op);
                const auto operands = model_.operands(op);
                const auto *edges = parameter<std::vector<std::string>>(model_, model_.parameters(op), "event_edges");
                if (edges) for (std::size_t i = 0; i < edges->size(); ++i)
                    stage(out, {refs[i + 1].index, 0}, eventValue(operands[operands.size() - edges->size() + i]));
            }

            std::string stageCell(StateId target, const std::string &row) const
            {
                const auto &array = stateType(target);
                const auto &element = model_.types()[array.elementType.index - 1];
                const auto range = memoryRanges_[target.index];
                return "cpu_stage_cell(cpu_obj_,cpu_shadow_," + std::to_string(memoryDirtyBases_[target.index]) + "," +
                    std::to_string(object(ObjectRef::state(target)).offset) + "," + std::to_string(storageBytes(element)) +
                    "," + row + "," + std::to_string(range.offset) + "," + std::to_string(range.count) + "," +
                    (projected_[target.index] ? "true" : "false") + ")";
            }

            void writeCell(std::ostream &out, StateId target, const std::string &row,
                           const std::string &data, const std::string &mask = "UINT64_MAX") const
            {
                const auto &element = model_.types()[stateType(target).elementType.index - 1];
                const auto range = memoryRanges_[target.index];
                out << "cpu_write_cell<" << cppType(element) << ',' << element.width << ">(cpu_obj_,cpu_shadow_," << memoryDirtyBases_[target.index]
                    << ',' << object(ObjectRef::state(target)).offset << ',' << row << ',' << range.offset << ',' << range.count
                    << ',' << (projected_[target.index] ? "true" : "false") << ',' << data << ',' << mask << ");\n";
            }

            std::string commitEdgeGuard(const SimOp &op, const std::string &cachedGuard = {}) const
            {
                const auto operands = model_.operands(op);
                const auto refs = model_.objectRefs(op);
                const auto *edges = parameter<std::vector<std::string>>(model_, model_.parameters(op), "event_edges");
                std::string guard = model_.text(op.opType) == "core.state.latchWrite" ? "true" : "false";
                if (edges)
                    for (std::size_t i = 0; i < edges->size(); ++i)
                    {
                        const auto event = eventValue(operands[operands.size() - edges->size() + i]);
                        const StateId history{refs[i + 1].index, 0};
                        guard += " || (" + std::string((*edges)[i] == "posedge" ? "!" : "") + state(history) + " && " +
                                 ((*edges)[i] == "negedge" ? "!" : "") + event + ")";
                    }
                if (!cachedGuard.empty()) guard = cachedGuard;
                return guard;
            }

            // Parsed value of a two-state scalar constant (≤64 bits), if the value's
            // producer is a core.compute.constant.
            std::optional<std::uint64_t> scalarConstantValue(ValueId operand) const
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

            // All-ones 64-bit two-state constant masks make the commit merge the identity:
            // next == data, so the compact walk can compare current against data directly.
            bool isAllOnesConstant64(ValueId operand) const
            {
                if (type(operand).width != 64) return false;
                const auto bits = scalarConstantValue(operand);
                return bits && *bits == std::numeric_limits<std::uint64_t>::max();
            }

            // Emit one full 64-port pflag word-group as a compact walk: the arm bits
            // loaded as one uint64 drive a ctz iteration that visits exactly the armed
            // ports (unarmed ports cost nothing), while the per-port commit bodies share
            // one generic loop reading per-port descriptors (state/enable/data boundary
            // offsets and notify constants) instead of inline constant test chains.
            // Eligibility: every port of the group is a single-writer direct-commit
            // u64 two-state regWrite/latchWrite with an all-ones mask (next == data) and
            // a single-target notification. Evaluating an armed port is identical to the
            // per-bit walk (enable gate, current != data, store, notify), in the same
            // ascending port order; the group's bytes are consumed as a whole exactly
            // like the portMask==255 per-byte clear under a uniform guard.
            bool emitCommitCompactWalk(
                std::ostream &out,
                const std::vector<std::map<std::uint32_t, std::vector<OpId>>::const_iterator> &group, std::uint32_t base) const
            {
                if (!commitCompactWalk_ || group.size() != 8) return false;
                struct Lane
                {
                    std::uint32_t stateOffset = 0, enableOffset = 0, dataOffset = 0, notifyOffset = 0;
                    std::uint8_t notifyMask = 0, notifyFlags = 0;
                    bool present = false;
                };
                std::array<Lane, 64> lanes;
                for (auto entry : group)
                {
                    if (entry->second.size() != 8) return false;
                    for (const auto opId : entry->second)
                    {
                        const auto &operation = model_.operations()[opId.index - 1];
                        const auto name = model_.text(operation.opType);
                        if (name != "core.state.regWrite" && name != "core.state.latchWrite") return false;
                        const auto refs = model_.objectRefs(operation);
                        const auto operands = model_.operands(operation);
                        if (refs.empty() || refs[0].kind != ObjectKind::State || operands.size() < 3) return false;
                        const StateId target{refs[0].index, 0};
                        if (!directCommitStates_[target.index]) return false;
                        const auto &targetType = stateType(target);
                        if (targetType.kind != TypeKind::Logic || targetType.domain != LogicDomain::TwoState ||
                            targetType.width != 64)
                            return false;
                        if (!isAllOnesConstant64(operands[2])) return false;
                        // Constant operands render as literals via value(); their layout
                        // slots are not live boundary storage. A constant enable is folded
                        // into the descriptor flags; a constant data stays a fallback.
                        Lane &lane = lanes[(entry->first - base) * 8 + portArmBits_[opId.index]];
                        bool enableConstant = false;
                        std::uint64_t enableConstValue = 0;
                        if (const auto enableBits = scalarConstantValue(operands[0]))
                        {
                            if (*enableBits > 1) return false;
                            enableConstant = true;
                            enableConstValue = *enableBits;
                        }
                        else if (staticScalars_.count(operands[0].index))
                            return false;
                        if (staticScalars_.count(operands[1].index)) return false;
                        const auto &enableSlot = layout_.values[operands[0].index - 1];
                        const auto &dataSlot = layout_.values[operands[1].index - 1];
                        if (!enableConstant && enableSlot.kind != CpuStorageKind::Boundary) return false;
                        if (dataSlot.kind != CpuStorageKind::Boundary) return false;
                        const auto range = stateRanges_[target.index];
                        if (!projected_[target.index] && !range.count) return false;
                        if (range.count != 1) return false;
                        const auto &notification = stateTargets_[range.offset];
                        lane.present = true;
                        lane.stateOffset = static_cast<std::uint32_t>(object(ObjectRef::state(target)).offset);
                        lane.enableOffset = static_cast<std::uint32_t>(enableSlot.offset);
                        lane.dataOffset = static_cast<std::uint32_t>(dataSlot.offset);
                        lane.notifyOffset = notification.offset;
                        lane.notifyMask = notification.mask;
                        lane.notifyFlags = (notification.arm ? 1 : 0) | (projected_[target.index] ? 2 : 0) |
                                           (enableConstant ? 4 : 0) | (enableConstValue ? 8 : 0);
                    }
                }
                for (const auto &lane : lanes) if (!lane.present) return false;
                ++commitCompactGroups_;
                commitCompactPorts_ += 64;
                out << "{ // cpu_compact_walk\n"
                    << "static constexpr std::uint32_t cpu_cw_state[64]={";
                for (const auto &lane : lanes) out << lane.stateOffset << ',';
                out << "};\nstatic constexpr std::uint32_t cpu_cw_enable[64]={";
                for (const auto &lane : lanes) out << lane.enableOffset << ',';
                out << "};\nstatic constexpr std::uint32_t cpu_cw_data[64]={";
                for (const auto &lane : lanes) out << lane.dataOffset << ',';
                out << "};\nstatic constexpr std::uint32_t cpu_cw_ntf[64]={";
                for (const auto &lane : lanes) out << lane.notifyOffset << ',';
                out << "};\nstatic constexpr std::uint8_t cpu_cw_nfmask[64]={";
                for (const auto &lane : lanes) out << static_cast<unsigned>(lane.notifyMask) << ',';
                out << "};\nstatic constexpr std::uint8_t cpu_cw_nfflags[64]={";
                for (const auto &lane : lanes) out << static_cast<unsigned>(lane.notifyFlags) << ',';
                out << "};\n";
                out << "std::uint64_t cpu_todo=cpu_word8(cpu_pflags.data()," << base << ",8);\nif(cpu_todo){\n"
                    << "{const std::uint64_t cpu_zero=0;std::memcpy(cpu_pflags.data()+" << base << ",&cpu_zero,8);}\n"
                    << "do{const unsigned cpu_i=__builtin_ctzll(cpu_todo);cpu_todo&=cpu_todo-1;\n";
                if (dynamicStats_) out << "++cpu_dyn_port_eval;if(cpu_dyn_edge==1)++cpu_dyn_port_eval_pos;else if(cpu_dyn_edge==2)++cpu_dyn_port_eval_neg;if(cpu_dyn_round_cur==0)++cpu_dyn_port_eval_r0;else ++cpu_dyn_port_eval_rN;\n";
                out << "const bool cpu_en=(cpu_cw_nfflags[cpu_i]&4)?((cpu_cw_nfflags[cpu_i]&8)!=0):cpu_at<bool>(cpu_bnd_,cpu_cw_enable[cpu_i]);\n"
                    << "if(cpu_en){\n"
                    << "const std::uint64_t cpu_d=cpu_at<std::uint64_t>(cpu_bnd_,cpu_cw_data[cpu_i]);\n"
                    << "auto &cpu_c=cpu_at<std::uint64_t>(cpu_obj_,cpu_cw_state[cpu_i]);\n"
                    << "if(cpu_c!=cpu_d){cpu_c=cpu_d;\n";
                if (dynamicStats_) out << "++cpu_dyn_port_fire;if(cpu_dyn_edge==1)++cpu_dyn_port_fire_pos;else if(cpu_dyn_edge==2)++cpu_dyn_port_fire_neg;\n";
                out << "cpu_direct_state_changed_one(cpu_cw_ntf[cpu_i],cpu_cw_nfmask[cpu_i],"
                       "static_cast<bool>(cpu_cw_nfflags[cpu_i]&1),static_cast<bool>(cpu_cw_nfflags[cpu_i]&2));\n"
                    << "}}}while(cpu_todo);\n}}\n";
                return true;
            }

            void directCommitBody(std::ostream &out, const SimOp &op) const
            {
                const auto operands = model_.operands(op);
                const StateId target{model_.objectRefs(op)[0].index, 0};
                const auto range = stateRanges_[target.index];
                // No commit observer can see this single writer before the next compute phase.
                out << "// cpu_direct_commit state=" << target.index << "\n"
                    << "auto &cpu_current=" << state(target) << ";\nconst auto cpu_value="
                    << normalize("(static_cast<std::uint64_t>(cpu_current)&~static_cast<std::uint64_t>(" + value(operands[2]) +
                        "))|(static_cast<std::uint64_t>(" + value(operands[1]) + ")&static_cast<std::uint64_t>(" + value(operands[2]) + "))",
                        stateType(target)) << ";\nif(cpu_current!=cpu_value){cpu_current=cpu_value;\n";                if (projected_[target.index] || range.count)
                {
                    if (range.count == 1)
                    {
                        const auto &notification = stateTargets_[range.offset];
                        out << "cpu_direct_state_changed_one(" << notification.offset << ',' << notification.mask << ','
                            << (notification.arm ? "true" : "false") << ','
                            << (projected_[target.index] ? "true" : "false") << ");\n";
                    }
                    else
                        out << "cpu_direct_state_changed(" << range.offset << ',' << range.count << ','
                            << (projected_[target.index] ? "true" : "false") << ");\n";
                }
                out << "}\n";
            }

            void commit(std::ostream &out, const SimOp &op, const std::string &cachedGuard = {}) const
            {
                const auto operands = model_.operands(op); const auto refs = model_.objectRefs(op);
                const auto opName = model_.text(op.opType);
                if (opName == "core.state.memFill")
                {
                    if (refs.empty() || operands.size() < 2) throw std::runtime_error("CPU memory fill has invalid arity");
                    const auto target = StateId{refs[0].index, 0}; const auto &array = stateType(target);
                    if (array.kind != TypeKind::Array) throw std::runtime_error("CPU memory fill target is not an array");
                    const auto &element = model_.types()[array.elementType.index - 1];
                    const auto *edges = parameter<std::vector<std::string>>(model_, model_.parameters(op), "event_edges");
                    const std::size_t eventCount = edges ? edges->size() : 0;
                    if (operands.size() <= eventCount) throw std::runtime_error("CPU memory fill has no data operand");
                    std::string guard = "false";
                    if (edges) for (std::size_t i = 0; i < eventCount; ++i)
                    {
                        const auto event = eventValue(operands[operands.size() - eventCount + i]);
                        const StateId history{refs[i + 1].index, 0};
                        guard += " || (" + std::string((*edges)[i] == "posedge" ? "!" : "") + state(history) + " && " +
                                 ((*edges)[i] == "negedge" ? "!" : "") + event + ")";
                    }
                    if (!cachedGuard.empty()) guard = cachedGuard;
                    out << "if((" << guard << ") && " << value(operands[0]) << "){ for(std::size_t i=0;i<" << array.count << ";++i) ";
                    if (isScalarLogic(element)) writeCell(out, target, "i", value(operands[1]));
                    else out << "cpu_at<" << cppType(element) << ">(" << stageCell(target, "i") << ",0)="
                             << normalize(value(operands[1]), element) << ";\n";
                    out << "}\n";
                    if (edges) for (std::size_t i = 0; i < eventCount; ++i)
                        stage(out, {refs[i + 1].index, 0}, eventValue(operands[operands.size() - eventCount + i]));
                    return;
                }
                if (opName == "core.state.memAssign")
                    throw std::runtime_error("CPU C++ emit memAssign array values are not implemented");
                if (opName == "core.state.memWrite")
                {
                    if (refs.empty() || operands.size() < 4) throw std::runtime_error("CPU memory write has invalid arity");
                    const auto target = StateId{refs[0].index, 0}; const auto &array = stateType(target);
                    if (array.kind != TypeKind::Array) throw std::runtime_error("CPU memory write target is not an array");
                    const auto &element = model_.types()[array.elementType.index - 1];
                    std::string guard = "false";
                    const auto *edges = parameter<std::vector<std::string>>(model_, model_.parameters(op), "event_edges");
                    if (edges) for (std::size_t i = 0; i < edges->size(); ++i)
                    {
                        const auto event = eventValue(operands[operands.size() - edges->size() + i]);
                        const StateId history{refs[i + 1].index, 0};
                        guard += " || (" + std::string((*edges)[i] == "posedge" ? "!" : "") + state(history) + " && " +
                                 ((*edges)[i] == "negedge" ? "!" : "") + event + ")";
                    }
                    if (!cachedGuard.empty()) guard = cachedGuard;
                    // cpu_mem_guard_hoist: the run wrapper already tested this op's
                    // snapshot guard; history sampling stays unconditional below,
                    // emitted by the run handler after the guarded write block.
                    const bool guardStripped = commitMemWalk_ && memGuardStripped_.contains(op.id.index);
                    std::string enableText;
                    if (const auto cached = memEnableCache_.find(operands[0].index);
                        commitMemWalk_ && cached != memEnableCache_.end())
                    { enableText = cached->second; ++memEnableCacheSites_; }
                    else enableText = value(operands[0]);
                    if (dynamicStats_)
                    {
                        const std::string mwGateEdgeSplit =
                            "if(cpu_dyn_edge==1)++cpu_dyn_mw_gate_pos;else if(cpu_dyn_edge==2)++cpu_dyn_mw_gate_neg;";
                        out << (guardStripped ? "++cpu_dyn_mw_gate;" + mwGateEdgeSplit + "\n"
                                              : "if(" + guard + "){++cpu_dyn_mw_gate;" + mwGateEdgeSplit + "}\n");
                    }
                    if (guardStripped)
                        out << "if(" << enableText << " && static_cast<std::size_t>("
                            << value(operands[1]) << ")<" << array.count << "){\n";
                    else
                        out << "if((" << guard << ") && " << enableText << " && static_cast<std::size_t>("
                            << value(operands[1]) << ")<" << array.count << "){\n";
                    if (dynamicStats_) out << "++cpu_dyn_mw_fire;if(cpu_dyn_edge==1)++cpu_dyn_mw_fire_pos;else if(cpu_dyn_edge==2)++cpu_dyn_mw_fire_neg;\n";
                    if (isScalarLogic(element))
                        writeCell(out, target, value(operands[1]), value(operands[2]), value(operands[3]));
                    else
                    {
                        out << "auto &cpu_cell=cpu_at<" << cppType(element) << ">(" << stageCell(target, value(operands[1])) << ",0);\n";
                        if (element.kind == TypeKind::Logic && element.width > 64)
                            out << "grhsim_apply_masked_words_inplace(cpu_cell," << value(operands[2]) << ','
                                << value(operands[3]) << ',' << element.width << ");\n";
                        else out << "cpu_cell=" << normalize("(static_cast<std::uint64_t>(cpu_cell)&~static_cast<std::uint64_t>(" + value(operands[3]) +
                            "))|(static_cast<std::uint64_t>(" + value(operands[2]) + ")&static_cast<std::uint64_t>(" + value(operands[3]) + "))", element) << ";\n";
                    }
                    out << "}\n";
                    if (edges && !guardStripped) for (std::size_t i = 0; i < edges->size(); ++i)
                        stage(out, {refs[i + 1].index, 0}, eventValue(operands[operands.size() - edges->size() + i]));
                    return;
                }
                if (opName == "core.state.memWriteSeq")
                {
                    if (refs.empty() || operands.size() < 3) throw std::runtime_error("CPU sequential memory write has invalid arity");
                    const auto target = StateId{refs[0].index, 0}; const auto &array = stateType(target);
                    if (array.kind != TypeKind::Array) throw std::runtime_error("CPU sequential memory write target is not an array");
                    const auto &element = model_.types()[array.elementType.index - 1];
                    const auto *edges = parameter<std::vector<std::string>>(model_, model_.parameters(op), "event_edges");
                    const std::size_t eventCount = edges ? edges->size() : 0;
                    if (operands.size() < eventCount || (operands.size() - eventCount) % 3 != 0)
                        throw std::runtime_error("CPU sequential memory write operands are not triples");
                    std::string guard = "false";
                    if (edges) for (std::size_t i = 0; i < eventCount; ++i)
                    {
                        const auto event = eventValue(operands[operands.size() - eventCount + i]);
                        const StateId history{refs[i + 1].index, 0};
                        guard += " || (" + std::string((*edges)[i] == "posedge" ? "!" : "") + state(history) + " && " +
                                 ((*edges)[i] == "negedge" ? "!" : "") + event + ")";
                    }
                    if (!cachedGuard.empty()) guard = cachedGuard;
                    out << "if(" << guard << "){\n";
                    for (std::size_t i = 0; i < operands.size() - eventCount; i += 3)
                    {
                        out << "if(" << value(operands[i]) << " && static_cast<std::size_t>(" << value(operands[i + 1]) << ")<"
                            << array.count << "){\n";
                        if (isScalarLogic(element)) writeCell(out, target, value(operands[i + 1]), value(operands[i + 2]));
                        else out << "cpu_at<" << cppType(element) << ">(" << stageCell(target, value(operands[i + 1]))
                                 << ",0)=" << normalize(value(operands[i + 2]), element) << ";\n";
                        out << "}\n";
                    }
                    out << "}\n";
                    if (edges) for (std::size_t i = 0; i < edges->size(); ++i)
                        stage(out, {refs[i + 1].index, 0}, eventValue(operands[operands.size() - edges->size() + i]));
                    return;
                }
                const auto *edges = parameter<std::vector<std::string>>(model_, model_.parameters(op), "event_edges");
                const std::string guard = commitEdgeGuard(op, cachedGuard);
                out << "if((" << guard << ") && " << value(operands[0]) << "){\n";
                const StateId target{refs[0].index, 0}; const auto range = stateRanges_[target.index];
                if (directCommitStates_[target.index])
                {
                    directCommitBody(out, op);
                    out << "}\n";
                }
                else if (isScalarLogic(stateType(target)))
                {
                    // Merge against the latest shadow so repeated writes retain program order.
                    out << "const auto cpu_next=" << at(stateType(target),
                        "(cpu_dirty[" + std::to_string(target.index) + "]?" + std::string(arenaShadow()) + ":" +
                            std::string(arenaObjects()) + ")",
                        object(refs[0]).offset) << ";\n";
                    stage(out, target, "(static_cast<std::uint64_t>(cpu_next)&~static_cast<std::uint64_t>(" + value(operands[2]) +
                        "))|(static_cast<std::uint64_t>(" + value(operands[1]) + ")&static_cast<std::uint64_t>(" + value(operands[2]) + "))");
                    out << "}\n";
                }
                else
                {
                    out << "auto &cpu_next=cpu_stage<" << cppType(stateType(target)) << ">(cpu_obj_,cpu_shadow_," << target.index << ','
                        << object(refs[0]).offset << ',' << range.offset << ',' << range.count << ','
                        << (projected_[target.index] ? "true" : "false") << ");\n";
                    if (stateType(target).kind == TypeKind::Logic && stateType(target).width > 64)
                        out << "grhsim_apply_masked_words_inplace(cpu_next," << value(operands[1]) << ','
                            << value(operands[2]) << ',' << stateType(target).width << ");}\n";
                    else
                        out << "cpu_next=" << normalize("(static_cast<std::uint64_t>(cpu_next)&~static_cast<std::uint64_t>(" + value(operands[2]) +
                            "))|(static_cast<std::uint64_t>(" + value(operands[1]) + ")&static_cast<std::uint64_t>(" + value(operands[2]) + "))", stateType(target)) << ";}\n";
                }
                if (edges)
                    for (std::size_t i = 0; i < edges->size(); ++i)
                        stage(out, {refs[i + 1].index, 0}, eventValue(operands[operands.size() - edges->size() + i]));
            }

            void header(std::ostream &out) const
            {
                out << "#pragma once\n#include \"" << prefix_ << "_runtime.hpp\"\n#include <memory>\n#include <stdexcept>\n";
                out << "template<class T> inline T &cpu_at(std::byte *data, std::size_t offset){return *reinterpret_cast<T*>(data+offset);}\n";
                // Zero test over a run of activity bytes; byte order is irrelevant for emptiness.
                out << "inline std::uint64_t cpu_word8(const std::uint8_t *data, std::size_t offset, std::size_t bytes){std::uint64_t value=0;std::memcpy(&value,data+offset,bytes);return value;}\n";
                // Tracked outputs are persistent initialized slots; local writes use the legacy helpers.
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
                out << "static_assert(sizeof(std::string*)==" << layout_.pointerBytes << ");\n"
                    << "template<> inline std::string &cpu_at<std::string>(std::byte *data,std::size_t offset){return *cpu_at<std::string*>(data,offset);}\n";
                out << "template<std::size_t N,std::size_t R> inline std::array<std::uint64_t,N> grhsim_concat_wide_scalar(const std::array<std::uint64_t,R>& lhs,std::size_t lhsWidth,std::uint64_t rhs,std::size_t rhsWidth,std::size_t totalWidth){std::array<std::uint64_t,N> out{};grhsim_insert_scalar_words(out,0,rhs,rhsWidth);grhsim_insert_words(out,rhsWidth,lhs,std::min(lhsWidth,totalWidth-rhsWidth));grhsim_trunc_words(out,totalWidth);return out;}\n";
                out << "template<std::size_t N> inline std::array<std::uint64_t,N> grhsim_concat_wide_scalar(std::uint64_t lhs,std::size_t lhsWidth,std::uint64_t rhs,std::size_t rhsWidth,std::size_t totalWidth){std::array<std::uint64_t,N> out{};grhsim_insert_scalar_words(out,0,rhs,rhsWidth);grhsim_insert_scalar_words(out,rhsWidth,lhs,std::min(lhsWidth,totalWidth-rhsWidth));grhsim_trunc_words(out,totalWidth);return out;}\n";
                out << "template<std::size_t N> inline std::array<std::uint64_t,N> grhsim_concat_scalar_scalar_wide(std::uint64_t lhs,std::size_t lhsWidth,std::uint64_t rhs,std::size_t rhsWidth,std::size_t totalWidth){std::array<std::uint64_t,N> out{};grhsim_insert_scalar_words(out,0,rhs,rhsWidth);grhsim_insert_scalar_words(out,rhsWidth,lhs,std::min(lhsWidth,totalWidth-rhsWidth));grhsim_trunc_words(out,totalWidth);return out;}\n";
                out << "template<std::size_t N,std::size_t R> inline std::array<std::uint64_t,N> grhsim_concat_scalar_wide(std::uint64_t lhs,std::size_t lhsWidth,const std::array<std::uint64_t,R>& rhs,std::size_t rhsWidth,std::size_t totalWidth){std::array<std::uint64_t,N> out{};grhsim_insert_words(out,0,rhs,std::min(rhsWidth,totalWidth));if(rhsWidth<totalWidth)grhsim_insert_scalar_words(out,rhsWidth,lhs,std::min(lhsWidth,totalWidth-rhsWidth));grhsim_trunc_words(out,totalWidth);return out;}\n";
                out << "class " << class_ << " {\npublic:\n";
                for (const auto &input : model_.inputs()) out << cppType(model_.types()[input.type.index - 1]) << ' ' << identifier(model_.text(input.name)) << "{};\n";
                for (const auto &output : model_.outputs()) out << cppType(model_.types()[output.type.index - 1]) << ' ' << identifier(model_.text(output.name)) << "{};\n";
                out << class_ << "(){cpu_bind_strings();}\n"
                    << "struct CpuRuntimeProfile{std::uint64_t evals=0,rounds=0,eval_ns=0,compute_ns=0,commit_ns=0,publish_ns=0;};\n"
                    << "void init();\nvoid eval();\n"
                    << "void set_runtime_profile_enabled(bool enabled){cpu_profile_enabled=enabled;if(enabled)cpu_profile_data={};}\n"
                    << "const CpuRuntimeProfile &cpu_runtime_profile() const{return cpu_profile_data;}\n"
                    << "void dump_runtime_profile() const;\nprivate:\n"
                    << "bool cpu_profile_enabled=false;CpuRuntimeProfile cpu_profile_data{};\n";
                if (hasSystemTasks_)
                    out << "bool cpu_first_eval=true;std::array<bool," << onceTasks_.size() << "> cpu_system_done{};\n"
                        << "std::vector<std::string> cpu_strobes;\nvoid cpu_system_task(std::string_view,std::span<const grhsim_task_arg>);\n";
                out << "std::unique_ptr<std::byte[]> cpu_objects{new std::byte[" << std::max<uint64_t>(layout_.objectBytes, 1) << "]{}};\n"
                    << "std::unique_ptr<std::byte[]> cpu_shadow{new std::byte[" << std::max<uint64_t>(layout_.objectBytes, 1) << "]{}};\n"
                    << "std::unique_ptr<std::byte[]> cpu_boundary{new std::byte[" << std::max<uint64_t>(layout_.boundaryBytes, 1) << "]{}};\n"
                    << "alignas(8) std::array<std::byte," << schedule_.inputShadowBytes << "> cpu_inputs{};\n"
                    << "std::unique_ptr<std::string[]> cpu_strings{new std::string[" << persistentStrings_.size() << "]};\n"
                    << "void cpu_bind_strings();\n"
                    << "std::uint64_t cpu_rng=UINT64_C(0x6a09e667f3bcc909);\n"
                    << "std::array<std::uint8_t," << layout_.runtimeBytes << "> cpu_flags{},cpu_next_arms{};\n"
                    << "std::array<std::uint8_t," << std::max<std::uint32_t>(portArmWordCount_, 1) << "> cpu_pflags{};\n";
                if (dynamicStats_)
                    out << "std::array<std::uint64_t," << dynKinds_.size() << "> cpu_dyn_wr{},cpu_dyn_ch{},cpu_dyn_silent{};\n"
                        << "std::array<std::uint64_t," << mapping_.partitionTree.partitions.size() + 1 << "> cpu_dyn_sn_act{},cpu_dyn_sn_body{},cpu_dyn_sn_grp{},cpu_dyn_sn_chg{};\n"
                        << "std::array<std::uint64_t," << dynTaskSpan_ << "> cpu_dyn_cm_ent{};\n"
                        << "std::uint64_t cpu_dyn_grp_pub=0,cpu_dyn_grp_fire=0,cpu_dyn_port_eval=0,cpu_dyn_port_fire=0;\n"
                        << "std::uint64_t cpu_dyn_mw_gate=0,cpu_dyn_mw_fire=0;\n"
                        << "std::uint64_t cpu_dyn_in_chk=0,cpu_dyn_in_chg=0,cpu_dyn_pub_calls=0,cpu_dyn_pub_pending=0,cpu_dyn_pub_changes=0;\n"
                        << "std::uint64_t cpu_dyn_cm_stable=0,cpu_dyn_cm_inactive=0;\n"
                        << "std::uint32_t cpu_dyn_edge=0,cpu_dyn_round_cur=0;\n"
                        << "std::uint64_t cpu_dyn_eval_pos=0,cpu_dyn_eval_neg=0,cpu_dyn_eval_other=0;\n"
                        << "std::uint64_t cpu_dyn_round_hist[8]={};\n"
                        << "std::uint64_t cpu_dyn_port_eval_pos=0,cpu_dyn_port_eval_neg=0,cpu_dyn_port_eval_r0=0,cpu_dyn_port_eval_rN=0;\n"
                        << "std::uint64_t cpu_dyn_port_fire_pos=0,cpu_dyn_port_fire_neg=0;\n"
                        << "std::uint64_t cpu_dyn_mw_gate_pos=0,cpu_dyn_mw_gate_neg=0,cpu_dyn_mw_fire_pos=0,cpu_dyn_mw_fire_neg=0;\n"
                        << "std::uint64_t cpu_dyn_cm_ent_pos=0,cpu_dyn_cm_ent_neg=0,cpu_dyn_cm_ent_r0=0,cpu_dyn_cm_ent_rN=0;\n"
                        << "std::uint64_t cpu_dyn_sn_act_pos=0,cpu_dyn_sn_act_neg=0;\n"
                        << "std::uint64_t cpu_dyn_pub_pending_r0=0,cpu_dyn_pub_pending_rN=0,cpu_dyn_pub_changes_r0=0,cpu_dyn_pub_changes_rN=0;\n"
                        << "std::uint64_t cpu_dyn_pub_pending_pos=0,cpu_dyn_pub_pending_neg=0,cpu_dyn_pub_changes_pos=0,cpu_dyn_pub_changes_neg=0;\n";
                out << "std::vector<std::uint8_t> cpu_dirty=std::vector<std::uint8_t>(" << dirtyBytes_ << ");\n"
                    << "struct Pending{std::size_t state,offset,size; std::uint32_t begin,count; bool projection;bool memory=false;};\n"
                    << "struct Target{std::uint32_t offset; std::uint8_t mask; bool arm;};\n"
                    << "static const std::array<Target," << stateTargets_.size() << "> cpu_targets;\nstd::vector<Pending> cpu_pending;\n"
                    << "static const std::array<Target," << memoryReaders_.size() << "> cpu_memory_readers;\n"
                    << "std::array<std::size_t," << memoryReaders_.size() << "> cpu_read_offsets{};\n"
                    << "bool cpu_direct_again=false;\nvoid cpu_direct_state_changed(std::uint32_t begin,std::uint32_t count,bool projection);\n"
                    << "void cpu_direct_state_changed_one(std::uint32_t offset,std::uint8_t mask,bool arm,bool projection){cpu_direct_again=cpu_direct_again||projection;if(arm)cpu_next_arms[offset]=1;else cpu_flags[offset]|=mask;}\n"
                    << "std::byte *cpu_stage_cell(std::byte *__restrict cpu_obj_,std::byte *__restrict cpu_shadow_,std::size_t key,std::size_t offset,std::size_t size,std::size_t row,std::uint32_t begin,std::uint32_t count,bool projection){\n"
                    << "key+=row;offset+=row*size;if(!cpu_dirty[key]){cpu_dirty[key]=1;std::memcpy(cpu_shadow_+offset,cpu_obj_+offset,size);cpu_pending.push_back({key,offset,size,begin,count,projection,true});}return cpu_shadow_+offset;}\n"
                    << "template<class T,unsigned Width> void cpu_write_cell(std::byte *__restrict cpu_obj_,std::byte *__restrict cpu_shadow_,std::size_t key,std::size_t offset,std::size_t row,std::uint32_t begin,std::uint32_t count,bool projection,std::uint64_t data,std::uint64_t mask){\n"
                    << "key+=row;offset+=row*sizeof(T);const T current=cpu_at<T>(cpu_dirty[key]?cpu_shadow_:cpu_obj_,offset);\n"
                    << "const auto merged=(static_cast<std::uint64_t>(current)&~mask)|(data&mask);\n"
                    << "const T next=static_cast<T>(std::is_signed_v<T>?grhsim_sign_extend_i64(merged,Width):grhsim_trunc_u64(merged,Width));if(current==next)return;\n"
                    << "if(!cpu_dirty[key]){cpu_pending.push_back({key,offset,sizeof(T),begin,count,projection,true});cpu_dirty[key]=1;}cpu_at<T>(cpu_shadow_,offset)=next;}\n"
                    << "template<class T> T &cpu_stage(std::byte *__restrict cpu_obj_,std::byte *__restrict cpu_shadow_,std::uint32_t state,std::size_t offset,std::uint32_t begin,std::uint32_t count,bool projection){\n"
                    << "if(!cpu_dirty[state]){cpu_dirty[state]=1;std::memcpy(cpu_shadow_+offset,cpu_obj_+offset,sizeof(T));cpu_pending.push_back({state,offset,sizeof(T),begin,count,projection});}\n"
                    << "return cpu_at<T>(cpu_shadow_,offset);}\n"
                    << "template<class T> void cpu_write_scalar(std::byte *__restrict cpu_obj_,std::byte *__restrict cpu_shadow_,std::uint32_t state,std::size_t offset,std::uint32_t begin,std::uint32_t count,bool projection,T next){\n"
                    << "const T current=cpu_at<T>(cpu_dirty[state]?cpu_shadow_:cpu_obj_,offset);if(current==next)return;\n"
                    << "if(!cpu_dirty[state]){cpu_pending.push_back({state,offset,sizeof(T),begin,count,projection});cpu_dirty[state]=1;}\n"
                    << "cpu_at<T>(cpu_shadow_,offset)=next;}\n"
                    << "std::byte *cpu_stage_bytes(std::byte *__restrict cpu_obj_,std::byte *__restrict cpu_shadow_,std::uint32_t state,std::size_t offset,std::size_t size,std::uint32_t begin,std::uint32_t count,bool projection){\n"
                    << "if(!cpu_dirty[state]){cpu_dirty[state]=1;std::memcpy(cpu_shadow_+offset,cpu_obj_+offset,size);cpu_pending.push_back({state,offset,size,begin,count,projection});}\n"
                    << "return cpu_shadow_+offset;}\n"
                    << "std::byte *cpu_stage_bytes_overwrite(std::byte *__restrict cpu_obj_,std::byte *__restrict cpu_shadow_,std::uint32_t state,std::size_t offset,std::size_t size,std::uint32_t begin,std::uint32_t count,bool projection){\n"
                    << "if(!cpu_dirty[state]){cpu_dirty[state]=1;cpu_pending.push_back({state,offset,size,begin,count,projection});}\n"
                    << "return cpu_shadow_+offset;}\nbool cpu_publish();\n";
                for (std::size_t i = 0; i < initChunkCount(); ++i) out << "void cpu_init_" << i << "();\n";
                for (const auto &task : schedule_.numaNodes[0].cores[0].tasks) out << "void cpu_task_" << task.id.index << "();\n";
                for (const auto &partition : mapping_.partitionTree.partitions)
                {
                    if (partition.attrs.helperChunks.empty()) continue;
                    std::vector<OpId> ops;
                    for (auto node : partition.children)
                        ops.insert(ops.end(), mapping_.partitionTree.partitions[node.index - 1].ops.begin(),
                            mapping_.partitionTree.partitions[node.index - 1].ops.end());
                    for (std::size_t i = 0; i < partition.attrs.helperChunks.size(); ++i)
                    {
                        const auto range = partition.attrs.helperChunks[i];
                        out << "void cpu_helper_" << partition.id.index << '_' << i << "(std::byte *cpu_local,std::uint8_t &cpu_active_word";
                        for (const auto &param : computeGuardParams(partition.id, std::span<const OpId>(ops).subspan(range.offset, range.count)))
                            out << ",bool " << param;
                        out << ");\n";
                    }
                }
                out << "};\n";
            }

            void initBody(std::ostream &out, std::size_t chunk, std::size_t begin) const
            {
                out << "#include \"" << prefix_ << ".hpp\"\nvoid " << class_ << "::cpu_init_" << chunk << "(){\n";
                constexpr std::size_t chunkSteps = 4096;
                std::size_t index = 0;
                for (const auto &record : model_.initRecords())
                    for (const auto &step : model_.steps(record))
                    {
                        if (index >= begin + chunkSteps) { out << "}\n"; return; }
                        if (index >= begin)
                        {
                            initStep(out, record.state, step);
                        }
                        ++index;
                    }
                out << "}\n";
            }

            std::size_t initChunkCount() const
            {
                constexpr std::size_t chunkSteps = 4096;
                std::size_t steps = 0;
                for (const auto &record : model_.initRecords()) steps += model_.steps(record).size();
                return (steps + chunkSteps - 1) / chunkSteps;
            }

            void driver(std::ostream &out) const
            {
                out << "#include \"" << prefix_ << ".hpp\"\n#include <chrono>\n#include <cstdio>\nconst std::array<" << class_ << "::Target," << stateTargets_.size() << "> " << class_ << "::cpu_targets{{\n";
                for (auto target : stateTargets_) out << '{' << target.offset << ',' << target.mask << ',' << (target.arm ? "true" : "false") << "},\n";
                out << "}};\nconst std::array<" << class_ << "::Target," << memoryReaders_.size() << "> " << class_ << "::cpu_memory_readers{{\n";
                for (auto target : memoryReaders_) out << '{' << target.offset << ',' << target.mask << ",false},\n";
                out << "}};\nvoid " << class_ << "::cpu_bind_strings(){\n";
                for (std::size_t i = 0; i < persistentStrings_.size(); ++i)
                    out << "cpu_at<std::string*>(" << persistentStrings_[i].first << ',' << persistentStrings_[i].second
                        << ")=&cpu_strings[" << i << "];\n";
                out << "}\nvoid " << class_ << "::init(){\nstd::memset(cpu_objects.get(),0," << layout_.objectBytes << ");\nstd::memset(cpu_boundary.get(),0," << layout_.boundaryBytes << ");\n"
                    << "cpu_inputs.fill(std::byte{});cpu_flags.fill(0);cpu_next_arms.fill(0);cpu_pflags.fill(255);std::fill(cpu_dirty.begin(),cpu_dirty.end(),0);cpu_pending.clear();cpu_direct_again=false;cpu_read_offsets.fill(0);\n";
                out << "for(std::size_t i=0;i<" << persistentStrings_.size() << ";++i)cpu_strings[i].clear();\ncpu_bind_strings();\n";
                out << "cpu_profile_data={};\n";
                if (dynamicStats_)
                    out << "cpu_dyn_wr.fill(0);cpu_dyn_ch.fill(0);cpu_dyn_silent.fill(0);cpu_dyn_sn_act.fill(0);cpu_dyn_sn_body.fill(0);cpu_dyn_sn_grp.fill(0);cpu_dyn_sn_chg.fill(0);cpu_dyn_cm_ent.fill(0);\n"
                        << "cpu_dyn_grp_pub=0;cpu_dyn_grp_fire=0;cpu_dyn_port_eval=0;cpu_dyn_port_fire=0;cpu_dyn_in_chk=0;cpu_dyn_in_chg=0;\n"
                        << "cpu_dyn_pub_calls=0;cpu_dyn_pub_pending=0;cpu_dyn_pub_changes=0;cpu_dyn_cm_stable=0;cpu_dyn_cm_inactive=0;\n"
                        << "cpu_dyn_edge=0;cpu_dyn_round_cur=0;cpu_dyn_eval_pos=0;cpu_dyn_eval_neg=0;cpu_dyn_eval_other=0;\n"
                        << "std::memset(cpu_dyn_round_hist,0,sizeof(cpu_dyn_round_hist));\n"
                        << "cpu_dyn_port_eval_pos=0;cpu_dyn_port_eval_neg=0;cpu_dyn_port_eval_r0=0;cpu_dyn_port_eval_rN=0;\n"
                        << "cpu_dyn_port_fire_pos=0;cpu_dyn_port_fire_neg=0;\n"
                        << "cpu_dyn_mw_gate_pos=0;cpu_dyn_mw_gate_neg=0;cpu_dyn_mw_fire_pos=0;cpu_dyn_mw_fire_neg=0;\n"
                        << "cpu_dyn_cm_ent_pos=0;cpu_dyn_cm_ent_neg=0;cpu_dyn_cm_ent_r0=0;cpu_dyn_cm_ent_rN=0;\n"
                        << "cpu_dyn_sn_act_pos=0;cpu_dyn_sn_act_neg=0;\n"
                        << "cpu_dyn_pub_pending_r0=0;cpu_dyn_pub_pending_rN=0;cpu_dyn_pub_changes_r0=0;cpu_dyn_pub_changes_rN=0;\n"
                        << "cpu_dyn_pub_pending_pos=0;cpu_dyn_pub_pending_neg=0;cpu_dyn_pub_changes_pos=0;cpu_dyn_pub_changes_neg=0;\n";
                out << "cpu_rng=UINT64_C(0x6a09e667f3bcc909);\n";
                if (hasSystemTasks_) out << "cpu_first_eval=true;cpu_system_done.fill(false);cpu_strobes.clear();\n";
                for (std::size_t i = 0; i < initChunkCount(); ++i) out << "cpu_init_" << i << "();\n";
                for (const auto &slot : layout_.runtime)
                    if (slot.kind != CpuRuntimeKind::EventEdge) out << "cpu_flags[" << slot.offset << "]=" << (slot.kind == CpuRuntimeKind::ActiveWord ? 255 : 1) << ";\n";
                out << "}\nvoid " << class_ << "::cpu_direct_state_changed(std::uint32_t begin,std::uint32_t count,bool projection){\n"
                    << "cpu_direct_again=cpu_direct_again||projection;\n"
                    << "for(std::uint32_t i=begin;i<begin+count;++i){const auto &t=cpu_targets[i];if(t.arm)cpu_next_arms[t.offset]=1;else cpu_flags[t.offset]|=t.mask;}}\n";
                out << "bool " << class_ << "::cpu_publish(){";
                if (dynamicStats_) out << "++cpu_dyn_pub_calls;cpu_dyn_pub_pending+=cpu_pending.size();if(cpu_dyn_round_cur==0)cpu_dyn_pub_pending_r0+=cpu_pending.size();else cpu_dyn_pub_pending_rN+=cpu_pending.size();if(cpu_dyn_edge==1)cpu_dyn_pub_pending_pos+=cpu_pending.size();else if(cpu_dyn_edge==2)cpu_dyn_pub_pending_neg+=cpu_pending.size();";
                out << "bool again=cpu_direct_again;cpu_direct_again=false;for(const auto &p:cpu_pending){\n"
                    << "if(std::memcmp(cpu_objects.get()+p.offset,cpu_shadow.get()+p.offset,p.size)!=0){";
                if (dynamicStats_) out << "++cpu_dyn_pub_changes;if(cpu_dyn_round_cur==0)++cpu_dyn_pub_changes_r0;else ++cpu_dyn_pub_changes_rN;if(cpu_dyn_edge==1)++cpu_dyn_pub_changes_pos;else if(cpu_dyn_edge==2)++cpu_dyn_pub_changes_neg;";
                out << "std::memcpy(cpu_objects.get()+p.offset,cpu_shadow.get()+p.offset,p.size);\n"
                    << "again=again||p.projection;for(std::uint32_t i=p.begin;i<p.begin+p.count;++i){if(p.memory){if(cpu_read_offsets[i]==p.offset){const auto &t=cpu_memory_readers[i];cpu_flags[t.offset]|=t.mask;}}else{const auto &t=cpu_targets[i];if(t.arm)cpu_next_arms[t.offset]=1;else cpu_flags[t.offset]|=t.mask;}}}cpu_dirty[p.state]=0;}cpu_pending.clear();return again;}\n";
                out << "void " << class_ << "::eval(){\n";
                out << "using cpu_profile_clock=std::chrono::steady_clock;\n"
                    << "const bool cpu_profile=cpu_profile_enabled;\n"
                    << "const auto cpu_profile_eval_begin=cpu_profile?cpu_profile_clock::now():cpu_profile_clock::time_point{};\n"
                    << "auto cpu_profile_phase_begin=cpu_profile_eval_begin;\n"
                    << "const auto cpu_profile_tick=[&](std::uint64_t &counter){if(cpu_profile){const auto now=cpu_profile_clock::now();\n"
                    << "counter+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now-cpu_profile_phase_begin).count());cpu_profile_phase_begin=now;}};\n";
                for (const auto &input : model_.inputs()) out << at(model_.types()[input.type.index - 1], "cpu_objects.get()", object(ObjectRef::input(input.id)).offset)
                    << '=' << normalize("this->" + identifier(model_.text(input.name)), model_.types()[input.type.index - 1]) << ";\n";
                std::vector<InputId> inputByValue(model_.values().size() + 1);
                for (const auto &op : model_.operations()) if (model_.text(op.opType) == "core.input.read")
                    inputByValue[model_.results(op)[0].index] = {model_.objectRefs(op)[0].index, 0};
                std::uint32_t clockDynValue = 0;
                if (dynamicStats_)
                    for (const auto &op : model_.operations())
                        if (model_.text(op.opType) == "core.input.read" && type(model_.results(op)[0]).width == 1)
                        {
                            const std::uint32_t inputIndex = model_.objectRefs(op)[0].index;
                            const auto found = std::find_if(model_.inputs().begin(), model_.inputs().end(),
                                                            [&](const auto &io) { return io.id.index == inputIndex; });
                            if (found != model_.inputs().end() && identifier(model_.text(found->name)) == "clock")
                            { clockDynValue = model_.results(op)[0].index; break; }
                        }
                if (dynamicStats_) out << "cpu_dyn_edge=3;\n";
                for (std::size_t i = 0; i < schedule_.inputFanout.size(); ++i)
                {
                    const auto &row = schedule_.inputFanout[i]; const auto &shadow = schedule_.inputShadows[i];
                    const auto current = at(type(row.source), "cpu_objects.get()", object(ObjectRef::input(inputByValue[row.source.index])).offset);
                    const auto previous = at(type(row.source), "cpu_inputs.data()", shadow.offset);
                    if (dynamicStats_) out << "++cpu_dyn_in_chk;";
                    out << "if(" << previous << "!=" << current << "){";
                    if (dynamicStats_) out << "++cpu_dyn_in_chg;";
                    out << previous << '=' << current << ";\n";
                    if (dynamicStats_ && row.source.index == clockDynValue)
                        out << "cpu_dyn_edge=((" << current << ")!=0)?1u:2u;\n";
                    activate(out, row.targets, false); out << "}\n";
                }
                if (dynamicStats_)
                    out << "if(cpu_dyn_edge==1)++cpu_dyn_eval_pos;else if(cpu_dyn_edge==2)++cpu_dyn_eval_neg;else ++cpu_dyn_eval_other;\n";
                out << "for(std::uint32_t cpu_round=0;cpu_round<100000;++cpu_round){\n";
                if (dynamicStats_) out << "cpu_dyn_round_cur=cpu_round;\n";
                CpuActivationTargets seeds{schedule_.roundSeeds, {}}; activate(out, seeds, false);
                out << "if(cpu_profile){++cpu_profile_data.rounds;cpu_profile_phase_begin=cpu_profile_clock::now();}\n";
                // Word-packed dispatch: adjacent single-byte task checks sharing one
                // 8-byte flags bucket get a uint64 emptiness prefilter; a zero word
                // proves every covered check false, so task order and semantics are
                // unchanged. Other tasks (multi-word, unconditional) emit as before.
                struct DispatchEntry
                {
                    std::string text;
                    std::uint32_t offset = ~std::uint32_t(0);
                    bool compute = false;
                };
                std::vector<DispatchEntry> dispatch;
                dispatch.reserve(schedule_.numaNodes[0].cores[0].tasks.size());
                for (const auto &task : schedule_.numaNodes[0].cores[0].tasks)
                {
                    DispatchEntry entry;
                    entry.compute = task.execution == CpuExecution::ActivityDrivenCompute;
                    std::string condition;
                    if (entry.compute)
                    {
                        bool first = true;
                        for (const auto word : mapping_.partitionTree.partitions[task.partition.index - 1].children)
                        {
                            if (!first) condition += "||";
                            const auto offset = wordOffsets_[word.index];
                            condition += "cpu_flags[" + std::to_string(offset) + "]";
                            if (first) entry.offset = offset;
                            else entry.offset = ~std::uint32_t(0);
                            first = false;
                        }
                    }
                    else if (task.execution == CpuExecution::DomainGatedCommit)
                    {
                        entry.offset = armOffsets_[mapping_.partitionTree.partitions[task.partition.index - 1].parent.index];
                        condition = "cpu_flags[" + std::to_string(entry.offset) + "]";
                    }
                    if (!condition.empty()) entry.text = "if(" + condition + ")";
                    entry.text += "cpu_task_" + std::to_string(task.id.index) + "();\n";
                    dispatch.push_back(std::move(entry));
                }
                std::optional<bool> profileCompute;
                for (std::size_t cursor = 0; cursor < dispatch.size();)
                {
                    const bool computePhase = dispatch[cursor].compute;
                    if (profileCompute && *profileCompute != computePhase)
                        out << "cpu_profile_tick(cpu_profile_data." << (*profileCompute ? "compute_ns" : "commit_ns") << ");\n";
                    profileCompute = computePhase;
                    std::size_t end = cursor + 1;
                    if (dispatch[cursor].offset != ~std::uint32_t(0))
                    {
                        const std::uint32_t bucket = dispatch[cursor].offset / 8;
                        while (end < dispatch.size() && dispatch[end].offset != ~std::uint32_t(0) &&
                               dispatch[end].compute == computePhase && dispatch[end].offset / 8 == bucket)
                            ++end;
                        if (end - cursor >= 2)
                        {
                            const std::uint32_t base = bucket * 8;
                            out << "if(cpu_word8(cpu_flags.data()," << base << ','
                                << std::min<std::uint32_t>(8, layout_.runtimeBytes - base) << ")){";
                            for (std::size_t index = cursor; index < end; ++index) out << dispatch[index].text;
                            out << "}\n";
                            dispatchPackedBytes_ += end - cursor;
                            cursor = end;
                            continue;
                        }
                    }
                    for (std::size_t index = cursor; index < end; ++index) out << dispatch[index].text;
                    cursor = end;
                }
                if (profileCompute)
                    out << "cpu_profile_tick(cpu_profile_data." << (*profileCompute ? "compute_ns" : "commit_ns") << ");\n";
                out << "const bool cpu_again=cpu_publish();\ncpu_profile_tick(cpu_profile_data.publish_ns);\n";
                // A domain slot needs the copy/clear only when its next-arm or current
                // flag byte is non-zero; bucket prefilters keep the per-slot semantics.
                std::vector<std::uint32_t> domainSlots;
                for (const auto &slot : layout_.runtime) if (slot.kind == CpuRuntimeKind::DomainArm) domainSlots.push_back(slot.offset);
                std::sort(domainSlots.begin(), domainSlots.end());
                for (std::size_t cursor = 0; cursor < domainSlots.size();)
                {
                    const std::uint32_t bucket = domainSlots[cursor] / 8;
                    std::size_t end = cursor + 1;
                    while (end < domainSlots.size() && domainSlots[end] / 8 == bucket) ++end;
                    if (end - cursor >= 2)
                    {
                        const std::uint32_t base = bucket * 8;
                        const auto bytes = std::min<std::uint32_t>(8, layout_.runtimeBytes - base);
                        out << "if((cpu_word8(cpu_next_arms.data()," << base << ',' << bytes
                            << ")|cpu_word8(cpu_flags.data()," << base << ',' << bytes << "))!=0){";
                        for (std::size_t index = cursor; index < end; ++index)
                            out << "cpu_flags[" << domainSlots[index] << "]=cpu_next_arms[" << domainSlots[index] << "];cpu_next_arms[" << domainSlots[index] << "]=0;\n";
                        out << "}\n";
                        handoffPackedSlots_ += end - cursor;
                    }
                    else
                        for (std::size_t index = cursor; index < end; ++index)
                            out << "cpu_flags[" << domainSlots[index] << "]=cpu_next_arms[" << domainSlots[index] << "];cpu_next_arms[" << domainSlots[index] << "]=0;\n";
                    cursor = end;
                }
                out << "if(!cpu_again){\n";
                if (dynamicStats_) out << "++cpu_dyn_round_hist[cpu_round<8?cpu_round:7];\n";
                if (hasSystemTasks_)
                    out << "cpu_first_eval=false;for(const auto &text:cpu_strobes)std::cout<<text<<'\\n';cpu_strobes.clear();\n";
                for (const auto &output : model_.outputs()) out << "this->" << identifier(model_.text(output.name)) << '=' <<
                    at(model_.types()[output.type.index - 1], "cpu_objects.get()", object(ObjectRef::output(output.id)).offset) << ";\n";
                out << "if(cpu_profile){++cpu_profile_data.evals;cpu_profile_data.eval_ns+=static_cast<std::uint64_t>(\n"
                    << "std::chrono::duration_cast<std::chrono::nanoseconds>(cpu_profile_clock::now()-cpu_profile_eval_begin).count());}\n"
                    << "return;}}throw std::runtime_error(\"CPU model did not converge\");}\n";
                out << "void " << class_ << "::dump_runtime_profile() const{if(!cpu_profile_data.evals)return;const auto &p=cpu_profile_data;\n"
                    << "std::fprintf(stderr,\"[grhsim-cpu-phase] evals=%llu rounds=%llu eval_ns=%llu compute_ns=%llu commit_ns=%llu publish_ns=%llu\\n\",\n"
                    << "static_cast<unsigned long long>(p.evals),static_cast<unsigned long long>(p.rounds),\n"
                    << "static_cast<unsigned long long>(p.eval_ns),static_cast<unsigned long long>(p.compute_ns),\n"
                    << "static_cast<unsigned long long>(p.commit_ns),static_cast<unsigned long long>(p.publish_ns));";
                if (dynamicStats_)
                {
                    std::vector<std::string> names(dynKinds_.size());
                    for (const auto &[name, index] : dynKinds_) names[index] = name;
                    out << "\nstatic const char *cpu_dyn_names[" << names.size() << "]={";
                    for (std::size_t i = 0; i < names.size(); ++i) out << (i ? ",\"" : "\"") << names[i] << "\"";
                    out << "};\n"
                        << "for(std::size_t i=0;i<" << names.size() << ";++i)if(cpu_dyn_wr[i]||cpu_dyn_ch[i]||cpu_dyn_silent[i])"
                        << "std::fprintf(stderr,\"[grhsim-dyn] kind %s wr=%llu ch=%llu silent=%llu\\n\",cpu_dyn_names[i],"
                        << "static_cast<unsigned long long>(cpu_dyn_wr[i]),static_cast<unsigned long long>(cpu_dyn_ch[i]),static_cast<unsigned long long>(cpu_dyn_silent[i]));\n"
                        << "for(std::size_t i=0;i<" << mapping_.partitionTree.partitions.size() + 1 << ";++i)if(cpu_dyn_sn_act[i])"
                        << "std::fprintf(stderr,\"[grhsim-dyn] sn %zu act=%llu body=%llu grp=%llu chg=%llu\\n\",i,"
                        << "static_cast<unsigned long long>(cpu_dyn_sn_act[i]),static_cast<unsigned long long>(cpu_dyn_sn_body[i]),"
                        << "static_cast<unsigned long long>(cpu_dyn_sn_grp[i]),static_cast<unsigned long long>(cpu_dyn_sn_chg[i]));\n"
                        << "for(std::size_t i=0;i<" << dynTaskSpan_ << ";++i)if(cpu_dyn_cm_ent[i])"
                        << "std::fprintf(stderr,\"[grhsim-dyn] commit %zu ent=%llu\\n\",i,static_cast<unsigned long long>(cpu_dyn_cm_ent[i]));\n"
                        << "std::fprintf(stderr,\"[grhsim-dyn] totals grp_pub=%llu grp_fire=%llu port_eval=%llu port_fire=%llu "
                        << "in_chk=%llu in_chg=%llu pub_calls=%llu pub_pending=%llu pub_changes=%llu cm_stable=%llu cm_inactive=%llu mw_gate=%llu mw_fire=%llu\\n\",\n"
                        << "static_cast<unsigned long long>(cpu_dyn_grp_pub),static_cast<unsigned long long>(cpu_dyn_grp_fire),\n"
                        << "static_cast<unsigned long long>(cpu_dyn_port_eval),static_cast<unsigned long long>(cpu_dyn_port_fire),\n"
                        << "static_cast<unsigned long long>(cpu_dyn_in_chk),static_cast<unsigned long long>(cpu_dyn_in_chg),\n"
                        << "static_cast<unsigned long long>(cpu_dyn_pub_calls),static_cast<unsigned long long>(cpu_dyn_pub_pending),\n"
                        << "static_cast<unsigned long long>(cpu_dyn_pub_changes),static_cast<unsigned long long>(cpu_dyn_cm_stable),\n"
                        << "static_cast<unsigned long long>(cpu_dyn_cm_inactive),\n"
                        << "static_cast<unsigned long long>(cpu_dyn_mw_gate),static_cast<unsigned long long>(cpu_dyn_mw_fire));\n"
                        << "std::fprintf(stderr,\"[grhsim-dyn] edge evals pos=%llu neg=%llu other=%llu\\n\","
                        << "static_cast<unsigned long long>(cpu_dyn_eval_pos),static_cast<unsigned long long>(cpu_dyn_eval_neg),"
                        << "static_cast<unsigned long long>(cpu_dyn_eval_other));\n"
                        << "std::fprintf(stderr,\"[grhsim-dyn] round_hist\");for(const auto h:cpu_dyn_round_hist)"
                        << "std::fprintf(stderr,\" %llu\",static_cast<unsigned long long>(h));std::fprintf(stderr,\"\\n\");\n"
                        << "std::fprintf(stderr,\"[grhsim-dyn] edge port_eval pos=%llu neg=%llu fire_pos=%llu fire_neg=%llu eval_r0=%llu eval_rN=%llu\\n\","
                        << "static_cast<unsigned long long>(cpu_dyn_port_eval_pos),static_cast<unsigned long long>(cpu_dyn_port_eval_neg),"
                        << "static_cast<unsigned long long>(cpu_dyn_port_fire_pos),static_cast<unsigned long long>(cpu_dyn_port_fire_neg),"
                        << "static_cast<unsigned long long>(cpu_dyn_port_eval_r0),static_cast<unsigned long long>(cpu_dyn_port_eval_rN));\n"
                        << "std::fprintf(stderr,\"[grhsim-dyn] edge mw_gate pos=%llu neg=%llu fire_pos=%llu fire_neg=%llu\\n\","
                        << "static_cast<unsigned long long>(cpu_dyn_mw_gate_pos),static_cast<unsigned long long>(cpu_dyn_mw_gate_neg),"
                        << "static_cast<unsigned long long>(cpu_dyn_mw_fire_pos),static_cast<unsigned long long>(cpu_dyn_mw_fire_neg));\n"
                        << "std::fprintf(stderr,\"[grhsim-dyn] edge cm_ent pos=%llu neg=%llu r0=%llu rN=%llu sn_act pos=%llu neg=%llu\\n\","
                        << "static_cast<unsigned long long>(cpu_dyn_cm_ent_pos),static_cast<unsigned long long>(cpu_dyn_cm_ent_neg),"
                        << "static_cast<unsigned long long>(cpu_dyn_cm_ent_r0),static_cast<unsigned long long>(cpu_dyn_cm_ent_rN),"
                        << "static_cast<unsigned long long>(cpu_dyn_sn_act_pos),static_cast<unsigned long long>(cpu_dyn_sn_act_neg));\n"
                        << "std::fprintf(stderr,\"[grhsim-dyn] round pub_pending r0=%llu rN=%llu changes r0=%llu rN=%llu\\n\","
                        << "static_cast<unsigned long long>(cpu_dyn_pub_pending_r0),static_cast<unsigned long long>(cpu_dyn_pub_pending_rN),"
                        << "static_cast<unsigned long long>(cpu_dyn_pub_changes_r0),static_cast<unsigned long long>(cpu_dyn_pub_changes_rN));\n"
                        << "std::fprintf(stderr,\"[grhsim-dyn] edge pub_pending pos=%llu neg=%llu changes pos=%llu neg=%llu\\n\","
                        << "static_cast<unsigned long long>(cpu_dyn_pub_pending_pos),static_cast<unsigned long long>(cpu_dyn_pub_pending_neg),"
                        << "static_cast<unsigned long long>(cpu_dyn_pub_changes_pos),static_cast<unsigned long long>(cpu_dyn_pub_changes_neg));\n";
                }
                out << "}\n";
                if (hasSystemTasks_) systemTaskDriver(out);
            }

            void systemTaskDriver(std::ostream &out) const
            {
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
if(name=="strobe"){cpu_strobes.push_back(std::move(text));return;}
if(name=="info"||name=="warning"||name=="error"||name=="fatal")text="["+std::string(name)+"] "+text;
if(!terminal||!text.empty()){
    *stream<<text;
    if(name!="write"&&name!="fwrite")*stream<<'\n';
}
if(terminal){
    for(const auto &pending:cpu_strobes)std::cout<<pending<<'\n';
    cpu_strobes.clear();std::cout.flush();std::cerr.flush();std::exit(exitCode);
}
}
)CPP";
            }

            std::string stableCommitHistories(const CpuScheduledTask &task) const
            {
                if (task.execution != CpuExecution::DomainGatedCommit) return {};
                struct Group { ValueId value; std::vector<uint64_t> offsets; };
                std::vector<Group> groups;
                std::size_t count = 0;
                const auto &tree = mapping_.partitionTree;
                for (auto unit : tree.partitions[task.partition.index - 1].children)
                    for (auto id : tree.partitions[unit.index - 1].ops)
                    {
                        const auto &op = model_.operations()[id.index - 1];
                        const auto name = model_.text(op.opType);
                        if (name != "core.state.regWrite" && name != "core.state.memWrite" &&
                            name != "core.state.memFill" && name != "core.state.memWriteSeq") return {};
                        const auto *edges = parameter<std::vector<std::string>>(model_, model_.parameters(op), "event_edges");
                        if (!edges || edges->empty()) return {};
                        const auto events = model_.operands(op).last(edges->size());
                        const auto histories = model_.objectRefs(op).last(edges->size());
                        for (std::size_t i = 0; i < edges->size(); ++i)
                        {
                            if (((*edges)[i] != "posedge" && (*edges)[i] != "negedge") ||
                                !privateByteHistories_[histories[i].index]) return {};
                            auto group = std::find_if(groups.begin(), groups.end(), [&](const auto &g) { return g.value == events[i]; });
                            if (group == groups.end())
                            {
                                if (groups.size() == 8) return {};
                                groups.push_back({events[i], {}}); group = groups.end() - 1;
                            }
                            group->offsets.push_back(object(histories[i]).offset); ++count;
                        }
                    }
                if (count < 16) return {};
                std::ostringstream out;
                out << "/* cpu_stable_history_scan histories=" << count << " groups=" << groups.size() << " */ ([&](){";
                for (auto &group : groups)
                {
                    auto &offsets = group.offsets;
                    std::sort(offsets.begin(), offsets.end());
                    offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
                    // Every byte is a distinct private history; no task can have staged it earlier.
                    if (offsets.back() - offsets.front() + 1 == offsets.size())
                        out << "if(std::memchr(" << arenaObjects() << "+" << offsets.front() << ",!bool(" << eventValue(group.value) << "),"
                            << offsets.size() << "))return false;";
                    else
                    {
                        out << "{const auto cpu_current=std::byte{static_cast<unsigned char>(bool(" << eventValue(group.value)
                            << "))};static constexpr std::size_t cpu_histories[]={";
                        for (std::size_t i = 0; i < offsets.size(); ++i) out << (i ? "," : "") << offsets[i];
                        out << "};for(auto cpu_offset:cpu_histories)if(" << arenaObjects() << "[cpu_offset]!=cpu_current)return false;}";
                    }
                }
                out << "return true;}())";
                return out.str();
            }

            std::string historyEdgePossibility(std::vector<uint64_t> offsets, unsigned previous) const
            {
                std::sort(offsets.begin(), offsets.end());
                offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
                if (offsets.size() < 16) return {};
                std::vector<std::pair<uint64_t, uint64_t>> ranges;
                for (auto offset : offsets)
                    if (!ranges.empty() && offset == ranges.back().first + ranges.back().second) ++ranges.back().second;
                    else ranges.emplace_back(offset, 1);
                if (ranges.size() * 4 > offsets.size()) return {};
                std::ostringstream out;
                out << "/* cpu_history_edge_scan histories=" << offsets.size() << " ranges=" << ranges.size() << " */ ";
                if (ranges.size() == 1)
                    out << "std::memchr(" << arenaObjects() << "+" << ranges[0].first << ',' << previous << ',' << ranges[0].second << ")!=nullptr";
                else
                {
                    out << "([&](){static constexpr std::size_t cpu_history_ranges[][2]={";
                    for (std::size_t i = 0; i < ranges.size(); ++i)
                        out << (i ? "," : "") << '{' << ranges[i].first << ',' << ranges[i].second << '}';
                    out << "};for(const auto &cpu_range:cpu_history_ranges)"
                        << "if(std::memchr(" << arenaObjects() << "+cpu_range[0]," << previous << ",cpu_range[1]))return true;return false;}())";
                }
                return out.str();
            }

            std::string commitEdgePossibility(const CpuScheduledTask &task) const
            {
                if (task.execution != CpuExecution::DomainGatedCommit) return {};
                const auto &tree = mapping_.partitionTree;
                struct Term { CpuEvent event; bool byteHistories = true; std::vector<uint64_t> offsets; };
                std::vector<Term> terms;
                for (auto unit : tree.partitions[task.partition.index - 1].children)
                    for (auto id : tree.partitions[unit.index - 1].ops)
                    {
                        const auto &op = model_.operations()[id.index - 1];
                        const auto name = model_.text(op.opType);
                        if (name != "core.state.regWrite" && name != "core.state.memWrite" &&
                            name != "core.state.memFill" && name != "core.state.memWriteSeq") return {};
                        const auto *edges = parameter<std::vector<std::string>>(model_, model_.parameters(op), "event_edges");
                        if (!edges || edges->empty()) return {};
                        const auto events = model_.operands(op).last(edges->size());
                        const auto histories = model_.objectRefs(op).last(edges->size());
                        for (std::size_t i = 0; i < edges->size(); ++i)
                        {
                            if ((*edges)[i] != "posedge" && (*edges)[i] != "negedge") return {};
                            const CpuEvent event{events[i], (*edges)[i] == "posedge" ? CpuEventEdge::Posedge : CpuEventEdge::Negedge};
                            const auto index = static_cast<std::size_t>(std::find_if(terms.begin(), terms.end(),
                                [&](const auto &term) { return term.event == event; }) - terms.begin());
                            if (index == terms.size()) terms.push_back({event});
                            if (terms.size() > 8) return {};
                            auto &term = terms[index];
                            const auto &historyType = stateType({histories[i].index, histories[i].generation});
                            const auto &slot = object(histories[i]);
                            term.byteHistories &= historyType.kind == TypeKind::Logic && historyType.width == 1 &&
                                !historyType.isSigned && historyType.domain == LogicDomain::TwoState && layout_.types[slot.type.index - 1].size == 1;
                            term.offsets.push_back(slot.offset);
                        }
                    }
                if (terms.empty()) return {};
                std::string result = "false";
                for (auto &term : terms)
                {
                    const bool negative = term.event.edge == CpuEventEdge::Negedge;
                    const auto level = std::string(negative ? "!" : "") + eventValue(term.event.value);
                    // Inspect every current history, never a representative or a pending shadow.
                    const auto history = term.byteHistories ? historyEdgePossibility(std::move(term.offsets), negative ? 1 : 0) : std::string{};
                    result += " || " + (history.empty() ? level : "(" + level + " && (" + history + "))");
                }
                return result;
            }

            std::map<uint32_t, std::string> commitEdgeSnapshots(std::ostream &out, const CpuScheduledTask &task) const
            {
                std::map<uint32_t, std::string> guards;
                if (!sharedHistoryTaskIds_.contains(task.id.index)) return guards;
                using Key = std::vector<std::tuple<uint32_t, uint32_t, std::string>>;
                std::map<Key, std::vector<OpId>> groups;
                const auto &tree = mapping_.partitionTree;
                for (auto unit : tree.partitions[task.partition.index - 1].children)
                    for (auto id : tree.partitions[unit.index - 1].ops)
                    {
                        const auto &op = model_.operations()[id.index - 1];
                        const auto *edgeParameter = parameter<std::vector<std::string>>(model_, model_.parameters(op), "event_edges");
                        if (!edgeParameter || edgeParameter->empty()) continue;
                        const auto &edges = *edgeParameter;
                        const auto events = model_.operands(op).last(edges.size());
                        const auto histories = model_.objectRefs(op).last(edges.size());
                        if (std::any_of(histories.begin(), histories.end(), [&](ObjectRef history) {
                            return history.kind != ObjectKind::State || !sharedHistoryEligible_[history.index];
                        })) continue;
                        Key key;
                        for (std::size_t i = 0; i < edges.size(); ++i)
                        {
                            const auto alias = historyAliases_[histories[i].index];
                            key.emplace_back(events[i].index, alias ? alias.index : histories[i].index, edges[i]);
                        }
                        groups[std::move(key)].push_back(id);
                    }
                std::size_t index = 0;
                for (const auto &[key, ops] : groups)
                {
                    if (ops.size() < 2) continue;
                    const auto name = "cpu_edge_snapshot_" + std::to_string(index++);
                    // Private visible histories and boundary events cannot change during this commit task.
                    out << "const bool " << name << "=(" << eventGuard(model_.operations()[ops.front().index - 1], 1)
                        << "); // cpu_edge_snapshot uses=" << ops.size() << '\n';
                    for (auto id : ops) guards.emplace(id.index, name);
                }
                return guards;
            }

            std::map<uint32_t, std::string> computeGuardMap(PartitionId unit) const
            {
                std::map<uint32_t, std::string> guards;
                const auto found = computeGuardGroups_.find(unit.index);
                if (found == computeGuardGroups_.end()) return guards;
                for (const auto &group : found->second)
                    for (auto id : group.ops) guards.emplace(id.index, group.name);
                return guards;
            }

            void emitComputeGuardLocals(std::ostream &out, PartitionId unit) const
            {
                const auto found = computeGuardGroups_.find(unit.index);
                if (found == computeGuardGroups_.end()) return;
                for (const auto &group : found->second)
                    // Visible histories and boundary events cannot change during this unit invocation.
                    out << "const bool " << group.name << "=("
                        << eventGuard(model_.operations()[group.first.index - 1], group.historyBase)
                        << "); // cpu_cevent uses=" << group.ops.size() << '\n';
            }

            std::vector<std::string> computeGuardParams(PartitionId unit, std::span<const OpId> chunk) const
            {
                std::vector<std::string> params;
                const auto found = computeGuardGroups_.find(unit.index);
                if (found == computeGuardGroups_.end()) return params;
                for (const auto &group : found->second)
                    for (auto id : chunk)
                        if (std::find(group.ops.begin(), group.ops.end(), id) != group.ops.end())
                        { params.push_back(group.name); break; }
                return params;
            }

            void taskBody(std::ostream &out, const CpuScheduledTask &task) const
            {
                out << "#include \"" << prefix_ << ".hpp\"\n";
                if (task.execution == CpuExecution::ActivityDrivenCompute)
                    for (const auto &function : model_.functions()) out << dpiDeclaration(function) << '\n';
                out << "void " << class_ << "::cpu_task_" << task.id.index << "(){\n";
                deferredHistoryStores_.clear();
                localizeBuffers_ = true;
                emitBufferLocals(out);
                const auto eventCache = taskEventCache(task);
                activeEventCache_ = &eventCache;
                for (const auto &[event, name] : eventCache)
                    out << "const bool " << name << "=" << value(ValueId{event, 0}) << ";\n";
                const auto &tree = mapping_.partitionTree;
                if (task.execution != CpuExecution::ActivityDrivenCompute)
                {
                    if (dynamicStats_) out << "++cpu_dyn_cm_ent[" << task.id.index << "];if(cpu_dyn_edge==1)++cpu_dyn_cm_ent_pos;else if(cpu_dyn_edge==2)++cpu_dyn_cm_ent_neg;if(cpu_dyn_round_cur==0)++cpu_dyn_cm_ent_r0;else ++cpu_dyn_cm_ent_rN;\n";
                    if (const auto stable = stableCommitHistories(task); !stable.empty())
                    {
                        if (dynamicStats_)
                            out << "if(" << stable << "){++cpu_dyn_cm_stable;return;} // cpu_stable_history_skip task=" << task.id.index << '\n';
                        else
                            out << "if(" << stable << ")return; // cpu_stable_history_skip task=" << task.id.index << '\n';
                    }
                    const auto possibility = commitEdgePossibility(task);
                    if (!possibility.empty())
                    {
                        out << "if(!(" << possibility << ")){ // cpu_inactive_edge_sample task=" << task.id.index << '\n';
                        if (dynamicStats_) out << "++cpu_dyn_cm_inactive;\n";
                        // A false edge still samples history, including writes that overwrite pending values.
                        for (auto unit : tree.partitions[task.partition.index - 1].children)
                            for (auto op : tree.partitions[unit.index - 1].ops) sampleEvents(out, model_.operations()[op.index - 1], 1);
                        if (const auto batches = historyBatches_.find(task.id.index); batches != historyBatches_.end())
                            for (const auto &batch : batches->second) sampleHistoryBatch(out, batch);
                        flushDeferredHistoryStores(out);
                        out << "return;}\n";
                    }
                    const auto guards = commitEdgeSnapshots(out, task);
                    const auto findGuard = [&](OpId id) {
                        const auto found = guards.find(id.index);
                        return found == guards.end() ? std::string{} : found->second;
                    };
                    // cpu_mem_walk setup: cache the distinct boundary-bool enable
                    // operands of this task's memWrite ports into locals. Commit
                    // task bodies never write the boundary arena (commit ops produce
                    // no values and history sampling stages states), so after the
                    // early-return preambles the bytes stay stable for the whole walk.
                    memEnableCache_.clear();
                    memGuardStripped_.clear();
                    if (commitMemWalk_)
                    {
                        std::vector<std::uint32_t> enables;
                        std::unordered_map<std::uint32_t, std::uint32_t> uses;
                        for (auto unit : tree.partitions[task.partition.index - 1].children)
                            for (auto opId : tree.partitions[unit.index - 1].ops)
                            {
                                const auto &op = model_.operations()[opId.index - 1];
                                if (model_.text(op.opType) != "core.state.memWrite") continue;
                                const auto enable = model_.operands(op)[0];
                                if (++uses[enable.index] != 1) continue;
                                const auto &enableType = type(enable);
                                if (enableType.kind != TypeKind::Logic || enableType.domain != LogicDomain::TwoState ||
                                    enableType.width != 1 || enableType.isSigned) continue;
                                if (layout_.values[enable.index - 1].kind != CpuStorageKind::Boundary) continue;
                                if (staticScalars_.contains(enable.index) || readAliases_[enable.index]) continue;
                                enables.push_back(enable.index);
                            }
                        // Only shared enables pay off: a single-use value caches into a
                        // local for the same instruction count as the direct test.
                        for (const auto index : enables)
                        {
                            if (uses[index] < 2) continue;
                            const auto name = "cpu_men_" + std::to_string(index);
                            out << "const bool " << name << '=' << value(ValueId{index, 0}) << ";\n";
                            memEnableCache_.emplace(index, name);
                            ++memEnableCacheValues_;
                        }
                    }
                    // Event sampling stays unconditional in original op order
                    // (shared histories overwrite in program order); only the
                    // armed write ports move into the change-gated groups.
                    const auto memRunGuardOf = [&](OpId id) -> std::string {
                        if (!commitMemWalk_ || portArmWords_[id.index] != ~std::uint32_t(0)) return {};
                        const auto &op = model_.operations()[id.index - 1];
                        if (model_.text(op.opType) != "core.state.memWrite") return {};
                        return findGuard(id);
                    };
                    bool anyArmed = false;
                    for (auto unit : tree.partitions[task.partition.index - 1].children)
                        for (auto opIt = tree.partitions[unit.index - 1].ops.begin();
                             opIt != tree.partitions[unit.index - 1].ops.end();)
                        {
                            const auto op = *opIt;
                            if (portArmWords_[op.index] != ~std::uint32_t(0))
                            {
                                anyArmed = true;
                                sampleEvents(out, model_.operations()[op.index - 1], 1);
                                ++opIt;
                                continue;
                            }
                            // cpu_mem_guard_hoist: a run of same-snapshot-guard
                            // memWrite ports evaluates the pure-read guard once;
                            // port order inside the block is unchanged, and the
                            // deferred history sampling is independent of the
                            // writes (writes touch memory cells, sampling stages
                            // state histories; pending record order is commutative
                            // across disjoint keys).
                            const std::string runGuard = memRunGuardOf(op);
                            if (!runGuard.empty())
                            {
                                auto runEnd = std::next(opIt);
                                while (runEnd != tree.partitions[unit.index - 1].ops.end() &&
                                       memRunGuardOf(*runEnd) == runGuard) ++runEnd;
                                if (runEnd - opIt >= 4)
                                {
                                    out << "if(" << runGuard << "){ // cpu_mem_guard_hoist ops=" << runEnd - opIt << '\n';
                                    for (auto it = opIt; it != runEnd; ++it)
                                    {
                                        memGuardStripped_.insert(it->index);
                                        commit(out, model_.operations()[it->index - 1], runGuard);
                                        ++memGuardHoistSites_;
                                    }
                                    out << "}\n";
                                    for (auto it = opIt; it != runEnd; ++it)
                                        memWriteStages(out, model_.operations()[it->index - 1]);
                                    ++memGuardHoistRuns_;
                                    opIt = runEnd;
                                    continue;
                                }
                            }
                            commit(out, model_.operations()[op.index - 1], findGuard(op));
                            ++opIt;
                        }
                    // A port is evaluated only when one of its boundary
                    // operands changed since its last evaluation (its arm bit
                    // is set). Bits are consumed only for ports whose edge
                    // guard actually fires here; unarmed ports provably end
                    // in current==value, so state updates and notifications
                    // are unchanged. The per-word walk additionally gets a
                    // uint64 emptiness prefilter per 8-word group: a zero word
                    // proves all covered per-word blocks no-ops, leaving
                    // consumption and persistence semantics untouched.
                    if (anyArmed)
                    {
                        std::map<std::uint32_t, std::vector<OpId>> wordOps;
                        for (auto unit : tree.partitions[task.partition.index - 1].children)
                            for (auto op : tree.partitions[unit.index - 1].ops)
                            {
                                const auto word = portArmWords_[op.index];
                                if (word == ~std::uint32_t(0)) continue;
                                wordOps[word].push_back(op);
                            }
                        std::string taskGuard = findGuard(wordOps.begin()->second.front());
                        for (const auto &[word, ops] : wordOps)
                            for (auto op : ops)
                                if (findGuard(op) != taskGuard) taskGuard.clear();
                        if (!taskGuard.empty()) out << "if(" << taskGuard << "){ // cpu_shared_task_edge\n";
                        for (auto it = wordOps.cbegin(); it != wordOps.cend();)
                        {
                            const std::uint32_t base = it->first;
                            std::vector<std::map<std::uint32_t, std::vector<OpId>>::const_iterator> group;
                            for (auto next = it; next != wordOps.end() && next->first < base + 8; ++next) group.push_back(next);
                            it = group.empty() ? std::next(it) : std::next(group.back());
                            std::string sharedGuard = findGuard(group.front()->second.front());
                            for (auto entry : group)
                                for (auto op : entry->second)
                                    if (findGuard(op) != sharedGuard) sharedGuard.clear();
                            if (!sharedGuard.empty())
                            {
                                if (taskGuard.empty()) out << "if(" << sharedGuard << "){ // cpu_shared_port_edge\n";
                                ++sharedEdgeBlocks_;
                                for (auto entry : group) sharedEdgePorts_ += entry->second.size();
                            }
                            const bool compact = !sharedGuard.empty() && emitCommitCompactWalk(out, group, base);
                            const bool packed = !compact && group.size() >= 2;
                            if (packed)
                            {
                                out << "if(cpu_word8(cpu_pflags.data()," << base << ',' << (group.back()->first - base + 1) << ")){\n";
                                pflagPackedWords_ += group.size();
                            }
                            if (!compact)
                            for (auto entry : group)
                            {
                                out << "{const std::uint8_t cpu_armed=cpu_pflags[" << entry->first << "];if(cpu_armed){";
                                if (sharedGuard.empty()) out << "std::uint8_t cpu_consumed=0;";
                                out << '\n';
                                std::uint32_t portMask = 0;
                                for (auto op : entry->second)
                                {
                                    const auto &operation = model_.operations()[op.index - 1];
                                    const auto bit = 1u << portArmBits_[op.index];
                                    portMask |= bit;
                                    out << "if(cpu_armed&" << bit << "){";
                                    if (dynamicStats_) out << "++cpu_dyn_port_eval;if(cpu_dyn_edge==1)++cpu_dyn_port_eval_pos;else if(cpu_dyn_edge==2)++cpu_dyn_port_eval_neg;if(cpu_dyn_round_cur==0)++cpu_dyn_port_eval_r0;else ++cpu_dyn_port_eval_rN;";
                                    if (sharedGuard.empty())
                                        out << "if(" << commitEdgeGuard(operation, findGuard(op)) << "){cpu_consumed|=" << bit << ';';
                                    out << "if(" << value(model_.operands(operation)[0]) << "){\n";
                                    if (dynamicStats_) out << "++cpu_dyn_port_fire;if(cpu_dyn_edge==1)++cpu_dyn_port_fire_pos;else if(cpu_dyn_edge==2)++cpu_dyn_port_fire_neg;\n";
                                    directCommitBody(out, operation);
                                    out << (sharedGuard.empty() ? "}}}\n" : "}}\n");
                                }
                                out << "cpu_pflags[" << entry->first << ']';
                                if (sharedGuard.empty()) out << "&=~cpu_consumed";
                                else if (portMask == 255) out << "=0";
                                else out << "&=~" << portMask;
                                out << ";}}\n";
                            }
                            if (packed) out << "}\n";
                            if (!sharedGuard.empty() && taskGuard.empty()) out << "}\n";
                        }
                        if (!taskGuard.empty()) out << "}\n";
                    }
                    // Private shadow writes commute; guards keep reading individual visible histories.
                    if (const auto batches = historyBatches_.find(task.id.index); batches != historyBatches_.end())
                        for (const auto &batch : batches->second) sampleHistoryBatch(out, batch);
                    flushDeferredHistoryStores(out);
                }
                else
                    for (auto word : tree.partitions[task.partition.index - 1].children)
                    {
                        const auto offset = wordOffsets_[word.index];
                        out << "{std::uint8_t cpu_active_word=cpu_flags[" << offset << "];if(cpu_active_word){cpu_flags[" << offset << "]=0;\n";
                        for (auto unit : tree.partitions[word.index - 1].children)
                        {
                            const auto &partition = tree.partitions[unit.index - 1];
                            out << "if(cpu_active_word&" << activeMasks_[unit.index] << "){cpu_active_word&=~" << activeMasks_[unit.index] << ";\n";
                            if (dynamicStats_) out << "++cpu_dyn_sn_act[" << unit.index << "];if(cpu_dyn_edge==1)++cpu_dyn_sn_act_pos;else if(cpu_dyn_edge==2)++cpu_dyn_sn_act_neg;\n";
                            // Quiescent units (hist == event on every guard term) are inert: all
                            // edge guards are false and every embedded history sample is a
                            // current==next no-op, so the whole body is skipped.
                            const auto quiescence = computeQuiescence_.find(unit.index);
                            const bool quiescent = quiescence != computeQuiescence_.end() && !quiescence->second.empty();
                            if (quiescent)
                                out << "if(" << quiescenceCheck(quiescence->second) << "){ // cpu_quiescence_skip unit=" << unit.index << '\n';
                            // Single-(event, direction) units are also inert while the
                            // event sits at the inactive level: every edge guard is
                            // false regardless of the histories. Skipping the body
                            // there halves the activations of always-woken endpoint
                            // units (both clock edges wake them); history sampling
                            // below stays outside the wrapper so edge tracking is
                            // unchanged.
                            int edgeDirection = 0;
                            if (quiescent)
                                if (const auto found = computeEdgeDirection_.find(unit.index);
                                    found != computeEdgeDirection_.end())
                                    edgeDirection = found->second;
                            if (edgeDirection != 0)
                                out << "if(" << (edgeDirection < 0 ? "!" : "") << eventValue(quiescence->second.front().event)
                                    << "){ // cpu_edge_direction\n";
                            if (dynamicStats_) out << "++cpu_dyn_sn_body[" << unit.index << "];\n";
                            out << "alignas(8) std::byte cpu_local[" << std::max<uint64_t>(frameSizes_[unit.index], 1) << "]{};\n";
                            // Strings outlive all helper calls for this supernode invocation.
                            for (auto stringOffset : localStrings_[unit.index])
                                out << "std::string cpu_string_" << stringOffset << ";cpu_at<std::string*>(cpu_local,"
                                    << stringOffset << ")=&cpu_string_" << stringOffset << ";\n";
                            if (!partition.attrs.helperChunks.empty())
                            {
                                std::vector<OpId> ops;
                                for (auto node : partition.children)
                                    ops.insert(ops.end(), tree.partitions[node.index - 1].ops.begin(), tree.partitions[node.index - 1].ops.end());
                                emitComputeGuardLocals(out, unit);
                                for (std::size_t i = 0; i < partition.attrs.helperChunks.size(); ++i)
                                {
                                    const auto range = partition.attrs.helperChunks[i];
                                    out << "cpu_helper_" << unit.index << '_' << i << "(cpu_local,cpu_active_word";
                                    for (const auto &param : computeGuardParams(unit, std::span<const OpId>(ops).subspan(range.offset, range.count)))
                                        out << ',' << param;
                                    out << ");\n";
                                }
                            }
                            else
                            {
                                std::vector<OpId> ops;
                                for (auto node : partition.children)
                                    ops.insert(ops.end(), tree.partitions[node.index - 1].ops.begin(), tree.partitions[node.index - 1].ops.end());
                                emitComputeGuardLocals(out, unit);
                                const auto guards = computeGuardMap(unit);
                                computeGroup(out, ops, unit, guards);
                            }
                            if (edgeDirection != 0) out << "}\n";
                            if (const auto samples = directSampleUnits_.find(unit.index); samples != directSampleUnits_.end())
                                for (const auto &sample : samples->second)
                                    out << "{const bool cpu_dsample=" << eventValue(sample.event) << ";if(" << state(sample.state)
                                        << "!=cpu_dsample){" << state(sample.state) << "=cpu_dsample;"
                                        << (sample.projection ? "cpu_direct_again=true;" : "") << "}}\n";
                            if (quiescent) out << "}\n";
                            out << "}\n";
                        }
                        out << "cpu_flags[" << offset << "]|=cpu_active_word;}}\n";
                    }
                out << "}\n";
                activeEventCache_ = nullptr;
                if (task.execution == CpuExecution::ActivityDrivenCompute)
                    for (auto word : tree.partitions[task.partition.index - 1].children)
                        for (auto unit : tree.partitions[word.index - 1].children)
                        {
                            const auto &partition = tree.partitions[unit.index - 1];
                            if (partition.attrs.helperChunks.empty()) continue;
                            std::vector<OpId> ops;
                            for (auto node : partition.children) ops.insert(ops.end(), tree.partitions[node.index - 1].ops.begin(), tree.partitions[node.index - 1].ops.end());
                            for (std::size_t i = 0; i < partition.attrs.helperChunks.size(); ++i)
                            {
                                const auto range = partition.attrs.helperChunks[i];
                                const auto chunk = std::span<const OpId>(ops).subspan(range.offset, range.count);
                                out << "void " << class_ << "::cpu_helper_" << unit.index << '_' << i << "(std::byte *cpu_local,std::uint8_t &cpu_active_word";
                                for (const auto &param : computeGuardParams(unit, chunk)) out << ",bool " << param;
                                out << "){\n";
                                emitBufferLocals(out);
                                const auto guards = computeGuardMap(unit);
                                computeGroup(out, chunk, unit, guards);
                                out << "}\n";
                            }
                        }
                localizeBuffers_ = false;
            }

            struct Target { uint32_t offset, mask; bool arm; };
            const GrhSimModel &model_;
            const CpuBackendMapping &mapping_;
            const CpuDataLayout &layout_;
            const CpuSchedulePlan &schedule_;
            std::string prefix_, class_;
            std::vector<uint32_t> wordOffsets_, armOffsets_;
            std::vector<uint64_t> frameSizes_;
            std::vector<std::vector<uint64_t>> localStrings_;
            std::vector<std::pair<std::string_view, uint64_t>> persistentStrings_;
            std::map<uint32_t, std::string> staticStrings_;
            std::map<uint32_t, std::string> staticScalars_;
            mutable const std::map<uint32_t, std::string> *activeEventCache_ = nullptr;
            mutable const std::map<uint32_t, std::string> *activeStateCache_ = nullptr;
            std::vector<OpId> producers_;
            bool hasSystemTasks_ = false;
            std::map<uint32_t, std::size_t> onceTasks_;
            std::vector<uint32_t> activeOffsets_, activeMasks_;
            std::vector<Range> stateRanges_;
            std::vector<bool> projected_;
            std::vector<const CpuActivationTargets *> fanout_;
            std::vector<Target> stateTargets_;
            std::vector<StateId> readAliases_;
            std::vector<std::vector<PartitionId>> aliasConsumers_;
            std::vector<PartitionId> computeOwners_;
            std::vector<uint64_t> memoryDirtyBases_;
            std::vector<Range> memoryRanges_;
            std::vector<uint32_t> memoryReadIds_;
            std::vector<Target> memoryReaders_;
            uint64_t dirtyBytes_ = 0;
            std::vector<bool> batchedHistories_;
            std::vector<bool> directCommitStates_;
            std::vector<bool> privateByteHistories_;
            std::vector<StateId> historyAliases_;
            std::vector<bool> sharedHistoryEligible_;
            uint64_t sharedHistoryCount_ = 0, sharedHistoryTasks_ = 0;
            std::set<uint32_t> sharedHistoryTaskIds_;
            uint64_t computeSharedHistoryCount_ = 0, computeSharedHistoryUnits_ = 0;
            uint64_t computeGuardSnapshots_ = 0, computeGuardUses_ = 0;
            std::map<uint32_t, const std::vector<ValueId> *> helperReadCaches_;
            uint64_t helperReadCacheValues_ = 0;
            mutable const std::map<uint32_t, std::string> *activeValueCache_ = nullptr;
            mutable bool localizeBuffers_ = false;
            std::map<std::uint32_t, std::vector<ComputeGuardGroup>> computeGuardGroups_;
            std::map<std::uint32_t, std::vector<QuiescenceTerm>> computeQuiescence_;
            std::map<std::uint32_t, int> computeEdgeDirection_;
            uint64_t computeQuiescenceUnits_ = 0, computeQuiescenceTerms_ = 0, edgeDirectionUnits_ = 0;
            mutable uint64_t gateHoistedRuns_ = 0, gateHoistedGates_ = 0, gateMergedGates_ = 0, gateColdHints_ = 0;
            mutable uint64_t commitCompactGroups_ = 0, commitCompactPorts_ = 0;
            struct DirectSample { StateId state; ValueId event; bool projection; };
            std::vector<char> directSampleStates_;
            std::map<std::uint32_t, std::vector<DirectSample>> directSampleUnits_;
            uint64_t directSampleStateCount_ = 0, directSampleUnitCount_ = 0;
            // Commit-side private 1-bit histories sampled by deferred direct stores,
            // retiring their pending/publish/re-arm round trip (see
            // planCommitDirectHistories).
            std::vector<char> commitDirectHistories_;
            uint64_t commitDirectHistoryCount_ = 0;
            mutable std::vector<std::pair<StateId, std::string>> deferredHistoryStores_;
            uint64_t directCommitCount_ = 0;
            std::map<uint32_t, std::vector<HistoryBatch>> historyBatches_;
            uint64_t historyCandidates_ = 0, historyPrivateRejected_ = 0, historyLayoutRejected_ = 0;
            uint64_t historyBatchStates_ = 0, historyBatchCount_ = 0, historyMaxBatch_ = 0, historyMaxPattern_ = 0;
            std::vector<std::uint32_t> portArmWords_;
            std::vector<std::uint8_t> portArmBits_;
            std::vector<std::vector<PortArmTarget>> portArmTargets_;
            std::uint32_t portArmWordCount_ = 0;
            std::uint64_t portArmPortCount_ = 0, portArmTaskCount_ = 0, portArmValueCount_ = 0;
            mutable std::uint64_t dispatchPackedBytes_ = 0, handoffPackedSlots_ = 0, pflagPackedWords_ = 0;
            mutable std::uint64_t sharedEdgeBlocks_ = 0, sharedEdgePorts_ = 0;
            bool dynamicStats_ = false;
            bool commitCompactWalk_ = false;
            // --commit-mem-walk: per-task scratch for memWrite walk skeleton
            // reduction (snapshot-guard run hoisting + boundary-bool enable cache).
            bool commitMemWalk_ = false;
            mutable std::unordered_set<std::uint32_t> memGuardStripped_;
            mutable std::unordered_map<std::uint32_t, std::string> memEnableCache_;
            mutable std::uint64_t memGuardHoistRuns_ = 0, memGuardHoistSites_ = 0;
            mutable std::uint64_t memEnableCacheValues_ = 0, memEnableCacheSites_ = 0;
            std::map<std::string, std::uint32_t> dynKinds_;
            std::uint32_t dynTaskSpan_ = 1;
            std::uint32_t dynKind(const SimOp &op) const { return dynKinds_.at(std::string(model_.text(op.opType))); }
            bool dynBoundary(ValueId result) const
            {
                return dynamicStats_ && layout_.values[result.index - 1].kind == CpuStorageKind::Boundary;
            }
        };

        class EmitCppPass final : public Pass
        {
        public:
            explicit EmitCppPass(std::filesystem::path path, bool dynamicStats = false, bool commitCompactWalk = false,
                                 bool commitMemWalk = false)
                : Pass("cpu.st.emit-cpp", PassKind::Emit), path_(std::move(path)), dynamicStats_(dynamicStats),
                  commitCompactWalk_(commitCompactWalk), commitMemWalk_(commitMemWalk) {}
            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            { return emitCpuCpp(model, path_, diagnostics, dynamicStats_, commitCompactWalk_, commitMemWalk_); }
        private:
            std::filesystem::path path_;
            bool dynamicStats_ = false;
            bool commitCompactWalk_ = false;
            bool commitMemWalk_ = false;
        };
    }

    PassResult emitCpuCpp(const GrhSimModel &model, const std::filesystem::path &directory, diag::Diagnostics &diagnostics,
                          bool dynamicStats, bool commitCompactWalk, bool commitMemWalk)
    {
        if (!verifyGrhSimModel(model, defaultDialectRegistry(), diagnostics)) return {false, false, {}};
        if (!model.cpuMapping() || model.cpuMapping()->stage != CpuMappingStage::Schedule)
        { diagnostics.error("CPU C++ emit requires a complete schedule", "cpu.st.emit-cpp"); return {false, false, {}}; }
        try
        {
            Emitter emitter(model, dynamicStats, commitCompactWalk, commitMemWalk); emitter.validate();
            diagnostics.info(emitter.historyBatchSummary(), "cpu.st.emit-cpp");
            auto result = emitter.write(directory);
            diagnostics.info(emitter.packSummary(), "cpu.st.emit-cpp");
            return result;
        }
        catch (const std::exception &error)
        { diagnostics.error(error.what(), "cpu.st.emit-cpp"); return {false, false, {}}; }
    }

    void registerCpuEmitPasses(PassRegistry &registry)
    {
        std::string error;
        if (!registry.registerPass("cpu.st.emit-cpp", PassKind::Emit,
            [](std::span<const std::string_view> args, std::string &error) -> std::unique_ptr<Pass> {
                if (args.empty() || args.size() % 2)
                { error = "expected --output <empty-directory> [--dynamic-stats <true|false>] [--commit-compact-walk <true|false>] [--commit-mem-walk <true|false>]"; return {}; }
                std::filesystem::path output;
                bool dynamicStats = false;
                bool commitCompactWalk = false;
                bool commitMemWalk = false;
                for (std::size_t i = 0; i < args.size(); i += 2)
                {
                    if (args[i] == "--output" && output.empty() && !args[i + 1].empty()) output = args[i + 1];
                    else if (args[i] == "--dynamic-stats" && (args[i + 1] == "true" || args[i + 1] == "false"))
                        dynamicStats = args[i + 1] == "true";
                    else if (args[i] == "--commit-compact-walk" && (args[i + 1] == "true" || args[i + 1] == "false"))
                        commitCompactWalk = args[i + 1] == "true";
                    else if (args[i] == "--commit-mem-walk" && (args[i + 1] == "true" || args[i + 1] == "false"))
                        commitMemWalk = args[i + 1] == "true";
                    else
                    { error = "expected --output <empty-directory> [--dynamic-stats <true|false>] [--commit-compact-walk <true|false>] [--commit-mem-walk <true|false>]"; return {}; }
                }
                if (output.empty())
                { error = "expected --output <empty-directory> [--dynamic-stats <true|false>] [--commit-compact-walk <true|false>] [--commit-mem-walk <true|false>]"; return {}; }
                return std::make_unique<EmitCppPass>(std::move(output), dynamicStats, commitCompactWalk, commitMemWalk);
            }, error)) throw std::logic_error(error);
    }
}
