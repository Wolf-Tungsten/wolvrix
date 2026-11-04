#ifndef WOLVRIX_CORE_DETAIL_STRING_ARENA_HPP
#define WOLVRIX_CORE_DETAIL_STRING_ARENA_HPP

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace wolvrix::lib::grh::detail {

// Segmented bytes preserve string_view addresses as the graph grows. Each
// appended string has one owning copy; indexes can retain string_view keys.
class StringArena {
public:
    StringArena() = default;
    StringArena(const StringArena&) = delete;
    StringArena& operator=(const StringArena&) = delete;
    StringArena(StringArena&&) noexcept = default;
    StringArena& operator=(StringArena&&) noexcept = default;

    std::string_view append(std::string_view text) {
        if (text.empty()) return {};
        if (text.size() == std::numeric_limits<std::size_t>::max())
            throw std::length_error("graph string arena capacity");
        const std::size_t required = text.size() + 1;
        if (blocks_.empty() || !blocks_.back().bytes.unique() ||
            required > blocks_.back().capacity - blocks_.back().used) {
            const std::size_t previous = blocks_.empty() ? 2048 : blocks_.back().capacity;
            const std::size_t capacity = std::max(required,
                std::min(std::size_t{65536}, std::min(std::size_t{65536}, previous) * 2));
            blocks_.push_back(Block{std::make_shared<char[]>(capacity), capacity, 0});
        }
        Block& block = blocks_.back();
        char* destination = block.bytes.get() + block.used;
        std::memcpy(destination, text.data(), text.size());
        destination[text.size()] = '\0';
        block.used += required;
        return {destination, text.size()};
    }

    void retain(const StringArena& source) {
        if (this == &source) return;
        blocks_.insert(blocks_.end(), source.blocks_.begin(), source.blocks_.end());
    }

private:
    struct Block {
        std::shared_ptr<char[]> bytes;
        std::size_t capacity = 0;
        std::size_t used = 0;
    };

    std::vector<Block> blocks_;
};

} // namespace wolvrix::lib::grh::detail

#endif
