# split-static-array

F28 支持边界：带新版 SemanticTree 的图会在本 pass 修改 IR 之前明确拒绝，
目前尚未实现单一叶 Node 的拆分／成员演化维护。以下数组变换适用于无新树的图；
来源 SID、semanticBindings 与 source-endpoint trace 说明记录 F26/F27 的旧接口，
不是 F28 新模型的支持声明。可选全物理 op/value trace 设施保留。

静态索引解析与 `dead-code-elim` 共用私有 `constant_index.hpp`：字面量先使用
`slang::SVInt` 读取，再按常量 producer 的 width/signedness 投影；等宽 assign
仅改变最后索引的 signedness 解释。下划线、字面量宽度和 GRH value 宽度都参与实际
数值，未知位或负数保守保留动态路径。回归同时覆盖 `4'b1_0`、`8'h12` 投影为 2 bits，
以及 signed/unsigned assign 前后的零扩展、符号扩展和越界索引。

`split-static-array` 把可静态定位的组合数组和状态数组按成员拆开，避免 op 级 DCE 因一个静态读而保活整个聚合 value 或完整 `regSymbol` 状态族。它对应 GSIM `splitOptionalArray` 的全局静态成员化目标，但保持 GRH 的 op/value 表示，不复制 GSIM 的 Node/ENode 分析。

pass 名称为 `split-static-array`，实现位于 `lib/transform/split_static_array.cpp`。FIRRTL/mixed 流程顺序为 `comb-loop-elim → split-static-array → dead-code-elim`。本 pass 不内嵌 DCE，也不进入 `simplify` 迭代。

## 组合数组重写

- 穿过等宽 `kAssign` 和静态位切片；
- 从 `kArrayCreate` 选择单个或连续多个元素，支持多维行主序索引；连续成员组重新生成 `kArrayCreate`，与原切片结果类型不同则增加显式 `kAssign` 视图；
- 从 `kConcat` 选择单个操作数或跨多个操作数的连续位段，并按原高低位顺序重建 `kConcat`；跨段选择目前要求扁平结果类型；
- 将 shaped `kMux` 的静态选择分配到两个分支，并复用相同选择结果。

原静态读的结果 value 保持不变，读取 op 改为 `kAssign`。只有需要新成员视图时才创建 `kAssign`、`kArrayCreate`、`kConcat`、`kSliceStatic` 或 `kMux`；未读取元素不会预先物化。原聚合 value 和驱动链由后续独立 DCE 回收。

动态或越界索引、端口数组、非扁平结果类型的跨 concat 操作数选择以及不满足 node 共享约束的情况保持原样。MUX 分支选择失败会回滚该次新建成员，不留下临时节点。

## 状态数组重写

pass 先按 `regSymbol` 收集完整状态族，只有整族满足条件才改写：

- 至少存在一个读口；所有数组读写都必须是完整维度的常量索引，且在界内。动态、越界或部分维度索引会使整个状态族保持原样。`arrayEntryNum<=1` 的状态族不拆，带 `firrtl.memory` 属性的存储也不进入普通状态数组拆分。
- 静态 `kRegisterReadArrayPort` 和静态数组写口直接改接对应标量成员的 `kRegisterReadPort`、`kRegisterWritePort` 或 `kRegisterWriteMaskPort`。
- whole read 用成员读逐层重建 `kArrayCreate`，保持原多维形状；原结果是扁平或其他 tiling view 时，通过 `kAssign` 保留原 result value。沿等宽 `kAssign`、`kConcat`、`kArrayCreate`、数据分支等宽的 `kMux`、`kSliceArray` 和嵌套 `kSliceStatic` 找到的静态位切片也会继续按成员重写。
- whole write 按成员选择 next-value 和 mask。选择可穿过 `kAssign`、`kArrayCreate`、`kConcat`、`kSliceStatic` 和 `kMux`，递归上限为 256；不能展开时保留普通 `kSliceStatic` 视图，并登记到本次 pass 末尾统一重写，使后拆分的来源状态族也能继续成员化。切片遍历使用增量工作队列，重写期间新建的 fallback 也会入队；选择失败回滚时取消对应登记。缓存命中当前切片自身 result 时先失效重算，不得形成自赋值。
- `kRegisterFillPort` 复制为各成员的普通写口。enable、事件操作数、`eventEdge` 和原 `writerOrder` 均保留；不同成员状态各自维持原写口优先级。
- 声明的 `Constant` 初始化按行主序切片；覆盖全数组且非随机的 `Fill` 初始化转换为成员 `Constant`。Readmem、随机或复杂多段初始化使整族跳过。
- 成员名按行主序索引生成，并像 GSIM 一样忽略大小为 1 的维度，例如 `[1,3]` 的成员使用 `_0/_1/_2` 而非 `_0_0/_0_1/_0_2`。与既有实体冲突时，非端口 value 或寄存器声明先加 `_0` 后缀，并同步更新 `regSymbol`、声明标记及受影响状态族的候选成员名。端口、其他 op 类型或后缀仍冲突的对象使当前状态族保守跳过。

