# XiangShan GrhSIM Emit Width Distribution and Wide-Op Lowering

> Historical measurements for the retired emitter. The counts and lowering
> below describe that version. Current storage/ABI and measurements are indexed
> in the [unified GRH simulation workflow](../grhsim.md).

This note summarizes how wide logic values appear in the current XiangShan GrhSIM emit flow, and how those wide operations are lowered into generated C++.

## Scope

- Data source: `build/xs/grhsim/wolvrix_xs_post_stats.json`
- Counting rule: only values with `type == "logic"`
- "Wide" in this document means result width `> 64` bits

This filter matters. The raw stats file also contains non-logic values such as `string`, and those can produce very large apparent widths that are not relevant to logic op lowering.

## Width Distribution

Observed logic-result totals in the XiangShan post-transform GRH:

- total logic results: `5,119,863`
- wide logic results (`> 64` bits): `42,461` (`0.829%`)
- unique logic widths: `344`
- maximum observed logic width: `79,263`

Width buckets:

| Width bucket | Count | Share |
| --- | ---: | ---: |
| `1` | 3,246,839 | 63.417% |
| `2-8` | 1,164,351 | 22.742% |
| `9-16` | 149,630 | 2.923% |
| `17-32` | 83,454 | 1.630% |
| `33-64` | 433,128 | 8.460% |
| `65-128` | 28,887 | 0.564% |
| `129-256` | 7,788 | 0.152% |
| `257-512` | 2,259 | 0.044% |
| `513-1024` | 3,166 | 0.062% |
| `1025-4096` | 214 | 0.004% |
| `>4096` | 147 | 0.003% |

The distribution is strongly skewed toward scalar and near-scalar logic. Wide logic exists, but it is a thin tail.

Most common exact widths:

| Width | Count |
| --- | ---: |
| `1` | 3,246,839 |
| `2` | 435,849 |
| `64` | 340,141 |
| `8` | 202,938 |
| `3` | 123,476 |
| `5` | 115,978 |
| `4` | 115,672 |
| `6` | 104,522 |
| `7` | 65,916 |
| `9` | 62,468 |

## Which Wide Ops Show Up Most

Among the `42,461` wide logic results, the most common defining op kinds are:

| Op kind | Count | Share among wide results |
| --- | ---: | ---: |
| `kConcat` | 10,691 | 25.18% |
| `kMux` | 5,951 | 14.02% |
| `kAssign` | 5,573 | 13.12% |
| `kShl` | 3,388 | 7.98% |
| `kOr` | 2,966 | 6.99% |
| `kSliceDynamic` | 2,908 | 6.85% |
| `kLShr` | 2,209 | 5.20% |
| `kAnd` | 2,133 | 5.02% |
| `kRegisterReadPort` | 1,813 | 4.27% |
| `kAdd` | 1,673 | 3.94% |

This is the key pattern in XiangShan:

- very wide values are mostly built by `kConcat`
- datapath manipulation is then dominated by `kMux`, shifts, slices, and bitwise ops
- arithmetic wide ops exist, but they are not the main source of very large widths

Largest observed logic results:

| Width | Kind | Name |
| --- | --- | --- |
| `79,263` | `kConcat` | `endpoint$in` |
| `65,536` | `kConcat` | `cpu$l_soc$core_with_l2$l2top$inner_busPMU_1$_GEN_112` |
| `32,768` | `kConcat` | `cpu$l_soc$core_with_l2$core$backend$inner_ctrlBlock$rob$_GEN_8227` |
| `25,600` | `kConcat` | `cpu$l_soc$core_with_l2$core$backend$inner_ctrlBlock$rob$_GEN_8224` |
| `19,208` | `kConcat` / `kAssign` | `gatewayIn_packed_38_bore` family |

## Storage Model For Wide Values

The current emitter uses two different wide-value storage models:

- all persistent wide values, state, shadows, and pending write buffers use `std::array<std::uint64_t, N>`
- `N = ceil(width / 64)`

