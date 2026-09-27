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
- `width` (`int32_t`)：位宽，仅 Logic 类型有效，必须 > 0
- `type` (`ValueType`)：数据类型
  - `Logic`：四态逻辑（0/1/x/z）
  - `Real`：实数
  - `String`：字符串
- `isSigned` (`bool`)：是否有符号，仅 Logic 类型有效
- `isInput/Output/Inout` (`bool`)：端口标记，通过 `bindXxxPort` 设置
- `definingOp` (`OperationId`)：定义该 Value 的 Operation，端口 Value 为 invalid
- `users` (`ValueUser[]`)：使用该 Value 的 Operation 列表及操作数位置

## 1.2 Operation

操作节点，Graph 的顶点，对输入数据执行特定语义操作并产生输出。涵盖组合计算、数据搬运、存储访问、层次调用等多种行为，通过 `OperationKind` 区分具体语义。

**字段**：
- `kind` (`OperationKind`)：操作类型，决定语义。例如
  - 组合运算：`kAdd`, `kMul`, `kAnd`, `kMux` 等
  - 数据搬运：`kConcat`, `kSliceStatic`, `kAssign` 等
  - 存储访问：`kRegister`, `kMemoryReadPort` 等
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

## 2.3 数组扁平化

SystemVerilog 的 packed array、struct、union 等复合类型在 GRH 中被扁平化为单个 Logic Value。

**扁平化规则**：
- 多维数组按 packed 布局展开为单个大位宽
- 高位维度在前，低位维度在后
- 结构体按字段声明顺序拼接

**示例**：`input [3:0][7:0] arr`

扁平化为 32 位的 Logic Value：
```
位范围        对应元素
[31:24]  →  arr[3]
[23:16]  →  arr[2]
[15:8]   →  arr[1]
[7:0]    →  arr[0]
```

元素访问通过 `kSliceStatic`（常量索引）或 `kSliceDynamic`（变量索引）Operation 实现。

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
- **时序存储**：`kRegister`, `kMemory`, `kLatch` 及其读写端口，涉及状态保持
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



**kSliceArray**（数组元素访问）

**operands**:
- `oper[0]`: 扁平化数组信号（位宽 `W`）
- `oper[1]`: 数组下标（无符号解释）

**results**:
- `res[0]`: 数组元素，位宽 `sliceWidth`

**attrs**:
- `sliceWidth` (int64_t): 单个元素位宽，必须整除 `W`

**语义**:
```
res[0] = oper[0][oper[1] * sliceWidth +: sliceWidth]
```

**说明**: 多维数组访问通过 `kSliceArray` 级联实现



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

GRH IR 使用**声明 + ReadPort + WritePort**的组合建模寄存器。所有对寄存器的访问必须通过对应的 ReadPort/WritePort。

### kRegister

寄存器声明。Operation 的 symbol 作为寄存器名称。

**operands**: 无

**results**: 无

**attrs**:
- `width` (int64_t): 位宽
- `isSigned` (bool): 是否有符号
- `initValue` (string, 可选): 初始化值

**初始化语义**:

`initValue` 为单值，表示寄存器的初始赋值。若源设计存在多条 initial 赋值，应在构建 IR 前规整为单值。

**语义示例**：
```
initial begin
    <symbol> = <initValue>;
end
```

**示例**（`initValue` = `$random`）：
- `initValue` = "$random"
- 语义示例：
  ```sv
  initial begin
      reg_name = $random;
  end
  ```



### kRegisterReadPort

寄存器读端口。

**operands**: 无

**results**:
- `res[0]`: 读出的寄存器值，位宽同目标 kRegister

**attrs**:
- `regSymbol` (string): 指向目标 kRegister 的 symbol

**语义**:
```
res[0] = <regSymbol>
```



### kRegisterWritePort

寄存器写端口。支持多事件触发（如时钟上升沿 + 异步复位下降沿）。

**注意**: 同一个 kRegister 可以拥有多个 kRegisterWritePort，用于保留完整的语义信息（如时钟域分离、异步复位单独建模等）。

**operands**:
- `oper[0]` (updateCond): 更新使能条件（1-bit），为 1 时允许更新
- `oper[1]` (nextValue): 更新值（位宽同目标 kRegister），reset/enable 优先级需通过 `kMux` 在外部编码
- `oper[2]` (mask): 逐位写掩码（位宽同目标 kRegister），`mask[i]=1` 时写入第 `i` 位
- `oper[3]`..`oper[N-1]` (events): 触发事件信号（如时钟、复位）

**results**: 无