原数组声明在全部读写改接完成后删除。成员声明和写口只在可拆状态族中物化，未读成员及其写口、自保持逻辑和独占输入锥由后续独立 DCE 删除。拆分不能提高删除能力时仍保持等价的状态行为。

默认不保护声明符号，原声明标记会转移到各成员声明。显式 `keep_declared_symbols=True` 时，声明状态族不做替换，以保留原聚合声明；组合数组仍按上述规则拆分，其原声明 value 和驱动锥继续受保护。

## 来源与 SemanticTree

组合视图的新 op 合并原读和直接生产者的 `originModules` 与 FIRRTL node 属性；SemanticId 沿用原读，原读没有来源时才回退到直接生产者，避免沿视图链构造膨胀的并集。新 value 同样沿用原读结果，必要时回退到当前数组 value。

状态成员声明和成员读写复制原声明或端口的操作属性、来源位置及 SemanticId，成员 value 沿用原读结果的 SemanticId。多个成员因此仍可映射回原来的父状态或表达式。pass 不直接删除语义节点；无引用声明、表达式及其空父节点由后续 DCE 的 SemanticTree prune 统一处理。

`semanticBindings` 同时跟随真实端点迁移。成员 current 绑定到实际成员读结果，包含保留 ID 的 indexed read；成员 next 绑定到实际 scalar data 和对应 writer operation。原 writer 被复用为成员 0 时，其旧 next/clock/reset 上下文原子替换，其他成员 writer 克隆相应上下文。原 source shape、shapeFieldDepths 和已固定 projection 保持不变，只将本次选中的开放轴固定为成员坐标。例如源 `[2,1]` 的第二个 scalar 成员仍记录 `shape=[2,1], projection=[1,0]`，不会变成无数组轴的新源对象。

组合静态读从 base 的原生 binding 投影到实际 read result；位序保持的 `kAssign`、`kArrayCreate`、`kConcat` 和 `kSliceStatic` 连接可以继续追溯到上游 source。端口数组保留 `kSliceArray` 时也维护该读关系。成功选择已有 scalar operand 时，selected value 同样获得对应 source projection。无法解释的几何不补造来源；递归选择失败回滚时不污染已有 value 的绑定。

## 可选原生演化记录

普通 pass 默认不写 trace。通过 C++ `SplitStaticArrayOptions` 或 Python 参数启用：

```python
session.run_pass("split-static-array", design="design.main",
                 trace_node_evolution="ptmp/split.jsonl",
                 trace_input_sha256=firrtl_sha256,
                 trace_source_endpoints=False)
```

`trace_source_endpoints=False` 记录全部存活 operation/value；`True` 记录来源端点及 writer 上下文，前序已绑定且仍存活的实体即使丢失最后一条绑定也保留在 after 库存中。多图路径必须含 `{graph}`。SHA256 是调用方验证的输入身份，producer 不把传入字符串当成自行计算的证明。

