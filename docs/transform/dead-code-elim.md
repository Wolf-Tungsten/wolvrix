# dead-code-elim

## 功能概述

`dead-code-elim` 删除不能从可观察边界反向到达的 operation 和 value。它既可以由
`simplify` 调用，也可以作为独立 pass 使用。FIRRTL/mixed 流程按
`comb-loop-elim → split-static-array → dead-code-elim` 独立执行一次，不把该次调用混入
`simplify` 的迭代统计。

实现位于 `lib/transform/dead_code_elim.cpp`，公开入口为
`include/transform/dead_code_elim.hpp`，pass factory 名称为 `dead-code-elim`。

## op 级活跃性分析

本 pass 不做 GSIM 式 Node/ENode 分析。它从根开始，沿真实 op/value 依赖做反向可达性传播，
到达不动点后删除未标记的 op。主要根包括：

- 输入、输出和 inout 的 in/out/oe 值；
- `keep_declared_symbols=True` 时的声明 value 和声明状态；
- `kInstance`、`kBlackbox`；
- `kSystemFunction`、`kSystemTask`；
- `kDpicImport`、`kDpicCall`；
- `kXMRWrite`。

register/latch 的声明、读口和写口还按 `regSymbol` / `latchSymbol` 组成状态族。
状态族一旦活跃，其声明及全部写口保持活跃，写口的数据、使能、索引和事件依赖继续参与反向传播。
因此，独立 DCE 的保活结果不只取决于单个读结果是否仍有用户。

独立 DCE 默认使用 `keep_declared_symbols=False`，与 GSIM 一样不把声明/DontTouch 锚点作为额外保活根；
显式设置为 True 时仍恢复声明保护。其他 pass 的默认声明策略不因该默认值改变。SemanticId、旧 node 属性和
来源路径都不是保活根，不能为了保留来源而增加 `kAssign` 或其他计算载体。

## 位区间需求

活跃性同时保存 root reason mask 和半开位区间集合。mask 记录“为何保活”，区间记录
“哪些位被需求”；全位需求用内联 `allBits` 标志表示。正位宽 value 的空需求不传播
root reason。零位宽根仍保持原有结构保活行为。

- `kAssign` 将需求原样传给源值；`kMux` 将数据需求传给两个分支，条件保持全位依赖；
- `kSliceStatic` 把结果局部区间加上切片起点，映射到源值；常量索引的 `kSliceArray`
  采用相同方向的偏移，未知索引保守需求全部源值；
- `kConcat` / `kArrayCreate` 先求操作数在结果中的平坦区间，再求交并减去该操作数起点；
- 状态族的需求继续传给写口数据；使能、索引和事件操作数保持全位依赖。

状态族需求使用整个状态的平坦位坐标；indexed read 的结果和 indexed write 的 data
则使用所选行的局部坐标。当前不在这两者之间细化索引映射：
`kRegisterReadArrayPort` 的任意非空需求保守提升为整个状态需求；
`kRegisterWriteArrayPort` / `kRegisterWriteArrayMaskPort` 的数据、mask 和其他操作数
保持全位依赖；`kRegisterFillPort` 的广播元素数据同样保持全位依赖。
常量和动态索引都采用此策略，避免把局部低位误认为状态低位，或把
状态高位直接截到较窄 data 上并删掉其定义。普通全状态读写仍保留区间精度。

例如 `arrayCreate(e0,e1,e2)` 的元素宽度均为 8 时，结果 `[0,8)` 来自 e0，
`[8,16)` 来自 e1，`[16,24)` 来自 e2。需求 `[8,16)` 只向 e1 传播局部 `[0,8)`。
`concat(e2,e1,e0)` 具有相同位图，但操作数顺序相反；concat 通过一次反向遍历维护
低位偏移，部分需求的映射为线性复杂度，全位需求直接遍历全部操作数。

同一 value、op 或状态族的位需求可能在 reason mask 不再增长后继续扩大。此时新位需求
仍携带已有累计 root reason，否则后来才触达的源 operation 会被误删。回归覆盖分别需求
每个数组元素、全数组需求，以及同一 port reason 经延迟路径到达的不相交区间；后者
同时覆盖状态读口、状态族与写口数据传播。
另有常量/动态索引、带/不带 mask 的回归交叉覆盖 indexed read + whole write 和
whole read + indexed write，检查两种坐标方向都不会丢失必须的数据定义。
广播 fill 也覆盖高位状态需求，防止把整个状态的位区间误当成一个元素的局部需求。