**attrs**:
- `regSymbol` (string): 指向目标 kRegister 的 symbol
- `eventEdge` (string[]): 触发边沿列表，`"posedge"` 或 `"negedge"`，长度等于事件信号数

**语义**:

设目标 kRegister 位宽为 `W`。按 `eventEdge`/`events` 构建敏感列表，`updateCond` 控制是否更新，`nextValue` 为新值。

**案例 1：只有时钟**
- `events` = [clk], `eventEdge` = ["posedge"]
- `updateCond` = 1'b1, `nextValue` = d
```sv
always @(posedge clk)
    reg_q <= d;
```

**案例 2：带使能**
- `events` = [clk], `eventEdge` = ["posedge"]
- `updateCond` = en, `nextValue` = d
```sv
always @(posedge clk)
    if (en)
        reg_q <= d;
```

**案例 3：带同步复位**
- `events` = [clk], `eventEdge` = ["posedge"]
- `updateCond` = 1'b1, `nextValue` = rst ? 0 : d
```sv
always @(posedge clk)
    reg_q <= rst ? 0 : d;
```

**案例 4：带异步复位（低有效）**
- `events` = [clk, rst_n], `eventEdge` = ["posedge", "negedge"]
- `updateCond` = !rst_n || en, `nextValue` = !rst_n ? 0 : d
```sv
always @(posedge clk or negedge rst_n)
    if (!rst_n)
        reg_q <= 0;
    else if (en)
        reg_q <= d;
```

**案例 5：带写掩码**
- `events` = [clk], `eventEdge` = ["posedge"]
- `updateCond` = 1'b1, `nextValue` = new_val
- `mask` 为变量，位宽 `W`
```sv
always @(posedge clk)
    for (int i = 0; i < W; i++)
        if (mask[i])
            reg_q[i] <= new_val[i];
```

## 6.5 存储器

GRH IR 将存储器拆分为声明（kMemory）、读端口（kMemoryReadPort）和写端口（kMemoryWritePort）三种 Operation。所有对存储器的访问必须通过对应的 ReadPort/WritePort。

### kMemory

存储器声明。Operation 的 symbol 作为存储器名称。

**operands**: 无

**results**: 无

**attrs**:
- `width` (int64_t): 每行（word）的位宽
- `row` (int64_t): 总行数，决定寻址空间
- `isSigned` (bool): 是否有符号
- `initKind` (string[], 可选): 初始化类型数组，支持 `readmemh`/`readmemb`/`literal`
- `initFile` (string[], 可选): 初始化文件数组（`readmemh`/`readmemb` 使用）
- `initValue` (string[], 可选): 初始化值数组（`literal` 使用）
- `initStart` (int64_t[], 可选): 初始化起始地址（`<0` 表示省略 readmem 范围 / literal 全量初始化）
- `initLen` (int64_t[], 可选): 初始化长度（行数）；`initStart < 0` 时忽略；`initStart >= 0` 且 `initLen <= 0` 表示 readmem 仅给 start（读到末尾）

**初始化语义**:

`initKind` 为数组支持多种初始化方式混合。第 `i` 个元素按索引顺序执行，越靠后的优先级越高。

**长度与默认值**：
- `initKind` 与 `initFile` **必须同时存在且长度相同**，即使 `initKind` 是 `literal` 也需要占位的 `initFile` 项。
- `initStart`/`initLen` **必须存在且长度等于 `initKind`**。
- `initStart < 0`：readmem 省略范围参数；literal 表示全量初始化。
- `initStart >= 0` 且 `initLen <= 0`：readmem 仅给起始地址（读到末尾）；literal 视为非法。
- `initValue` 缺省或长度不足时默认 `"0"`。  
  若需要随机初始化，请显式写成 `$random` 或 `$random(seed)`。

| `initKind[i]` | 所需 attr | 等价 SV 片段 |
|---------------|-----------|----------|
| `readmemh` | `initFile[i]`, `initStart[i]`, `initLen[i]` | `$readmemh("<initFile[i]>", <symbol>[, start[, finish]]);` |
| `readmemb` | `initFile[i]`, `initStart[i]`, `initLen[i]` | `$readmemb("<initFile[i]>", <symbol>[, start[, finish]]);` |
| `literal` | `initValue[i]`, `initStart[i]`, `initLen[i]` | `<symbol>[<addr>] = <initValue[i]>;` / 区间 `for` 循环 |

