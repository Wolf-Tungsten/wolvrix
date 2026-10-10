# GrhSIM Core Dialect

本文定义 GrhSIM `core` 方言，以及 GRH IR 到该方言的转换边界。[GrhSIM IR Overview](../overview.md)
定义 `GrhSimModel`、方言和后端映射之间的关系及执行语义。

## 1. 定位

`core` 是 GRH IR 到 `GrhSimModel` 的后端无关承接层，不是面向某个后端的最小执行原语集。
转换的首要目标是保留 GRH 中已经明确的语义结构：

- graph input/output 分别成为 `I`/`O` 对象；
- storage declaration 成为 `S` 对象；
- 每个 ReadPort、WritePort 或 FillPort 对应一个 core op；
- system function、system task 和 DPI call 分别保留为一个 core op，DPI import 成为 `F` 表项；
- 组合 op 保持原有语义粒度和数据依赖；
- CPU 类型、物理布局、分区和调度不属于 core 方言。

特别地，一个 GRH 写口不能在转换时拆成 edge、select 和 state write 等多个 op。需要更低层
或更高效 op 的后端，可以使用自己的方言表示，但不能反过来改变 core 的承接粒度。同样，
DPI call 不能按参数方向拆成多个调用或写回 op，system task 也不能降成无副作用的普通计算。

## 2. 类型

```text
core.logic<width, signed, domain>   // 另带 shape: int64[]（见下）
core.real
core.string
```

- `core.logic` 表示固定位宽逻辑值：`width` 必须大于零，`signed` 表示算术解释是否有符号，
  `domain` 为 `2-state` 或 `4-state`；
- `core.real` 和 `core.string` 分别直接承接 GRH 的 `Real` 和 `String`，没有 `width`、
  `signed` 或 `domain` 参数。

**数组形状（shape，stage-06 起与 [GRH 的数组类型](../../grh/grh-ir.md) §2.3 完全同构）**：
一个 `core.logic` 类型的完整描述是 `width`（**总位宽**，含全部维度）加 `shape`
（`int64[]`，行主序、最外维在前，空 = 标量）；数组的元素位宽
`elementWidth = width / ∏shape`。shape 挂在**类型**上，value 与 state 一视同仁：状态的
类型就是它保存的内容的类型，数组状态就是 shape 非空的 `core.logic` 状态。只有
`core.logic` 可以携带 shape；`core.real`/`core.string` 不数组化。

**shape 是纯结构视图，不改变任何 op 的扁平位语义**：任何 op 对带 shape value 的语义等同
于对其 `width` 位扁平位向量的语义；物理存储保持线性（元素按行主序摊平、连续存放），
shape 只影响数组寻址与语义校验。元素 `(i0, i1, ..., in-1)` 的线性位偏移为
`(i0*d1*...*dn-1 + i1*d2*...*dn-1 + ... + in-1) * elementWidth`，canonical index 0 对应
最低元素地址。shape 感知点只有 `core.compute.sliceArray` 与第 5 节的索引访问 op
（readArray/writeArray/fill）；其余 op 对 shape 透明。

verifier 对 shape 的强制规则（与 GRH verify pass 同构）：各维必须大于 0；各维乘积不得
溢出 uint64；`width % ∏shape == 0`（必须整除）。非 logic 类型不得携带 shape。

> 演进说明（stage-06）：旧的一维构造子 `core.array<element_type, count>` 已退役，由
> `width = 元素宽 × count`、`shape = [count]` 的 `core.logic` 表达；多维数组同理由
> 多维 shape 表达。JSON 格式相应演进为 `wolvrix.grhsim.v3`（类型条目携带 shape，
> v2 checkpoint 拒绝；core 方言指纹为 `wolvrix.grhsim.core.v2`）。

这些类型同时用于 `I`、`O`、graph value 和 `S` 对象。寄存器、锁存器、memory（数组状态）
和边沿历史的区别体现在读写状态的 op 上（见第 5、6 节），不由状态的类型区分。core 为
全部类型定义 `InitSpec`（见第 2.1 节）。方言扩展可以增加其他类型，但不能改变已有
core 类型的含义。

