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
core.logic<width, signed, domain>
core.real
core.string
core.array<element_type, count>
```

- `core.logic` 表示固定位宽逻辑值：`width` 必须大于零，`signed` 表示算术解释是否有符号，
  `domain` 为 `2-state` 或 `4-state`；
- `core.real` 和 `core.string` 分别直接承接 GRH 的 `Real` 和 `String`，没有 `width`、
  `signed` 或 `domain` 参数；
- `core.array` 是定长同类型序列，`count` 必须非负。

这四个类型同时用于 `I`、`O`、graph value 和 `S` 对象：状态的类型就是它保存的内容的
类型。寄存器、锁存器、memory 和边沿历史的区别体现在读写状态的 op 上（见第 5、6 节），
不由状态的类型区分。core 为全部四个类型定义 `InitSpec`（见第 2.1 节）。方言扩展可以
增加其他类型，但不能改变已有 core 类型的含义。

GRH 数据类型到 core 的转换是一一对应的：`Logic` 转换为 `core.logic`，`Real` 转换为
`core.real`，`String` 转换为 `core.string`。GRH 已经扁平化的 packed array、struct 和 union
继续表示为一个 `core.logic`，转换不得恢复出 `core.array` 或其他聚合类型。

`core.init.const` 步骤中的 `value` 按状态内容的类型书写：`core.logic` 用等宽的 2-state 或
4-state logic 值，`core.real` 用实数值，`core.string` 用字符串值，`core.array` 用恰好包含
`count` 个元素的序列。

### 2.1 初始化描述 `InitSpec`

core 方言为全部四个类型定义 `InitSpec`：一个**非空**的初始化步骤序列，按序应用，后者覆盖
前者：

```text
InitSpec = InitStep[]

InitStep =
  core.init.const    { value }                          // 全量赋值
  core.init.random   { seed: Integer? }                 // 对应 $random / $random(seed)
  core.init.readmem  { file, format: hex|bin, start?, count? }  // 仅 core.array
  core.init.fill     { value 或 random, start?, count? }          // 仅 core.array
