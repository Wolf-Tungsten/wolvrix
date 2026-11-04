#ifndef WOLVRIX_CORE_DETAIL_SHARED_COLUMN_HPP
#define WOLVRIX_CORE_DETAIL_SHARED_COLUMN_HPP

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace wolvrix::lib::grh::detail {

// Ordinary graph construction has one flat vector. Flatten may retain complete
// source buffers through a fixed-size page directory; indexing stays O(1).
// A mutable access detaches only the affected page, preserving source ownership.
template <typename T, std::size_t PageSize = 256>
class PagedColumn {
public:
    struct Stats { std::size_t sharedRows = 0, copiedRows = 0; };
    PagedColumn() = default;
    PagedColumn(const PagedColumn&) = delete;
    PagedColumn& operator=(const PagedColumn&) = delete;
    PagedColumn(PagedColumn&& other) noexcept { *this = std::move(other); }
    PagedColumn& operator=(PagedColumn&& other) noexcept {
        if (this == &other) return *this;
        flat_ = std::move(other.flat_);
        sharedFlat_ = std::move(other.sharedFlat_);
        pages_ = std::move(other.pages_);
        size_ = std::exchange(other.size_, 0);
        sharedRows_ = std::exchange(other.sharedRows_, 0);
        copiedRows_ = std::exchange(other.copiedRows_, 0);
        paged_ = std::exchange(other.paged_, false);
        return *this;
    }

    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return !size_; }
    void reserve(std::size_t count) {
        if (paged_) pages_.reserve((count + PageSize - 1) / PageSize);
        else { writableFlat(); flat_.reserve(count); }
    }
    void resize(std::size_t count) {
        if (!paged_) { writableFlat(); flat_.resize(count); size_ = count; return; }
        if (count < size_) {
            pages_.resize((count + PageSize - 1) / PageSize);
            size_ = count;
            return;
        }
        const auto old = size_;
        pages_.reserve((count + PageSize - 1) / PageSize);
        while (pages_.size() < (count + PageSize - 1) / PageSize)
            pages_.push_back({std::make_shared<std::vector<T>>(PageSize), 0, true});
        size_ = count;
        for (std::size_t i = old; i < count; ++i) writableAt(i) = T{};
    }
    template <typename... Args>
    T& emplace_back(Args&&... args) {
        T saved(std::forward<Args>(args)...);
        if (!paged_) {
            writableFlat();
            auto& item = flat_.emplace_back(std::move(saved));
            size_ = flat_.size();
            return item;
        }
        const auto index = size_;
        resize(size_ + 1);
        return writableAt(index) = std::move(saved);
    }
    void push_back(const T& value) { emplace_back(value); }
    void push_back(T&& value) { emplace_back(std::move(value)); }
    void shrink_to_fit() {
        if (paged_) pages_.shrink_to_fit();
        else { writableFlat(); flat_.shrink_to_fit(); }
    }
    const T& operator[](std::size_t index) const noexcept {
        if (!paged_) return sharedFlat_ ? (*sharedFlat_)[index] : flat_[index];
        const auto& page = pages_[index / PageSize];
        return (*page.storage)[page.offset + index % PageSize];
    }
    T& operator[](std::size_t index) { return writableAt(index); }
    const T& at(std::size_t index) const {
        if (index >= size_) throw std::out_of_range("graph column index");
        return (*this)[index];
    }
    T& at(std::size_t index) {
        if (index >= size_) throw std::out_of_range("graph column index");
        return writableAt(index);
    }
    T& writableAt(std::size_t index) {
        if (index >= size_) throw std::out_of_range("graph column index");
        if (!paged_) { writableFlat(); return flat_[index]; }
        auto& page = pages_[index / PageSize];
        if (!page.owned || page.storage.use_count() != 1) {
            auto copy = std::make_shared<std::vector<T>>(PageSize);
            const auto count = std::min(PageSize, size_ - (index / PageSize) * PageSize);
            std::copy_n(page.storage->begin() + page.offset, count, copy->begin());
            page = {std::move(copy), 0, true};
            copiedRows_ += count;
        }
        return (*page.storage)[index % PageSize];
    }

    void appendSharedRowsFrom(PagedColumn& source, std::span<const std::size_t> rows) {
        if (&source == this) throw std::invalid_argument("cannot share a column with itself");
        for (auto row : rows) if (row >= source.size_) throw std::out_of_range("graph column source row");
        if (rows.empty()) return;
        makePaged();
        pages_.reserve((size_ + rows.size() + PageSize - 1) / PageSize);
        std::size_t cursor = 0;
        while (cursor < rows.size()) {
            const auto local = size_ % PageSize;
            const auto count = std::min(PageSize - local, rows.size() - cursor);
            bool contiguous = local == 0 && count == PageSize;
            for (std::size_t i = 1; contiguous && i < count; ++i)
                contiguous = rows[cursor + i] == rows[cursor] + i;
            auto span = contiguous ? source.shareRange(rows[cursor], count) : Page{};
            if (span.storage) {
                span.owned = false;
                pages_.push_back(std::move(span));
                size_ += count;
                sharedRows_ += count;
            } else {
                if (!local) pages_.push_back({std::make_shared<std::vector<T>>(PageSize), 0, true});
                const auto base = size_;
                size_ += count;
                for (std::size_t i = 0; i < count; ++i)
                    writableAt(base + i) = std::as_const(source)[rows[cursor + i]];
                copiedRows_ += count;
            }
            cursor += count;
        }
    }
    Stats stats() const noexcept { return {sharedRows_, copiedRows_}; }
    void resetStats() noexcept { sharedRows_ = copiedRows_ = 0; }

private:
    struct Page {
        std::shared_ptr<std::vector<T>> storage;
        std::size_t offset = 0;
        bool owned = false;
    };
    void writableFlat() {
        if (!sharedFlat_) return;
        if (sharedFlat_.use_count() == 1) flat_ = std::move(*sharedFlat_);
        else flat_ = *sharedFlat_;
        sharedFlat_.reset();
    }
    Page shareRange(std::size_t begin, std::size_t count) {
        if (!paged_) {
            if (!sharedFlat_) sharedFlat_ = std::make_shared<std::vector<T>>(std::move(flat_));
            return {sharedFlat_, begin, false};
        }
        const auto& first = pages_[begin / PageSize];
        const auto offset = first.offset + begin % PageSize;
        for (std::size_t used = 0; used < count;) {
            const auto row = begin + used;
            const auto& part = pages_[row / PageSize];
            if (part.storage != first.storage || part.offset + row % PageSize != offset + used) return {};
            used += std::min(count - used, PageSize - row % PageSize);
        }
        return {first.storage, offset, false};
    }
    void makePaged() {
        if (paged_) return;
        if (!sharedFlat_) {
            // A flatten reservation can greatly exceed the root's live rows.
            // Retain only its live buffer rather than the unused reservation.
            if (flat_.capacity() > flat_.size() * 2) flat_.shrink_to_fit();
            sharedFlat_ = std::make_shared<std::vector<T>>(std::move(flat_));
        }
        std::vector<Page> pages;
        pages.reserve((size_ + PageSize - 1) / PageSize);
        for (std::size_t base = 0; base < size_; base += PageSize) {
            const auto count = std::min(PageSize, size_ - base);
            if (count == PageSize) pages.push_back({sharedFlat_, base, false});
            else {
                auto tail = std::make_shared<std::vector<T>>(PageSize);
                std::copy_n(sharedFlat_->begin() + base, count, tail->begin());
                pages.push_back({std::move(tail), 0, true});
            }
        }
        pages_ = std::move(pages);
        sharedFlat_.reset();
        paged_ = true;
    }
    std::vector<T> flat_;
    std::shared_ptr<std::vector<T>> sharedFlat_;
    std::vector<Page> pages_;
    std::size_t size_ = 0, sharedRows_ = 0, copiedRows_ = 0;
    bool paged_ = false;
};

} // namespace wolvrix::lib::grh::detail
#endif