GRH 数据类型到 core 的转换是一一对应的：`Logic` 转换为 `core.logic`，`Real` 转换为
`core.real`，`String` 转换为 `core.string`。GRH 声明/value 上的 shape 是 GRH 的一等信息，
**必须原样保留并直接承接为 core 类型的 shape**（stage-06 起翻转了旧契约）；只有 GRH 侧
已经扁平化、不带 shape 的聚合（packed array、struct 和 union）继续表示为一个无 shape 的
`core.logic`，转换不得为它们重新捏造 shape 或其他聚合类型。

`core.init.const` 步骤中的 `value` 按状态内容的类型书写：`core.logic` 用等宽的 2-state 或
4-state logic 值（数组状态即为总位宽的单个值，不逐元素书写），`core.real` 用实数值，
`core.string` 用字符串值。

### 2.1 初始化描述 `InitSpec`

core 方言为全部类型定义 `InitSpec`：一个**非空**的初始化步骤序列，按序应用，后者覆盖
前者：

```text
InitSpec = InitStep[]

InitStep =
  core.init.const    { value }                          // 全量赋值
  core.init.random   { seed: Integer? }                 // 对应 $random / $random(seed)
  core.init.readmem  { file, format: hex|bin, start?, count? }  // 仅数组状态
  core.init.fill     { value 或 random, start?, count? }          // 仅数组状态
```

- `core.init.const` 适用于所有类型：`value` 是一个目标状态类型的值。
- `core.init.random` 仅适用于 `core.logic` 状态，对整个状态赋予随机值；`seed` 缺省时对应
  无参 `$random`。
- `core.init.readmem` 仅适用于数组状态（shape 非空的 `core.logic` 状态），对应
  `$readmemh`/`$readmemb`：`file` 为数据文件路径，`start` 缺省为 0，`count` 缺省为从
  `start` 读到末尾。`start`/`count` 是**线性行地址**（行主序摊平后的元素下标），不引入
  多维 init 寻址。
- `core.init.fill` 仅适用于数组状态，对应一段地址的重复赋值：`value` 为静态元素值，
  `random` 表示每行独立采样 `$random`；`start`/`count` 同为线性行地址，缺省为 0 和覆盖
  到末尾。

标量（shape 为空）状态的 `InitSpec` 只允许一个全量步骤（const 或 random）。数组状态允许
多个 readmem/fill 步骤组合。

core 方言不为缺失的初值指定隐式默认值。`InitSpec` 应用后必须覆盖状态对象的全部内容；GRH 未
显式给出初值时（如无 `initValue` 的寄存器或锁存器），转换必须按 SV 语义显式补一个全量
`core.init.const` 步骤（4-state 为全 X）。数组状态的 readmem/fill 步骤只覆盖部分地址时，
序列必须以这样的全量步骤打底，未覆盖地址按 SV 语义保持全 X。

## 3. Op 命名与公共约定

Core op 使用以下命名空间：

```text
core.input.*
core.output.*
core.state.*
core.compute.*
core.system.*
core.dpi.*
```

每种 op 分别定义 operands、results、object refs 和 parameters。Operands/results 通过 value
表达数据流，object refs 直接引用 I/O/S/F 对象，parameters 只保存不通过 value 传递的静态
信息。

## 4. 输入、输出和组合 op

| op | operands | object refs | parameters | results | 语义 |
| --- | --- | --- | --- | --- | --- |
| `core.input.read` | 无 | 一个 `I` 对象 | 无 | `%value` | 返回输入对象的当前值 |
| `core.output.write` | `%value` | 一个 `O` 对象 | 无 | 无 | 将 `%value` 定义为输出对象的值 |

GRH 的纯组合 op 一对一映射为 `core.compute` op。转换只把 GRH attr 改存为 parameters，不改变
operands、results 或运算语义。当前直接承接以下关键结构：

