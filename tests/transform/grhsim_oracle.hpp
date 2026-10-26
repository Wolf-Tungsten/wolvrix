#ifndef WOLVRIX_TEST_GRHSIM_ORACLE_HPP
#define WOLVRIX_TEST_GRHSIM_ORACLE_HPP

// Test-only logical observations. No emitter layout, optimizer evaluator or
// SessionStore artifact is used by the unpartitioned reference. The candidate
// interpreter separately consumes ownership and the published sink schedule.
#include "core/grh.hpp"
#include "core/state.hpp"
#include "transform/grhsim/ops.hpp"
#include "transform/grhsim/sink-schedule.hpp"
#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>

namespace grhsim_test {
namespace grh = wolvrix::lib::grh;
namespace sim = wolvrix::lib::transform::grhsim;
using grh::OperationKind;
using slang::SVInt;

struct Observation {
    std::map<std::string, std::string> outputs, states;
    std::vector<std::map<std::string, std::string>> rounds;
    std::vector<std::string> effects;
    friend bool operator==(const Observation& left, const Observation& right) {
        // Dead-state elimination may remove rounds with no observable change.
        // Preserve the sequence of projected changes and every external effect.
        const auto changes = [](auto rounds) {
            rounds.erase(std::unique(rounds.begin(), rounds.end()), rounds.end());
            return rounds;
        };
        return left.outputs == right.outputs && left.states == right.states &&
            left.effects == right.effects && changes(left.rounds) == changes(right.rounds);
    }
};

class Oracle {
public:
    explicit Oracle(const grh::Graph& graph, std::vector<std::string> probes = {}, const sim::SinkSchedule* schedule = nullptr)
        : graph_(graph), probes_(std::move(probes)), schedule_(schedule) { init(); }

    void init() {
        state_.clear(); history_.clear(); pendingHistory_.clear(); held_.clear(); samples_.clear();
        inputs_.clear(); cache_.clear(); effects_.clear(); random_ = 417; evaluations_ = 0; historyUpdates_ = 0;
        taskPrevious_.clear(); heldSamples_.clear(); pulses_.clear(); phase_.clear(); samplingEvents_ = false;
        for (const auto& port : graph_.inputPorts()) inputs_.emplace(port.value, SVInt(graph_.valueWidth(port.value), 0, graph_.valueSigned(port.value)));
        for (const auto op : graph_.operations()) {
            if (graph_.opKind(op) != OperationKind::kRegister && graph_.opKind(op) != OperationKind::kLatch) continue;
            const auto type = grh::stateLogicalType(graph_, op);
            if (type.kind != grh::ValueType::Logic) throw std::runtime_error("Oracle fixture requires Logic state");
            SVInt bits(type.width, 0, type.isSigned);
            for (const auto& step : grh::stateInitSpec(graph_, op)) {
                if (step.kind == grh::InitKind::Constant) bits = literal(step.value, type.width, type.isSigned);
                else if (step.kind == grh::InitKind::Fill) {
                    const auto rows = type.width / type.leafWidth();
                    const auto begin = step.start.value_or(0), count = step.count.value_or(rows - begin);
                    for (int64_t row = begin; row < begin + count; ++row)
                        bits.set(row * type.leafWidth() + type.leafWidth() - 1, row * type.leafWidth(), literal(step.value, type.leafWidth(), type.isSigned));
                } else throw std::runtime_error("Oracle fixture initializer is not const/fill");
            }
            bits.flattenUnknowns(); state_.emplace(op, bits);
        }
        // All fixture event cones use the legacy statically evaluable subset.
        for (const auto op : graph_.operations()) {
            if (graph_.opKind(op) == sim::kEdgeDetect)
                history_["edge/" + std::to_string(op.index)] = !zero(literal(sim::attribute<std::string>(graph_, op, "prevInit").value(), 1, false));
            if (const auto edges = sim::attribute<std::vector<std::string>>(graph_, op, "eventEdge")) {
                const auto args = graph_.opOperands(op);
                for (std::size_t i = 0; i < edges->size(); ++i) {
                    const auto event = args[args.size() - edges->size() + i];
                    history_[rawKey(event)] = !zero(value(event));
                }
            }
        }
        cache_.clear(); effects_.clear(); samples_.clear();
    }