```

- `core.init.const` 适用于所有类型：`value` 是一个目标状态类型的值。
- `core.init.random` 仅适用于 `core.logic` 状态，对整个状态赋予随机值；`seed` 缺省时对应
  无参 `$random`。
- `core.init.readmem` 仅适用于 `core.array` 状态，对应 `$readmemh`/`$readmemb`：`file` 为
  数据文件路径，`start` 缺省为 0，`count` 缺省为从 `start` 读到末尾。
- `core.init.fill` 仅适用于 `core.array` 状态，对应一段地址的重复赋值：`value` 为静态元素
  值，`random` 表示每行独立采样 `$random`；`start` 缺省为 0，`count` 缺省为覆盖到末尾。

非 array 状态的 `InitSpec` 只允许一个全量步骤（const 或 random）。`core.array` 状态允许
多个 readmem/fill 步骤组合。

core 方言不为缺失的初值指定隐式默认值。`InitSpec` 应用后必须覆盖状态对象的全部内容；GRH 未
显式给出初值时（如无 `initValue` 的寄存器或锁存器），转换必须按 SV 语义显式补一个全量
`core.init.const` 步骤（4-state 为全 X）。array 状态的 readmem/fill 步骤只覆盖部分地址时，
序列必须以这样的全量步骤打底，未覆盖地址按 SV 语义保持全 X。边沿历史状态的初值没有 GRH
来源，由转换显式选择一个合法值并写入 `InitSpec`。

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
| `kSliceArray` | `core.compute.sliceArray` |
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
| `core.state.regWrite` | `%updateCond, %nextValue, %mask, %events...` | 一个 `S` 对象及各 event 的历史状态 | `event_edges` | 无 |
| `core.state.latchWrite` | `%updateCond, %nextValue, %mask` | 一个 `S` 对象 | 无 | 无 |
| `core.state.memRead` | `%address` | 一个 `core.array` 类型的 `S` 对象 | 无 | 一个元素类型的值 |
| `core.state.memWrite` | `%updateCond, %address, %data, %mask, %events...` | 一个 `core.array` 类型的 `S` 对象及各 event 的历史状态 | `event_edges` | 无 |
| `core.state.memFill` | `%updateCond, %data, %events...` | 一个 `core.array` 类型的 `S` 对象及各 event 的历史状态 | `event_edges` | 无 |
| `core.state.memAssign` | `%updateCond, %data, %events...` | 一个 `core.array` 类型的 `S` 对象及各 event 的历史状态 | `event_edges` | 无 |
| `core.state.memWriteSeq` | `%updateCond_0, %address_0, %data_0, ..., %updateCond_{N-1}, %address_{N-1}, %data_{N-1}, %events...` | 一个 `core.array` 类型的 `S` 对象及各 event 的历史状态 | `event_edges` | 无 |

`updateCond` 和每个 event 必须是一位 logic。`mask` 与目标状态的 logic 位宽相同；mask 位为
1 的部分允许更新，其他部分保持原值。

`core.state.read` 返回目标状态的当前值。`memRead` 返回目标状态中 `%address` 指定的元素，
地址必须小于目标状态的元素个数。

`latchWrite` 在 `%updateCond` 为真时，用 `%nextValue` 的被 mask 选中部分更新目标状态。
`regWrite` 和 `memWrite` 还要求至少一个 event 命中对应边沿；`memWrite` 只更新 `%address`
指定的元素。条件不成立时目标状态保持原值。

`memFill` 在 `%updateCond` 为真且 event 命中时，把 `%data` 广播写入目标状态的全部元素；
`%data` 必须是元素类型。`memAssign` 在同样条件下把 `%data` 整体赋给目标状态；`%data`
必须是与目标状态同类型的 `core.array` 值。

`memWriteSeq` 把对同一 array 状态的一组**有序**条件索引写表达为单个 op。operands 末尾的
`%events...` 是整个 op 的事件列表，至少一个，不与单个写口关联：任一 event 命中对应边沿即
触发整个 op，触发后按写口在 operands 中的顺序依次应用——`%updateCond_k` 为真则把 `%data_k`
写入 `%address_k` 指定的元素，地址相同时后应用的写覆盖先应用的写；无 event 命中时状态
保持不变。写口数 `N = (|operands| - |events|) / 3`，必须整除；`N = 1` 时语义退化为
`memWrite`。该 op 承接
GRH/SV 中由 if/else-if 链表达的多端口优先级写：链序对应 operand 顺序，链头（最高优先级）
排在 operands 最后。CPU 后端可以把它直接生成为顺序 store（gsim 形态），其他后端也可以
展开为优先级选择逻辑。不同写口需要不同事件的情况不属于该 op 的承接范围，保持显式多 op
形态。

同一个 array 状态可以有多个写 op，但任意两个写 op 不能在同一次 `G` 应用中写入同一 bit；
op 的排列顺序不产生覆盖语义。需要有序覆盖的多写口必须收进单个 `memWriteSeq`，由 operand
顺序表达覆盖关系。

## 6. Event 与边沿历史

`event_edges` 与 event operands 一一对应，每项只能是 `posedge` 或 `negedge`。GRH 转换为每个
“事件敏感 op + event 位置”创建一个 `S` 对象保存该 event 的上一次取值（称为该 event 的历史
状态，类型与 event operand 相同），并把它作为同一个 op 的 object ref。事件敏感 op 包括
register/memory 写口、memory fill/assign、system task 和 DPI call；一个 GRH op 仍只产生一个
core op。

每次应用 `G` 时，事件敏感 op 都把 event 的当前值写入对应历史状态，不受 `updateCond`
是否成立影响。边沿由历史状态和当前 event 共同判定：

- 对 2-state logic，`posedge` 为 `0 -> 1`，`negedge` 为 `1 -> 0`；
- 对 4-state logic，边沿集合采用 SystemVerilog 的 `posedge`/`negedge` 规则。

多个 events 中任意一个命中即可触发该 op。历史状态的初始化必须由 `Init` 中对应的
`InitSpec` 明确给出（见 [2.1](#21-初始化描述-initspec)）。

## 7. 系统调用与 DPI

系统调用和 DPI 是 GRH 中已有的仿真语义，也由 core 方言直接承接。它们不是 CPU 或其他后端
私自增加的操作。后端不支持模型实际使用的系统调用或 DPI 类型时，必须拒绝构建映射，不能把
调用删除、替换为常量或当作空操作。

### 7.1 系统调用

| op | operands | object refs | parameters | results |
| --- | --- | --- | --- | --- |
| `core.system.function` | `%args...` | 无 | `name`, `has_side_effects`, `proc_kind`, `has_timing` | `%result` |
| `core.system.task` | `%callCond, %args..., %events...` | 各 event 的历史状态 | `name`, `event_edges`, `proc_kind`, `has_timing` | 无 |

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
| `core.dpi.call` | `%callCond, %inputArgs..., %inoutArgs..., %events...` | 一个 `F` 对象及各 event 的历史状态 | `event_edges` | `%return?, %outputArgs..., %inoutArgs...` |

`core.dpi.call` 的第一个 object ref 是被调用的 `F` 表项（`decl = core.dpi`），其余 object refs
是各 event 的 event-history。输入和 inout operand 均按 signature 中的声明顺序排列；results
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
| `kRegister` | `StateObject`（类型同 GRH 声明）+ `Init` 条目 |
| `kRegisterReadPort` | `core.state.read` |
| `kRegisterWritePort` | `core.state.regWrite` |
| `kLatch` | `StateObject`（类型同 GRH 声明）+ `Init` 条目 |
| `kLatchReadPort` | `core.state.read` |
| `kLatchWritePort` | `core.state.latchWrite` |
| `kMemory` | `StateObject`（`core.array` 类型）+ `Init` 条目 |
| `kMemoryReadPort` | `core.state.memRead` |
| `kMemoryWritePort` | `core.state.memWrite` |
| `kMemoryFillPort` | `core.state.memFill` |
| `kSystemFunction` | `core.system.function` |
| `kSystemTask` | `core.system.task` |
| `kDpicImport` | `ExternFunction`（`decl = core.dpi`） |
| `kDpicCall` | `core.dpi.call` |

GRH port 的目标 symbol 转换为目标 StateId，operands 保持原顺序，`eventEdge` 转换为
`event_edges`。GRH DPI import 的并行属性数组规范化为一个 `DpiSignature`，DPI call 的
`targetImportSymbol` 解析为对应 `F` 表项的 FuncId；这两步只改变引用和元数据的表示，不改变
调用粒度。

`kRegister` 的 `initValue` 转换为一个 `core.init.const` 步骤，`"$random"` 转换为
`core.init.random`；缺省时按 SV 语义补全 X 的 const 步骤。`kMemory` 的 `initKind` 数组逐项
转换为 `core.init.readmem`/`core.init.fill` 步骤，保持原顺序；部分覆盖时在序列开头补全 X 的
const 步骤。除 event-history 状态对象外，转换不得为了实现端口或调用语义增加新的 op。

## 9. 写口转换示例

GRH 写口：

```text
kRegisterWritePort @q
  operands: [%enable, %d, %mask, %clk]
  eventEdge: [posedge]