静态数组索引与 `split-static-array` 共享私有 `constant_index.hpp` 解析入口：
使用 `slang::SVInt` 读取字面量，先按常量 producer 的 width/signedness 定宽，再按
最终等宽 `kAssign` 视图解释 signedness；最多穿过 15 个 assign（常量在第 16 层内）。
未知位、负索引、无法表示为 uint64 的值和不支持的 producer 保守视为动态索引。
例如 `4'b1_0` 是 2；`8'h12` 放入 2-bit value 后也是 2；signed 8-bit producer
的 `4'hf` 先扩为 8'hff，再经 unsigned assign 得到 255，不能误选元素 15。

## 删除顺序

1. 遍历 op，记录 kind、operands、results 和状态符号；
2. 标记端口、声明和效果根，传播 op/value 及状态族活跃性；
3. 删除未标记的 op，保留旧 `firrtl.node.*` 元数据所需的 `DeadRootBatch` 行为；
4. 删除无用户、无有效定义且非端口、非声明保护的 value；
5. 回收不再被存活 op/value 引用的 SemanticTree 分支。

`eraseOpUnchecked` / `eraseValueUnchecked` 只用于已经由活跃性分析证明可删除的实体。
Graph compaction 是独立的后续步骤，不属于本 pass 的活跃性分析。

## SemanticTree 回收

每次 op/value 删除结束后，pass 扫描所有**存活** op/value 的 SemanticId：

- 直接引用的节点及其祖先保留；
- 实例 Module 与同名实例 Declaration 的结构关联保留；
- 没有存活引用的完整分支及其名称字节删除；
- 只剩根节点时也保留根；
- 无 SemanticTree 的图跳过该步骤，电路 DCE 行为不变。

“节点内部 op 全被删除”不是充分删除条件。顶层 input 等合法边界可能没有定义 op，
但仍有存活 value 引用；这种节点必须保留。只有节点及其后代都不再被任何存活 op/value 引用时才可删除。

删除采用批量压缩而非逐节点搬移：先构造 `oldToNew`，再一次性替换节点数组、名称字符数组、
SemanticId 池及长列表驻留哈希。存活来源路径、列表顺序和重复项保持不变，数字 SemanticId 可以重编号。
跨 checkpoint 的身份应使用来源路径，而不是局部数字 ID。

仅来源回收也会使 pass 的 `changed=true`，但不会唤醒 simplify 的 `redundant-elim` dirty 工作项；
只有实际电路变化才触发该后续工作。

## 独立使用

```python
with wolvrix.Session() as session:
    session.read_json_file("post-elim.json", out_design="design.main")
    session.run_pass("split-static-array", design="design.main")
    session.run_pass(
        "dead-code-elim",
        design="design.main",
        args=["-output-key=dce.report"],
        keep_declared_symbols=False,
    )
```

可用参数：

- `-output-key=KEY` 或 `-output-key KEY`：在 session 中写入寄存器活跃性报告；
- `-sample-limit=N` 或 `-sample-limit N`：限制报告中的示例数量。

`KEY.phase` 另存每张图的子阶段数据，包括 analysis、op/value 删除、SemanticTree 回收的毫秒数，
以及删除/保留的来源节点、名称字节和直接 SemanticId 数。设置 `WOLVRIX_SIMPLIFY_PHASE_TIMING=1`
也会向 stderr 输出总耗时和 `[dce-phase]` 子阶段行。

## 与 simplify 的关系

`simplify` 仍在既有优化序列中复用同一个 `DeadCodeElimPass`，其 dirty 调度、常量折叠和冗余消除不变。
FIRRTL/mixed 在 `comb-loop-elim` 后先做静态数组拆分，再执行独立 DCE；这不替代 simplify 内部规则。
评估独立 DCE 时应单独读取其统计，不能使用整个 simplify 的耗时或删除量。

## 正确性约束

- 端口、效果和外部边界不得因来源回收而丢失；声明符号仅在显式开启保护时必须保留；
- 无 op 但有存活 value 的来源不得误删；
- 所有存活 SemanticId 必须有效，存活路径在压缩前后不变；
- 长列表的共享、顺序和重复项在重编号后保持正确；
- 第二次连续执行应达到幂等，不再删除 op/value 或来源节点；
- store→load→store 的 v4 JSON 必须保持一致。
