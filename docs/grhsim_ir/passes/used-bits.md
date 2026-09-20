# `grhsim.used-bits` / `grhsim.used-bits-analyze`

反向"实际使用位"（used-bits）不动点分析，以及由它驱动的两种语义恢复：死锥消除与
宽度收窄。形态参照 gsim 的 `usedBits`（`reference/gsim/src/usedBits.cpp`）：仿真可观测的
只有 sink（输出、DPI/system 实参、状态写口、事件），高位不可观测的 logic 值可以按更窄的
类型重新表达。

`grhsim.used-bits-analyze` 只运行分析并打印计数（`dead_values` / `narrowable_values` /
`downgrades` / `wide_words_before/after` / `dead_states` / `narrowable_states`），不修改模型。

## 分析规则

对每个二态 `core.logic` value 和状态计算前缀使用位 `used`（含义：只有位 `[0, used)` 被
任何 sink 观测）。`used == 0` 即死锥，`used == width` 即全使用。规则按 op kind 反向传播：

- sink 全使用：`output.write`、`system.function/task`、`dpi.call`、全部 memory（array）
  端口与读写地址、`input.read` 结果、event/条件 operand。
- 截断透明族（`assign`、`and/or/xor/xnor/not`、`add/sub/mul`、`shl` 的数据 operand、
  `mux`/`bitSelect` 两臂、`prioritySelect` 臂与默认值、`concat` 按原始布局区间、
  `replicate` 取 `min(k, 源宽)`、`sliceStatic` 取 `start+k`）：operand 继承结果的
  `used` 前缀。左移量、条件 operand 全使用。
- 非透明族（`div/mod/lshr/ashr`、比较、归约、`logicAnd/Or/Not`、`sliceDynamic/
  sliceArray`）：operand 全使用（结果的低位依赖 operand 的全部位）。
- `state.read` 把结果的 `used` 并入状态的 `usedState`；`regWrite/latchWrite` 把
  `data`/`mask` operand 提升到状态的写口需求：**可收窄状态**（见下）为 `usedState`，
  不可收窄状态一律按 operand 全宽——否则存活写口会引用已被收窄的值。

状态的结构收窄资格（与不动点无关，先于分析计算）：logic 二态、init 全为
const/random、只以 read/regWrite/latchWrite 目标身份被引用、写口 data/mask 与状态
等宽。event history 身份被引用的状态不满足资格。

前缀性质保证：收窄一个值时，其全部消费者都只需要低 `[0, k)` 位，因此边界处只需截断
（`sliceStatic`），永不需要扩展。

## 变换动作

1. **收窄重建（Case A）**：截断透明族的 op 用更窄结果类型重建；operand 按需经
   `sliceStatic` 截到目标宽度（标量且 cast 可表达时直接复用，宽值一律显式 slice 以满足
   单字数 array helper 的等宽假设）。`concat` 只保留切入位以下的 operand 并截断跨界者；
   `replicate` 在 `k <= 源宽` 时退化为 slice、整倍数时改写 `rep`；常量保留原字面量
   （emit 内联时按新宽度截断）。
2. **边界 slice（Case B）**：非透明 op 保持原宽，结果经复用已有或新建的
   `sliceStatic(0, k-1)` 供下游收窄锥使用。
3. **状态收窄**：logic 二态状态（init 全为 const/random、引用者仅为 read/regWrite/
   latchWrite、写口 data/mask 与状态等宽）连同读、写、init 一起重建为窄宽度；event
   history 引用不变。
4. **死锥消除**：结果全死的 `core.compute.*` / `state.read` / `memRead`，以及
   `usedState == 0` 状态的 `regWrite/latchWrite`；最后清扫不再被任何存活 op 引用的
   logic 二态状态（含孤儿 event history）。system/dpi/memory 写口等副作用 op 不删除。

变换后所有存活形态仍走既有 emit 路径：`sliceStatic`、`assign` 与各算术/位运算 op 的
标量/宽值发射对 operand 与 result 宽度一致或截断关系均有既定处理；mapping 因语义版本
递增失效，由后续 `cpu.st.*` 全量重建。

pass 幂等：收窄后 `used == width`，边界 slice 复用已有同形 op，第二次运行不产生变化。
