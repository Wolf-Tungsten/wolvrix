#include "core/allocator.hpp"

#if defined(WOLVRIX_BUNDLED_MIMALLOC_DEFAULTS)
#include <mimalloc.h>
#if defined(WOLVRIX_MIMALLOC_STATS)
#include <mimalloc-stats.h>
#endif

namespace {

struct AllocatorDefaults {
    AllocatorDefaults() noexcept {
        // Configure our private heap before business APIs can start workers.
        // Explicit MIMALLOC_GENERIC_COLLECT settings retain precedence.
        mi_option_set_default(mi_option_generic_collect, WOLVRIX_MIMALLOC_GENERIC_COLLECT);
    }
};

const AllocatorDefaults allocatorDefaults;

} // namespace
#endif

namespace wolvrix::lib::allocator {

Configuration configuration() noexcept {
#if defined(WOLVRIX_BUNDLED_MIMALLOC_DEFAULTS)
    return {true, true, mi_option_get_clamp(mi_option_generic_collect, 1, 1000000L)};
#elif defined(SLANG_USE_MIMALLOC)
    return {true, false, 0};
#else
    return {false, false, 0};
#endif
}

Statistics statistics() noexcept {
#if defined(WOLVRIX_MIMALLOC_STATS)
    mi_stats_merge();
    mi_stats_t stats{};
    mi_stats_get(sizeof(stats), &stats);
    const auto normal = static_cast<uint64_t>(stats.malloc_normal_count.total);
    const auto huge = static_cast<uint64_t>(stats.malloc_huge_count.total);
    return {true, normal + huge, normal, huge, stats.threads.current};
#else
    return {};
#endif
}

} // namespace wolvrix::lib::allocator