| GRH op | core op |
| --- | --- |
| `kConstant` | `core.compute.constant` |
| `kAssign` | `core.compute.assign` |
| `kMux` | `core.compute.mux` |
| `kConcat` | `core.compute.concat` |
| `kSliceStatic` | `core.compute.sliceStatic` |
| `kSliceDynamic` | `core.compute.sliceDynamic` |
| `kSliceArray` | `core.compute.sliceArray`（多索引承接：operands 为 `[base, i0..ik-1]`，`1 <= k <= rank(base.shape)`；结果为元素或带剩余维 shape 的子数组。语义是**数组值访问**，对应 GRH stage-02 的 kSliceArray 重定义——不再有 `sliceWidth` 参数，元素几何完全来自 base 的 shape；物理上等价于行主序位段选取，但不是 packed 位向量切片 op） |
| `kEq` | `core.compute.eq` |
| `kLogicAnd` | `core.compute.logicAnd` |

这些 op 的 operand 顺序、result 类型、parameters 和运算结果与对应的
[GRH IR 定义](../../grh/grh-ir.md)相同。其他 GRH op 只有在 core 方言中补充明确定义后才能转换，
不能被静默展开或改写为另一种图形。

### 4.1 变换产生的逐位选择

`core.compute.bitSelect` 的 operands 为 `mask, whenSet, whenClear`，唯一 result
逐位满足 `result[i] = mask[i] ? whenSet[i] : whenClear[i]`。所有 operand/result
的完整 TypeId 相同，当前支持 1–64 位 two-state logic，无 parameters/object refs。
signed 类型也按比特选择，最终按 result signedness 解释。例如
`bitSelect(4'b1010, 4'b1100, 4'b0011) = 4'b1001`。

这与普通 mux 的“condition 非零时选择整个 true value”语义不同。
[bitwise-muxes](../passes/bitwise-muxes.md) 仅将全部值为 unsigned 1-bit 的
mux 转为此 op，保留原 producer 求值与依赖。GRH lowering 不直接生成它。

## 5. 状态端口 op

| op | operands | object refs | parameters | results |
| --- | --- | --- | --- | --- |
| `core.state.read` | 无 | 一个 `S` 对象 | 无 | 一个与目标状态同类型的值 |
| `core.state.readArray` | `%i0, ..., %ik-1`（`1 <= k <= rank`） | 一个带 shape 的 `S` 对象 | 无 | `k == rank` → 元素类型的值；`k < rank` → 带剩余维 shape 的子数组值 |
| `core.state.write` | `%updateCond, %nextValue, %mask, %events...` | 一个 `S` 对象 | `event_edges` | 无 |
| `core.state.writeArray` | `%updateCond, %i0..%ik-1, %data, %mask, %events...`（`1 <= k <= rank`） | 一个带 shape 的 `S` 对象 | `event_edges` | 无 |
| `core.state.fill` | `%updateCond, %data, %events...` | 一个带 shape 的 `S` 对象 | `event_edges` | 无 |
| `core.state.latchWrite` | `%updateCond, %nextValue, %mask` | 一个标量 `S` 对象 | 无 | 无 |

命名约定为**访问形态命名 + `Array` 后缀标记索引变体**：`read`/`readArray` 和
`write`/`writeArray` 两对分别是整体访问与索引访问，`fill` 是整体广播写，`latchWrite`
是唯一的例外——latch 是电平敏感的不同状态种类，值得独立名字。时钟/电平差异由 event
操作数表达而非名字前缀：`write`/`writeArray`/`fill` 至少携带一个 event（`event_edges`
逐一标注边沿），`latchWrite` 无 event、电平敏感。访问形态（整体/索引/掩码/填充）是 op
语义，状态的存储分类（regLatch/mem，见 [Overview](../overview.md) 第 3.5 节）是后端
策略，二者完全解耦。

公共约定：

