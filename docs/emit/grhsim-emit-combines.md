# GrhSIM Emit Combine Framework

> 历史 `grhsim-cpp` 方案。旧 emitter 已退役，正文保留设计背景；当前访问分析
> 由 [grhsim-cpu-st-cpp](grhsim-cpu-st-cpp.md) 私有实现，S9 优化另行评估。

## 背景

`grhsim-cpp` 目前主要按标准 GRH op 形态生成 C++。这种做法保持了 core GRH 的干净语义，但会把某些前端 lowering 形态原样带到仿真代码中。

`XsIcacheReplacerLarge` 暴露了一个典型问题：

```text
Chisel Vec register dynamic read
  -> FIRRTL dynamic Vec index
  -> SV no aggregate 下的 wide concat + dynamic slice
  -> GRH 中的 kConcat(register reads...) + kSliceArray/kSliceDynamic
  -> GrhSIM 生成 C++ 时先物化大 concat，再 slice
```

从硬件语义看，`concat + slice` 是合法表达；从仿真执行看，目标只是读取一组 scalar state 中的一个元素。GrhSIM 如果照字面物化 packed concat，会在读侧产生额外的大量 load、pack、slice 工作。

这个问题不是孤例。类似的后端机会还包括：

- 已知结构的 pack/unpack 被普通 wide helper 物化。
- slice/concat/mux 组合可以降成更直接的 scalar 或 state access。
- 某些 GRH 子图在仿真侧可以替换成 runtime helper、indexed access 或局部表达式。
- 一些中间 op 被替换后不应该继续发射代码。

因此需要先建立一个 GrhSIM emit 内部的 combine 框架，再在该框架上实现 scalar state array read 优化。这个框架不改变 core GRH IR，也不引入公开 GRH dialect；它是 GrhSIM emitter 的 target-specific pattern recognition 与 lowering 层。

## 目标

- 不修改 core GRH IR、JSON schema、`OperationKind` 枚举。
- 在 GrhSIM emit 内部建立可扩展的 pattern combine 框架。
- 形成明确的中间数据记录，描述：
  - 哪些 value 被特殊表达式替换。
  - 哪些 op 被特殊 statement 替换。
  - 哪些 op/value 因已被消费而不再发射。
  - 替换后的 activity dependency 和 materialization 约束。
- 支持多个 combine rule 独立注册、匹配、验证和提交。
- 支持冲突检测，避免两个 rule 同时拥有同一个 op/value。
- 先实现通用框架，再把 `concat(register/latch reads...) + indexed slice` 作为第一个 rule 接入。
- 允许将当前过大的 `grhsim_cpp.cpp` 拆成多个内部模块。

## 非目标

- 不把标量 `kRegister` / `kLatch` 合并成数组寄存器（kRegister + shape）。
- 不引入新的公开 GRH dialect。
- 不要求 ingest、transform pass 或其他 emitter 理解 GrhSIM 专用 combine。
- 不改变 SV emit 或 JSON store/load 行为。
- 不在第一版做跨 supernode 的复杂 code motion。
- 不在第一版做按动态 index 的精细 activity dependency；先采用保守依赖。

## 总体设计

GrhSIM emit 增加一层内部 lowering：

```text
Core GRH
  标准 RTL 语义图，不变。

EmitModel base build
  建立 ports/state/value/schedule/materialization 所需基础信息。

EmitCombine discovery
  各 combine rule 扫描 GRH，提出候选替换。

EmitCombine validation
  检查 ownership、side effect、materialization、waveform、dependency 等约束。

EmitCombine commit
  将通过验证的候选写入 EmitModel 的 combine overlay。

Codegen
  查询 combine overlay：
    - 对被替换 value 生成特殊表达式。
    - 对被替换 op 生成特殊 statement。
    - 对被 skip 的 op 不发射普通代码。
```

核心原则：

- matcher 只提出 candidate，不直接改 graph。
- validation 统一处理冲突和安全条件。
- commit 后 codegen 只看 combine overlay，不重新做复杂 pattern 判断。
- 未命中或验证失败的子图继续走现有普通 emit 路径。