**示例**（从文件初始化，再用 `$random` 覆盖部分地址）：
- `initKind` = ["readmemh", "literal"]
- `initFile` = ["mem.hex", ""]
- `initValue` = ["", "$random"]
- `initStart` = [-1, 0]
- `initLen` = [0, 1]
- 语义示例：
  ```sv
  initial begin
      $readmemh("mem.hex", mem);  // initKind[0]
      mem[0] = $random;            // initKind[1]，覆盖地址 0
  end
  ```

**示例**（literal 区间初始化）：
- `initKind` = ["literal"]
- `initFile` = [""]
- `initValue` = ["8'hFF"]
- `initStart` = [16]
- `initLen` = [8]
- 语义示例：
  ```sv
  initial begin
      for (int __i = 16; __i < 24; __i = __i + 1)
          mem[__i] = 8'hFF;
  end
  ```



### kMemoryReadPort

存储器读端口（异步读）。

**operands**:
- `oper[0]` (addr): 读地址（位宽由寻址空间决定）

**results**:
- `res[0]` (data): 读出的数据，位宽同目标 kMemory 的 `width`

**attrs**:
- `memSymbol` (string): 指向目标 kMemory 的 symbol

**语义**:
```
res[0] = <memSymbol>[addr]
```

**说明**: 同步读通过 `kRegister` 捕获 `kMemoryReadPort` 的输出实现。



### kMemoryWritePort

存储器写端口。写端口不提供复位语义，复位行为由上层逻辑显式控制 `updateCond`/`data`。

**operands**:
- `oper[0]` (updateCond): 写入条件（1-bit），为 1 时允许写入
- `oper[1]` (addr): 写地址
- `oper[2]` (data): 写数据（位宽同目标 kMemory 的 `width`）
- `oper[3]` (mask): 逐位写掩码（位宽同目标 kMemory 的 `width`）
- `oper[4]`..`oper[N-1]` (events): 触发事件信号（如时钟）

**results**: 无

**attrs**:
- `memSymbol` (string): 指向目标 kMemory 的 symbol
- `eventEdge` (string[]): 触发边沿列表，长度等于事件信号数
- `memoryWrite.priorityGroup` (string, optional): 有序写组名称；同组写端口必须指向同一 kMemory，并使用相同的事件与边沿
- `memoryWrite.priority` (int64_t, optional): 组内优先级，`0` 为最高优先级

两个 `memoryWrite.priority*` 属性必须同时出现。同组 priority 必须唯一、非负且连续，即恰好为 `[0, N)`。调度与 emitter 必须将同组端口保留在同一顺序域，并按 priority 从大到小执行，使 priority `0` 最后写入。这样，同地址且掩码重叠时由最高优先级端口获胜；地址不同或掩码不重叠的 enabled writes 均生效。没有这两个属性的多个写端口不提供基于普通 Operation 顺序的碰撞优先级保证。

**语义**:

设目标 kMemory 每行位宽为 `W`。

**简单写（时钟上升沿触发，无掩码）**：
- `events` = [clk], `eventEdge` = ["posedge"]
- `updateCond` = 1'b1
- `mask` = {W{1'b1}}
```sv
always @(posedge clk)
    mem[addr] <= data;
```

**带写使能**：
- `events` = [clk], `eventEdge` = ["posedge"]
- `updateCond` = wen, `mask` = {W{1'b1}}
```sv
always @(posedge clk)
    if (wen)
        mem[addr] <= data;
```

**带写掩码（逐位）**：
- `events` = [clk], `eventEdge` = ["posedge"]
- `updateCond` = wen, `mask` 为变量
```sv
always @(posedge clk)
    if (wen)
        for (int i = 0; i < W; i++)
            if (mask[i])
                mem[addr][i] <= data[i];
```

**两个有序写端口**：
- low port: `memoryWrite.priorityGroup` = `"writes"`, `memoryWrite.priority` = 1
- high port: `memoryWrite.priorityGroup` = `"writes"`, `memoryWrite.priority` = 0
```sv
always @(posedge clk) begin
    if (low_wen)
        mem[low_addr] <= low_data;
    if (high_wen)
        mem[high_addr] <= high_data;
end
```

若两个地址相同，后执行的 high port 覆盖 low port；若地址不同，两次写入都保留。

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
| kRegister | `oper[0]` (updateCond), `oper[1]` (nextValue), `oper[2]` (mask), `oper[3]`.. (events) |
| kMemory | `oper[0]` (updateCond), `oper[1]` (addr), `oper[2]` (data), `oper[3]` (mask), `oper[4]`.. (events) |

**results**: 无

**attrs**:
- `xmrPath` (string): 层次路径
- `eventEdge` (string[], 可选): 触发边沿列表
  - latch 目标: 必须为空
  - register/memory 目标: 数量需与事件操作数匹配
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
