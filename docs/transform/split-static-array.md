# split-static-array

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