- `updateCond` 和每个 event 必须是一位 logic；索引操作数 `%i0..%ik-1` 必须是标量 logic
  （无 shape）。越界索引的行为是实现定义（仿真端按实现截断/钳位，不做跨实现约定）。
- `mask` 是**位掩码**（`mask[i]=1` 时写入第 `i` 位，不提供元素粒度掩码）：
  `write`/`latchWrite` 的 mask 宽等于目标状态的总位宽，`writeArray` 的 mask 宽等于
  `%data` 的总位宽。mask 位为 1 的部分允许更新，其他部分保持原值。

`core.state.read` 返回目标状态的当前值，结果类型就是目标状态的类型——数组状态读出带全
shape 的整体值。

`core.state.readArray` 返回目标状态中 `%i0..%ik-1` 寻址到的元素或子数组。记目标状态的
shape 为 `[d0, ..., dn-1]`（rank = n），索引数 `k` 满足 `1 <= k <= n`：`k == n` 时结果为
元素类型（宽 `elementWidth`，无 shape）；`k < n` 时结果为带剩余维 shape 的子数组（宽
`elementWidth × ∏shape[k:]`，shape 为 `shape[k:]`）。

`core.state.write` 在 `%updateCond` 为真且至少一个 event 命中对应边沿时，用 `%nextValue`
被 mask 选中的部分更新目标状态；`%nextValue` 的类型必须等于目标状态的类型。它承接旧
`regWrite`（标量写）与旧 `memAssign`（数组整体写 = mask 全 1、`%nextValue` 带全 shape
的 `write`）。条件不成立时目标状态保持原值。

`core.state.latchWrite` 的更新规则与 `write` 相同，但无 event、电平敏感，且目标必须是
标量状态（shape 为空）。

`core.state.writeArray` 在 `write` 的同样条件下更新 `%i0..%ik-1` 寻址到的元素或子数组；
`%data` 的类型规则与 `readArray` 的结果几何对称（`k == rank` → 元素类型；`k < rank` →
带剩余维 shape 的子数组类型）。

`core.state.fill` 在 `%updateCond` 为真且 event 命中时，把 `%data` 写入目标状态的全部
元素：`%data` 通常是元素宽度的标量（广播填充）；作为例外，`%data` 可以是目标状态总位宽
的打包 logic 值（packed fill，来自整宽聚合写），此时按行主序逐行切片，线性行号为 `i`
的元素取 `%data[i*elementWidth +: elementWidth]`。

**同一状态的多个写 op（`write`/`writeArray`/`fill` 的任意组合）按 op 声明顺序应用：
同轮同时激活时按声明顺序依次生效，后声明的写覆盖先声明的写**（与 GRH stage-03 §3.4 的
同 regSymbol 写端口优先级规则同构；转换层 1:1 保序生成）。**同 state 写 op 的相对顺序
是 IR 语义的一部分，pass 不得重排**（verifier 断言兜底）。跨事件的写口同样适用该规则，
不再有任何"同顺序域"限制。

> 演进说明（stage-06）：旧 op 族 `regWrite`/`memRead`/`memWrite`/`memFill`/`memAssign`/
> `memWriteSeq` 已整体退役，由上面的统一族取代（对应关系：`regWrite`→`write`，
> `memRead`→`readArray`，`memWrite`→`writeArray`，`memFill`→`fill`，`memAssign`→数组
> 整体 `write`）。旧约束"同一 array 状态的任意两个写 op 不能在同一次 `G` 应用中写入同一
> bit，需要有序覆盖的多写口必须收进单个 `memWriteSeq`（operand 顺序表达覆盖关系）"随之
> 删除——有序覆盖现在由声明顺序语义表达，`memWriteSeq` 的"全部写口须同事件"限制也不复
> 存在。

## 6. Event 与边沿标注