    Observation eval(const std::map<std::string, uint64_t>& inputs) {
        for (const auto& port : graph_.inputPorts()) {
            const auto found = inputs.find(port.name);
            if (found != inputs.end()) inputs_[port.value] = SVInt(graph_.valueWidth(port.value), found->second, graph_.valueSigned(port.value));
        }
        ++evaluations_; samples_.clear(); effects_.clear(); deferred_.clear();
        if (const auto stage = graph_.metadata(sim::kStage); stage && *stage != grh::AttributeValue(std::string("converted")))
            return partitionEval();
        Observation observation;
        for (uint32_t round = 0; round < 64; ++round) {
            cache_.clear(); executed_.clear(); pendingHistory_.clear(); pulses_.clear(); samplingEvents_ = true;
            for (const auto op : graph_.operations()) {
                const auto kind = graph_.opKind(op);
                if ((kind == OperationKind::kSystemFunction || kind == sim::kSystemFunction) &&
                    sim::systemFunctionEffect(graph_, op) != "pure")
                    for (const auto result : graph_.opResults(op)) value(result);
                if (kind == OperationKind::kDpicCall || kind == sim::kDpiCall) call(op);
                if (kind == OperationKind::kSystemTask || kind == sim::kSystemTask) task(op);
            }
            auto next = state_;
            std::vector<grh::OperationId> writers;
            for (const auto op : graph_.operations())
                if (grh::isRegisterWritePortKind(graph_.opKind(op)) || graph_.opKind(op) == OperationKind::kLatchWritePort || sim::isWriter(graph_.opKind(op))) writers.push_back(op);
            std::stable_sort(writers.begin(), writers.end(), [&](auto a, auto b) {
                return sim::attribute<int64_t>(graph_, a, "writerOrder").value_or(a.index) < sim::attribute<int64_t>(graph_, b, "writerOrder").value_or(b.index);
            });
            for (const auto op : writers) write(op, next);
            // Every detector samples history once, even when all consumers are disabled.
            for (const auto op : graph_.operations())
                if (graph_.opKind(op) == sim::kEdgeDetect) value(graph_.opResults(op)[0]);
            for (const auto& [key, current] : pendingHistory_) { history_[key] = current; ++historyUpdates_; }
            samplingEvents_ = false;
            const bool changed = next != state_;
            state_ = std::move(next); cache_.clear();
            observation.rounds.push_back(project());
            if (!changed) break;
            if (round == 63) throw std::runtime_error("Oracle fixture did not converge");
        }
        cache_.clear();
        for (const auto& port : graph_.outputPorts()) observation.outputs.emplace(port.name, text(value(port.value)));
        for (const auto op : graph_.operations()) {
            const auto kind = graph_.opKind(op);
            if (kind != OperationKind::kSystemTask && kind != sim::kSystemTask) continue;
            const auto name = sim::attribute<std::string>(graph_, op, "name").value();
            if (name != "monitor" && name != "strobe") continue;
            const auto args = graph_.opOperands(op);
            const auto first = kind == sim::kSystemTask ? 2u : 1u;
            const auto events = sim::attribute<std::vector<std::string>>(graph_, op, "eventEdge").value_or(std::vector<std::string>{}).size();
            const bool event = events || sim::attribute<bool>(graph_, op, "eventDriven").value_or(false);
            bool changed = false;
            if (!event) {
                auto& previous = taskPrevious_[op];
                if (previous.empty()) for (std::size_t i = first - 1; i < args.size(); ++i) previous.emplace_back(graph_.valueWidth(args[i]), 0, false);
                for (std::size_t i = first - 1; i < args.size(); ++i) {
                    const auto current = value(args[i]); changed |= !exactlyEqual(current, previous[i - first + 1]); previous[i - first + 1] = current;
                }
            }
            if ((event ? deferred_.contains(op) : changed) && !zero(value(args[first - 1])))
                log(op, args.subspan(first, args.size() - first - events));
        }
        observation.states = project(); observation.effects = effects_;
        return observation;
    }

