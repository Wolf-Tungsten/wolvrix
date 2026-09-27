# GrhSIM IR Overview

[GRH IR](../grh/grh-ir.md) 描述电路结构，GrhSIM IR 描述电路的仿真行为：给定一组输入，
电路的输出是什么、内部状态变成什么。用 GrhSIM IR 书写的一份具体产物称为一个
`GrhSimModel`，它包含仿真行为的完整描述，以及若干仿真后端的映射方案。

每一种目标机器称为一个仿真后端，例如 CPU 是一个仿真后端，Corvus 是另一个仿真后端。
`GrhSimModel` 为每个仿真后端保存一份映射方案，记录这个模型在该后端上如何实现；
不同后端的映射方案互不影响。

本文定义 GrhSIM IR 的数据结构、执行语义和扩展边界；具体方言、后端映射和 pass 系统在各自
的子目录中定义。

## 1. 总体结构

一个 `GrhSimModel` 由六个语义分量和若干仿真后端的映射方案组成：

```text
GrhSimModel
  I: InputObject[]
  O: OutputObject[]
  S: StateObject[]
  F: ExternFunction[]
  G: SimGraph
  Init: Map<StateId, InitSpec>
  mappings: Map<BackendId, BackendMapping>
```

- `I`、`O`、`S` 分别记录泛化的输入、输出和持久状态；
- `F` 记录模型调用的外部函数声明；这些声明位于模型级，不属于 `G`；
- `G` 是仿真流图，op 是顶点，value 是 op 之间的数据流边；
- `Init` 完整定义 `S` 的初始状态；
- `mappings` 以 `BackendId`（仿真后端的唯一名字，例如 `"cpu"`）为键，记录该模型在对应
  后端上的映射方案 `BackendMapping`；
- `BackendMapping` 的具体结构由对应后端定义，不存在所有后端共享的固定分量。

`mappings` 中的映射引用前六个分量，但不由它们唯一决定：同一个模型可以有多种合法的后端
映射，映射记录的是后端做出的实现选择。这些选择只决定"怎么算"，不改变"算什么"——映射
不得改变 `G` 和 `Init` 定义的模型语义。

由于映射中的每个决策都针对具体的模型内容做出（引用其中的对象、op 和 value），前六个
分量一旦被修改，所有已生成的后端映射立即失效、不得继续使用，必须重新生成（具体执行规则
见 [Pass System](./passes/overview.md)）。

`mappings` 可以为空：尚未生成后端映射的模型仍是合法的 `GrhSimModel`，例如刚完成 GRH 转换、
还没进入后端流程的中间产物。但模型要用于执行，必须至少携带一个与当前模型一致的后端
映射。

## 2. 方言

`GrhSimModel` 中出现的类型、op 和外部函数声明都由方言定义。
一种方言就是一套类型系统（数据结构）加一套 op 系统（对数据的操作）：

```text
Dialect
  name: String
  version: String
  types: DialectType[]
  op_types: DialectOpType[]
  function_decls: DialectFunctionDecl[]
```

初始 [core 方言](./dialects/core.md) 是 GRH IR 到 `GrhSimModel` 的后端无关承接层。

### 2.1 类型 `DialectType`

一种类型定义一个取值集合。类型的书写形式是类型名加
可选参数，如 `core.logic<32, false, 4-state>`。要作为 `S` 对象类型的类型，还必须定义
`InitSpec` 的表示（见第 3.2 节）。

类型本身不标注用途；一个类型能出现在哪些位置由使用处的约束决定：op 定义决定其 operand
和 result 的类型，`S` 对象的类型必须带有 `InitSpec` 定义。

### 2.2 Op `DialectOpType`

一种 op 定义规定该类 op 的 operands 和 results（数量与类型）、object refs（指向的对象
种类，以及每个引用是读取还是写入）和 parameters（名称、类型和含义），并给出语义：
results 的取值和对对象的写入如何由 operands 与读取的对象决定。parameters 只保存不经
value 传递的静态信息（如常量、slice 范围）。

使用事件的 op 还必须在定义中标出：operands 中哪些位置是检测边沿的事件信号，以及
object refs 中哪些对象保存对应事件的边沿历史。每个事件具体检测 posedge 还是 negedge
由 op 实例给出（如 core 各事件 op 的 `event_edges` parameter），不属于 op 定义。

