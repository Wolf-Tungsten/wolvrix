#ifndef WOLVRIX_CORE_DETAIL_STRING_POOL_HPP
#define WOLVRIX_CORE_DETAIL_STRING_POOL_HPP

#include "core/detail/string_arena.hpp"
#include "slang/util/FlatMap.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace wolvrix::lib::grh::detail {

// Text identity is independent of SymbolId reservations. Symbols and payload
// records borrow the same bytes while maintaining their own semantic indexes.
template <typename Hasher = std::hash<std::string_view>>
class BasicStringPool {
public:
    using Id = uint32_t;

    BasicStringPool() {
        textById_.emplace_back();
        hashNext_.push_back(0);
    }

    // Zero is the empty text ID and also the miss result for nonempty text.
    Id lookup(std::string_view text) const {
        if (text.empty()) return 0;
        const auto found = idsByHash_.find(hasher_(text));
        if (found == idsByHash_.end()) return 0;
        for (Id id = found->second; id; id = hashNext_[id]) {
            if (textById_[id] == text) return id;
        }
        return 0;
    }

    Id intern(std::string_view text) {
        return internImpl(text, false);
    }

    // Retain the source arena's ownership and merge its text index once per
    // source definition. Source and target may subsequently grow independently.
    std::vector<Id> retain(const BasicStringPool& source) {
        if (this == &source) throw std::invalid_argument("cannot retain the same string pool");
        arena_.retain(source.arena_);
        std::vector<Id> remap(source.textById_.size(), 0);
        for (std::size_t i = 1; i < remap.size(); ++i)
            remap[i] = internImpl(source.textById_[i], true);
        return remap;
    }

private:
    Id internImpl(std::string_view text, bool retained) {
        if (text.empty()) return 0;
        // Hash text once. Integer table growth never hashes stored text again.
        auto [head, inserted] = idsByHash_.try_emplace(hasher_(text), 0);
        for (Id id = head->second; id; id = hashNext_[id]) {
            if (textById_[id] == text) return id;
        }
        const Id id = static_cast<Id>(textById_.size());
        try {
            if (textById_.size() >= uint32_t{0x80000000})
                throw std::length_error("graph string pool capacity");
            const auto stored = retained ? text : arena_.append(text);
            textById_.push_back(stored);
            try {
                hashNext_.push_back(head->second);
            } catch (...) {
                textById_.pop_back();
                throw;
            }
        } catch (...) {
            if (inserted) idsByHash_.erase(head);
            throw;
        }
        head->second = id;
        return id;
    }

public:

    std::string_view text(Id id) const noexcept {
        return id < textById_.size() ? textById_[id] : std::string_view{};
    }
    std::size_t size() const noexcept { return textById_.size() - 1; }
    void reserve(std::size_t count) {
        idsByHash_.reserve(count);
        textById_.reserve(count + 1);
        hashNext_.reserve(count + 1);
    }
    void compact() {
        textById_.shrink_to_fit();
        hashNext_.shrink_to_fit();
    }

private:
    [[no_unique_address]] Hasher hasher_;
    StringArena arena_;
    std::vector<std::string_view> textById_;
    std::vector<Id> hashNext_;
    slang::flat_hash_map<std::size_t, Id, std::hash<std::size_t>> idsByHash_;
};

using StringPool = BasicStringPool<>;

} // namespace wolvrix::lib::grh::detail

#endif