## Emit 模块拆分

当前 `lib/emit/grhsim_cpp.cpp` 过大，combine 框架会进一步增加职责。建议把新框架放到独立模块，并逐步把现有 emit 职责迁出；不要求第一步一次性完成全量拆分。

建议目录：

```text
wolvrix/lib/emit/grhsim/
  grhsim_emit.cpp
  grhsim_model.hpp
  grhsim_model.cpp
  grhsim_combine.hpp
  grhsim_combine.cpp
  grhsim_combine_scalar_state_array.cpp
  grhsim_expr.hpp
  grhsim_expr.cpp
  grhsim_schedule_emit.cpp
  grhsim_state_emit.cpp
  grhsim_runtime_emit.cpp
  grhsim_writer.cpp
```

职责划分：

- `grhsim_emit.cpp`
  - 保留外部入口。
  - 编排 model build、combine、emit chunks。

- `grhsim_model.*`
  - `EmitModel`、state/value storage、ports、writes、waveform、materialized values。

- `grhsim_combine.*`
  - combine framework、candidate 数据结构、conflict validation、stats。

- `grhsim_combine_scalar_state_array.cpp`
  - 第一个具体 rule：scalar state array read。

- `grhsim_expr.*`
  - scalar/word expression lowering。
  - 查询 combine overlay 生成 value replacement expression。

- `grhsim_schedule_emit.cpp`
  - supernode/schedule op 发射。
  - 查询 op action，决定普通发射、replacement 发射或 skip。

- `grhsim_state_emit.cpp`
  - state init、commit、shadow、public port refresh。

- `grhsim_runtime_emit.cpp`
  - runtime helper/header emit。

- `grhsim_writer.cpp`
  - 文件切分、输出路径、limited stream、cleanup。

第一阶段可以采用混合形态：

- 保留现有 `grhsim_cpp.cpp` 作为外部入口和主要 codegen 文件。
- 新增 `grhsim_combine.*` 和具体 rule 文件。
- 在旧 emit 流程中调用 combine framework。
- 后续按职责逐步迁出 model、expression、schedule emit 和 runtime emit。

这样可以先建立框架并验证第一个优化，不把模块拆分和行为改动绑成一个高风险大改。

## 中间数据模型

### Combine Overlay

`EmitModel` 增加一个 `EmitCombineModel` 字段：

```cpp
struct EmitCombineModel {
    std::vector<EmitCombineRecord> records;

    std::unordered_map<ValueId, EmitValueReplacement, ValueIdHash> valueReplacementByValue;
    std::unordered_map<OperationId, EmitOpAction, OperationIdHash> opActionByOp;

    std::unordered_map<ValueId, uint32_t, ValueIdHash> valueOwnerByValue;
    std::unordered_map<OperationId, uint32_t, OperationIdHash> opOwnerByOp;

    EmitCombineStats stats;
};
```

含义：

- `records` 保存所有已提交 combine，便于 diagnostics 和后续 helper emit。
- `valueReplacementByValue` 描述某个 value 的表达式或 statement 如何替换。
- `opActionByOp` 描述某个 op 的 emit 动作。
- `valueOwnerByValue` / `opOwnerByOp` 用于冲突检测。
- `stats` 记录命中数量、回退原因和 suppression 数量。

### Op Action

不要只用 `suppressedOps` 一个集合。更通用的动作应区分三类：

```cpp
enum class EmitOpActionKind {
    Normal,
    Skip,
    EmitReplacement
};

struct EmitOpAction {
    EmitOpActionKind kind = EmitOpActionKind::Normal;
    uint32_t recordId = 0;
};
```

语义：

- `Normal`：走现有普通 emit。
- `Skip`：该 op 的计算已完全被其他 replacement 消费，不发射任何代码。
- `EmitReplacement`：该 op 不发射原始逻辑，而是发射 combine 提供的替代 statement。

例如 scalar state array read 中：