`event_edges` 与事件敏感 op 尾部的 event operands 一一对应，每项只能是 `posedge` 或
`negedge`。事件敏感 op 包括状态写口（`write`/`writeArray`/`fill`）、system task 和
DPI call；一个 GRH op 仍只产生一个 core op。GRH 转换只产出这份原始标注（M5 起不再
附带每 event 的 `__event_*` 历史状态对象）；边沿判定全部由六阶段 lowering
（`grhsim.lower-edge-detect`，见 [lower-edge-detect](../passes/lower-edge-detect.md)）
改写成 `core.event.edgeDet` 后承担。

边沿定义：

- 对 2-state logic，`posedge` 为 `0 -> 1`，`negedge` 为 `1 -> 0`；
- 对 4-state logic，边沿集合采用 SystemVerilog 的 `posedge`/`negedge` 规则。

多个 events 中任意一个命中即可触发该 op。

> 演进说明（M2/M5）：M2 的 `grhsim.lower-edge-detect` 把事件标注改写为新形态：每个
> `(event, edge)` 去重聚类生成一个 6.1 节的
> `core.event.edgeDet`，事件逻辑锥克隆进 P_event，消费 op 尾部的 event operands
> 摘除、`event_edges` 参数替换为 `event_acts`（int64 数组，
> 元素为聚类下标、保持原顺序）。
> `core.system.task` 改写后不再持有 object refs，`core.dpi.call` 只保留首部 Function
> 引用；`write`/`latchWrite`/`system.task`/`dpi.call` 的 phase 置为 `general`，
> `writeArray`/`fill` 与目标是数组状态的 `write` 在语义归类前保持无 phase。verifier
> 在模型出现 `event_acts` 后禁止任何 `event_edges` 残留。
> M5d-5 起这些写由语义层 `grhsim.split-phases`（B5）归类：按目标状态的存储分类，
> `mem` 类状态的写归 `mem` 相、`regLatch` 类状态的写归 `general` 相（见
> [grhsim.split-phases](../passes/grhsim-split-phases.md)）。
> M5 起 GRH 转换不再产出 `__event_*` 历史状态；lower-edge-detect 会顺带清理
> M5 前 checkpoint 里残留的此类未引用状态。

### 6.1 边沿检测 op `core.event.edgeDet`

`core.event.edgeDet` 是六阶段仿真模型（见 `pdocs/simulation-model-refactor`）中 `P_event`
阶段的边沿检测原语。它由 lowering pass（`grhsim.lower-edge-detect`）对事件敏感 op 的
(event, edge) 组合去重聚类后生成；GRH 转换不直接产出该 op。与第 6 节的"历史状态 + 事件
敏感 op 自判边沿"形态相比，edgeDet 把边沿判定从各消费 op 中剥离出来集中执行，消费 op
改为读取判定结果。

| op | operands | object refs | parameters | results |
| --- | --- | --- | --- | --- |
| `core.event.edgeDet` | `%event`（一个 logic 值） | 无 | `edge`, `act`, `prev`, `prevInit` | 无 |

- `%event` 是事件锥的末端值（P_event 阶段算出的该 event 信号当前值），必须是
  `core.logic` 类型；edgeDet 不产生任何 data value，也没有 object refs。
- `edge` 为 string 参数，取值 `posedge`/`negedge`/`both`，编码沿用 `event_edges` 并补
  `both` 双边沿。
- `act` 为非负 int64：该聚类在 eventActStore 中的 bit 下标，全模型唯一。
- `prev` 为非负 int64：该聚类在 prevEventStore 中的历史槽位下标，全模型唯一。同一
  event 信号可挂多个 edgeDet（例如同一 rst 的不同消费者分别检测上升沿和下降沿），各自
  独占 prev 槽位、互不干扰。
- `prevInit` 为 string 参数，书写该 event 信号 init 值的常量字面量（如 `1'h0`），即 prev
  槽位的初值；上电首个 eval 因此不报边沿。

执行语义：edgeDet 执行即把 prev 槽位与 `%event` 当前值按 `edge` 规则比较，命中则把
eventActStore 的 `act` bit 写 1——该 bit 是脉冲式的，仅在被检出边沿的当轮有效（每轮
P_event 覆盖重算，不粘滞）；无论是否命中，prev 都立刻更新为当前值，因此下一轮对同一事件
看到的是新值-新值，不再重复报边沿。

