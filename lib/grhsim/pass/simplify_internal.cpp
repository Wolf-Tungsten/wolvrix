#include "simplify_internal.hpp"

#include <algorithm>

namespace wolvrix::lib::grhsim
{
    namespace
    {
        // Applies `mutator` to every slice of the given target kind whose old
        // target index has a valid entry in `mapping` (indexed by old index,
        // slot 0 unused). The mutator rewrites the slice in place and returns
        // false to drop it. Records are collected before upserting because
        // upsertDeclProvenance replaces entries inside the model's record list.
        template <typename IdT, typename Mutator>
        std::size_t mutateProvenanceSlices(GrhSimModel &model, DeclProvenanceTarget target,
                                           std::span<const IdT> mapping, Mutator &&mutator)
        {
            if (model.declProvenances().empty()) return 0;
            std::vector<DeclProvenance> updated;
            for (const auto &record : model.declProvenances())
            {
                DeclProvenance next = record;
                bool changed = false;
                std::size_t kept = 0;
                for (auto slice : record.slices)
                {
                    if (slice.target != target || slice.targetIndex >= mapping.size() ||
                        !mapping[slice.targetIndex].valid())
                    {
                        next.slices[kept++] = slice;
                        continue;
                    }
                    if (!mutator(slice, slice.targetIndex)) continue; // dropped
                    next.slices[kept++] = slice;
                    changed = true;
                }
                if (!changed) continue;
                next.slices.resize(kept);
                updated.push_back(std::move(next));
            }
            for (auto &record : updated) model.upsertDeclProvenance(std::move(record));
            return updated.size();
        }
    } // namespace

    std::size_t redirectProvenanceValueSlices(GrhSimModel &model,
                                              std::span<const ValueId> redirect,
                                              DeclProvenanceKind kind)
    {
        return mutateProvenanceSlices(model, DeclProvenanceTarget::Value, redirect,
                                      [&](DeclProvenanceSlice &slice, uint32_t oldIndex) {
                                          slice.targetIndex = redirect[oldIndex].index;
                                          if (slice.kind == DeclProvenanceKind::Direct) slice.kind = kind;
                                          return true;
                                      });
    }

    std::size_t redirectProvenanceStateSlices(GrhSimModel &model,
                                              std::span<const StateId> redirect,
                                              DeclProvenanceKind kind)
    {
        return mutateProvenanceSlices(model, DeclProvenanceTarget::State, redirect,
                                      [&](DeclProvenanceSlice &slice, uint32_t oldIndex) {
                                          slice.targetIndex = redirect[oldIndex].index;
                                          if (slice.kind == DeclProvenanceKind::Direct) slice.kind = kind;
                                          return true;
                                      });
    }

    std::size_t clampProvenanceValueSlices(GrhSimModel &model,
                                           std::span<const ValueId> replacement,
                                           std::span<const uint32_t> replacementWidth)
    {
        return mutateProvenanceSlices(model, DeclProvenanceTarget::Value, replacement,
                                      [&](DeclProvenanceSlice &slice, uint32_t oldIndex) {
                                          const uint64_t width = replacementWidth[oldIndex];
                                          slice.targetIndex = replacement[oldIndex].index;
                                          if (slice.width == 0) return true; // whole-object marker
                                          if (slice.targetOffset >= width) return false;
                                          slice.width = std::min(slice.width, width - slice.targetOffset);
                                          return true;
                                      });
    }

    std::size_t clampProvenanceStateSlices(GrhSimModel &model,
                                           std::span<const StateId> replacement,
                                           std::span<const uint32_t> replacementWidth)
    {
        return mutateProvenanceSlices(model, DeclProvenanceTarget::State, replacement,
                                      [&](DeclProvenanceSlice &slice, uint32_t oldIndex) {
                                          const uint64_t width = replacementWidth[oldIndex];
                                          slice.targetIndex = replacement[oldIndex].index;
                                          if (slice.width == 0) return true; // whole-object marker
                                          if (slice.targetOffset >= width) return false;
                                          slice.width = std::min(slice.width, width - slice.targetOffset);
                                          return true;
                                      });
    }

    std::size_t mergeProvenanceValueSlices(GrhSimModel &model,
                                           std::span<const ValueId> target,
                                           std::span<const uint64_t> targetBase,
                                           DeclProvenanceKind kind)
    {
        return mutateProvenanceSlices(model, DeclProvenanceTarget::Value, target,
                                      [&](DeclProvenanceSlice &slice, uint32_t oldIndex) {
                                          if (slice.width == 0) return false; // whole-object marker
                                          slice.targetIndex = target[oldIndex].index;
                                          slice.targetOffset += targetBase[oldIndex];
                                          if (slice.kind == DeclProvenanceKind::Direct) slice.kind = kind;
                                          return true;
                                      });
    }

    std::size_t mergeProvenanceStateSlices(GrhSimModel &model,
                                           std::span<const StateId> target,
                                           std::span<const uint64_t> targetBase,
                                           DeclProvenanceKind kind)
    {
        return mutateProvenanceSlices(model, DeclProvenanceTarget::State, target,
                                      [&](DeclProvenanceSlice &slice, uint32_t oldIndex) {
                                          if (slice.width == 0) return false; // whole-object marker
                                          slice.targetIndex = target[oldIndex].index;
                                          slice.targetOffset += targetBase[oldIndex];
                                          if (slice.kind == DeclProvenanceKind::Direct) slice.kind = kind;
                                          return true;
                                      });
    }

} // namespace wolvrix::lib::grhsim