- 大 concat op 通常是 `Skip`。
- 只服务于该 concat 的 register read op 可以是 `Skip`。
- slice op 如果 result 需要 materialize 或 tracked change，应该是 `EmitReplacement`。
- slice op 如果 result 只被后续 inline expression 消费，可以是 `Skip`，由 `valueReplacementByValue` 处理。

### Value Replacement

value replacement 需要支持表达式和 statement 两种发射方式：

```cpp
enum class EmitValueReplacementKind {
    ScalarExpr,
    WordsExpr,
    CustomStatement
};

struct EmitValueReplacement {
    EmitValueReplacementKind kind = EmitValueReplacementKind::ScalarExpr;
    uint32_t recordId = 0;
    ValueId value{};
};
```

实际表达式不建议提前保存成字符串。它应由 codegen 阶段根据当前 `SupernodeLocalExprContext`、state alias、materialized storage 和 activation context 生成。

因此 `recordId` 指向 `EmitCombineRecord`，由 rule-specific emitter 生成：

```cpp
std::optional<std::string>
emitCombineScalarExpr(const Graph &graph,
                      const EmitModel &model,
                      const EmitCombineRecord &record,
                      ValueId value,
                      const SupernodeLocalExprContext *context);
```

### Combine Record

通用 record 保存 ownership 和调试信息；具体 payload 用 variant：

```cpp
enum class EmitCombineKind {
    ScalarStateArrayRead,
};

struct EmitCombineRecord {
    EmitCombineKind kind = EmitCombineKind::ScalarStateArrayRead;
    std::string ruleName;

    std::vector<OperationId> rootOps;
    std::vector<OperationId> consumedOps;
    std::vector<ValueId> consumedValues;
    std::vector<ValueId> replacedValues;

    EmitCombinePayload payload;
};
```

`rootOps` 是 pattern 的主 op，例如 concat。`consumedOps` 是被 combine 接管的 op，例如 concat、slice、只服务 concat 的 read port。`replacedValues` 是仍然保留语义、但由特殊 emit 生成的 value，例如 slice result。

## Candidate 生命周期

### 1. Discover

每个 rule 只读 `Graph` 和 base `EmitModel`，提出 candidate：

```cpp
class EmitCombineRule {
public:
    virtual std::string_view name() const noexcept = 0;

    virtual void discover(const Graph &graph,
                          const EmitModel &model,
                          EmitCombineCandidateSink &sink) const = 0;
};
```

discover 阶段不应：

- 修改 graph。
- 修改 materialized values。
- 写入 `opActionByOp`。
- 生成 C++ 字符串。

discover 阶段只记录：

- 匹配到哪些 op/value。
- 想替换哪些 value。
- 想 skip 或 replacement 哪些 op。
- 需要哪些额外依赖。
- 失败原因统计。

### 2. Validate

统一 validator 对 candidate 做安全检查：

- 同一个 op 只能被一个 committed record 拥有。
- 同一个 value 只能有一个 replacement owner。
- side-effect op 不能被 skip：
  - register/latch/memory write port。
  - memory fill。
  - system task。
  - DPIC call。
  - XMR write。
- public output、waveform、debug 强制观察的 value 不能被简单丢弃。
- 如果 value 已被 materialize，replacement 必须能生成 assignment statement。
- 如果 replacement 只能 inline expression，则该 value 不能跨 supernode 或 commit boundary。
- consumed op 的所有结果必须被 replacement 覆盖，或结果没有真实用户。
- consumed value 不能有未被 candidate 覆盖的普通用户。

validation 失败时，该 candidate 放弃，原图继续走普通 emit。

### 3. Commit

commit 阶段把 candidate 转为 `EmitCombineRecord`，并填充 overlay：

```text
opActionByOp[op] = Skip / EmitReplacement
valueReplacementByValue[value] = replacement
opOwnerByOp[op] = recordId
valueOwnerByValue[value] = recordId
records.push_back(record)
```

commit 后 codegen 不再重新跑 matcher。

## 与 materialization 的关系

combine 框架必须和 materialized value 规划协同，而不是简单把 value 从 storage 中删掉。

建议流程：

