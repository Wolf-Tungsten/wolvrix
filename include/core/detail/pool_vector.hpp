#ifndef WOLVRIX_CORE_DETAIL_POOL_VECTOR_HPP
#define WOLVRIX_CORE_DETAIL_POOL_VECTOR_HPP

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace wolvrix::lib::grh::detail {

// A vector-shaped mutable slice of a graph-owned arena. The arena must outlive
// every slice and must not be cleared while slices remain in use. Any mutation
// of the arena or one of its slices may invalidate all borrowed iterators.
template <typename T>
class PoolVector {
public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = T&;
    using const_reference = const T&;
    using iterator = T*;
    using const_iterator = const T*;

    PoolVector() = default;
    explicit PoolVector(std::vector<T>& pool) noexcept : pool_(&pool) {}
    explicit PoolVector(std::vector<T>* pool) noexcept : pool_(pool) {}

    PoolVector(const PoolVector& other) : pool_(other.pool_) {
        reserve(other.size_);
        for (size_type i = 0; i < other.size_; ++i) (*this)[i] = other[i];
        size_ = other.size_;
    }

    PoolVector& operator=(const PoolVector& other) {
        if (this == &other) return *this;
        if (!pool_) pool_ = other.pool_;
        assign(other.begin(), other.end());
        return *this;
    }

    PoolVector(PoolVector&& other) noexcept
        : pool_(other.pool_), offset_(other.offset_), size_(other.size_), capacity_(other.capacity_) {
        other.offset_ = other.size_ = other.capacity_ = 0;
    }

    PoolVector& operator=(PoolVector&& other) noexcept {
        if (this == &other) return *this;
        // The old slice becomes garbage. The arena retains object ownership.
        pool_ = other.pool_;
        offset_ = other.offset_;
        size_ = other.size_;
        capacity_ = other.capacity_;
        other.offset_ = other.size_ = other.capacity_ = 0;
        return *this;
    }

    PoolVector& operator=(const std::vector<T>& values) {
        assign(values.begin(), values.end());
        return *this;
    }

    void bind(std::vector<T>& pool) {
        if (pool_ == &pool) return;
        if (size_ || capacity_) throw std::logic_error("cannot rebind an allocated graph slice");
        pool_ = &pool;
    }

    [[nodiscard]] size_type size() const noexcept { return size_; }
    [[nodiscard]] size_type capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] size_type offset() const noexcept { return offset_; }
    [[nodiscard]] const std::vector<T>* backingPool() const noexcept { return pool_; }

    // Install a slice of an already sized arena. Bulk graph import allocates
    // each disjoint range once, then fills its elements without per-edge growth.
    void bindSlice(std::vector<T>& pool, size_type offset, size_type size) {
        if (offset > pool.size() || size > pool.size() - offset)
            throw std::out_of_range("graph bulk slice range");
        pool_ = &pool;
        offset_ = offset;
        size_ = capacity_ = size;
    }

    T* data() noexcept {
        return pool_ && !pool_->empty() ? pool_->data() + offset_ : nullptr;
    }
    const T* data() const noexcept {
        return pool_ && !pool_->empty() ? pool_->data() + offset_ : nullptr;
    }
    iterator begin() noexcept { return data(); }
    const_iterator begin() const noexcept { return data(); }
    const_iterator cbegin() const noexcept { return data(); }
    iterator end() noexcept { return size_ ? data() + size_ : data(); }
    const_iterator end() const noexcept { return size_ ? data() + size_ : data(); }
    const_iterator cend() const noexcept { return end(); }

    reference operator[](size_type index) noexcept { return (*pool_)[offset_ + index]; }
    const_reference operator[](size_type index) const noexcept { return (*pool_)[offset_ + index]; }
    reference at(size_type index) {
        if (index >= size_) throw std::out_of_range("graph slice index");
        return (*this)[index];
    }
    const_reference at(size_type index) const {
        if (index >= size_) throw std::out_of_range("graph slice index");
        return (*this)[index];
    }
    reference front() noexcept { return (*this)[0]; }
    const_reference front() const noexcept { return (*this)[0]; }
    reference back() noexcept { return (*this)[size_ - 1]; }
    const_reference back() const noexcept { return (*this)[size_ - 1]; }
    operator std::span<const T>() const noexcept { return {data(), size_}; }
    operator std::span<T>() noexcept { return {data(), size_}; }
    operator std::vector<T>() const { return size_ ? std::vector<T>(begin(), end()) : std::vector<T>{}; }

    void reserve(size_type requested) {
        if (requested <= capacity_) return;
        requirePool();
        const size_type doubled = capacity_ > std::numeric_limits<size_type>::max() / 2
            ? std::numeric_limits<size_type>::max() : capacity_ * 2;
        const size_type newCapacity = std::max(requested, std::max(size_type{1}, doubled));
        if (newCapacity > pool_->max_size() - pool_->size())
            throw std::length_error("graph slice capacity");
        if (capacity_ && offset_ + capacity_ == pool_->size()) {
            pool_->resize(offset_ + newCapacity);
            capacity_ = newCapacity;
            return;
        }
        const size_type newOffset = pool_->size();
        pool_->resize(newOffset + newCapacity);
        for (size_type i = 0; i < size_; ++i) {
            (*pool_)[newOffset + i] = std::move((*pool_)[offset_ + i]);
            (*pool_)[offset_ + i] = T{};
        }
        offset_ = newOffset;
        capacity_ = newCapacity;
    }

    void resize(size_type count) {
        if (count < size_) {
            for (size_type i = count; i < size_; ++i) (*this)[i] = T{};
        } else {
            reserve(count);
            for (size_type i = size_; i < count; ++i) (*this)[i] = T{};
        }
        size_ = count;
    }

    void resize(size_type count, const T& value) {
        const T saved(value);
        const size_type previous = size_;
        resize(count);
        for (size_type i = previous; i < count; ++i) (*this)[i] = saved;
    }

    template <std::input_iterator InputIt>
    void assign(InputIt first, InputIt last) {
        // Snapshot first: first/last may borrow this arena and reserve may
        // reallocate it. Assignment is the uncommon bulk-import path.
        std::vector<T> saved;
        for (; first != last; ++first) saved.push_back(*first);
        clear();
        reserve(saved.size());
        for (auto& value : saved) (*pool_)[offset_ + size_++] = std::move(value);
    }

    void assign(size_type count, const T& value) {
        const T saved(value);
        clear();
        reserve(count);
        for (; size_ < count; ++size_) (*pool_)[offset_ + size_] = saved;
    }

    void clear() noexcept {
        for (size_type i = 0; i < size_; ++i) (*this)[i] = T{};
        size_ = 0;
    }

    void push_back(const T& value) {
        T saved(value);
        reserve(size_ + 1);
        (*pool_)[offset_ + size_++] = std::move(saved);
    }
    void push_back(T&& value) {
        T saved(std::move(value));
        reserve(size_ + 1);
        (*pool_)[offset_ + size_++] = std::move(saved);
    }
    template <typename... Args>
    reference emplace_back(Args&&... args) {
        T saved(std::forward<Args>(args)...);
        reserve(size_ + 1);
        (*pool_)[offset_ + size_++] = std::move(saved);
        return back();
    }
    void pop_back() {
        if (!size_) throw std::out_of_range("empty graph slice");
        (*this)[--size_] = T{};
    }

    iterator insert(const_iterator position, const T& value) {
        const size_type index = positionIndex(position);
        T saved(value);
        reserve(size_ + 1);
        for (size_type i = size_; i > index; --i) (*this)[i] = std::move((*this)[i - 1]);
        (*this)[index] = std::move(saved);
        ++size_;
        return data() + index;
    }
    iterator insert(const_iterator position, T&& value) {
        const size_type index = positionIndex(position);
        T saved(std::move(value));
        reserve(size_ + 1);
        for (size_type i = size_; i > index; --i) (*this)[i] = std::move((*this)[i - 1]);
        (*this)[index] = std::move(saved);
        ++size_;
        return data() + index;
    }
    template <std::input_iterator InputIt>
    iterator insert(const_iterator position, InputIt first, InputIt last) {
        const size_type index = positionIndex(position);
        std::vector<T> saved;
        for (; first != last; ++first) saved.push_back(*first);
        if (saved.empty()) return size_ ? data() + index : data();
        reserve(size_ + saved.size());
        for (size_type i = size_; i > index; --i)
            (*this)[i + saved.size() - 1] = std::move((*this)[i - 1]);
        for (size_type i = 0; i < saved.size(); ++i) (*this)[index + i] = std::move(saved[i]);
        size_ += saved.size();
        return data() + index;
    }

    iterator erase(const_iterator position) {
        const size_type index = positionIndex(position);
        if (index >= size_) throw std::out_of_range("graph slice erase");
        return erase(data() + index, data() + index + 1);
    }
    iterator erase(const_iterator first, const_iterator last) {
        const size_type firstIndex = positionIndex(first);
        const size_type lastIndex = positionIndex(last);
        if (firstIndex > lastIndex) throw std::out_of_range("graph slice erase range");
        const size_type removed = lastIndex - firstIndex;
        if (!removed) return size_ ? data() + firstIndex : data();
        for (size_type i = firstIndex; i + removed < size_; ++i)
            (*this)[i] = std::move((*this)[i + removed]);
        resize(size_ - removed);
        return size_ ? data() + firstIndex : data();
    }

private:
    void requirePool() const {
        if (!pool_) throw std::logic_error("unbound graph slice");
    }
    size_type positionIndex(const_iterator position) const {
        if (!size_) {
            if (position != data()) throw std::out_of_range("graph slice iterator");
            return 0;
        }
        const auto index = static_cast<size_type>(position - data());
        if (index > size_) throw std::out_of_range("graph slice iterator");
        return index;
    }

    std::vector<T>* pool_ = nullptr;
    size_type offset_ = 0;
    size_type size_ = 0;
    size_type capacity_ = 0;
};

} // namespace wolvrix::lib::grh::detail

#endif
