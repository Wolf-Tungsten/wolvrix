# GRH IR 白皮书

GRH（Graph RTL Hierarchy）是基于 SSA 的 RTL 中间表示，用于表示 SystemVerilog 设计的结构和行为。

# 目录

- [1. 核心概念速通](#1-核心概念速通)
- [2. Value 详解](#2-value-详解)
- [3. Operation 详解](#3-operation-详解)
- [4. Graph 详解](#4-graph-详解)
- [5. Design 详解](#5-design-详解)
- [6. Operation 分类参考](#6-operation-分类参考)
  - [6.1 常量](#61-常量)
  - [6.2 组合运算](#62-组合运算)
  - [6.3 锁存器](#63-锁存器)
  - [6.4 寄存器](#64-寄存器)
  - [6.5 存储器](#65-存储器)
  - [6.6 层次结构](#66-层次结构)
  - [6.7 XMR](#67-xmr)
  - [6.8 系统调用](#68-系统调用)
  - [6.9 DPI](#69-dpi)

---

# 1. 核心概念速通

| 实体 | 说明 | IR 层级 |
|------|------|---------|
| **Value** | SSA 数据流边 | 边 |
| **Operation** | 计算节点 | 顶点 |
| **Graph** | 模块容器 | Module |
| **Design** | 设计，Graph 的集合 | Design |

## 1.1 Value

数据流边，描述 Operation 之间的数据传递关系，满足 SSA 特性（单定义、多使用）。

**字段**：
- `symbol` (`SymbolId`)：信号名（字符串）。Symbol 由 Graph 符号表驻留管理，Value 和 Operation 共享同一符号表，因此信号名不能与 Operation 的符号名冲突
- `width` (`int32_t`)：位宽，仅 Logic 类型有效，必须 > 0（数组 Value 为总位宽，含全部维度）
- `type` (`ValueType`)：数据类型
  - `Logic`：四态逻辑（0/1/x/z）
  - `Real`：实数
  - `String`：字符串
- `isSigned` (`bool`)：是否有符号，仅 Logic 类型有效
- `shape` (`int64[]`)：数组形状，空 = 标量（仅 Logic；行主序纯结构视图，元素宽 = width / ∏shape，见 §2.3）
- `isInput/Output/Inout` (`bool`)：端口标记，通过 `bindXxxPort` 设置
- `definingOp` (`OperationId`)：定义该 Value 的 Operation，端口 Value 为 invalid
- `users` (`ValueUser[]`)：使用该 Value 的 Operation 列表及操作数位置

## 1.2 Operation

操作节点，Graph 的顶点，对输入数据执行特定语义操作并产生输出。涵盖组合计算、数据搬运、存储访问、层次调用等多种行为，通过 `OperationKind` 区分具体语义。

**字段**：
- `kind` (`OperationKind`)：操作类型，决定语义。例如
  - 组合运算：`kAdd`, `kMul`, `kAnd`, `kMux` 等
  - 数据搬运：`kConcat`, `kSliceStatic`, `kAssign` 等
  - 存储访问：`kRegister`, `kRegisterReadArrayPort` 等
  - 层次调用：`kInstance`, `kBlackbox` 等
- `symbol` (`SymbolId`)：符号名（字符串）。Symbol 由 Graph 符号表驻留管理，Value 和 Operation 共享同一符号表，因此符号名不能与 Value 的信号名冲突
- `operands` (`ValueId[]`)：输入值列表，作为操作数的 Value 必须已定义
- `results` (`ValueId[]`)：输出值列表，通常由本 Operation 创建并绑定
- `attrs` (`AttrKV[]`)：属性表，存储额外元数据，如常量值、切片范围等

## 1.3 Graph

Graph 是 Operation 和 Value 的容器，对应一个 SystemVerilog 模块。

**功能**：
- 创建和管理 Operation/Value 及其连接关系
- 声明模块端口（输入/输出/双向）

**示例**：

```sv
module add_sub (
    input  [7:0] a, b,
    input        sel,
    output [7:0] y
);
    assign y = sel ? (a + b) : (a - b);
endmodule
```

对应的 Graph 结构：

```
Graph "add_sub"
├── InputPort "a" -> Value _a (8-bit)
├── InputPort "b" -> Value _b (8-bit)
├── InputPort "sel" -> Value _sel (1-bit)
├── OutputPort "y" -> Value _y (8-bit)
│
├── Operation add (kAdd)
│   ├── operands: [_a, _b]
│   └── result: Value _add_result (8-bit)
│
├── Operation sub (kSub)
│   ├── operands: [_a, _b]
│   └── result: Value _sub_result (8-bit)
│
└── Operation mux (kMux)
    ├── operands: [_sel, _add_result, _sub_result]
    └── result: _y
```

## 1.4 Design

Design 是 Graph 的集合，代表整个设计。

**功能**：
- 管理多个 Graph（模块）
- 标记顶层模块（设计入口）
- 支持模块别名（为 Graph 提供额外名称）

**示例**：

```sv
// 子模块
module child (
    input  [7:0] in,
    output [7:0] out
);
    assign out = in + 1;
endmodule

// 顶层模块
module top;
    wire [7:0] a, b;
    child u_child (.in(a), .out(b));
endmodule
```

对应的 Design 结构：

```
Design
├── Graph "child"
│   ├── InputPort "in"
│   ├── OutputPort "out"
│   └── Operation add (kConstant 1 + input)
│
├── Graph "top" [顶层]
│   ├── Value a, b (内部信号)
│   └── Operation inst (kInstance of "child")
│       ├── operand: a → 连接到 child.in
│       └── result: b ← 连接到 child.out
│
└── 顶层标记: ["top"]
```

---

# 2. Value 详解

## 2.1 Value 是什么

Value 是 GRH IR 中表示**数据流**的核心抽象，它是连接 Operation 的边，满足 SSA（静态单赋值）特性：

- **单定义**：每个 Value 在 Graph 内只被定义一次（通过某个 Operation 的 result 或作为端口 Value）
- **多使用**：可以被多个 Operation 作为操作数引用

Value 的本质作用：
1. **传递数据**：携带类型、位宽、值等数据信息
2. **建立依赖**：通过 `definingOp` 和 `users` 形成数据流图
3. **表示接口**：端口 Value 作为 Graph 与外部模块交互的边界

## 2.2 字段详解

### `symbol`

信号名，本质是字符串，由 Graph 的符号表驻留管理。Value 和 Operation 共享同一符号表，因此：
- 同一 Graph 内，Value 的 symbol 与 Operation 的 symbol 不能重复
- 通过 symbol 可以查找对应的 Value

### `type` 与 `width`/`isSigned`

Value 支持三种数据类型：

**Logic**
- 对应 SystemVerilog 的 `logic`/`wire`/`reg`
- 支持四态：0、1、x（未知）、z（高阻）
- 必须指定位宽（`width > 0`）和是否有符号（`isSigned`）
- 可为数组：`shape` 字段声明维度结构，`width` 恒为总位宽（见 §2.3）

**Real**
- 对应 SystemVerilog 的 `real`
- 用于浮点数运算
- `width` 和 `isSigned` 字段不参与语义

**String**
- 对应 SystemVerilog 的 `string`
- 用于存储文本数据
- `width` 和 `isSigned` 字段不参与语义

### `definingOp` 与 `users`

这两个字段建立 Value 与 Operation 之间的连接关系：

- `definingOp`：指向**定义**该 Value 的 Operation
  - 端口 Value：invalid（不由 Operation 定义，由外部驱动）
  - 内部 Value：指向产生该值的 Operation

- `users`：记录所有**使用**该 Value 的 Operation
  - 每个条目包含：Operation 引用 + 操作数索引
  - 用于替换、删除时的引用追踪

正向追溯：通过 `definingOp` 可以找到 Value 的**来源**（哪个 Operation 产生了它）
反向追踪：通过 `users` 可以找到 Value 的**去向**（被哪些 Operation 使用）

例如表达式 `c = (a + b) + 1`：
- `add1` Operation 计算 `a + b`，产生中间值 `t`
- `t.definingOp` = `add1`，`t.users` = [`add2`]
- `add2` Operation 计算 `t + 1`，产生结果 `c`
- `c.definingOp` = `add2`，`c.users` = []

### 端口标记

- `isInput`：是否绑定到输入端口
- `isOutput`：是否绑定到输出端口  
- `isInout`：是否绑定到双向端口

端口 Value 的特殊性：
- `definingOp` 为 invalid（不由 Operation 定义）
- 由外部驱动（input/inout）或驱动外部（output/inout）

## 2.3 数组类型（shape）

GRH 的 Value 原生支持**单一元素类型、一维到多维、每维长度编译期确定**的数组，类型记作
`UInt8[x][y]` 形式（元素类型 + 维度表 `[x, y]`）。数组 Value 仍服从 SSA：一个数组 Value
只有一个定义 Operation。

**类型模型**：

- `width` 恒为**总位宽**（含全部维度），与标量语义一致；所有按总位宽消费 `width` 的
  现存代码对数组 Value 自动正确。
- `shape` 是 packed 优先的纯结构视图：`std::vector<int64_t>`，空 = 标量，各维 > 0。
- 元素位宽 `elementWidth = width / ∏shape`，必须整除（构造期与 verifier 双重强制）。
- `shape` 只配 `ValueType::Logic`；`Real`/`String` 不数组化。

**行主序布局**：元素 `(i0, i1, ..., in-1)` 的线性位偏移为
`(i0*d1*...*dn-1 + i1*d2*...*dn-1 + ... + in-1) * elementWidth`。
shape 恒对应 "canonical index 0 = 最低元素地址" 的布局；SV 源中的维度方向
（`[4:7]`、`[0:3]` 升序等）不进 shape，由 ingest 在下标归一化时映射掉。

**width / shape 判定规则**：最内层 packed 维是元素位宽本身，不进 shape；shape 的每一维
都是"多少个元素"。unpacked 维折平后排在 packed 维外层（SV 语法天然 unpacked 在外）：

| SV 声明 | width | shape | elementWidth |
|---------|-------|-------|--------------|
| `logic [7:0] a` | 8 | `[]`（标量） | 8 |
| `logic [3:0][7:0] b` | 32 | `[4]` | 8 |
| `logic [1:0][3:0][7:0] c` | 64 | `[2,4]` | 8 |
| `logic [3:0][7:0] d [0:5]` | 192 | `[6,4]` | 8（unpacked 维 6 在外层） |
| `logic [7:0] e [0:3][0:5]` | 192 | `[4,6]` | 8（unpacked 二维） |

位布局与 SV 完全一致：如 `b[i]` = `bits[8i+7 : 8i]`，`b[0]` 是最低字节。

**ingest 视角**（stage-04）：声明层的数组维度全部进 shape（unpacked 前导、packed 内层、
最内层 packed 维为元素位宽）；数组状态统一为 kRegister+shape 与端口族（"reg 还是 mem"
由 grhsim 后端分类，与 ingest 无关）。struct 一律字段级拆分（SoA，`c__DOT__a` 命名、
维度迁移到叶子字段），永不成为 GRH value；struct 端口同样拆分。规则细节见
`docs/ingest/aggregate-types.md`。

**shape 不改变任何 op 语义**（核心原则）：任何 op 对带 shape Value 的语义 ≡ 对其
`width` 位扁平位向量的语义。这正是 SV 对 packed 聚合的 integral 语义（算术进位跨元素
边界合法）；合法 SV 源中 unpacked 数组不可能参与算术/移位/归约（前端直接拒绝），因此
**不设 op 白名单/黑名单**。

shape 感知点（真正读 shape 的地方）只有四个：

- `kSliceArray`：输入 Value 必须 shape 非空（见 §6.2.8）；
- 声明 op 的 `shape` 属性（§6.4）与读口 result 的 shape 继承；
- `kRegister` 数组端口族（索引读/写、部分索引、掩码、填充，§6.4）；
- 实例端口连接的 shape 匹配（totalBits 必须一致，shape 不一致给 warning）。

**shape 推导惯例**（供 ingest/pass 构造 result Value 时使用，不是 verifier 约束；
shape 缺失/未传播只损失结构信息，不影响正确性）：

- `kConcat`：操作数全同形 `[s..]` → result `[n, s..]`（n=操作数个数，沿 leading 维拼接）；
  其余组合 → 标量。
- `kMux` / `kRegisterReadPort` / `kLatchReadPort`：result 继承相应 shape（整体读 =
  声明的完整 shape）。
- `kRegisterReadArrayPort`：索引数 k==rank → 标量（元素）；k<rank → result shape =
  声明 shape 的剩余维（§6.4 端口族规则，该规则是 verifier 强制的）。
- `kSliceArray`：result shape = base shape 的剩余维（§6.2.8，verifier 强制）。
- `kSliceStatic` / `kSliceDynamic`：result 默认标量；需要时可在 totalBits 一致下声明 shape
  （把位段重解释为数组视图）。
- 其余 op：result 默认标量（shape 不传播）。

结构体等异构聚合不产生 GRH Value：ingest 字段级拆分为 `c__DOT__a[n]` 形态的同构数组，
类型系统无需承接异构。

---

# 3. Operation 详解

## 3.1 Operation 是什么

Operation 是 GRH IR 的**操作节点**，Graph 的顶点，代表对数据或系统执行的各种操作。与 Value 配合形成完整的数据流图。

Operation 的行为由 `kind` 字段决定，不同 kind 对应不同的语义：有的执行算术运算，有的定义存储元件，有的实例化其他模块，有的与仿真环境交互。

Operation 的核心特性：
- **类型驱动**：`kind` 字段决定语义，不同 kind 有不同的操作数要求和结果数量
- **输入输出明确**：通过 `operands` 接收 Value，通过 `results` 产生 Value（部分 Operation 可能没有 results）
- **可携带元数据**：`attrs` 存储额外信息，如常量值、切片范围、实例化参数等

## 3.2 字段详解

### `kind`

操作类型，由 `OperationKind` 枚举定义，决定 Operation 的语义。主要分类：

- **组合运算**：`kAdd`, `kSub`, `kMul`, `kAnd`, `kOr`, `kMux` 等，输出仅取决于输入
- **数据搬运**：`kConcat`, `kSliceStatic`, `kSliceDynamic`, `kAssign` 等，重组或传递数据
- **时序存储**：`kRegister`（含数组寄存器）, `kLatch` 及其读写端口，涉及状态保持
- **层次调用**：`kInstance`, `kBlackbox`, `kXMRRead/Write`，与外部模块交互
- **系统调用**：`kSystemFunction`, `kSystemTask`, `kDpicCall`，副作用明确

### `symbol`

符号名（字符串），由 Graph 符号表驻留管理。与 Value 共享同一符号表，因此：
- Operation 的 symbol 不能与 Value 的信号名重复
- 通过 symbol 可以查找对应的 Operation
- 部分 Operation（如内部临时节点）可能使用工具生成的内部名

### `operands` 与 `results`

- `operands`：输入 Value 列表，表示操作的数据来源
  - 每个 operand 必须是已定义的 Value
  - operand 的顺序通常有语义（如 kSub 的第0个是被减数，第1个是减数）

- `results`：输出 Value 列表，表示操作产生的数据
  - 通常由本 Operation 创建并绑定
  - 结果数量由 kind 决定：大部分产生1个结果，部分（如 kInstance）可能产生多个

**连接约束**：
- operand 的 Value 必须在同一 Graph 内定义
- result 的 Value 的 definingOp 指向本 Operation
- 删除 Operation 前需处理其 results 的 users

### `attrs`

属性表，存储 kind 无法表达的额外元数据，键值对形式：`{key: value}`

**key**：字符串，不经符号表驻留，直接存储原始字符串

**value** 支持以下类型（`AttributeValue`）：
- 标量：`bool`、`int64`、`double`、`string`
- 数组：`bool[]`、`int64[]`、`double[]`、`string[]`

**常见属性示例**：
- `kConstant`：`{"constValue": "8'hFF"}`
- `kSliceStatic`：`{"sliceStart": 8, "sliceEnd": 15}`
- `kInstance`：`{"moduleName": "adder", "instanceName": "u_add"}`
- `kRegister`：`{"initValue": "8'd0"}`

## 3.3 Operation 分类

所有 OperationKind 的详细分类介绍请参考 [6. Operation 分类参考](#6-operation-分类参考)。

---

# 4. Graph 详解

## 4.1 Graph 是什么

Graph 是 GRH IR 中**模块级别**的核心容器，对应一个 SystemVerilog 模块（`module`）。

一个 Graph 包含：
- **Operation 集合**：模块内的所有操作节点
- **Value 集合**：模块内的所有数据流边（SSA 形式）
- **端口声明**：与外部模块交互的接口定义
- **符号表**：管理 Value 和 Operation 的符号名

## 4.2 端口系统

Graph 支持三种端口类型，对应 SystemVerilog 的端口方向：

| 类型 | SystemVerilog | IR 表示 | 说明 |
|------|---------------|---------|------|
| **Input** | `input` | `Port{name, value}` | 外部输入到模块 |
| **Output** | `output` | `Port{name, value}` | 模块输出到外部 |
| **Inout** | `inout` | `InoutPort{name, in, out, oe}` | 双向端口，三态分解 |

### 4.2.1 Input / Output 端口

Input 和 Output 端口通过 **Value** 与 Graph 内部连接：

- **Input**：外部驱动 Value，内部可以读取
  - Value 的 `definingOp` 为 invalid（不由内部 Operation 定义）
  - Value 标记为 `isInput = true`

- **Output**：内部驱动 Value，外部可以读取  
  - Value 由某个 Operation 的结果产生
  - Value 标记为 `isOutput = true`

### 4.2.2 Inout 端口的三态分解

SystemVerilog 的 `inout` 在 GRH 中被分解为**三个信号**：

| 信号 | 方向 | 说明 |
|------|------|------|
| `in` | Input | 外部输入到模块的数据 |
| `out` | Output | 模块输出到外部的数据 |
| `oe` | Input | 输出使能（Output Enable），控制三态门 |

**示例**：GPIO Pin 控制器

SystemVerilog 实现：
```sv
module gpio_pin (
    inout  wire       pad,      // 物理引脚，双向
    input  wire       dir,      // 方向控制：1=输出，0=输入
    input  wire       drv_val,  // 输出驱动值
    output wire       rd_val    // 读取到的输入值
);
    // 三态门控制
    assign pad = dir ? drv_val : 1'bz;
    assign rd_val = pad;
endmodule
```

对应的 GRH 结构：

```
Graph "gpio_pin"
│
├── InoutPort "pad"
│   ├── in:  Value pad_in      (1-bit)  ← 外部输入到模块
│   ├── out: Value pad_out     (1-bit)  → 模块输出到外部
│   └── oe:  Value pad_oe      (1-bit)  ← 输出使能
│
├── InputPort "dir" → Value dir         (1-bit)
├── InputPort "drv_val" → Value drv_val (1-bit)
├── OutputPort "rd_val" → Value rd_val  (1-bit)
│
├── Operation pad_out_assign (kAssign)
│   ├── operands: [drv_val]
│   └── result: pad_out
│
├── Operation pad_oe_assign (kAssign)
│   ├── operands: [dir]
│   └── result: pad_oe
│
└── Operation rd_val_assign (kAssign)
    ├── operands: [pad_in]
    └── result: rd_val
```

**信号流向说明**：
- 当 `dir = 1`（输出模式）：`pad_oe = 1`，`pad_out = drv_val`，外部读取 `drv_val`
- 当 `dir = 0`（输入模式）：`pad_oe = 0`，`pad_out` 无效，模块通过 `pad_in` 读取外部信号
- `rd_val` 始终反映引脚当前状态（无论输入还是输出模式）

这种分解使得三态逻辑的语义更加明确，便于后续的分析和综合。

## 4.3 符号系统（Symbol System）

**符号（Symbol）**是 Graph 内用于**标识** Value 和 Operation 的字符串名称，可以理解为标识符（Identifier）。每个 Symbol 在 Graph 内通过 **Symbol Table**（符号表）进行管理。

在 GRH IR 中：
- 每个 **Value** 必须关联一个 Symbol（如信号名 `data_reg`）
- 每个 **Operation** 必须关联一个 Symbol（如操作名 `add_op`）
- **Symbol Table** 负责 Symbol 的分配、去重和查找

Symbol 的作用：
1. **可读性**：在调试和导出输出时显示有意义的名称
2. **可查找性**：通过 Symbol 定位特定的 Value 或 Operation
3. **调试映射**：关联回原始 SystemVerilog 源码中的标识符

### 4.3.1 符号唯一性约束

**关键约束**：同一 Graph 的 Symbol Table 内，所有 Symbol **必须唯一**，任意两个 Value、或任意两个 Operation、或 Value 与 Operation 之间都不能共享同一个 Symbol。

示例（非法）：
```
Graph  // 错误！Symbol 冲突
├── Value with Symbol "data"       // 第一个 "data"
├── Value with Symbol "data"       // 错误：Value 之间 Symbol 重复
├── Operation with Symbol "add"
└── Operation with Symbol "add"    // 错误：Operation 之间 Symbol 重复
```

此外，Value 与 Operation 的 Symbol 也不能重复：
```
Graph  // 错误！Symbol 冲突
├── Value with Symbol "foo"
└── Operation with Symbol "foo"    // 错误：不能与 Value 的 Symbol 重复
```

### 4.3.2 声明符号（Declared Symbol）

Graph 维护一个 **Declared Symbol** 列表，记录来自用户源码的显式声明标识符。

**用途**：
- 区分**用户声明的信号**（Declared Symbol）与工具生成的内部 Symbol
- 在输出阶段优先保留用户命名的 Symbol
- 死代码消除时保护声明但未使用的 Symbol（用于调试或保留接口）

**示例**：
```sv
module example (
    input  wire a,      // "a" 是 Declared Symbol
    output wire b       // "b" 是 Declared Symbol
);
    wire temp;          // "temp" 是 Declared Symbol
    assign b = a;
endmodule
```

上述模块的 Declared Symbol 列表包含：`a`、`b`、`temp`

即使 `temp` 未被使用，由于它是 Declared Symbol，工具可能会保留它用于调试或报告。

**Generate 块内声明**：

generate 块（for/if/case）内声明的网表与变量同样进入 Declared Symbol。由于同一声明在 elaboration 后存在多份副本，块内符号以**作用域限定名**登记：每一层 generate-for 轮次分量形如 `<块名>$<轮次>`，叶子为裸声明名，分量之间以 `$` 连接。

```sv
generate
    for (i = 0; i < 2; i++) begin : gen_loop
        logic [7:0] sig;    // 登记为 "gen_loop$0$sig"、"gen_loop$1$sig"
    end
endgenerate
```

嵌套 generate 作用域逐层拼接（如 `outer$2$inner$5$sig`）。standalone if/case generate 块分量只有块名、无轮次（如 `gen_if$sig`）。

**Generate 副本分组（Generate Group）**：

Graph 额外维护 **Generate Group** 列表（`generateGroups()`），作为 Declared Symbol 之上的纯来源注解，描述"同一 generate 块内声明的各 elaboration 副本"。每个组包含：
- `scope`：无轮次的作用域路径（嵌套时以 `$` 连接块名，如 `gen_loop`、`outer$inner`）
- `name`：裸声明名（如 `sig`）
- `symbols`：按 elaboration 顺序排列的各副本 SymbolId（如 `gen_loop$0$sig`、`gen_loop$1$sig`）

语义约定：
- 组成员**同时**出现在 Declared Symbol 列表中；组只是分组视图，不改变符号的声明语义
- 组仅在 ingest 阶段生成；`cloneGraph` 会随符号重映射复制组
- `eraseOp`/`eraseValue` 擦除组内成员实体时，会同步把该 SymbolId 从所有组中移除；组本身保留（允许空组）
- `setOpSymbol`/`setValueSymbol` 只改实体绑定的 Symbol，不维护组成员身份（成员身份按 SymbolId 键控）

不纳入 Declared Symbol / Generate Group 的类别：`parameter`/`localparam`、过程块（always/initial 等）内的局部变量。

**hier-flatten 下的形态约定**：

`hier-flatten` 内联子图时，declaredSymbol 与 generateGroups 按同一路径传播：

- 子图的 declared value/op 一律改名为 `$` 连接的层次路径（`inst$...$name`；generate 副本为 `inst$...$gen_loop$i$sig`，实例前缀叠加在 ingest 期已写入的 generate 分量之前）并重新登记为 declared，与 `symProtect` 模式无关（该选项目前只控制父图未声明端口值的改名）；未声明的子图符号一律改内部名 `_val_N`/`_op_N`。
- **顶图符号保持原名**（层次路径之根）；端口映射冲突时父图 declared 名优先，子图端口名不保留（不引入双名/alias）。
- generateGroups 随 declared 身份同路径传播：组的 `scope` 叠加实例路径前缀（如 `gen_loop` → `u_inst$gen_loop`，嵌套实例逐级叠加），`name` 保持裸声明名不变，组内成员重映射到克隆/改名后的新符号（成员名形如 `u_inst$gen_loop$i$sig`）。
- 同一模块的多实例产生各自独立的组（`scope` 前缀不同）；成员符号在内联中被丢弃时（如子图端口名被父图名取代）相应地从组中移除；组变空则不写入主图。
- flatten 对已展平图是幂等的：前缀只在内联 `kInstance` 时叠加，而实例 op 随内联被删除，重复执行不会二次加前缀。

**lower 到 GrhSIM IR 后的去向**：

`lower_grhsim`（`keep_declared_symbols=True`，默认）把 declaredSymbols 与 generateGroups
原样携带为模型的只读 metadata，并为每个能解析到存活 value/state/function 的声明建立
`declProvenance` 记录（direct 全覆盖 slice，数组声明保留形状），供 GrhSIM 侧优化与调试
反查。表示、更新规则与 JSON 编码见 `docs/grhsim_ir/overview.md` 第 3.4.1 节。

---

# 5. Design 详解

## 5.1 Design 是什么

Design 是 GRH IR 中**设计级别**的顶层容器，代表整个 SystemVerilog 设计。它是 Graph 的集合，容纳设计中的全部模块及其层次关系。

一个 Design 包含：
- **Graph 集合**：设计中的所有模块
- **顶层模块标记**：指定设计的入口点
- **模块别名映射**：为 Graph 提供额外名称，便于查找/输出
- **Design Symbol Table**：管理 Design 级符号（Graph 名称、Declared Symbol 等）

## 5.2 顶层模块

Design 支持标记**一个或多个**顶层模块（Top-Level Module）。

## 5.3 参数化模块处理

SystemVerilog 支持参数化模块（Parameterized Module）：
```sv
module adder #(parameter WIDTH = 8) (input [WIDTH-1:0] a, b, output [WIDTH-1:0] y);
    assign y = a + b;
endmodule

adder #(8)  u1 (...);  // 8位加法器
adder #(16) u2 (...);  // 16位加法器
```

**GRH IR 处理方式**：
- 不同参数值的实例产生**不同的 Graph**
- 因为内部 Value 的位宽不同，Operation 的结构也不同
- Graph 命名由前端决定，示例中的名称仅用于说明

上例在 GRH 中表示为：
```
Design
├── Graph "adder__8"   // WIDTH=8 的实例
│   └── Value 位宽为 8
├── Graph "adder__16"  // WIDTH=16 的实例
│   └── Value 位宽为 16
└── Graph "top"
    ├── Operation kInstance of "adder__8"
    └── Operation kInstance of "adder__16"
```

## 5.4 Design 符号系统

Design 维护 **Design Symbol Table** 管理 Graph 名称，与 Graph 内部的 Symbol Table 形成两级架构。

### 5.4.1 两级架构

| 符号表 | 作用域 | 管理对象 |
|--------|--------|----------|
| **Design Symbol Table** | Design 级别 | Graph 名称 |
| **Graph Symbol Table** | Graph 级别 | Value 和 Operation 的 Symbol |

Design Symbol Table 中每个 Graph 名称必须唯一。

### 5.4.2 模块别名

**模块别名（Graph Alias）**允许一个 Graph 拥有多个名称，同样由 Design Symbol Table 管理。

**应用场景**：
- **提高可读性**：为自动生成的唯一 Graph 名称（如 `adder__p_WIDTH_8`）提供简洁别名（如 `adder_8bit`）
- **保留原始名称**：防止参数化的顶层模块在输出时被强制改名，通过别名保留用户声明的模块名

---

# 6. Operation 分类参考

本章按功能分类介绍所有 Operation，包括操作语义、操作数要求、结果数量及常用属性。

## 6.1 常量

### kConstant

**operands**: 无

**results**:
- `res[0]`: 产生的常量值

**attrs**:
- `constValue` (string): Verilog 常量语法，如 `"8'hEF"`、`"16'sd-5"`，支持含 `x`/`z`

**语义**:
```
res[0] = constValue
```

## 6.2 组合运算

输出仅取决于当前输入的组合逻辑运算。

### 6.2.1 算术运算

四态语义：遵循 SystemVerilog 规则；当操作数含 `X`/`Z` 时，结果可能为 `X`（保守传播）。

**operands**:
- `oper[0]` (`L`): 左操作数
- `oper[1]` (`R`): 右操作数

**results**:
- `res[0]`: 运算结果

**attrs**: 无

| 操作符 | 语义 | `res[0]` 位宽 |
|--------|------|---------------|
| `kAdd` | `oper[0] + oper[1]` | `max(L, R)` |
| `kSub` | `oper[0] - oper[1]` | `max(L, R)` |
| `kMul` | `oper[0] * oper[1]` | `L + R` |
| `kDiv` | `oper[0] / oper[1]` | `L`（除数 0 行为由 SV 标准定义）|
| `kMod` | `oper[0] % oper[1]` | `L`（除数 0 行为由 SV 标准定义）|


### 6.2.2 位运算

按位逻辑运算。四态语义：遵循 SV 四态真值表；`0`/`1` 优先确定，无法确定时返回 `X`。

**二元位运算（kAnd / kOr / kXor / kXnor）**

- **operands**: `oper[0]` (`L`), `oper[1]` (`R`)
- **results**: `res[0]`，位宽 `max(L, R)`
- **attrs**: 无

| 操作符 | 语义 |
|--------|------|
| `kAnd` | `oper[0] & oper[1]` |
| `kOr` | `oper[0] \| oper[1]` |
| `kXor` | `oper[0] ^ oper[1]` |
| `kXnor` | `oper[0] ~^ oper[1]` |

**一元位运算（kNot）**

- **operands**: `oper[0]`
- **results**: `res[0]`，位宽 `width(oper[0])`
- **attrs**: 无
- **语义**: `res[0] = ~oper[0]`


### 6.2.3 比较运算

关系比较运算，结果均为 1-bit Logic。

**operands**:
- `oper[0]` (`L`): 左操作数
- `oper[1]` (`R`): 右操作数

**results**:
- `res[0]`: 比较结果，1-bit

**attrs**: 无

| 操作符 | 语义 | 四态处理 |
|--------|------|----------|
| `kLt` | `oper[0] < oper[1]` | 任一位含 `X`/`Z` 时结果为 `X` |
| `kLe` | `oper[0] <= oper[1]` | 任一位含 `X`/`Z` 时结果为 `X` |
| `kGt` | `oper[0] > oper[1]` | 任一位含 `X`/`Z` 时结果为 `X` |
| `kGe` | `oper[0] >= oper[1]` | 任一位含 `X`/`Z` 时结果为 `X` |
| `kEq` | `oper[0] == oper[1]` | 任一位含 `X`/`Z` 且不形成确定不等时结果为 `X` |
| `kNe` | `oper[0] != oper[1]` | 任一位含 `X`/`Z` 且不形成确定不等时结果为 `X` |
| `kCaseEq` | `oper[0] === oper[1]` | 按位精确比较（含 `X`/`Z`），结果恒为 `0`/`1` |
| `kCaseNe` | `oper[0] !== oper[1]` | 按位精确比较（含 `X`/`Z`），结果恒为 `0`/`1` |
| `kWildcardEq` | `oper[0] ==? oper[1]` | 任一操作数的 `X`/`Z` 视为通配符，结果恒为 `0`/`1` |
| `kWildcardNe` | `oper[0] !=? oper[1]` | 任一操作数的 `X`/`Z` 视为通配符，结果恒为 `0`/`1` |


### 6.2.4 逻辑运算

先将操作数规约为 1-bit 逻辑值 `{0,1,X}` 再计算，`X` 保持传播。

**二元逻辑运算（kLogicAnd / kLogicOr）**

- **operands**: `oper[0]`, `oper[1]`
- **results**: `res[0]`，1-bit
- **attrs**: 无

| 操作符 | 语义 |
|--------|------|
| `kLogicAnd` | `oper[0] && oper[1]` |
| `kLogicOr` | `oper[0] \|\| oper[1]` |

**一元逻辑运算（kLogicNot）**

- **operands**: `oper[0]`
- **results**: `res[0]`，1-bit
- **attrs**: 无
- **语义**: `res[0] = !oper[0]`


### 6.2.5 规约运算

对操作数所有位进行归约运算，结果为 1-bit。任一位含 `X`/`Z` 且无法确定结果时返回 `X`。

**operands**:
- `oper[0]`: 操作数

**results**:
- `res[0]`: 运算结果，1-bit

**attrs**: 无

| 操作符 | 语义 |
|--------|------|
| `kReduceAnd` | `&oper[0]` |
| `kReduceNand` | `~&oper[0]` |
| `kReduceOr` | `\|oper[0]` |
| `kReduceNor` | `~\|oper[0]` |
| `kReduceXor` | `^oper[0]` |
| `kReduceXnor` | `~^oper[0]` |

---

### 6.2.6 移位运算

四态语义：遵循 SystemVerilog 规则；当操作数或移位量含 `X`/`Z` 时，结果可能为 `X`（保守传播）。

**operands**:
- `oper[0]` (`L`): 被移位操作数
- `oper[1]`: 移位位数（无符号解释）

**results**:
- `res[0]`: 移位结果，位宽 `L`

**attrs**: 无

| 操作符 | 语义 |
|--------|------|
| `kShl` | `oper[0] << oper[1]`，逻辑左移 |
| `kLShr` | `oper[0] >> oper[1]`，逻辑右移 |
| `kAShr` | `oper[0] >>> oper[1]`，算术右移 |



### 6.2.7 数据选择（kMux）

**operands**:
- `oper[0]`: 选择条件（1-bit）
- `oper[1]` (`W`): 真分支值
- `oper[2]` (`W`): 假分支值

**results**:
- `res[0]`: 选择结果，位宽 `W`

**attrs**: 无

**语义**:
```
res[0] = oper[0] ? oper[1] : oper[2]
```

四态语义：
- `oper[0] = 1` 时，`res[0] = oper[1]`
- `oper[0] = 0` 时，`res[0] = oper[2]`
- `oper[0] = X/Z` 时，逐位融合：`oper[1][i] == oper[2][i]` 则取该值，否则为 `X`



### 6.2.8 切片

位/数组切片操作，用于从信号中提取部分位或数组元素。

**kSliceStatic**（静态常量切片）

**operands**:
- `oper[0]`: 被截取信号（位宽 `W`）

**results**:
- `res[0]`: 截取结果，位宽 `sliceEnd - sliceStart + 1`

**attrs**:
- `sliceStart` (int64_t): 起始位（含），LSB=0
- `sliceEnd` (int64_t): 结束位（含），要求 `sliceEnd >= sliceStart`

**语义**:
- 当 `sliceStart == sliceEnd` 时：`res[0] = oper[0][sliceStart]`（1-bit 位选择）
- 当 `sliceStart < sliceEnd` 时：`res[0] = oper[0][sliceEnd : sliceStart]`（范围选择）



**kSliceDynamic**（动态偏移切片）

**operands**:
- `oper[0]`: 被截取信号（位宽 `W`）
- `oper[1]`: 起始偏移（无符号解释）

**results**:
- `res[0]`: 截取结果，位宽 `sliceWidth`

**attrs**:
- `sliceWidth` (int64_t): 截取位宽，必须大于 0

**语义**:
- 当 `sliceWidth == 1` 时：`res[0] = oper[0][oper[1]]`（1-bit 位选择）
- 当 `sliceWidth > 1` 时：`res[0] = oper[0][oper[1] +: sliceWidth]`（索引部分选择）



**kSliceArray**（数组元素访问，stage-02 重定义）

**operands**:
- `oper[0]`: 数组 value（`shape = [d0, d1, ..., dn-1]` 非空，位宽 `W`）
- `oper[1]`..`oper[k]` (`i0`..`ik-1`): 下标（标量 value，按无符号解释），`1 <= k <= n`

**results**:
- `res[0]`: 当 `k == n` 时为元素标量；当 `k < n` 时为 shape = `[dk, ..., dn-1]` 的子数组

**attrs**: 无（不再有 `sliceWidth`；元素几何完全来自 oper[0] 的 shape）

**语义**:

元素位宽 `E = W / ∏shape`，结果等价于从 oper[0] 的扁平位表示中取出自
`offset = (i0*d1*...*dn-1 + i1*d2*...*dn-1 + ... + ik-1*dk*...*dn-1) * E`
开始的 `res[0].width` 位（行主序，见 §2.3）。

任一下标 >= 对应维长度时行为为实现定义（仿真端按实现截断/钳位，不做跨实现约定）。

**示例**（`a` 为 `UInt8[4][6]`，即 width=192、shape=[4,6]）：
```
%e = kSliceArray(%a, %i, %j)   // k=2=n，结果标量，width=8
%r = kSliceArray(%a, %i)       // k=1<n，结果 width=48、shape=[6]（子数组）
```

**分工惯例**：数组元素访问一律用 kSliceArray；纯位段访问用 kSliceStatic/kSliceDynamic
（后者可用于任何 value，含带 shape 的——packed 扁平语义下合法）。verifier（`grh.verify`）
强制：kSliceArray 的 oper[0] 必须 shape 非空、索引数 `1 <= k <= rank`、索引为标量、
结果 width/shape 与 shape 几何一致。



### 6.2.9 赋值与数据重组

**kAssign**（连续赋值）

- **operands**: `oper[0]`（输入信号，位宽 `W`）
- **results**: `res[0]`，位宽 `W`
- **attrs**: 无
- **语义**: `res[0] = oper[0]`

**kConcat**（位拼接）

- **operands**: `oper[0]`, `oper[1]`, ..., `oper[N-1]`（待拼接信号）
- **results**: `res[0]`，位宽 `sum(width(oper[i]))`
- **attrs**: 无
- **语义**: `res[0] = {oper[0], oper[1], ..., oper[N-1]}`
- **说明**: `oper[0]` 在高位，`oper[N-1]` 在低位

**kReplicate**（位复制）

- **operands**: `oper[0]`（被复制信号，位宽 `W`）
- **results**: `res[0]`，位宽 `W * rep`
- **attrs**:
  - `rep` (int64_t): 复制次数，必须大于 0
- **语义**: `res[0] = {rep{oper[0]}}`

## 6.3 锁存器

GRH IR 使用**声明 + ReadPort + WritePort**的组合建模锁存器。所有对锁存器的访问必须通过对应的 ReadPort/WritePort。

### kLatch

锁存器声明。Operation 的 symbol 作为锁存器名称。

**operands**: 无

**results**: 无

**attrs**:
- `width` (int64_t): 位宽
- `isSigned` (bool): 是否有符号



### kLatchReadPort

锁存器读端口。

**operands**: 无

**results**:
- `res[0]`: 读出的锁存器值，位宽同目标 kLatch

**attrs**:
- `latchSymbol` (string): 指向目标 kLatch 的 symbol

**语义**:
```
res[0] = <latchSymbol>
```



### kLatchWritePort

锁存器写端口。

**operands**:
- `oper[0]` (updateCond): 更新使能条件（1-bit），为 1 时允许更新
- `oper[1]` (nextValue): 更新值（位宽同目标 kLatch），reset/enable 优先级需通过 `kMux` 在外部编码
- `oper[2]` (mask): 逐位写掩码（位宽同目标 kLatch），`mask[i]=1` 时写入第 `i` 位

**results**: 无

**attrs**:
- `latchSymbol` (string): 指向目标 kLatch 的 symbol

**语义**:

设目标 kLatch 位宽为 `W`。以 `updateCond` 非常量（有条件更新）为例：

**Mask 全 1**：
```sv
always_latch
    if (updateCond)
        <latchSymbol> <= nextValue;
```

**Mask 为变量**：
```sv
always_latch
    if (updateCond)
        for (int i = 0; i < W; i++)
            if (mask[i])
                <latchSymbol>[i] <= nextValue[i];
```

## 6.4 寄存器

GRH IR 使用**声明 + 端口族**的组合建模全部时序状态。数组状态（含原"存储器"形态）
统一由带 `shape` 的 kRegister 声明承接——**IR 层不存在独立的 memory 概念**；reg/mem 的
存储分类（双缓冲逐行检测 vs 边界采样原地提交）是仿真后端的实现策略，由 grhsim 的
A7 select-state-stores 重新推导，不属于 IR 语义。

### kRegister

寄存器声明。Operation 的 symbol 作为寄存器名称。

**operands**: 无

**results**: 无

**attrs**（类型契约，与 value 的 width/shape 对应）:
- `width` (int64_t): 位宽（数组寄存器为总位宽，含全部维度）
- `isSigned` (bool): 是否有符号
- `shape` (int64[], 可选): 数组形状（见 §2.3），非空时声明一个数组寄存器；
  与 `width` 同属声明的类型契约，不是可选注解

**可选初始化属性**:
- `initValue` (string): 单值初始化（标量寄存器；数组寄存器上表示摊平后的整数组位串）
- `initKind` / `initFile` / `initValue` / `initStart` / `initLen`（数组形态）:
  分段初始化族，仅当 `shape` 非空时可用。地址空间按行主序摊平（第 0 维最外，
  下标不足时作用于高维子数组，与端口族的索引规则一致）

**shape 语义**:

`shape` 非空时，寄存器是 `width / ∏shape` 位元素的多维数组（行主序，canonical index 0 =
最低元素地址，见 §2.3）。整体读写使用 kRegisterReadPort/kRegisterWritePort 族，
按维度的索引读写使用 kRegisterReadArrayPort/kRegisterWriteArrayPort 族。

**初始化语义**:

单值 `initValue` 表示寄存器的初始赋值。若源设计存在多条 initial 赋值，应在构建 IR 前
规整为单值。`initKind` 族为数组、支持多种初始化方式混合：第 `i` 个元素按索引顺序执行，
越靠后的优先级越高。

**分段初始化族的编码约定**：
- `initKind` 与 `initFile` **必须同时存在且长度相同**，即使 `initKind` 是 `literal`
  也需要占位的 `initFile` 项。
- `initStart`/`initLen` **必须存在且长度等于 `initKind`**。
- `initStart < 0`：readmem 省略范围参数；literal 表示全量初始化。
- `initStart >= 0` 且 `initLen <= 0`：readmem 仅给起始地址（读到末尾）；literal 视为非法。
- `initValue` 缺省或长度不足时默认 `"0"`。若需要随机初始化，请显式写成 `$random`
  或 `$random(seed)`。

| `initKind[i]` | 所需 attr | 等价 SV 片段 |
|---------------|-----------|----------|
| `readmemh` | `initFile[i]`, `initStart[i]`, `initLen[i]` | `$readmemh("<initFile[i]>", <symbol>[, start[, finish]]);` |
| `readmemb` | `initFile[i]`, `initStart[i]`, `initLen[i]` | `$readmemb("<initFile[i]>", <symbol>[, start[, finish]]);` |
| `literal` | `initValue[i]`, `initStart[i]`, `initLen[i]` | `<symbol>[<addr>] = <initValue[i]>;` / 区间 `for` 循环 |

**示例**（标量寄存器，`initValue` = `$random`）：
```sv
initial begin
    reg_name = $random;
end
```

**示例**（数组寄存器：从文件初始化，再用 `$random` 覆盖部分地址）：
- kRegister `mem`，`width` = 128，`shape` = [16]（16 行 × 8 位）
- `initKind` = ["readmemh", "literal"]，`initFile` = ["mem.hex", ""]
- `initValue` = ["", "$random"]，`initStart` = [-1, 0]，`initLen` = [0, 1]
- 语义示例：
  ```sv
  initial begin
      $readmemh("mem.hex", mem);  // initKind[0]
      mem[0] = $random;            // initKind[1]，覆盖地址 0
  end
  ```



### 端口族总览与公共约定

读端口：

| op | operands | results |
|----|----------|---------|
| kRegisterReadPort | 无 | 全数组 value（shape 继承声明） |
| kRegisterReadArrayPort | `[i0, ..., ik-1]`，1 ≤ k ≤ rank | k==rank → 元素标量；k<rank → 子数组 value（shape=剩余维） |

写端口（均带 1-bit `updateCond` 与事件尾；attrs 均为 `regSymbol` + `eventEdge`）：

| op | operands | 语义 |
|----|----------|------|
| kRegisterWritePort | `[updateCond, nextValue, events...]` | 整体写（nextValue=全数组 value） |
| kRegisterWriteMaskPort | `[updateCond, nextValue, mask, events...]` | 整体写+位掩码（mask 宽=总位宽） |
| kRegisterWriteArrayPort | `[updateCond, i0..ik-1, data, events...]` | 索引写（k ≥ 1；k<rank 时 data 为子数组） |
| kRegisterWriteArrayMaskPort | `[updateCond, i0..ik-1, data, mask, events...]` | 索引写+位掩码（mask 宽=data 宽） |
| kRegisterFillPort | `[updateCond, data, events...]` | 广播填充（data=元素宽，写满全部 ∏shape 元素） |

公共约定：

- **`regSymbol` 必须指向同图的 kRegister**（全部写端口必然与声明同图）。
- **部分索引规则与 kSliceArray 完全对齐**（§6.2.8）：下标数 k < rank 时作用在高维
  子数组上——kSliceArray 作用于组合 value，数组端口作用于状态，共享同一套
  "下标不足 = 高维子数组"语义。线性位偏移一律按行主序公式（§2.3），IR 层不出现
  kMul/kAdd 寻址树。
- 索引操作数必须是标量（无 shape）。越界索引行为与 kSliceArray 同条款（实现定义）。
- **事件操作数起始下标由 `firstEventOperandIndex(op)` helper 集中计算**（grh.hpp）：
  事件尾与 `eventEdge` attr 一一对应，数组写端口的索引数随部分索引变化，边界由
  `eventEdge` 数量反推（声明的 rank 无法唯一确定部分索引数）。写端口的 `eventEdge`
  必须非空——没有边沿事件的"写"应建模为组合逻辑。
- mask 语义唯一：**位掩码**（mask[i]=1 时写入第 i 位）。不提供元素粒度掩码。
- **写端口优先级**：组 = 同 regSymbol 的全部写端口（含 kRegisterFillPort），
  不要求相同事件/边沿。同轮同时激活时按**op 声明顺序**依次应用，后声明者覆盖同地址
  先写者。**同 regSymbol 写端口 op 的相对顺序是 IR 语义的一部分——pass 不得重排**
  （JSON ops 数组有序，持久化免费；grh.verify 校验端口与声明同图兜底）。
  SV emitter 在同 always 块内按声明顺序排列（天然后写覆盖先写）；跨事件写端口的顺序
  无法被 SV 表达，emitter 报 warning 并按事件分块、块内按声明顺序输出（best-effort，
  语义以 grhsim 后端为准）。

### kRegisterReadPort

寄存器整体读端口。

**operands**: 无

**results**:
- `res[0]`: 读出的寄存器值，宽度与目标 kRegister 相同。shape 契约（stage-07 修订）：
  整体读结果是寄存器位内容的结构视图——可以为空（扁平视图，哪怕寄存器带 shape），
  也可以携带任意精确铺满总位宽的 tiling shape（哪怕寄存器是标量轨无 shape，
  例如视图来自端口连接对侧的 packed 类型）。verifier 只校验位宽一致与 shape 铺满。

**attrs**:
- `regSymbol` (string): 指向目标 kRegister 的 symbol

**语义**:
```
res[0] = <regSymbol>
```



### kRegisterReadArrayPort

寄存器索引读端口（状态的组合读，含原"异步存储器读"语义）。

**operands**:
- `oper[0]`..`oper[k-1]` (i0..ik-1): 索引（标量），1 ≤ k ≤ 声明 rank

**results**:
- `res[0]` (data): k==rank → 元素标量（宽=元素宽）；k<rank → 子数组 value
  （shape=声明 shape 的剩余维，宽=子数组总宽）

**attrs**:
- `regSymbol` (string): 指向目标 kRegister 的 symbol

**语义**（声明 `width=128, shape=[4,4]`，元素 8 位）:
```
kRegisterReadArrayPort(%i)     → value width=32 shape=[4]   // 读一行（子数组）
kRegisterReadArrayPort(%i, %j) → value width=8              // 读一个元素
```

**说明**: 同步读通过 kRegister 捕获本端口的输出实现（memory-read-retime pass 识别
该模式并改写为数据侧寄存器）。



### kRegisterWritePort

寄存器整体写端口。支持多事件触发（如时钟上升沿 + 异步复位下降沿）。

**operands**:
- `oper[0]` (updateCond): 更新使能条件（1-bit），为 1 时允许更新
- `oper[1]` (nextValue): 更新值（宽度同目标 kRegister），reset/enable 优先级需通过 `kMux` 在外部编码
- `oper[2]`..`oper[N-1]` (events): 触发事件信号（如时钟、复位）

**results**: 无

**attrs**:
- `regSymbol` (string): 指向目标 kRegister 的 symbol
- `eventEdge` (string[]): 触发边沿列表，`"posedge"` 或 `"negedge"`，长度等于事件信号数

**案例 1：只有时钟**
- `events` = [clk], `eventEdge` = ["posedge"], `updateCond` = 1'b1, `nextValue` = d
```sv
always @(posedge clk)
    reg_q <= d;
```

**案例 2：带使能**
- `events` = [clk], `eventEdge` = ["posedge"], `updateCond` = en, `nextValue` = d
```sv
always @(posedge clk)
    if (en)
        reg_q <= d;
```

**案例 3：带异步复位（低有效）**
- `events` = [clk, rst_n], `eventEdge` = ["posedge", "negedge"]
- `updateCond` = !rst_n || en, `nextValue` = !rst_n ? 0 : d
```sv
always @(posedge clk or negedge rst_n)
    if (!rst_n)
        reg_q <= 0;
    else if (en)
        reg_q <= d;
```



### kRegisterWriteMaskPort

寄存器整体写端口 + 逐位掩码。

**operands**:
- `oper[0]` (updateCond): 更新使能条件（1-bit）
- `oper[1]` (nextValue): 更新值（宽度同目标 kRegister）
- `oper[2]` (mask): 逐位写掩码（宽度同目标 kRegister），`mask[i]=1` 时写入第 `i` 位
- `oper[3]`..`oper[N-1]` (events): 触发事件信号

**results**: 无

**attrs**:
- `regSymbol` (string): 指向目标 kRegister 的 symbol
- `eventEdge` (string[]): 触发边沿列表，长度等于事件信号数

**案例**（变量掩码，寄存器位宽 W）：
- `events` = [clk], `eventEdge` = ["posedge"], `updateCond` = 1'b1
```sv
always @(posedge clk)
    for (int i = 0; i < W; i++)
        if (mask[i])
            reg_q[i] <= new_val[i];
```



### kRegisterWriteArrayPort

寄存器索引写端口（含原"存储器写"语义；无掩码变体）。

**operands**:
- `oper[0]` (updateCond): 更新使能条件（1-bit）
- `oper[1]`..`oper[k]` (i0..ik-1): 索引（标量），1 ≤ k ≤ 声明 rank
- `oper[k+1]` (data): 写数据（k==rank → 元素宽；k<rank → 子数组 value）
- `oper[k+2]`..`oper[N-1]` (events): 触发事件信号

**results**: 无

**attrs**:
- `regSymbol` (string): 指向目标 kRegister 的 symbol
- `eventEdge` (string[]): 触发边沿列表，长度等于事件信号数

**案例**（声明 `width=128, shape=[4,4]`，元素 8 位）：
```
kRegisterWriteArrayPort(%we, %i, %j, %d, %clk)   // d 8 位：写元素 [i][j]
kRegisterWriteArrayPort(%we, %i, %row, %clk)     // row 32 位 shape=[4]：写一行
```
等价 SV：
```sv
always @(posedge clk) begin
    if (we) r[i][j] <= d;
    if (we) r[i]    <= row;
end
```



### kRegisterWriteArrayMaskPort

寄存器索引写端口 + 逐位掩码（掩码宽 = data 宽）。

**operands**:
- `oper[0]` (updateCond): 更新使能条件（1-bit）
- `oper[1]`..`oper[k]` (i0..ik-1): 索引（标量），1 ≤ k ≤ 声明 rank
- `oper[k+1]` (data): 写数据（同 kRegisterWriteArrayPort 的宽度规则）
- `oper[k+2]` (mask): 逐位写掩码（宽度同 data）
- `oper[k+3]`..`oper[N-1]` (events): 触发事件信号

**results**: 无

**attrs**:
- `regSymbol` (string): 指向目标 kRegister 的 symbol
- `eventEdge` (string[]): 触发边沿列表，长度等于事件信号数

**案例**（变量掩码写元素，元素宽 8 位）：
- `events` = [clk], `eventEdge` = ["posedge"], `updateCond` = wen
```sv
always @(posedge clk)
    if (wen)
        for (int i = 0; i < 8; i++)
            if (mask[i])
                r[a][b][i] <= d[i];
```



### kRegisterFillPort

寄存器广播填充端口：将 `data` 写满全部 ∏shape 个元素（标量寄存器退化为整体写）。

**operands**:
- `oper[0]` (updateCond): 更新使能条件（1-bit）
- `oper[1]` (data): 填充值（宽=元素宽；标量寄存器为寄存器宽）
- `oper[2]`..`oper[N-1]` (events): 触发事件信号

**results**: 无

**attrs**:
- `regSymbol` (string): 指向目标 kRegister 的 symbol
- `eventEdge` (string[]): 触发边沿列表，长度等于事件信号数

**案例**（16 元素数组复位清零）：
- `events` = [clk], `eventEdge` = ["posedge"], `updateCond` = rst, `data` = 8'h00
```sv
always @(posedge clk)
    if (rst)
        for (int __i = 0; __i < 16; __i++)
            r[__i] <= 8'h00;
```

## 6.6 层次结构

用于建模模块实例化和跨层次引用。

### kInstance

模块实例化，用于实例化 Design 中已定义的 Graph。

**operands**:
- `oper[0]`..`oper[m-1]` (inputs): 输入信号，对应 `inputPortName`
- `oper[m]`..`oper[m+q-1]` (inoutIns): inout 读值（父图提供给子模块的 inout.in）

**results**:
- `res[0]`..`res[n-1]` (outputs): 输出信号，对应 `outputPortName`
- `res[n]`..`res[n+q-1]` (inoutOuts): inout 驱动值（子模块 out）
- `res[n+q]`..`res[n+2q-1]` (inoutOes): inout 输出使能（子模块 oe）

**attrs**:
- `moduleName` (string): 被实例化模块的名称，必须在 Design 中存在对应 Graph
- `instanceName` (string): 实例名称
- `inputPortName` (string[]): 输入端口名数组，长度等于 inputs 数
- `outputPortName` (string[]): 输出端口名数组，长度等于 outputs 数
- `inoutPortName` (string[], 可选): inout 端口名数组，长度等于 inout 数

**案例 1：简单实例化**（模块 `Adder` 有输入 `a, b`，输出 `sum`）：
```
operands:  [a_val, b_val]
results:   [sum_val]
attrs:
  moduleName:     "Adder"
  instanceName:   "u_add"
  inputPortName:  ["a", "b"]
  outputPortName: ["sum"]
```
语义示例：
```sv
Adder u_add (.a(a_val), .b(b_val), .sum(sum_val));
```

**案例 2：inout 端口处理**（模块 `IOPad` 有 inout 端口 `pad`）：
```
operands:  [rd_val]
results:   [drv_val, oe_val]
attrs:
  moduleName:     "IOPad"
  instanceName:   "u_pad"
  inoutPortName:  ["pad"]
```
其中 `drv_val` 对应 `inoutOut`，`oe_val` 对应 `inoutOe`，`rd_val` 对应 `inoutIn`。

语义示例：
```sv
IOPad u_pad (.pad(pad_wire));
assign pad_wire = oe_val ? drv_val : {W{1'bz}};
assign rd_val = pad_wire;
```



### kBlackbox

黑盒实例化，用于实例化未定义的模块（如工艺原语、仿真模型）。

**operands**:
- `oper[0]`..`oper[m-1]` (inputs): 输入信号
- `oper[m]`..`oper[m+q-1]` (inoutIns): inout 读值（父图提供给黑盒的 inout.in）

**results**:
- `res[0]`..`res[n-1]` (outputs): 输出信号
- `res[n]`..`res[n+q-1]` (inoutOuts): inout 驱动值（黑盒 out）
- `res[n+q]`..`res[n+2q-1]` (inoutOes): inout 输出使能（黑盒 oe）

**attrs**:
- `moduleName` (string): 黑盒模块名称
- `instanceName` (string): 实例名称
- `inputPortName` (string[]): 输入端口名数组
- `outputPortName` (string[]): 输出端口名数组
- `inoutPortName` (string[], 可选): inout 端口名数组
- `parameterNames` (string[], 可选): 参数名数组
- `parameterValues` (string[], 可选): 参数值数组

**案例：参数化黑盒**（工艺库中的 DFF 带参数 `INIT`）：
```
operands:  [d_val, clk_val, rst_n_val]
results:   [q_val]
attrs:
  moduleName:       "DFFR_X1"
  instanceName:     "u_dff"
  inputPortName:    ["D", "CK", "RN"]
  outputPortName:   ["Q"]
  parameterNames:   ["INIT"]
  parameterValues:  ["1'b0"]
```
语义示例：
```sv
DFFR_X1 #(.INIT(1'b0)) u_dff (.D(d_val), .CK(clk_val), .RN(rst_n_val), .Q(q_val));
```

**说明**: kBlackbox 与 kInstance 的区别在于 kBlackbox 不需要在 Design 中存在对应 Graph，支持参数化。

## 6.7 XMR

跨层次引用（XMR）用于访问其他模块层次中的信号或存储单元。XMR Operation 仅作为中间表示存在，必须在 resolve pass 后展开。

### kXMRRead

XMR 读。用于读取其他模块层次中的信号。

**operands**: 无

**results**:
- `res[0]`: 读出的信号值

**attrs**:
- `xmrPath` (string): 层次路径，格式为 `"u_top.u_sub.sig"`

**说明**:
- kXMRRead 必须在 resolve pass 后展开
- resolve pass 会为中间模块添加端口、更新实例连接；若目标为寄存器/锁存器则创建对应 ReadPort
- **限制**: 当前实现不支持 XMR 读内存（memory），会报错要求显式地址



### kXMRWrite

XMR 写。用于写入其他模块层次中的信号或存储单元。

**operands**（根据目标类型不同）：

| 目标类型 | operands |
|----------|----------|
| 普通信号 | `oper[0]` (data) |
| kLatch | `oper[0]` (updateCond), `oper[1]` (nextValue), `oper[2]` (mask) |
| kRegister（标量） | `oper[0]` (updateCond), `oper[1]` (nextValue), `oper[2]` (mask), `oper[3]`.. (events) |
| kRegister（数组） | `oper[0]` (updateCond), `oper[1]` (addr), `oper[2]` (data), `oper[3]` (mask), `oper[4]`.. (events) |

**results**: 无

**attrs**:
- `xmrPath` (string): 层次路径
- `eventEdge` (string[], 可选): 触发边沿列表
  - latch 目标: 必须为空
  - register（含数组寄存器）目标: 数量需与事件操作数匹配
  - 普通信号目标: 可选

**说明**:
- kXMRWrite 必须在 resolve pass 后展开
- 对普通信号展开为 `kAssign`，对存储单元展开为对应的 WritePort

## 6.8 系统调用

### kSystemFunction

系统函数调用（表达式侧）。用于表达 `$time/$random/$sformatf` 等返回值的系统函数。

**operands**:
- `oper[0]`..`oper[N-1]` (args): 输入参数，可变数量

**results**:
- `res[0]`: 函数返回值

**attrs**:
- `name` (string): 系统函数名（去掉 `$`），IR 不限制具体函数名
- `hasSideEffects` (bool, 可选): 是否带副作用（如 `$random`）

**常见系统函数示例**（非穷举）：

| 函数 | 参数 | 说明 |
|------|------|------|
| `$time/$stime/$realtime` | 无 | 当前仿真时间 |
| `$random/$urandom` | 0~1 个 | 随机数 |
| `$urandom_range` | 1~2 个 | 范围随机数 |
| `$sformatf/$psprintf` | 至少 1 个 | 格式化字符串 |
| `$clog2/$size` | 1 个 | 可折叠为常量 |
| `$fopen/$ferror` | 1~2 个 | 文件操作 |
| `$itor/$rtoi/$realtobits/$bitstoreal` | 1 个 | 类型转换 |

**示例**（获取当前时间）：
```
operands:  []
results:   [time_val]
attrs:
  name: "time"
```
语义示例：
```sv
assign time_val = $time;
```



### kSystemTask

系统任务调用（语句侧）。用于表达 `$display/$finish` 等不返回值的系统任务。

**operands**:
- `oper[0]` (callCond): 调用条件（1-bit），为 1 时执行
- `oper[1]`..`oper[N-1-event_count]` (args): 任务参数
- `oper[N-event_count]`..`oper[N-1]` (events): 触发事件信号

**results**: 无

**attrs**:
- `name` (string): 系统任务名（去掉 `$`），IR 不限制具体任务名
- `eventEdge` (string[]): 触发边沿列表，长度等于事件信号数
- `procKind` (string): 过程块类型，建议值为 `"initial"`/`"final"`/`"always"`/`"always_comb"`/`"always_latch"`/`"always_ff"`
- `hasTiming` (bool): 是否显式时序控制

**常见系统任务示例**（非穷举）：
- `$display/$write/$strobe`
- `$fwrite/$fdisplay/$fclose/$fflush`
- `$info/$warning/$error/$fatal`
- `$finish/$stop`
- `$dumpfile/$dumpvars`

**示例**（时钟上升沿打印调试信息）：
```
operands:  [1'b1, "debug: %h", data_val, clk]
attrs:
  name:      "display"
  eventEdge: ["posedge"]
  procKind:  "always"
```
语义示例：
```sv
always @(posedge clk)
    $display("debug: %h", data_val);
```



## 6.9 DPI

DPI（Direct Programming Interface）用于调用 C/C++ 函数。

### kDpicImport

DPI 函数声明。建模 `import "DPI-C" function ...;`

**operands**: 无

**results**: 无

**attrs**:
- `argsDirection` (string[]): 形参方向数组，`"input"`/`"output"`/`"inout"`
- `argsWidth` (int64_t[]): 形参位宽数组（integral 类型有效）
- `argsName` (string[]): 形参名称数组
- `argsSigned` (bool[]): 形参有符号标记数组（integral 类型有效）
- `argsType` (string[]): 形参类型数组（如 `"logic"`, `"int"`, `"real"`, `"string"` 等）
- `hasReturn` (bool): 是否有返回值（`false` 表示 `void`）
- `returnWidth` (int64_t): 返回值位宽（integral 类型有效）
- `returnSigned` (bool): 返回值有符号标记（integral 类型有效）
- `returnType` (string): 返回值类型

**示例**（导入 C 函数 `add`）：
```
attrs:
  argsDirection: ["input", "input"]
  argsWidth:     [32, 32]
  argsName:      ["a", "b"]
  argsSigned:    [false, false]
  argsType:      ["logic", "logic"]
  hasReturn:     true
  returnWidth:   32
  returnSigned:  false
  returnType:    "logic"
```
语义示例：
```sv
import "DPI-C" function logic [31:0] add (
    input logic [31:0] a,
    input logic [31:0] b
);
```



### kDpicCall

DPI 函数调用。

**operands**:
- `oper[0]` (updateCond): 调用条件（1-bit）
- `oper[1]`..`oper[m]` (inArgs): 输入参数
- `oper[m+1]`..`oper[m+q]` (inoutArgs): inout 参数
- `oper[m+q+1]`..`oper[N-1]` (events): 触发事件

**results**:
- `res[0]` (可选): 返回值（`hasReturn=true` 时存在）
- `res[hasReturn?1:0]`.. (outArgs): 输出参数
- 后续 (inoutArgs): inout 参数输出侧

**attrs**:
- `targetImportSymbol` (string): 目标 kDpicImport 的 symbol
- `eventEdge` (string[]): 触发边沿列表
- `inArgName` (string[]): 输入参数名数组
- `outArgName` (string[]): 输出参数名数组
- `inoutArgName` (string[]): inout 参数名数组
- `hasReturn` (bool): 是否有返回值

**约束**:
- `targetImportSymbol` 必须在 Design 中解析到唯一的 kDpicImport
- `eventEdge` 长度必须等于事件信号数

**示例 1**（调用 `add` 函数）：
```
operands:  [1'b1, a_val, b_val, clk]
results:   [sum_val]
attrs:
  targetImportSymbol: "add"
  eventEdge:          ["posedge"]
  hasReturn:          true
```
语义示例：
```sv
always @(posedge clk)
    sum_val = add(a_val, b_val);
```

**示例 2**（调用带 inout 参数的 `swap` 函数）：
- kDpicImport: `swap(inout int a, inout int b)`
```
operands:  [1'b1, x_val, y_val, clk]
results:   [x_out, y_out]
attrs:
  targetImportSymbol: "swap"
  eventEdge:          ["posedge"]
  inArgName:          ["x", "y"]
  inoutArgName:       ["x", "y"]
  hasReturn:          false
```
语义示例：
```sv
always @(posedge clk) begin
    swap(x_val, y_val);
    x_out = x_val;
    y_out = y_val;
end
```