```text
1. build base EmitModel: states, ports, writes, schedule ids.
2. discover combine candidates.
3. compute protected values:
   - inputs/outputs/inouts
   - waveform values
   - boundary fanout values
   - commit operands
   - non-logic / side-effect results
4. validate and commit combines:
   - protected replaced value 允许存在，但必须有 replacement statement。
   - protected consumed-only value 不允许被删除。
5. choose materializedValues:
   - replaced value 如果 persistent，仍然 materialize。
   - consumed-only suppressed value 不进入 materialized storage。
6. codegen:
   - materialized replaced value 发射 replacement assignment。
   - local replaced value 可 inline replacement expression。
```

需要区分两类 value：

- `replacedValues`：语义仍存在，只是由特殊 codegen 产生。
- `consumedValues`：中间值被 combine 吃掉，不再作为独立值发射。

`_GEN_175` 这类大 concat 属于 `consumedValues`。slice result 属于 `replacedValues`。

## Codegen Hook

### Expression Resolution

`resolvedScheduleValueExpr()` 或其下层应先查询 combine overlay：

```text
if value has ScalarExpr replacement:
    return emit combine scalar expression
else:
    return existing valueRef/local expr path
```

这用于非 materialized local value 的 inline 替换。

### Operation Emission

schedule op emit 前查询 `opActionByOp`：

```text
Normal:
  existing emit path

Skip:
  emit nothing

EmitReplacement:
  dispatch to combine replacement statement emitter
```

replacement statement emitter 必须复用现有 assignment helpers，例如 scalar assignment 的 change detect 和 activation propagation，不能绕过 tracked-change 语义。

### Activity Dependency

combine record 可以声明额外依赖：

```cpp
struct EmitCombineDependency {
    std::vector<ValueId> valueDeps;
    std::vector<std::string> stateDeps;
};
```

第一版只做保守处理：被替换 op 所在 supernode 继续保留原有 scheduling；如果 replacement 需要额外 state dependency，必须把对应 state 加入 activation fanout。

对于 scalar state array read：

```text
direct_state_array_read(view, idx)
  依赖 idx value
  依赖 view 内所有 state members
```

后续可以再做常量 index 精细化。

## 首个 Rule：Scalar State Array Read

### 目标 Pattern

第一版只匹配最小必要形态：

```text
read_0  = kRegisterReadPort(reg_0)
read_1  = kRegisterReadPort(reg_1)
...
read_N  = kRegisterReadPort(reg_N)

packed  = kConcat(read_N, ..., read_1, read_0)
elem    = kSliceArray(packed, index)          // elemWidth == reg width
```

也支持等价的 dynamic slice：

```text
start = index * elemWidth [+ constant row offset]
elem  = kSliceDynamic(packed, start)
```

匹配成功后，GrhSIM emit 内部把 `elem` 看成：

```text
direct_state_array_read([reg_0, reg_1, ..., reg_N], index)
```

### Payload

```cpp
struct ScalarStateArrayView {
    std::vector<std::string> stateSymbols;
    int32_t elemWidth = 0;
    bool lsbFirst = true;
    bool isLatch = false;

    bool contiguousStorage = false;
    std::size_t baseSlot = 0;
    std::size_t strideBytes = 0;
};

struct DirectStateArrayRead {
    uint32_t viewId = 0;
    ValueId index{};
    int64_t rowOffset = 0;
    bool reverseIndex = false;
    int32_t indexDomainSize = 0;
};

struct ScalarStateArrayReadPayload {
    ScalarStateArrayView view;
    std::unordered_map<ValueId, DirectStateArrayRead, ValueIdHash> readByResult;
};
```

### 匹配规则

第一版保守匹配：

1. `kConcat` result 至少有一个用户。
2. `kConcat` 的所有 operand 都来自同类 state read：
   - 全部是 `kRegisterReadPort`，或
   - 全部是 `kLatchReadPort`。
3. 每个 read port 目标 state 存在于 `model.stateBySymbol`。
4. 所有成员 state 都是 logic scalar width。
5. 所有成员 state width 相同，且等于 slice element width。
6. concat operand 顺序可以明确映射到 element index。
7. concat result 的所有用户都必须是兼容的 element read：
   - `kSliceArray(concat, idx)` 且 `sliceWidth == elemWidth`
   - 或 `kSliceDynamic(concat, start)` 且 `start` 可解析为 `idx * elemWidth + constant`