Representative generated storage looks like:

```cpp
std::vector<std::array<std::uint64_t, 3>> value_words_3_slots_;
std::vector<std::array<std::uint64_t, 4>> state_logic_words_4_slots_;
std::vector<std::vector<std::array<std::uint64_t, 3>>> state_mem_words_3_slots_;
std::vector<std::array<std::uint64_t, 3>> state_shadow_words_3_slots_;
std::vector<std::array<std::uint64_t, 3>> memory_write_data_words_3_slots_;
```

So the current rule is:

- wide logic always stays on fixed-size word arrays
- wide ops are lowered directly to words helpers, without `_BitInt` conversion on the hot path

## How Wide Ops Are Lowered

The main lowering entry is `emitWordLogicOperation` in `wolvrix/lib/emit/grhsim_cpp.cpp`. Wide ops are emitted as direct words helper calls.

Representative lowering cases:

```cpp
case OperationKind::kAdd:
    emitLogicAssignFromWordsExpr(stream, graph, model, resultValue,
                                 binaryWordsBufferOpExpr(operandWords(0, resultWidth),
                                                         operandWords(1, resultWidth),
                                                         resultWidth,
                                                         "grhsim_add_words"));
    return true;

case OperationKind::kShl:
    emitLogicAssignFromWordsExpr(stream, graph, model, resultValue,
                                 shiftWordsBufferOpExpr(operandWords(0, resultWidth),
                                                        valueRef(model, operands[1]),
                                                        resultWidth,
                                                        "grhsim_shl_words"));
    return true;

case OperationKind::kConcat:
    emitLogicAssignFromWordsExpr(stream, graph, model, resultValue,
                                 concatWordsExpr(...));
    return true;
```

In practice, wide lowering falls into a small set of implementation patterns.

### 1. Arithmetic and Bitwise Ops

Wide arithmetic and bitwise ops run word-by-word on `uint64_t` arrays. For example, `grhsim_add_words` uses a carry chain across 64-bit words:

```cpp
inline void grhsim_add_words(const std::uint64_t *lhs,
                             std::size_t lhsWords,
                             const std::uint64_t *rhs,
                             std::size_t rhsWords,
                             std::size_t width,
                             std::uint64_t *out,
                             std::size_t outWords)
{
    unsigned __int128 carry = 0;
    for (std::size_t i = 0; i < outWords; ++i) {
        const unsigned __int128 lhsWord = i < lhsWords ? lhs[i] : 0;
        const unsigned __int128 rhsWord = i < rhsWords ? rhs[i] : 0;
        const unsigned __int128 sum = lhsWord + rhsWord + carry;
        out[i] = static_cast<std::uint64_t>(sum);
        carry = sum >> 64u;
    }
    grhsim_trunc_words_buffer(out, outWords, width);
}
```

There is also a template wrapper used by generated code:

```cpp
template <std::size_t N>
inline std::array<std::uint64_t, N> grhsim_add_words(
    const std::array<std::uint64_t, N> &lhs,
    const std::array<std::uint64_t, N> &rhs,
    std::size_t width)
{
    unsigned __int128 lhs128 = 0;
    unsigned __int128 rhs128 = 0;
    if (grhsim_try_u128_words(lhs, width, lhs128) &&
        grhsim_try_u128_words(rhs, width, rhs128)) {
        return grhsim_from_u128_words<N>(lhs128 + rhs128, width);
    }
    ...
}
```

So there are two tiers:

- widths that fit the helper's `u128` fast path can use compact arithmetic
- truly large widths fall back to explicit multi-word loops

### 2. Shifts

Wide shifts decompose the shift amount into `wordShift` and `bitShift`, then move data across word boundaries:

