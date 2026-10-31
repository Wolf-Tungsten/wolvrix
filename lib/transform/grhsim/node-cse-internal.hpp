#ifndef WOLVRIX_GRHSIM_NODE_CSE_INTERNAL_HPP
#define WOLVRIX_GRHSIM_NODE_CSE_INTERNAL_HPP

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wolvrix::lib::transform::grhsim::detail {

// Hashes index buckets; complete signatures establish equivalence. The injected
// hasher lets the regression exercise collisions without a production option.
class NodeTerms {
public:
    using Hasher = std::function<std::size_t(std::string_view)>;
    explicit NodeTerms(Hasher hash = std::hash<std::string_view>{}) : hash_(std::move(hash)) {}
    std::size_t intern(std::string text) {
        auto& bucket = buckets_[hash_(text)];
        for (const auto id : bucket) if (keys_[id] == text) return id;
        const auto id = keys_.size();
        keys_.push_back(std::move(text));
        bucket.push_back(id);
        return id;
    }
private:
    Hasher hash_;
    std::vector<std::string> keys_;
    std::unordered_map<std::size_t, std::vector<std::size_t>> buckets_;
};
}
#endif