8. slice result 的 bit width 必须等于 `elemWidth`。
9. concat result 没有其他普通用户。

不满足任一条件则放弃该 concat，走现有 emit 路径。

### Index 语义

`kSliceArray` 语义是：

```text
result = packed[index * elemWidth +: elemWidth]
```

因此 matcher 需要从 concat 顺序导出：

```text
index 0 -> packed LSB element -> stateSymbols[0]
index 1 -> next element       -> stateSymbols[1]
...
```

对 `kSliceDynamic`，只支持可安全解析的 start：

```text
index * elemWidth
index * elemWidth + constant
(index + constant) * elemWidth
```

如果表达式更复杂，第一版不匹配。

### 代码生成策略

第一版优先使用 `switch`，不依赖 state storage 连续性：

```cpp
([&]() -> std::uint8_t {
    const auto grhsim_idx = static_cast<std::uint64_t>(idx_expr) & 63u;
    switch (grhsim_idx) {
    case 0: return static_cast<std::uint8_t>(state_ref_repl_0);
    case 1: return static_cast<std::uint8_t>(state_ref_repl_1);
    ...
    default: return std::uint8_t{};
    }
})()
```

后续可在 matcher 中证明以下条件时启用 fast path：

- 所有 state 都在 `state_logic_storage_`。
- storage slot 按 index 顺序连续。
- element scalar kind 相同。
- stride 固定。

则生成：

```cpp
grhsim_value_storage_ref<ElemCppType>(
    state_logic_storage_,
    baseSlot + normalizedIndex * strideBytes)
```

第一版不要求实现连续 storage fast path。

## 未来 Combine Rule

框架建立后，可以继续添加其他 rule：

- `concat(slice(x, ...), slice(x, ...))` 重组为直接 bit range 或 cast。
- `slice(concat(...), constant-range)` 直接选 operand 或 operand slice。
- `mux(index == c, state_a, state_b)` 结合 index-domain 降低分支。
- `wide concat of uniform scalars` 生成更紧凑 helper 或 table-driven helper。
- `pack -> unpack` 在局部范围内消除中间 packed value。
- 常见 one-hot select 形态降成 indexed read 或 masked access。

这些 rule 都应走同一个 candidate/validate/commit/codegen 框架，而不是在普通 expression lowering 中散落特例判断。

## Diagnostics

建议增加可选统计：

```text
WOLVRIX_GRHSIM_EMIT_COMBINE_STATS=1
```

输出内容：

- 每个 rule 的 candidate 数量。
- 每个 rule 的 committed 数量。
- replacement value 数量。
- `Skip` op 数量。
- `EmitReplacement` op 数量。
- fallback reason top N。

scalar state array read 的 fallback reason 至少包括：

- non-state-read concat operand
- mixed state kind
- width mismatch
- concat has non-slice user
- unsupported dynamic slice start
- value forced materialized but rule has no statement emitter
- ownership conflict

## 实施步骤

1. 新增 GrhSIM combine 模块：
   - `lib/emit/grhsim/grhsim_combine.hpp`
   - `lib/emit/grhsim/grhsim_combine.cpp`
   - `lib/emit/grhsim/grhsim_combine_scalar_state_array.cpp`

   这一阶段不要求把 `grhsim_cpp.cpp` 全量拆开，只把新框架放进新模块。

2. 在 combine 模块中定义：
   - `EmitCombineModel`
   - `EmitCombineRecord`
   - `EmitCombineCandidate`
   - `EmitOpAction`
   - `EmitValueReplacement`
   - stats 和 fallback reason

3. 在 `EmitModel` 中加入 `EmitCombineModel combines`。

4. 在 build model 流程中加入 combine lifecycle：

```text
build base model
discover candidates
validate candidates
commit combines
plan materialized values
emit code
```

5. 在 expression resolution 中接入 value replacement hook。