```

对应的 core op：

```text
core.state.regWrite
  object_refs: [@S.q, @S.q_event0]
  operands: [%enable, %d, %mask, %clk]
  event_edges: [posedge]
```

`@S.q_event0` 是新增的 event-history 状态。写口的 enable、data、mask、event 和 edge 均直接
保留，没有拆成其他图顶点。

## 10. 验证

Core 模型必须满足：

- `memRead`/`memWrite`/`memFill`/`memAssign`/`memWriteSeq` 的目标是 `core.array` 类型的状态；
- read result、data、mask 和目标状态的类型及位宽匹配；
- 历史状态的类型与对应 event operand 的类型一致；
- mem 地址落在目标状态的元素个数范围内；`memFill` 的 `%data` 为元素类型，`memAssign` 的
  `%data` 与目标状态同类型；
- `memWriteSeq` 的 operands 数量减去 event 数后必须能被 3 整除；
- 同一 array 状态的多个写 op 不会在同一次 `G` 应用中写入同一 bit；
- system function 有且只有一个 result，system task 的条件、参数和 event 分段合法；
- `F` 表项的 FuncId 唯一、`signature` 完整，DPI call 的目标、operands 和 results 与 signature
  的方向、顺序和类型一致；
- 每个 `S` 对象的 `InitSpec` 非空，步骤种类与目标状态的类型匹配（readmem/fill 仅用于
  array 状态），范围和值类型合法，且应用后完整覆盖目标状态的全部内容；
- 所有事件敏感 op 的 events、`event_edges` 和历史状态数量及类型一致；
- 一个 GRH storage port、system call 或 DPI call 只对应一个 core op。

## 11. 参考

- [GrhSIM IR Overview](../overview.md)
- [GRH IR](../../grh/grh-ir.md)