全模型约束（verifier 强制）：所有 edgeDet 的 `act` 互不相同、`prev` 互不相同，且
`(%event, edge)` 二元组互不相同（去重聚类的正确性）；edgeDet 的 phase 归属必须为
`event`。

例：两个 write 共享 `(clk, posedge)` 聚类，另一个 rst 同时有 posedge/negedge 两个
edgeDet：

```text
; q1/q2 的 write 都消费 (clk, posedge)，共享一个检测器：
core.event.edgeDet
  operands: [%clk_ev]
  parameters: { edge: posedge, act: 0, prev: 0, prevInit: 1'h0 }
  results: []

; rst 的上升沿和下降沿各有一个消费者聚类：
core.event.edgeDet
  operands: [%rst_ev]
  parameters: { edge: posedge, act: 1, prev: 1, prevInit: 1'h0 }
  results: []

core.event.edgeDet
  operands: [%rst_ev]
  parameters: { edge: negedge, act: 2, prev: 2, prevInit: 1'h0 }
  results: []
```

## 7. 系统调用与 DPI

系统调用和 DPI 是 GRH 中已有的仿真语义，也由 core 方言直接承接。它们不是 CPU 或其他后端
私自增加的操作。后端不支持模型实际使用的系统调用或 DPI 类型时，必须拒绝构建映射，不能把
调用删除、替换为常量或当作空操作。

### 7.1 系统调用

| op | operands | object refs | parameters | results |
| --- | --- | --- | --- | --- |
| `core.system.function` | `%args...` | 无 | `name`, `has_side_effects`, `proc_kind`, `has_timing` | `%result` |
| `core.system.task` | `%callCond, %args..., %events...` | 无 | `name`, `event_edges`, `proc_kind`, `has_timing` | 无 |

`name` 是不含 `$` 的 SystemVerilog 系统函数或任务名。`has_side_effects` 是必填布尔值；它只用于
`core.system.function`，明确该函数是否会改变随机数、文件或其他外部状态。`proc_kind` 必须是
`initial`、`final`、`always`、`always_comb`、`always_latch` 或 `always_ff`，`has_timing` 表示原
过程是否包含显式时序控制。这两个 parameter 是调用语义的一部分，不是后端调度提示。

`core.system.function` 的所有 operands 都是函数实参，并产生恰好一个与 GRH result 同类型的
结果。`core.system.task` 中，令 `q = len(event_edges)`，最后 `q` 个 operands 是 events，第一个
operand 是逻辑类型的 `callCond`（任意非零位表示真，不限于 1 bit），中间部分全部是任务实参。在对应过程被激活时，任务只有在
`callCond` 为真，且无 event 或至少一个 event 命中指定边沿时才执行。

例如，`$clog2(x)` 直接产生一个 result：

```text
core.system.function
  operands: [%x]
  parameters:
    name: clog2
    has_side_effects: false
    proc_kind: always_comb
    has_timing: false
  results: [%width]
```

GRH 中在时钟上升沿执行的 `$display` 也直接转换为一个 op：

```text
core.system.task
  operands: [%enable, %format, %data, %clk]
  object_refs: [@S.display_event0]
  parameters:
    name: display
    event_edges: [posedge]
    proc_kind: always
    has_timing: true
  results: []
```

其中 `%format` 和 `%data` 是任务实参，`%clk` 是唯一 event；`@S.display_event0` 保存该 event 的
上一次取值。该 op 不能拆成格式化、输出和事件判断等多个 core op。

> 演进说明（M2b）：`name` 为 `strobe`/`monitor` 的 `core.system.task` 是 time-slot 任务，
> `grhsim.migrate-timeslot-tasks` pass（见
> [migrate-timeslot-tasks](../passes/migrate-timeslot-tasks.md)）把它们迁入 P_output：带
> `event_acts` 的追加 `timeslotFlag` 参数（event_acts 保留给 M4 建映射），无事件的改用
> `__tslot_prev_<opId>_<i>` 历史状态 + ne/or changed 归约改写 guard，prev 状态由 Output 相
> `core.state.latchWrite` 在 P_output 末尾（M5 emit 契约）回写。其他 name 的 system task
> 不参与该迁移。