6. 在 schedule op emit 中接入 op action hook。

7. 添加 diagnostics 环境变量。

8. 实现 `ScalarStateArrayReadCombineRule`：
   - 先支持 `kConcat -> kSliceArray`
   - 再支持 `kSliceDynamic`
   - 第一版使用 switch lowering

9. 增加 focused tests。

10. 在 `XsIcacheReplacerLarge` 上严格重测性能。

11. 在框架稳定后逐步拆分 `grhsim_cpp.cpp`：
   - 先迁出 combine-independent helper。
   - 再迁出 expression lowering。
   - 最后迁出 schedule/state/runtime emit。

## 测试计划

### 框架测试

新增 emit 侧 fixture，覆盖：

- 两个 candidate 争夺同一个 op/value 时只能 commit 一个。
- side-effect op 不能被 skip。
- protected value 如果没有 statement replacement，candidate 必须失败。
- `Skip` op 不出现在生成代码中。
- `EmitReplacement` op 发射替代 statement。
- 未匹配 graph 的生成代码保持普通路径。

### Scalar State Array Read 测试

覆盖：

- 4 个 scalar register concat 后 `kSliceArray`。
- concat result 有非 slice user 时不匹配。
- width mismatch 时不匹配。
- mixed register/latch 时不匹配。
- `kSliceDynamic(index * width)` 能匹配。
- unsupported `kSliceDynamic` start 回退普通 emit。

测试重点不只是生成成功，还要检查生成 C++ 中：

- 不再出现对应的大 concat materialization。
- slice result 使用 direct state read helper/lambda。
- 未匹配 case 保持原行为。

### 回归测试

运行现有 CTest：

```bash
ctest --test-dir wolvrix/build --output-on-failure -R emit
```

必要时补充 HDLBits GrhSIM smoke：

```bash
make run_hdlbits_grhsim DUT=071 SKIP_PY_INSTALL=1
```

### 性能验证

在 `testcase/xs-components` 对 `XsIcacheReplacerLarge` 重测：

```bash
make -C testcase/xs-components \
  CASE=XsIcacheReplacerLarge \
  BUILD_DIR=build-strict-noagg-nomerge \
  BENCH_REPEAT=5 \
  BENCH_VECTORS=100000 \
  one
```

对比：

- GrhSIM wall time。
- vectors/s。
- `perf stat` 的 instructions、cycles、branches、branch-misses。
- 每个 combine rule 的 committed 数量。
- 生成代码中 matched concat/slice 数量。

预期读侧收益来自：

- 消除大 concat 物化。
- 消除 concat 后的 dynamic slice helper。
- 把 victim dynamic read 降成对 scalar state group 的直接 indexed read。

## 风险与开放问题

- 框架引入后，materialization、activity schedule、expression inline 之间的边界必须清楚，否则容易出现“value 被替换但仍被普通路径使用”的错误。
- `Skip` 和 `EmitReplacement` 不能混用混乱；op action 必须单点决策。
- 如果 waveform/debug 强制观察 concat 中间值，不能 suppress concat。
- dynamic slice start 的解析必须严格，不能把 bit index 和 element index 混淆。
- concat operand order 必须用 GRH 的 LSB/MSB 语义确认，避免 index 反向。
- `switch` lowering 可能增加代码体积；如果同一个 view 有多个 read，应考虑生成 shared helper。
- 连续 storage fast path 要等第一版语义稳定后再做。
- 模块拆分应保持外部 emit API 不变，避免一次性重构影响测试面过大。

## 成功标准

- core GRH IR 和 JSON schema 零变化。
- GrhSIM emit 拥有通用 combine overlay，而不是单个 hard-coded 特例。
- 支持 value replacement、op replacement、op skip 三类动作。
- 未匹配设计生成代码不变或等价。
- 匹配 case 不再生成对应大 concat 物化。
- `XsIcacheReplacerLarge` 在 no vec aggregate、no scalar-reg merge 条件下 GrhSIM 性能明显改善。
- 所有相关 correctness tests 通过，checksum 与优化前一致。