```cpp
inline void grhsim_shl_words(const std::uint64_t *value,
                             std::size_t valueWords,
                             std::size_t amount,
                             std::size_t width,
                             std::uint64_t *out,
                             std::size_t outWords)
{
    std::fill_n(out, outWords, UINT64_C(0));
    if (amount >= width) {
        return;
    }
    const std::size_t wordShift = amount / 64u;
    const std::size_t bitShift = amount & 63u;
    for (std::size_t i = outWords; i-- > 0;) {
        ...
    }
    grhsim_trunc_words_buffer(out, outWords, width);
}
```

The template form used in generated code converts the shift operand with `grhsim_index_words` and then performs the same word-level shift.

### 3. Concat and Replicate

Wide concatenation is built incrementally with `grhsim_insert_words`, which writes a source word array into a destination word array at a given LSB position:

```cpp
template <std::size_t DestN, std::size_t SrcN>
inline void grhsim_insert_words(std::array<std::uint64_t, DestN> &dest,
                                std::size_t destLsb,
                                const std::array<std::uint64_t, SrcN> &src,
                                std::size_t srcWidth)
{
    grhsim_clear_range_words(dest, destLsb, srcWidth);
    const std::size_t srcWords = (srcWidth + 63u) / 64u;
    const std::size_t destWord = destLsb / 64u;
    const std::size_t bitShift = destLsb & 63u;
    for (std::size_t i = 0; i < srcWords && i < SrcN; ++i) {
        ...
    }
}
```

There is also a special helper for "many equal-width scalar pieces":

```cpp
inline void grhsim_concat_uniform_scalars_words(const std::uint64_t *values,
                                                std::size_t count,
                                                std::size_t elemWidth,
                                                std::size_t totalWidth,
                                                std::uint64_t *out,
                                                std::size_t destWords)
{
    std::fill_n(out, destWords, UINT64_C(0));
    std::size_t cursor = totalWidth;
    for (std::size_t i = 0; i < count; ++i) {
        ...
        grhsim_insert_scalar_words_buffer(out, destWords, cursor, values[i], elemWidth);
    }
    grhsim_trunc_words_buffer(out, destWords, totalWidth);
}
```

This is why wide `kConcat` is the dominant wide-op pattern in XiangShan: it maps naturally to repeated insert operations into a packed word buffer.

### 4. Slice and Cast

Wide slices use source-word indexing plus cross-word merge when the slice start is not 64-bit aligned:

```cpp
inline void grhsim_slice_words(const std::uint64_t *src,
                               std::size_t srcWords,
                               std::size_t start,
                               std::size_t width,
                               std::uint64_t *out,
                               std::size_t destWords)
{
    std::fill_n(out, destWords, UINT64_C(0));
    const std::size_t srcWord = start / 64u;
    const std::size_t bitShift = start & 63u;
    ...
    grhsim_trunc_words_buffer(out, destWords, width);
}
```

Cast helpers use the same word-array representation and then truncate or sign-extend as needed.

## Generated XiangShan Code Examples

### Example 1: 548-bit concat

Generated in `build/xs/grhsim/grhsim_emit/grhsim_SimTop_sched_1069.cpp`:

```cpp
const auto local_value_... = ([&]() -> std::array<std::uint64_t, 9> {
    std::array<std::uint64_t, 9> next_words{};
    std::size_t concat_cursor = 548;
    concat_cursor -= 137;
    grhsim_insert_words(next_words, concat_cursor, local_value_..., 137);
    concat_cursor -= 137;
    grhsim_insert_words(next_words, concat_cursor, local_value_..., 137);
    concat_cursor -= 137;
    grhsim_insert_words(next_words, concat_cursor, local_value_..., 137);
    concat_cursor -= 137;
    grhsim_insert_words(next_words, concat_cursor, local_value_..., 137);
    return next_words;
}());
```

This is the standard wide-concat pattern: allocate destination words, walk a cursor from MSB toward LSB, and insert each operand.

### Example 2: 512-bit concat from 64 byte scalars

Generated in `build/xs/grhsim/grhsim_emit/grhsim_SimTop_sched_1001.cpp`:

```cpp
const auto next_words = ([&]() -> std::array<std::uint64_t, 8> {
    std::array<std::uint64_t, 8> out{};
    const auto values = std::array<std::uint64_t, 64>{ ... };
    grhsim_concat_uniform_scalars_words(values.data(), values.size(), 8, 512,
                                        out.data(), out.size());
    return out;
}());
```

This is an optimized form for uniform scalar concatenation.

### Example 3: 129-bit add

Generated in `build/xs/grhsim/grhsim_emit/grhsim_SimTop_sched_2322.cpp`:

```cpp
const auto local_value_... = ([&]() -> std::array<std::uint64_t, 3> {
    std::array<std::uint64_t, 3> out{};
    const auto lhs = local_value_...;
    const auto rhs = local_value_...;
    out = grhsim_add_words(lhs, rhs, 129);
    return out;
}());
```

This shows the template helper API that generated code uses for multi-word arithmetic.

### Example 4: 256-bit shift-left

Generated in `build/xs/grhsim/grhsim_emit/grhsim_SimTop_sched_1431.cpp`:

```cpp
const auto local_value_... = ([&]() -> std::array<std::uint64_t, 4> {
    std::array<std::uint64_t, 4> out{};
    const auto value = value_words_4_slots_[271];
    out = grhsim_shl_words(value, grhsim_index_words(value_u8_slots_[142567], 256), 256);
    return out;
}());
```

The shift amount is first normalized with `grhsim_index_words`, then the word-level shift helper is applied.

### Example 5: dynamic slice from a 4096-bit source

Generated in `build/xs/grhsim/grhsim_emit/grhsim_SimTop_sched_940.cpp`:

```cpp
const auto next_words = ([&]() -> std::array<std::uint64_t, 1> {
    std::array<std::uint64_t, 1> out{};
    const auto src = value_words_64_slots_[12];
    grhsim_slice_words(src.data(), src.size(),
                       grhsim_index_words(value_u16_slots_[33018], 4096),
                       1, out.data(), out.size());
    return out;
}());
```

The source is `64` words wide (`4096` bits), and the slice helper extracts the requested bit into a one-word result.

## Takeaways

- XiangShan logic widths are overwhelmingly small: `99.171%` of logic results are `<= 64` bits.
- Wide logic results are rare (`0.829%`), but they are real and sometimes very large.
- The dominant wide-op source is `kConcat`, not arithmetic.
- The wide-value implementation model is simple and uniform:
  - storage: `std::array<std::uint64_t, N>`
  - lowering: `emitWordLogicOperation`
  - execution: word-array runtime helpers
  - cleanup: every helper truncates back to the declared result width
- For moderate widths, runtime helpers can sometimes use a `u128` fast path.
- For truly large widths, GrhSIM falls back to explicit multi-word loops over `uint64_t` storage.

## Update: Current Supernode Implementation

The current supernode emitter stays on a single representation for wide values:

- storage uses `std::array<std::uint64_t, N>`
- supernode-internal wide temporaries also use `std::array<std::uint64_t, N>`
- wide ops lower directly to pure words helpers

### Single-user inline rule

Inside a supernode, if a non-materialized logic value has exactly one user, the emitter records its expression and substitutes that expression directly at the use site.

- single-user scalar locals inline as scalar expressions
- single-user wide locals inline as words helper expressions
- multi-user wide locals still get a named temporary, but that temporary is also a words value

### Helper boundary

The current hot-path helpers are all words-based, for example:

- `grhsim_cast_words<...>(...)`
- `grhsim_add_words(...)`
- `grhsim_shl_words(...)`
- `grhsim_slice_words<...>(...)`
- `grhsim_concat_words<...>(...)`
- `grhsim_replicate_words<...>(...)`

So state writes, task args, DPI wide inputs, and supernode-local wide ops all stay on the same array representation.

### Toolchain note

Because generated GrhSIM C++ no longer depends on `_BitInt(N)`, the emitter no longer needs a clang-only requirement for wide-value support.