### 7.2 DPI

DPI import 只是声明，不产生计算：它不占 `G` 的 op，进入 `GrhSimModel` 的外部函数表 `F`。
core 方言定义 `F` 表项的声明种类 `core.dpi`，其 `signature` 结构为：

```text
DpiArgument
  name: String
  direction: input | output | inout
  type: TypeRef

DpiSignature
  symbol: String
  arguments: DpiArgument[]
  return_type: TypeRef?
```

`signature` 完整保留 GRH `kDpicImport` 的 symbol、参数方向、名称和语义类型；void import 的
`return_type` 为空。首版只接受能表示为 core `TypeRef` 的 DPI 参数和返回类型，其他类型必须由
扩展方言定义。

| op | operands | object refs | parameters | results |
| --- | --- | --- | --- | --- |
| `core.dpi.call` | `%callCond, %inputArgs..., %inoutArgs..., %events...` | 一个 `F` 对象 | `event_edges` | `%return?, %outputArgs..., %inoutArgs...` |

`core.dpi.call` 的唯一 object ref 是被调用的 `F` 表项（`decl = core.dpi`）。输入和 inout
operand 均按 signature 中的声明顺序排列；results
先放可选返回值，再按声明顺序放 output，最后放 inout 的调用后值。参数数量和类型都从目标
signature 确定，不在 call 中重复保存。最后 `len(event_edges)` 个 operands 是 events，调用条件
和边沿判定方式与 system task 相同。

例如，`add` 的声明和一次上升沿调用表示为：

```text
F:
  @F.add
    decl: core.dpi
    signature:
      symbol: add
      arguments:
        - {name: a, direction: input, type: core.logic<32, false, 4-state>}
        - {name: b, direction: input, type: core.logic<32, false, 4-state>}
      return_type: core.logic<32, false, 4-state>

core.dpi.call
  operands: [%enable, %a, %b, %clk]
  object_refs: [@F.add, @S.add_event0]
  parameters:
    event_edges: [posedge]
  results: [%sum]
```

`core.system.task`、`has_side_effects = true` 的 `core.system.function` 和 `core.dpi.call` 都是可观察
操作。`G` 不规定它们之间的相对顺序；需要确定顺序时，必须用 value 依赖显式表达。`F` 表项
只是声明，不参与执行。

## 8. GRH 到 Core 的映射

| GRH IR | GrhSIM core |
| --- | --- |
| graph input | `InputObject` + `core.input.read` |
| graph output | `OutputObject` + `core.output.write` |
| `kRegister`（标量） | `StateObject`（类型同 GRH 声明）+ `Init` 条目 |
| `kRegister`（数组，shape 非空） | `StateObject`（`core.logic` 类型直接继承 GRH 的 width+shape）+ `Init` 条目 |
| `kRegisterReadPort` | `core.state.read` |
| `kRegisterReadArrayPort` | `core.state.readArray` |
| `kRegisterWritePort` | `core.state.write`（转换器补全 1 mask 常量） |
| `kRegisterWriteMaskPort` | `core.state.write` |
| `kRegisterWriteArrayPort` | `core.state.writeArray`（转换器补全 1 mask 常量） |
| `kRegisterWriteArrayMaskPort` | `core.state.writeArray` |
| `kRegisterFillPort` | `core.state.fill` |
| `kLatch` | `StateObject`（类型同 GRH 声明）+ `Init` 条目 |
| `kLatchReadPort` | `core.state.read` |
| `kLatchWritePort` | `core.state.latchWrite` |
| `kSystemFunction` | `core.system.function` |
| `kSystemTask` | `core.system.task` |
| `kDpicImport` | `ExternFunction`（`decl = core.dpi`） |
| `kDpicCall` | `core.dpi.call` |