### 2.3 外部函数声明 `DialectFunctionDecl`

一种外部函数声明定义 `signature` 的结构：参数的方向、名称和类型，以及返回值的表示。
声明本身不定义执行行为，外部函数如何绑定到具体实现由后端规定。core 方言的 `core.dpi`
是一个例子（见 core 方言第 7.2 节）。

### 2.4 引用方言定义

模型引用方言中的定义时，写一个 `"方言名.定义名"` 的字符串。本文按用途给三种引用各起一个
别名：`TypeRef` 引用一种类型（如 `"core.logic"`），`OpRef` 引用一种 op（如
`"core.state.regWrite"`），`FuncRef` 引用一种外部函数声明种类（如 `"core.dpi"`）。方言
版本的兼容性在加载方言时检查，引用本身不带版本。

### 2.5 方言与后端的边界

方言和后端映射是两个独立扩展点。方言规定 `GrhSimModel` 可以出现哪些 op、类型和外部函数
声明；后端规定自己的 `BackendMapping` 结构，并声明支持哪些方言。

后端可以提供自己的方言。后端方言的类型在取值集合之外，还定义该后端目标机上的物理表示
（大小、对齐和存储结构），例如 [cpu 方言](./dialects/cpu.md) 的类型；这类类型只出现在
后端映射中，不能作为 I/O/S 对象或 value 的类型。模型语义分量引用的类型只定义取值集合，
不含任何物理表示信息。

## 3. `GrhSimModel`

本节展开 `GrhSimModel` 的各个语义分量。`InputId`、`OpId`、`ValueId` 等带 `Id` 后缀的
名字都只是全局唯一标识符，没有内部结构。对象的 Id 就是它在对应分量中的身份：`StateId`
指向 `S` 中的一个 `StateObject`，`FuncId` 指向 `F` 中的一个条目，以此类推；`Init` 这样的
映射表正是以这些 Id 为键。

### 3.1 `I`、`O`、`S` 和 `F`

`I`、`O`、`S` 描述模型拥有的数据对象，各自由一个方言类型刻画；`F` 描述模型依赖的外部
函数，各自由一个方言声明种类刻画：

```text
InputObject
  id: InputId
  type: TypeRef

OutputObject
  id: OutputId
  type: TypeRef

StateObject
  id: StateId
  type: TypeRef

ExternFunction
  id: FuncId
  decl: FuncRef
  signature: 由 decl 指向的声明种类定义

ObjectRef = InputId | OutputId | StateId | FuncId
```

| 分量 | 含义 | graph 中的访问方式 |
| --- | --- | --- |
| `I` | 外部输入 | op 读取 |
| `O` | 对外输出 | op 写入 |
| `S` | 持久内部状态 | op 读取和写入 |
| `F` | 外部函数声明 | op 通过 object refs 引用（只读） |

`S` 对象的类型就是它保存的内容的类型；寄存器、锁存器、memory 和边沿历史的区别体现在读写
它们的 op 上，不由状态的类型区分。临时计算结果由 value 传递，不属于 `S`。

`F` 的每个条目声明一个由模型外部实现的函数，例如一个 C 函数。`decl` 指明该函数属于哪种
声明种类；`signature` 的结构由该种类定义，描述参数方向、类型和返回值。`F` 条目只提供
声明，本身不产生任何执行行为。

### 3.2 `Init`

```text
Init = Map<StateId, InitSpec>
```

`Init` 是一个以 `StateId` 为键、`InitSpec` 为值的映射表，且必须是**全映射**：`S` 中每个
`StateId` 恰好有一个条目，不多也不少。`InitSpec` 由目标 `StateObject` 的类型所属
方言定义，由一组**有序初始化步骤**组成；步骤按序应用，后者覆盖前者，应用结果必须使该状态
对象的初始内容完全确定——不存在隐式默认值，步骤未覆盖的部分即为非法。

例如，一个数组状态的 `InitSpec` 可以先从文件加载内容，再用一个步骤覆盖其中几行，效果与
SystemVerilog 中 `$readmemh` 之后紧跟逐行赋值一致。