    std::size_t historyUpdates() const { return historyUpdates_; }

private:
    std::vector<std::string> owner(grh::OperationId op) const {
        return sim::attribute<std::vector<std::string>>(graph_, op, sim::kPartition).value_or(std::vector<std::string>{});
    }
    bool region(grh::OperationId op, std::string_view name) const {
        const auto path = owner(op); return !path.empty() && path[0] == name;
    }
    void execute(grh::OperationId op) {
        const auto kind = graph_.opKind(op);
        if (kind == sim::kDpiCall) call(op);
        else if (kind == sim::kSystemTask) task(op);
        else for (const auto result : graph_.opResults(op)) value(result);
    }
    Observation partitionEval() {
        if (schedule_ && !sim::verifySinkSchedule(graph_, *schedule_).empty()) throw std::runtime_error("Oracle received an invalid schedule");
        Observation observation;
        for (uint32_t round = 0; round < 64; ++round) {
            cache_.clear(); executed_.clear(); pendingHistory_.clear(); pulses_.clear(); phase_ = "event"; samplingEvents_ = true;
            for (const auto op : graph_.operations()) if (region(op, "event")) execute(op);
            for (const auto& [key, current] : pendingHistory_) { history_[key] = current; ++historyUpdates_; }
            samplingEvents_ = false;
            for (const auto op : graph_.operations())
                if (sim::attribute<std::string>(graph_, op, "grhsim.timeslot") == "event" && fire(op)) deferred_.insert(op);
            phase_ = "general";
            // Execute actual leaf partitions in a dependency/order DAG. This
            // is independent of the optimizer's cutter and does not flatten
            // away the ownership constraints under test.
            std::map<std::vector<std::string>, std::vector<grh::OperationId>> parts;
            std::map<std::vector<std::string>, std::set<std::vector<std::string>>> dependencies;
            std::map<int64_t, grh::OperationId> effects;
            for (const auto op : graph_.operations()) if (region(op, "general") && !sim::isWriter(graph_.opKind(op))) {
                parts[owner(op)].push_back(op);
                if (const auto order = sim::attribute<int64_t>(graph_, op, "grhsim.effectOrder")) effects.emplace(*order, op);
            }
            for (const auto& [part, operations] : parts) for (const auto op : operations)
                for (const auto input : graph_.opOperands(op)) {
                    const auto producer = graph_.valueDef(input);
                    if (producer && parts.contains(owner(producer)) && owner(producer) != part) dependencies[part].insert(owner(producer));
                }
            grh::OperationId previous;
            for (const auto& [order, op] : effects) {
                if (previous && owner(previous) != owner(op)) dependencies[owner(op)].insert(owner(previous)); previous = op;
            }
            std::set<std::vector<std::string>> done;
            while (done.size() < parts.size()) {
                bool progress = false;
                for (const auto& [part, operations] : parts) {
                    if (done.contains(part) || std::any_of(dependencies[part].begin(), dependencies[part].end(), [&](const auto& dep) { return !done.contains(dep); })) continue;
                    for (const auto op : operations) execute(op);
                    done.insert(part); progress = true;
                }
                if (!progress) throw std::runtime_error("Oracle partition DAG is cyclic");
            }
            const auto before = state_;
            auto next = state_;
            std::vector<grh::OperationId> writers;
            if (schedule_) for (const auto& run : schedule_->generalRuns) {
                for (const auto& key : run.requiredNonsinkKeys) if (!done.contains(key)) throw std::runtime_error("Missing sink dependency execution");
                writers.insert(writers.end(), run.orderedWriters.begin(), run.orderedWriters.end());
            } else {
                for (const auto op : graph_.operations()) if (sim::isWriter(graph_.opKind(op))) writers.push_back(op);
                std::stable_sort(writers.begin(), writers.end(), [&](auto a, auto b) { return sim::attribute<int64_t>(graph_, a, "writerOrder").value() < sim::attribute<int64_t>(graph_, b, "writerOrder").value(); });
            }
            for (const auto op : writers) for (const auto input : graph_.opOperands(op)) value(input); // General snapshots.
            for (const auto op : writers) {
                const auto target = sim::writerTarget(graph_, op);
                if (sim::attribute<std::string>(graph_, target, "grhsim.statePolicy") != "large-sink") write(op, next);
            }
            phase_ = "large";
            if (schedule_) {
                for (const auto& run : schedule_->largeCommitRuns) for (const auto op : run.orderedWriters) write(op, next);
            } else for (const auto op : writers)
                if (sim::attribute<std::string>(graph_, sim::writerTarget(graph_, op), "grhsim.statePolicy") == "large-sink") write(op, next);
            state_ = std::move(next); phase_ = ""; cache_.clear(); observation.rounds.push_back(project());
            if (state_ == before) break;
            if (round == 63) throw std::runtime_error("Partition oracle fixture did not converge");
        }
        phase_ = "output"; cache_.clear();
        for (const auto& port : graph_.outputPorts()) observation.outputs.emplace(port.name, text(value(port.value)));
        // All task comparisons observe old histories; commit every new history
        // together only after every Output task has evaluated.
        for (const auto op : graph_.operations()) if (region(op, "output") && graph_.opKind(op) == sim::kSystemTask) {
            const auto args = graph_.opOperands(op);
            if ((sim::attribute<std::string>(graph_, op, "grhsim.timeslot") != "event" || deferred_.contains(op)) && !zero(value(args[1]))) log(op, args.subspan(2));
        }
        std::vector<std::pair<grh::OperationId, SVInt>> histories;
        for (const auto op : graph_.operations()) if (graph_.opKind(op) == sim::kHistoryWrite)
            histories.emplace_back(sim::writerTarget(graph_, op), value(graph_.opOperands(op)[0]));
        for (const auto& [state, bits] : histories) state_.at(state) = bits;
        observation.states = project(); observation.effects = effects_; phase_.clear();
        return observation;
    }
    static bool zero(const SVInt& bits) { return !bits.hasUnknown() && bits == 0; }
    static std::string text(const SVInt& bits) { return bits.toString(slang::LiteralBase::Hex, true, bits.getBitWidth()); }
    static SVInt literal(const std::string& input, int32_t width, bool sign) {
        SVInt bits;
        if (input == "x" || input == "'x") bits = SVInt::createFillX(width, sign);
        else if (input == "'1") bits = ~SVInt(width, 0, sign);
        else { bits = SVInt::fromString(input); bits.setSigned(sign); bits = bits.resize(width); }
        return bits;
    }
    SVInt normalize(grh::ValueId id, SVInt bits) const {
        bits.setSigned(graph_.valueSigned(id)); bits = bits.resize(graph_.valueWidth(id)); bits.setSigned(graph_.valueSigned(id));
        // The reference executes the explicitly selected production TwoState policy.
        bits.flattenUnknowns(); return bits;
    }
    std::string rawKey(grh::ValueId value) const { return "raw/" + std::to_string(value.index); }
    static bool edge(std::string_view mode, bool previous, bool current) {
        if (mode == "posedge") return !previous && current;
        if (mode == "negedge") return previous && !current;
        if (mode == "both") return previous != current;
        throw std::runtime_error("Unknown oracle edge");
    }
    bool fire(grh::OperationId op) {
        const auto kind = graph_.opKind(op);
        if ((sim::isWriter(kind) && kind != sim::kLatchWrite) || kind == sim::kDpiCall || kind == sim::kSystemTask)
            return !zero(value(graph_.opOperands(op)[0]));
        const auto edges = sim::attribute<std::vector<std::string>>(graph_, op, "eventEdge").value_or(std::vector<std::string>{});
        const auto args = graph_.opOperands(op);
        bool active = edges.empty();
        for (std::size_t i = 0; i < edges.size(); ++i) {
            const auto event = args[args.size() - edges.size() + i];
            const auto key = rawKey(event); const bool current = !zero(value(event));
            active |= edge(edges[i], history_[key], current); pendingHistory_[key] = current;
        }
        return active;
    }
    std::map<std::string, std::string> project() {
        std::map<std::string, std::string> result;
        for (const auto& name : probes_) {
            if (const auto state = graph_.findOperation(name); state && state_.contains(state)) result.emplace(name, text(state_.at(state)));
            else if (const auto view = graph_.findValue(name)) result.emplace(name, text(value(view)));
            else throw std::runtime_error("Missing state projection: " + name);
        }
        return result;
    }
    uint64_t random() { random_ ^= random_ << 13; random_ ^= random_ >> 7; random_ ^= random_ << 17; return random_; }
    void log(grh::OperationId op, std::span<const grh::ValueId> args) {
        std::string entry = std::string(graph_.getOperation(op).symbolText()) + ":";
        for (const auto arg : args) entry += text(value(arg)) + ",";
        effects_.push_back(std::move(entry));
    }
    void task(grh::OperationId op) {
        const auto kind = graph_.opKind(op); const auto args = graph_.opOperands(op);
        const auto first = kind == sim::kSystemTask ? 2u : 1u;
        const auto name = sim::attribute<std::string>(graph_, op, "name").value();
        if (name == "monitor" || name == "strobe") { if (fire(op)) deferred_.insert(op); return; }
        if (!fire(op) || zero(value(args[first - 1]))) return;
        const auto events = sim::attribute<std::vector<std::string>>(graph_, op, "eventEdge").value_or(std::vector<std::string>{}).size();
        log(op, args.subspan(first, args.size() - first - events));
    }
    void call(grh::OperationId op) {
        const auto args = graph_.opOperands(op), results = graph_.opResults(op);
        const auto first = graph_.opKind(op) == sim::kDpiCall ? 2u : 1u;
        auto& held = held_[op];
        if (held.empty()) for (const auto result : results) held.emplace_back(graph_.valueWidth(result), 0, graph_.valueSigned(result));
        if (!phase_.empty() && phase_ != "general") {
            for (std::size_t i = 0; i < results.size(); ++i) cache_.insert_or_assign(results[i], held[i]);
            return;
        }
        if (executed_.insert(op).second && fire(op) && !zero(value(args[first - 1]))) {
            const auto events = sim::attribute<std::vector<std::string>>(graph_, op, "eventEdge").value_or(std::vector<std::string>{}).size();
            log(op, args.subspan(first, args.size() - first - events));
            uint64_t hash = 17;
            for (std::size_t i = first; i < args.size() - events; ++i) hash = hash * 131 + value(args[i]).getRawPtr()[0];
            for (std::size_t i = 0; i < results.size(); ++i) held[i] = normalize(results[i], SVInt(64, hash + i, false));
        }
        for (std::size_t i = 0; i < results.size(); ++i) cache_.insert_or_assign(results[i], held[i]);
    }
    SVInt value(grh::ValueId id) {
        if (const auto found = cache_.find(id); found != cache_.end()) return found->second;
        if (const auto found = inputs_.find(id); found != inputs_.end()) return found->second;
        const auto op = graph_.valueDef(id);
        if (!op) return SVInt(graph_.valueWidth(id), 0, graph_.valueSigned(id));
        const auto kind = graph_.opKind(op);
        const auto args = graph_.opOperands(op);
        const auto a = [&](std::size_t i) { return value(args[i]); };
        SVInt result;
        if (kind == OperationKind::kConstant) result = literal(sim::attribute<std::string>(graph_, op, "constValue").value(), graph_.valueWidth(id), graph_.valueSigned(id));
        else if (kind == OperationKind::kRegisterReadPort || kind == OperationKind::kLatchReadPort || kind == OperationKind::kRegisterReadArrayPort) {
            const auto target = graph_.findOperation(sim::attribute<std::string>(graph_, op, kind == OperationKind::kLatchReadPort ? "latchSymbol" : "regSymbol").value());
            result = state_.at(target);
            if (kind == OperationKind::kRegisterReadArrayPort) {
                const auto type = grh::stateLogicalType(graph_, target); int64_t offset = 0, stride = type.width; bool valid = true;
                for (std::size_t i = 0; i < args.size(); ++i) {
                    const auto index = a(i).as<uint64_t>();
                    if (!index || *index >= static_cast<uint64_t>(type.shape[i])) { valid = false; break; }
                    stride /= type.shape[i]; offset += *index * stride;
                }
                result = valid ? result.lshr(SVInt(static_cast<uint64_t>(offset))) : SVInt(graph_.valueWidth(id), 0, false);
            }
        } else if (kind == sim::kEdgeDetect) {
            if (!samplingEvents_ && pulses_.contains(id)) return pulses_.at(id);
            const auto key = "edge/" + std::to_string(op.index); const bool current = !zero(a(0));
            result = SVInt(1, edge(sim::attribute<std::string>(graph_, op, "edge").value(), history_[key], current), false);
            pendingHistory_[key] = current;
            pulses_.insert_or_assign(id, result);
        } else if (kind == OperationKind::kDpicCall || kind == sim::kDpiCall) { call(op); return cache_.at(id); }
        else if (kind == OperationKind::kSystemFunction || kind == sim::kSystemFunction) {
            const auto name = sim::attribute<std::string>(graph_, op, "name").value();
            const bool pure = sim::systemFunctionEffect(graph_, op) == "pure";
            const auto sample = sim::attribute<std::string>(graph_, op, "sampleId").value_or("grhsim.sample/" + std::string(graph_.getOperation(op).symbolText()));
            if (!pure && !phase_.empty() && phase_ != "general") {
                if (samples_.contains(sample)) return normalize(id, samples_.at(sample));
                if (heldSamples_.contains(sample)) return normalize(id, heldSamples_.at(sample));
                return SVInt(graph_.valueWidth(id), 0, graph_.valueSigned(id));
            }
            if (!pure && samples_.contains(sample)) return normalize(id, samples_.at(sample));
            if (name == "clog2") result = SVInt(static_cast<uint64_t>(slang::clog2(a(0))));
            else if (name == "random" || name == "urandom") result = SVInt(64, random(), false);
            else if (name == "time") result = SVInt(64, evaluations_, false);
            else throw std::runtime_error("Unsupported oracle system function: " + name);
            if (!pure) { samples_.emplace(sample, normalize(id, result)); heldSamples_.insert_or_assign(sample, normalize(id, result)); log(op, args); }
        } else if (kind == OperationKind::kAssign) result = a(0);
        else if (kind == OperationKind::kAdd) result = a(0) + a(1);
        else if (kind == OperationKind::kSub) result = a(0) - a(1);
        else if (kind == OperationKind::kMul) result = a(0) * a(1);
        else if (kind == OperationKind::kDiv) result = a(0) / a(1);
        else if (kind == OperationKind::kMod) result = a(0) % a(1);
        else if (kind == OperationKind::kAnd) result = a(0) & a(1);
        else if (kind == OperationKind::kOr) result = a(0) | a(1);
        else if (kind == OperationKind::kXor) result = a(0) ^ a(1);
        else if (kind == OperationKind::kXnor) result = ~(a(0) ^ a(1));
        else if (kind == OperationKind::kNot) result = ~a(0);
        else if (kind == OperationKind::kLogicNot) result = SVInt(!a(0));
        else if (kind == OperationKind::kLogicAnd) result = SVInt(a(0) && a(1));
        else if (kind == OperationKind::kLogicOr) result = SVInt(a(0) || a(1));
        else if (kind == OperationKind::kEq || kind == OperationKind::kCaseEq) result = SVInt(a(0) == a(1));
        else if (kind == OperationKind::kNe || kind == OperationKind::kCaseNe) result = SVInt(a(0) != a(1));
        else if (kind == OperationKind::kLt) result = SVInt(a(0) < a(1));
        else if (kind == OperationKind::kLe) result = SVInt(a(0) <= a(1));
        else if (kind == OperationKind::kGt) result = SVInt(a(0) > a(1));
        else if (kind == OperationKind::kGe) result = SVInt(a(0) >= a(1));
        else if (kind == OperationKind::kShl) result = a(0).shl(a(1));
        else if (kind == OperationKind::kLShr) result = a(0).lshr(a(1));
        else if (kind == OperationKind::kAShr) result = a(0).ashr(a(1));
        else if (kind == OperationKind::kReduceAnd) result = SVInt(a(0).reductionAnd());
        else if (kind == OperationKind::kReduceOr) result = SVInt(a(0).reductionOr());
        else if (kind == OperationKind::kReduceXor) result = SVInt(a(0).reductionXor());
        else if (kind == OperationKind::kMux) result = zero(a(0)) ? a(2) : a(1);
        else if (kind == sim::kBitSelect) result = (a(0) & a(1)) | (~a(0) & a(2));
        else if (kind == sim::kPrioritySelect) {
            const auto count = (args.size() - 1) / 2; result = a(args.size() - 1);
            for (std::size_t i = 0; i < count; ++i) if (!zero(a(i))) { result = a(count + i); break; }
        } else if (kind == OperationKind::kConcat || kind == OperationKind::kArrayCreate) {
            std::vector<SVInt> parts;
            for (std::size_t i = 0; i < args.size(); ++i) parts.push_back(a(kind == OperationKind::kConcat ? i : args.size() - i - 1));
            result = SVInt::concat(parts);
        } else if (kind == OperationKind::kReplicate) result = a(0).replicate(SVInt(static_cast<uint64_t>(sim::attribute<int64_t>(graph_, op, "rep").value())));
        else if (kind == OperationKind::kSliceStatic) result = a(0).slice(sim::attribute<int64_t>(graph_, op, "sliceEnd").value(), sim::attribute<int64_t>(graph_, op, "sliceStart").value());
        else if (kind == OperationKind::kSliceDynamic) result = a(0).lshr(a(1));
        else if (kind == OperationKind::kSliceArray) {
            uint64_t row = 0; const auto shape = graph_.valueShape(args[0]);
            for (std::size_t i = 1; i < args.size(); ++i) row = row * shape[i - 1] + a(i).as<uint64_t>().value();
            result = a(0).lshr(SVInt(row * graph_.valueWidth(id)));
        } else if (kind == sim::kMaskedOr) {
            result = SVInt(graph_.valueWidth(id), 0, graph_.valueSigned(id));
            if (const auto views = sim::attribute<std::vector<int64_t>>(graph_, op, "views")) {
                for (std::size_t i = 0; i < views->size(); i += 4)
                    if (!zero(a((*views)[i]).slice((*views)[i + 1], (*views)[i + 1])))
                        result |= a((*views)[i + 2]).lshr(SVInt(static_cast<uint64_t>((*views)[i + 3]))).trunc(graph_.valueWidth(id));
            } else for (int32_t i = 0; i < graph_.valueWidth(args[0]); ++i)
                if (!zero(a(0).slice(i, i))) result |= a(1).lshr(SVInt(static_cast<uint64_t>(i * graph_.valueWidth(id)))).trunc(graph_.valueWidth(id));
        } else throw std::runtime_error("Unsupported oracle op: " + std::string(grh::toString(kind)));
        result = normalize(id, result); cache_.insert_or_assign(id, result); return result;
    }
    void write(grh::OperationId op, std::unordered_map<grh::OperationId, SVInt, grh::OperationIdHash>& next) {
        const auto kind = graph_.opKind(op);
        const auto args = graph_.opOperands(op);
        const bool converted = sim::isWriter(kind), latch = kind == sim::kLatchWrite || kind == OperationKind::kLatchWritePort;
        const auto first = converted && !latch ? 1u : 0u;
        if (!fire(op) || zero(value(args[first]))) return;
        const auto target = graph_.findOperation(sim::attribute<std::string>(graph_, op, latch ? "latchSymbol" : "regSymbol").value());
        const auto type = grh::stateLogicalType(graph_, target);
        const bool indexed = kind == sim::kWriteArray || kind == sim::kWriteArrayMask || kind == OperationKind::kRegisterWriteArrayPort || kind == OperationKind::kRegisterWriteArrayMaskPort;
        const bool masked = latch || kind == sim::kWriteMask || kind == sim::kWriteArrayMask || kind == OperationKind::kRegisterWriteMaskPort || kind == OperationKind::kRegisterWriteArrayMaskPort;
        const bool fill = kind == sim::kFill || kind == OperationKind::kRegisterFillPort;
        const auto events = sim::attribute<std::vector<std::string>>(graph_, op, "eventEdge").value_or(std::vector<std::string>{}).size();
        const auto end = args.size() - events, dataIndex = end - (masked ? 2 : 1);
        int64_t offset = 0, width = type.width;
        if (indexed) {
            const auto rank = dataIndex - first - 1;
            for (std::size_t i = 0; i < rank; ++i) {
                const auto index = value(args[first + 1 + i]).as<uint64_t>();
                if (!index || *index >= static_cast<uint64_t>(type.shape[i])) return;
                width /= type.shape[i]; offset += *index * width;
            }
        }
        auto data = value(args[dataIndex]);
        if (fill && data.getBitWidth() == static_cast<unsigned>(type.leafWidth()))
            data = data.replicate(SVInt(static_cast<uint64_t>(type.width / type.leafWidth())));
        const auto mask = masked ? value(args[end - 1]).resize(width) : ~SVInt(width, 0, false);
        const auto old = next.at(target).slice(offset + width - 1, offset);
        next.at(target).set(offset + width - 1, offset, (old & ~mask) | (data.resize(width) & mask));
    }