GRH port 的目标 symbol 转换为目标 StateId，operands 保持原顺序（数组端口的多索引操作数
原样保留，线性化推迟到后端 mapping——转换契约是逐 op 承接、不在转换时引入新计算），
`eventEdge` 转换为 `event_edges`。**同一 regSymbol 的多个写端口各成一个 op、按声明顺序
保序生成**——声明顺序即写优先级语义（见第 5 节），转换层不丢失也不重建排序信息。GRH
DPI import 的并行属性数组规范化为一个 `DpiSignature`，DPI call 的
`targetImportSymbol` 解析为对应 `F` 表项的 FuncId；这两步只改变引用和元数据的表示，不改变
调用粒度。

标量 `kRegister` 的 `initValue` 转换为一个 `core.init.const` 步骤，`"$random"` 转换为
`core.init.random`；缺省时按 SV 语义补全 X 的 const 步骤。数组 `kRegister`（shape 非空）的
`initKind` 数组逐项转换为 `core.init.readmem`/`core.init.fill` 步骤，保持原顺序；部分覆盖时
在序列开头补全 X 的 const 步骤。转换不得为了实现端口或调用语义增加新的 op。

## 9. 写口转换示例

GRH 写口：

```text
kRegisterWriteMaskPort @q
  operands: [%enable, %d, %mask, %clk]
  eventEdge: [posedge]
```

对应的 core op：

```text
core.state.write
  object_refs: [@S.q]
  operands: [%enable, %d, %mask, %clk]
  event_edges: [posedge]
```

写口的 enable、data、mask、event 和 edge 均直接
保留，没有拆成其他图顶点；object refs 只有目标状态（M5 起不再附带每 event 的
`__event_*` 历史状态）。

## 10. 验证

Core 模型必须满足：

- 类型合法：`core.logic` 的 `width` 大于零；`shape` 各维大于 0、乘积不溢出 uint64 且
  整除 `width`；非 logic 类型不得携带 shape；
- `readArray`/`writeArray`/`fill` 的目标是带 shape 的（数组）状态，`latchWrite` 的目标是
  标量状态；
- `read` 的 result 类型等于目标状态的类型（数组状态读出带全 shape 的整体值）；
- `readArray`/`writeArray` 的索引数满足 `1 <= k <= rank`，索引操作数都是标量 logic；
  `readArray` 的 result 与 `writeArray` 的 `%data` 类型为寻址到的元素（`k == rank`）或
  带剩余维 shape 的子数组（`k < rank`，宽 `elementWidth × ∏shape[k:]`、shape 为
  `shape[k:]`）；
- `write` 的 `%nextValue` 类型等于目标状态类型；`write`/`latchWrite` 的 `%mask` 是宽等于
  目标状态总位宽的标量 logic，`writeArray` 的 `%mask` 是宽等于 `%data` 总位宽的标量
  logic；
- `fill` 的 `%data` 为元素宽度的标量，或目标状态总位宽的打包值（packed fill，见第 5 节）；
- 同一状态的多个写 op 的相对顺序即覆盖优先级（声明顺序语义，见第 5 节），转换与 pass
  不得重排；
- system function 有且只有一个 result，system task 的条件、参数和 event 分段合法；
- `F` 表项的 FuncId 唯一、`signature` 完整，DPI call 的目标、operands 和 results 与 signature
  的方向、顺序和类型一致；
- 每个 `S` 对象的 `InitSpec` 非空，步骤种类与目标状态的类型匹配（readmem/fill 仅用于
  数组状态），范围和值类型合法，且应用后完整覆盖目标状态的全部内容；
- 所有事件敏感 op 的 events 与 `event_edges` 数量一致、类型为一位 logic；
- 一个 GRH storage port、system call 或 DPI call 只对应一个 core op。

## 11. 参考

- [GrhSIM IR Overview](../overview.md)
- [GRH IR](../../grh/grh-ir.md)