初始化步骤可以引用模型实例创建时才可用的来源，例如随机数或外部文件。因此同一模型的不同
实例可以得到不同的初始状态 `s_init`；这种差异是模型语义的一部分，不是未定义行为。

`Init` 不属于 `G`，后端也不能把初始化语义转移到自己的数据布局中。

### 3.3 `G`

```text
SimGraph
  ops: SimOp[]
  values: SimValue[]

SimOp
  id: OpId
  op_type: OpRef
  operands: ValueId[]
  results: ValueId[]
  object_refs: ObjectRef[]
  parameters: defined by op_type

SimValue
  id: ValueId
  type: TypeRef
```

每个 value 由一个 op 的 `results` 唯一定义，并可被多个 op 的 `operands` 使用。value 的生产者、
使用者和图的邻接关系都从 op 反查，不重复存储。value 只表达 op 之间的数据流，不表示持久
状态；是否为 value 分配物理存储由后端映射决定。

op 之间的依赖只通过 value 数据流表达；`ops` 的数组顺序不代表任何执行顺序，也不携带
其他语义。

`op_type` 指向该 op 的类型定义。`object_refs` 直接引用 I/O/S/F 对象，每个引用
是读取还是写入由 `op_type` 规定；`parameters` 只保存常量、slice 范围等不经 value 传递的
静态参数，其名称、类型和含义同样由 `op_type` 规定。

### 3.4 来源注解（metadata）

除上述语义分量外，`GrhSimModel` 还携带若干只读 metadata：模型名、对象/value/op 的名字、
origin（源码位置与来源），以及从 GRH 图继承的两份符号清单：

```text
declaredSymbols: String[]          # 源码显式声明符号名（扁平化后为 '$' 连接的层次路径）
generateGroups:  GenerateGroup[]   # generate 作用域声明的逐轮副本分组

GenerateGroup
  scope: String                    # generate 作用域路径，'$' 连接
  name: String                     # 裸声明名
  symbols: String[]                # 各轮副本符号名，下标 = generate-for 轮次
```

metadata 的共同契约：

- 只存名字，不存实体 Id，不构成语义约束；改写 metadata 只产生 metadata revision，
  不会使后端映射失效；
- pass 可以读取（例如用 generateGroups 做并行化候选提示），但**没有维护义务**：
  `compact()` 与各 pass 照常重编号、改写实体，被注解的实体消失后锚点名字可能
  无法再解析到存活 value，属预期行为；
- 等价性判断必须由语义验证兜底，不得以名字文本匹配作为优化触发条件。

JSON checkpoint 中 `declaredSymbols` 与 `generateGroups` 是 `mappings` 之后的可选尾键，
缺省为空集合；旧格式 checkpoint 不含这两键，可直接读取。

## 4. 执行语义

### 4.1 单次图状态转移

把模型某一刻的全部输入、输出和状态分别记为元组 `i`、`o` 和 `s`——形式地说，它们分别取值于
`I`、`O`、`S` 三类对象类型的乘积空间。`G` 诱导确定的状态转移：

```text
eval_G : (I, S) -> (O, S)
(o_next, s_next) = eval_G(i, s)
```

对任意 `i` 和 `s`，`G` 必须唯一确定配对的 `o_next` 与 `s_next`。该映射只规定结果，
不规定后端内部的分区、调度或物理表示。`Init` 在新模型实例创建时求值，给出该实例的初始
状态 `s_init`；`InitSpec` 含随机数或外部文件来源时，不同实例的 `s_init` 可以不同。

### 4.2 从 `G` 推导静止投影 `E`

判断一次迭代后模型是否静止，只需要比较状态中的两类部分：可能影响输出的，和可能影响边沿
判定的。`E` 就是把这两类部分从完整状态中摘出来的投影：`E(s) = (e_edge, e_out)`。

- `e_edge` 包含所有可能影响边沿事件的状态；
- `e_out` 包含所有可能影响输出的状态。

`E` 不作为 `GrhSimModel` 分量存储，而是从 `G` 推导：

