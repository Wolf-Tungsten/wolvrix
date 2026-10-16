#ifndef WOLVRIX_TRANSFORM_MUX_TREE_FOLD_HPP
#define WOLVRIX_TRANSFORM_MUX_TREE_FOLD_HPP

#include "core/transform.hpp"

namespace wolvrix::lib::transform
{

    // Recover dynamic array accesses from the constant-index expansion that
    // firtool --preserve-aggregate produces for field-level dynamic accesses
    // (ExpandSubaccesses). Three patterns are folded per rank-1 array register:
    //
    //   read mux tree:   idx==c0 ? arr[c0] : (idx==c1 ? arr[c1] : ... arr[cN-1])
    //                    -> kRegisterReadArrayPort(arr, idx)   (full coverage)
    //   write decoder:   per-entry writes arr[e] <= (hitP ? dP : ... : arr[e])
    //                    with hitI = enI & (ptrI == e)
    //                    -> one kRegisterWriteArrayPort(enI, ptrI, dI) per source
    //                    (priority = chain nesting order, op declaration order),
    //                    const-default tails become a low-priority kRegisterFillPort
    //                    and uniform const reset writes a top-priority fill.
    //   padded read:     kSliceArray(concat(replicate(arr[p]), arr...), idx)
    //                    -> kRegisterReadArrayPort(arr, idx < N ? idx : p)
    //
    // With guardedWrites enabled (two-state only), also merge multi-write
    // guarded decoders after register-write-normalize. Whole/dynamic writes
    // remain ordered barriers; each recovered source must cover its complete
    // in-bounds address domain and admit a common priority order across rows.
    //
    // Rewritten cones are swept locally (dead ops removed even when their wire
    // values carry declared symbols — the fold retires those wires; runs after
    // xmr-resolve so no cross-module reference can point into them).
    class MuxTreeFoldPass : public Pass
    {
    public:
        explicit MuxTreeFoldPass(bool guardedWrites = false);

        PassResult run() override;

    private:
        bool guardedWrites_;
    };

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_MUX_TREE_FOLD_HPP
