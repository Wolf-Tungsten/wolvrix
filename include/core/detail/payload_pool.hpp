#ifndef WOLVRIX_CORE_DETAIL_PAYLOAD_POOL_HPP
#define WOLVRIX_CORE_DETAIL_PAYLOAD_POOL_HPP

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace wolvrix::lib::grh::detail {

// Append-only scalar payload storage. Each list occupies one contiguous range;
// imported graph segments retain their buffers without copying their elements.
// Appending after a segment was shared starts a new, independently owned tail.
template <typename T>
class PayloadPool {
public:
    std::size_t size() const noexcept { return size_; }

    std::size_t append(std::span<const T> values) {
        std::vector<T> aliased;
        if (!segments_.empty() && segments_.back().values.unique() && !values.empty()) {
            const auto& tail = *segments_.back().values;
            const std::less<const T*> before;
            if (!tail.empty() && !before(values.data(), tail.data()) &&
                before(values.data(), tail.data() + tail.size())) {
                aliased.assign(values.begin(), values.end());
                values = aliased;
            }
        }
        return appendGenerated(values.size(), [&](std::size_t i) { return values[i]; });
    }

    template <typename Function>
    std::size_t appendGenerated(std::size_t count, Function&& element) {
        const std::size_t offset = size_;
        if (!count) return offset;
        if (segments_.empty() || !segments_.back().values.unique())
            segments_.push_back({size_, std::make_shared<std::vector<T>>()});
        auto& tail = *segments_.back().values;
        const std::size_t begin = tail.size();
        tail.resize(begin + count);
        try {
            for (std::size_t i = 0; i < count; ++i) tail[begin + i] = element(i);
        } catch (...) {
            tail.resize(begin);
            throw;
        }
        size_ += count;
        return offset;
    }

    std::size_t share(const PayloadPool& source) {
        if (&source == this) throw std::invalid_argument("cannot share a payload pool with itself");
        const std::size_t base = size_;
        segments_.reserve(segments_.size() + source.segments_.size());
        for (const auto& segment : source.segments_)
            segments_.push_back({base + segment.offset, segment.values});
        size_ += source.size_;
        return base;
    }

    std::span<const T> slice(std::size_t offset, std::size_t count) const noexcept {
        if (offset > size_ || count > size_ - offset || !count) return {};
        auto found = std::upper_bound(segments_.begin(), segments_.end(), offset,
            [](std::size_t index, const Segment& segment) { return index < segment.offset; });
        if (found == segments_.begin()) return {};
        --found;
        const std::size_t local = offset - found->offset;
        if (local > found->values->size() || count > found->values->size() - local) return {};
        return {found->values->data() + local, count};
    }

    template <typename Function>
    void forEachSegment(Function&& function) const {
        for (const auto& segment : segments_)
            function(segment.offset, std::span<const T>(*segment.values));
    }

    void shrink_to_fit() {
        for (auto& segment : segments_)
            if (segment.values.unique()) segment.values->shrink_to_fit();
        segments_.shrink_to_fit();
    }

private:
    struct Segment {
        std::size_t offset;
        std::shared_ptr<std::vector<T>> values;
    };
    std::vector<Segment> segments_;
    std::size_t size_ = 0;
};

} // namespace wolvrix::lib::grh::detail

#endif