1. 从写入 `O` 的 op 沿 value 边反向遍历，得到输出相关的状态依赖闭包；
2. 从执行边沿判定的 op 沿事件 operand 反向遍历，得到边沿相关的状态依赖闭包；
3. 遍历到读取某个 `S` 对象的 op 后，继续从写入该对象的 op 沿 operands 和其他状态读取反向
   传播，直到闭包不再变化。

两个闭包分别确定 `e_out` 和 `e_edge`。不得遗漏闭包内的状态，也不得随意加入闭包外的状态；
`G` 发生变化后必须重新推导 `E`。

定义 `s ~E t` 表示 `E(s) == E(t)`。`E` 必须满足：对于任意 `i`、`s` 和 `t`，如果
`s ~E t`，则二者经过一次 `G` 转移后产生相同输出和相同的下一静止投影。

### 4.3 `eval`

一次 `eval` 在固定输入下重复应用 `eval_G`，直到相邻两次状态的 `E` 相同。它是一个从
`(i, s)` 到 `(o, s_next)` 的部分映射，等价于：

```text
while (true) {
    (o, s_next) = eval_G(i, s)
    if (E(s_next) == E(s)) {
        return (o, s_next)
    }
    s = s_next
}
```

输入 `i` 在整个调用期间保持不变，中间输出不对外发布。如果不存在有限次迭代使 `E` 稳定，
该次 `eval` 不收敛。

新实例以 `Init` 求值得到的 `s_init` 为状态。调用方设置输入后调用 `eval`，返回的状态成为下一次
调用的起始状态；时钟、复位和其他时间推进由调用方改变输入后再次调用 `eval` 表达。

## 5. 后端映射 `BackendMapping`

`BackendId` 是一个仿真后端的唯一名字（例如 `"cpu"`）；`mappings` 就是一张"后端名字 →
映射方案"的表。`BackendMapping` 只描述当前模型如何在对应后端上实现，不能改变模型语义。
不同后端的映射结构可以完全不同，不要求共享固定字段。

每个映射必须只引用当前模型中存在的 I/O/S/F 对象、op、value 和类型，并且对应后端必须支持
模型使用的全部方言及其中实际出现的 op、类型和 parameter 取值。模型改变后，对应
`BackendMapping` 必须重新生成。

[CPU 后端](./backends/cpu.md) 是一个具体案例，其 `CpuBackendMapping` 结构只适用于 CPU，
不构成其他后端的模板。

## 6. Pass 系统

GrhSIM IR pass 可以改写 `GrhSimModel` 的语义分量，也可以创建或改写后端映射。Python 负责
选择 pass、提供参数和决定执行顺序；pass 本身可以由 C++ 或 Python 实现。

语义分量被改写后，所有旧后端映射立即失效，pass manager 必须在返回前重新生成目标后端映射。
完整接口和失效规则见 [Pass System](./passes/overview.md)。

## 7. 验证

`GrhSimModel` 的语义分量必须满足：

- I/O/S/F、op 和 value ID 唯一且引用有效；
- `F` 中每个 `ExternFunction` 的 `decl` 可解析到已加载方言的声明种类，且 `signature` 满足该
  种类的定义；
- `Init` 完整覆盖 `S`，每个 `InitSpec` 的步骤合法、类型正确，且应用后完整覆盖目标状态；
- 每个 `OpRef` 和 `TypeRef` 都能在对应方言中找到；
- value 唯一定义，op 的 operands、results、object refs 和 parameters 满足其方言定义；
- output 和 state 写入具有确定语义；
- `E` 可以从 `G` 推导并满足静止判断的充分性要求。

`mappings` 中存在的每个 `BackendMapping` 必须满足：

- 只引用当前 `GrhSimModel` 中存在的 ID 和类型；
- 对应后端支持 `GrhSimModel` 使用的全部方言及其中实际出现的 op、类型和 parameter 取值；
- 后端专属验证规则成立。

用于执行的 `GrhSimModel` 必须至少携带一个满足上述条件的后端映射。

## 8. 文档

- [Flows](./flows/README.md)
- [CPU 单线程活动度仿真 Flow](./flows/cpu-st.md)
- [Core Dialect](./dialects/core.md)
- [CPU Dialect](./dialects/cpu.md)
- [CPU 后端](./backends/cpu.md)
- [Pass System](./passes/overview.md)
- [GRH IR](../grh/grh-ir.md)