    const grh::Graph& graph_;
    std::vector<std::string> probes_;
    std::unordered_map<grh::OperationId, SVInt, grh::OperationIdHash> state_;
    std::unordered_map<grh::ValueId, SVInt, grh::ValueIdHash> inputs_, cache_;
    std::unordered_map<grh::ValueId, SVInt, grh::ValueIdHash> pulses_;
    std::unordered_map<grh::OperationId, std::vector<SVInt>, grh::OperationIdHash> held_;
    std::unordered_map<grh::OperationId, std::vector<SVInt>, grh::OperationIdHash> taskPrevious_;
    std::map<std::string, bool> history_, pendingHistory_;
    std::map<std::string, SVInt> samples_;
    std::map<std::string, SVInt> heldSamples_;
    const sim::SinkSchedule* schedule_ = nullptr;
    std::string phase_;
    bool samplingEvents_ = false;
    std::set<grh::OperationId, bool(*)(grh::OperationId, grh::OperationId)> deferred_{[](auto a, auto b) { return a.index < b.index; }};
    std::unordered_set<grh::OperationId, grh::OperationIdHash> executed_;
    std::vector<std::string> effects_;
    uint64_t random_ = 417, evaluations_ = 0;
    std::size_t historyUpdates_ = 0;
};
} // namespace grhsim_test
#endif