公共 `GraphEvolutionTrace` 输出 `wolvrix.node-evolution.v1` JSONL。typed ref 包含原生 ID 与 generation，库存内的 `sources` 表示源端点，`structure` 记录实际操作数、结果、属性、定义及 owner；writer 的绑定上下文保留在其 operation 上。pass 内步骤记录静态选择、成员化、重组、保留、符号冲突及事务回滚的原因。共享实体关联的步骤归并为唯一边界事件，原始步骤保留在 `steps`，不会把回滚临时对象作为最终 after 成员。成功完成才写完整 footer。

trace 开关不参与绑定传递或优化决策。来源覆盖、成员投影与库存完整性分别核验；这些记录不声明行为等价。零宽/空数组且无物理端点的 SemanticTree 节点不在物理库存分母中。

## 示例

组合数组：

```text
array = kAssign(packed)                         // shape=[3], width=24
packed = kConcat(e2, e1, e0)
out = kSliceArray(array, 0)                    // width=8
```

拆分后：

```text
out = kAssign(e0)
```

若 `array` 没有其他用户，独立 DCE 删除 `array`、`packed` 以及未被其他逻辑使用的 `e1/e2` 驱动。

状态数组及其自保持写入：

```text
r: kRegister, shape=[3], width=24
current = kRegisterReadPort(r)
next = kConcat(e2, e1, current[7:0])
kRegisterWritePort(r, enable, next, clock)
out = kRegisterReadArrayPort(r, 1)
```

拆分后形成 `r_0/r_1/r_2`，分别写入 `current_0`、`e1`、`e2`，`out` 直接读取 `r_1`。若 `r_2` 没有其他读者，独立 DCE 删除其声明、写口及 `e2` 的独占驱动。

## 原生演化与后态重放

`trace_node_evolution` 可选输出 pass 前后的全部存活 operation／value，以及 graph context。完整库存声明 `inventoryScope=all-live` 和 `replayContract=native-field-delta.v1`；仅来源端点模式不宣称完整图可重放。trace 关闭时同样维护正常 semantic bindings，trace 不改变普通 pass 输出。

原生步骤解释实际拆分、保留、删除和改写；边界 `mutation` 以 `eventSeq` 关联真实事件，记录 `create/erase/update` 与 `expect/set/unset`。消费者从 before 库存和 mutation 独立生成后态；实际 after 只用于逐字段核对。未被事件解释的变化，或者只用 retain 解释的内容修改，使 `replayComplete=false`，不能作为通过证据。

`steps[].seq` 是 recorder 接收步骤时的全局顺序，组件归并后仍保留，不能使用 event 顺序代替实际步骤顺序。静态选择步骤原生记录 `geometry.kind=packed-selection`、`base` typed ref、变换当时的 `physicalShape` 和从低位起算的 `offset/width`（位）；状态成员步骤记录 `kind=array-member`、原声明 `base`、原物理数组形状、行优先 `element/coordinates` 及其位区间。例如 `[2,3]` 的 8 位成员 `[1,2]` 为 `element=5, offset=40, width=8`。这些是物理选择位置，不替代 `sources[]` 的 FIRRTL 来源 projection。所有步骤与组件的 `partition` 仍保守为 false，不将重叠视图宣称为无重叠划分。

完整节点载荷包含真实 symbol、位置、类型／domain、operands／results、attrs、完整 binding context 与 state target。graph context 包含 metadata、端口引用、declared symbols 和 generate groups。反向 uses 可由 operands 重建；分配器／intern pool 和静态来源树节点不属于本合约的活跃节点范围，header 明示排除项。GRH ref 同时保留 ID 和 generation，禁止因 ID 重用而混淆实体。

全节点重放和跨端测量的阶段记录统一见 `pdocs/firrtl-ingest/F27-node-evolution.md`；来源对应、结构重放及电路行为等价是分别验证的结论。
