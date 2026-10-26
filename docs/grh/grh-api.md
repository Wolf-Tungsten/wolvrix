# GRH API 使用指南

GRH（Graph RTL Hierarchy）是基于 SSA 的 RTL 中间表示，本文档介绍其 C++ API 的使用方法。

## 目录

- [1. 概述](#1-概述)
- [2. Design 操作](#2-design-操作)
- [3. Graph 操作](#3-graph-操作)
- [4. Graph 的双模态](#4-graph-的双模态)

---

## 1. 概述

GRH（Graph RTL Hierarchy）是基于 SSA 的 RTL 中间表示。本文档介绍 C++ API 的使用方法。

GRH IR 核心概念（Value、Operation、Graph、Design）的详细介绍请参考 [grh-ir.md](./grh-ir.md)。

---

## 2. Design 操作

Design 管理 Graph 集合及模块层次关系，Graph 名称直接使用字符串。

### 2.1 创建与查询

```cpp
// 创建 Design
Design design;

// 遍历所有 Graph
for (const auto& [name, graphPtr] : design.graphs()) {
    Graph& graph = *graphPtr;
}

// 按名称查找
Graph* found = design.findGraph("MyModule");
```

> `findGraph` 以指针表示“可能不存在”，未命中返回 `nullptr`。

### 2.2 创建 Graph

```cpp
// 创建 Graph（同名已存在会抛异常）
Graph& graph = design.createGraph("MyModule");

// 安全创建：先查后建
if (!design.findGraph("MyModule")) {
    design.createGraph("MyModule");
}
```

### 2.3 顶层模块

```cpp
design.markAsTop("TopModule");      // 标记顶层
design.unmarkAsTop("Testbench");    // 取消标记
bool isTop = design.isTopGraph("TopModule");  // 检查

for (const auto& name : design.topGraphs()) {  // 遍历顶层
    Graph* top = design.findGraph(name);
}
```

### 2.4 Graph 别名

```cpp
// 为参数化模块注册别名
Graph& graph = design.createGraph("Module_Width8");
design.registerGraphAlias("Module#(8)", graph);

// 查找时别名自动解析
Graph* found = design.findGraph("Module#(8)");  // 指向 Module_Width8
```

### 2.5 克隆 Graph

```cpp
// 从已有 graph 克隆到新名称（会完整复制 value/op/ports/attrs/srcLoc/declaredSymbols）
Graph& cloned = design.cloneGraph("SrcModule", "DstModule");
```

> 说明：克隆会为新图分配新的 GraphId，不会自动标记为 top graph。  
> Graph 不支持拷贝/移动，复制只能通过该接口显式进行。

### 2.6 克隆 Design

```cpp
// 完整复制整个 design（包含所有 graph/alias/top/declaredSymbols）
Design copied = design.clone();
```

> 说明：复制后的 Design 与原始对象独立，所有 Graph 都会分配新的 GraphId。  
> Design 不支持拷贝构造，复制应使用 `clone()`。

---

## 3. Graph 操作

Graph 内部使用 Symbol ID 管理 value/op 命名；端口名使用字符串，不进入 Symbol ID 体系。创建/绑定 value/op 必须使用 Symbol ID；查询操作支持 Symbol ID 或字符串两种方式。

### 3.1 Symbol ID

Graph 内部使用字符串 interning 机制：每个符号对应唯一的 Symbol ID，避免重复存储字符串。

#### 申请与查找

```cpp
// 申请符号（若已存在则返回同一个 ID）
SymbolId sym = graph.internSymbol("my_signal");

// 查找已存在符号（不会插入新符号）
SymbolId lookup = graph.lookupSymbol("my_signal");
std::string_view text = graph.symbolText(sym);  // 获取原始字符串
```

> internSymbol 的严格语义：
> - 若符号文本已 intern 但**未绑定到 value/op**，返回已有 ID（不新建）。
> - 若符号文本已 intern 且**已绑定到 value/op**，返回 invalid。
> - 若符号文本尚未 intern，则分配新 ID。
> - `lookupSymbol` 未命中返回 invalid。

#### 推荐用法（避免异常）

```cpp
// 作为新 value/op 的入口：先 intern，再检查 valid
SymbolId sym = graph.internSymbol("my_signal");
if (!sym.valid()) {
    // 已被 value/op 占用，选择新名字或生成内部符号
    uint32_t counter = 0;
    sym = graph.makeInternalValSym();
}
ValueId v = graph.createValue(sym, 8, false);
```

> `makeInternalValSym`/`makeInternalOpSym` 由 Graph 内部维护计数器生成唯一名字，  
> 不会复用已存在的符号文本（包含未绑定但已 intern 的符号）。
> 仅提供 op/val 的内部符号生成；其他命名由调用方自行管理。

#### 处理命名冲突

```cpp
// 方式 1：仅查找（不插入）
SymbolId sym = graph.lookupSymbol("signal");

// 方式 2：自动生成内部符号（由 Graph 内部维护计数器）
SymbolId valSym = graph.makeInternalValSym();  // 例如 "_val_0"
SymbolId opSym = graph.makeInternalOpSym();    // 例如 "_op_0"
```

> 注意：**value 与 operation 的 symbol 在图内必须唯一**。严格语义下 `internSymbol` 会在已绑定时返回 invalid，  
> 建议用 `internSymbol` 作为入口规避异常；如果复用已有 SymbolId（非新 intern），仍需检查 `findValue/findOperation`。  
> 端口名使用字符串存储，不占用命名空间，因此端口名可与 value/op 同名。

```cpp
// 判断 symbol 是否可用于创建 Operation
if (!graph.findValue(sym).valid() && !graph.findOperation(sym).valid()) {
    OperationId op = graph.createOperation(OperationKind::kAdd, sym);
}
```

### 3.2 遍历与查询

```cpp
// 遍历所有 Operation
for (OperationId opId : graph.operations()) {
    Operation op = graph.getOperation(opId);
    if (op.kind() == OperationKind::kAdd) {
        // 处理加法
    }
}

// 遍历所有 Value
for (ValueId valueId : graph.values()) {
    Value value = graph.getValue(valueId);
}

// 遍历端口
for (const auto& port : graph.inputPorts()) {
    std::string_view name = port.name;
}

// 按名称查找
ValueId valueId = graph.findValue("signal_name");      // 字符串方式
ValueId valueId2 = graph.findValue(symbolId);          // Symbol ID 方式
```

#### 获取详细信息

```cpp
// Value 信息
Value value = graph.getValue(valueId);
value.symbolText();   // 信号名
value.width();        // 位宽
value.isInput();      // 是否为输入端口
value.definingOp();   // 定义该 Value 的 Operation
value.users();        // 使用者列表
// 可变图的 users 枚举顺序不保证稳定；需要顺序时按显式属性或 op 顺序排序。
// freeze 会按 operation/operand 顺序重新构建 use-list。
graph.valueUsers(valueId); // 借用当前 use-list；图修改/freeze 后失效，修改期间需先复制。

// Operation 信息
Operation op = graph.getOperation(opId);
op.kind();            // 操作类型
op.operands();        // 输入值列表
op.results();         // 输出值列表
op.attr("key");       // 获取属性

// 属性读取
if (auto attr = op.attr("width")) {
    int64_t width = std::get<int64_t>(*attr);
}
```

### 3.3 创建实体

**标准流程：先申请符号，再创建实体。**
**注意：value / operation 的 symbol 不能互相冲突。**

完整类型使用 `LogicalType {kind, width, isSigned, domain, shape}`。`LogicDomain` 为
`TwoState` / `FourState`，旧重载默认 FourState。`shape` 的乘积必须整除总位宽；
Real/String 规范化为 width=1、unsigned、FourState、无 shape。
`createValue(sym, type)`、`valueLogicalType(id)` 和 `setValueLogicalType(id, type)`
分别创建、读取和显式改变完整类型；改变类型的调用者必须同步生产者与消费者。
`sameValueType(a,b)` 比较完整类型，不能仅以位宽相等判定 SSA alias 合法。

```cpp
// 创建 Value
SymbolId sym = graph.internSymbol("data");
ValueId value = graph.createValue(sym, 32, false);  // width=32, unsigned

// 同型元素构造数组；自动推导总位宽、signedness 和 shape=[N]+element.shape。
// createArray({})、元素类型不一致或总宽超过 int32 上限会抛异常。
ValueId array = graph.createArray(std::array{lhs, rhs}); // array[0]=lhs, array[1]=rhs

// 创建 Operation（opSym 不能与已有 value/op 的 symbol 冲突）
SymbolId opSym = graph.internSymbol("add");
OperationId op = graph.createOperation(OperationKind::kAdd, opSym);
graph.addOperand(op, lhs);
graph.addOperand(op, rhs);
graph.addResult(op, result);

// 创建常量
SymbolId valSym = graph.internSymbol("const_val");
SymbolId constSym = graph.internSymbol("const_op");
ValueId constVal = graph.createValue(valSym, 8, false);
OperationId constOp = graph.createOperation(OperationKind::kConstant, constSym);
graph.addResult(constOp, constVal);
graph.setAttr(constOp, "constValue", std::string("8'hFF"));

// 绑定端口
ValueId clk = graph.createValue(graph.internSymbol("clk"), 1, false);
graph.bindInputPort("clk", clk);
```

创建时的约束与异常：

- `createValue`/`createOperation` 会在以下情况抛异常：
  - `SymbolId` 无效（invalid）或不属于当前 Graph 的符号表
  - `SymbolId` 已绑定到其他 value/op（bind 冲突）
  - `ValueType::Logic` 且 `width <= 0`
  - 传入跨图 `ValueId`/`OperationId`（GraphId 不匹配）
- 推荐用 `internSymbol` + `valid()` 过滤避免常规路径异常；必要时使用 `lookupSymbol` 复用已有符号。

便捷重载（自动分配内部符号）：

```cpp
// 生成内部临时 Value（_val_N）
ValueId tmp = graph.createValue(32, false);

// 生成内部 Operation（_op_N）
OperationId tmpOp = graph.createOperation(OperationKind::kAdd);
```

> 说明：上述重载会使用 `makeInternalValSym/makeInternalOpSym` 生成内部符号。  
> 若需要稳定或对外可见的名字，请显式传入 `SymbolId`。

### 3.3.1 端口绑定与查询

端口名使用字符串，不要求来自 Symbol ID。

```cpp
// 绑定输入/输出端口（同名会更新绑定值）
ValueId inVal = graph.createValue(graph.internSymbol("in"), 8, false);
ValueId outVal = graph.createValue(graph.internSymbol("out"), 8, false);
graph.bindInputPort("in", inVal);
graph.bindOutputPort("out", outVal);

// 绑定双向端口（in/out/oe 三个 ValueId）
ValueId ioIn = graph.createValue(graph.internSymbol("io__in"), 1, false);
ValueId ioOut = graph.createValue(graph.internSymbol("io__out"), 1, false);
ValueId ioOe = graph.createValue(graph.internSymbol("io__oe"), 1, false);
graph.bindInoutPort("io", ioIn, ioOut, ioOe);
```

> 约束：inout 端口使用的 value 不能同时绑定为 input/output，否则会报错。
> 端口名不能为空；同名绑定会覆盖旧的 value。

查询端口与端口值：

```cpp
for (const auto& port : graph.inputPorts()) {
    std::string_view name = port.name;
    ValueId value = port.value;
}
for (const auto& port : graph.outputPorts()) {
    std::string_view name = port.name;
    ValueId value = port.value;
}
for (const auto& port : graph.inoutPorts()) {
    std::string_view name = port.name;
    ValueId in = port.in;
    ValueId out = port.out;
    ValueId oe = port.oe;
}

ValueId inValue = graph.inputPortValue("in");
ValueId outValue = graph.outputPortValue("out");
```

> `inputPortValue`/`outputPortValue` 未命中返回 invalid。

同时 `Value` 上的标记会同步维护：

```cpp
Value value = graph.getValue(inVal);
value.isInput();   // true
value.isOutput();  // false
value.isInout();   // false
```

移除端口绑定：

```cpp
bool removedIn = graph.removeInputPort("in");
bool removedOut = graph.removeOutputPort("out");
bool removedIo = graph.removeInoutPort("io");
```

移除规则与效果：

- 若端口存在则移除并返回 `true`，同时更新 `Value` 的 `isInput/isOutput/isInout` 标记。
- 不存在则返回 `false`；端口名为空会抛异常。

### 3.4 修改与删除

```cpp
// 修改 Operation
graph.replaceOperand(op, 0, newValue);              // 替换操作数
graph.setOpKind(op, OperationKind::kMul);           // 修改类型
graph.setAttr(op, "key", value);                    // 设置属性
graph.eraseAttr(op, "key");                         // 删除属性

// 替换 Value 的所有使用
graph.replaceAllUses(oldValue, newValue);

// 删除（安全版会检查是否有使用）
if (graph.eraseOp(op)) { /* 删除成功 */ }
if (graph.eraseValue(value)) { /* 删除成功 */ }

// 删除（强制版，不检查使用，慎用）
graph.eraseOpUnchecked(op);
graph.eraseValueUnchecked(value);
```

删除与替换细则（常见失败原因）：

- `eraseOp`：若任一 result 仍被使用则失败。
- `eraseValue`：若存在 users、被端口绑定、或仍是某 op 的 result，则失败。
- `eraseOp(op, replacementResults)`：要求替换数量与 results 数量一致；会先替换 uses 再删除 op。

---

## 4. Graph 的双模态

`Design::instanceId()` 标识当前设计实例，`revision()` 是保守变更序号。Graph 的实体、
类型、符号绑定、端口、属性、来源及冻结重编号会使 revision 变化；预留容量等操作也
可以使缓存失效。只读遍历不会改变它。clone/load/设计替换获得新的实例身份。

`freeze()` 会压缩存活实体、重编号 OperationId/ValueId，并将 generation 重置为 0。
旧 ID 的数字可能重新指向另一实体，不能作为跨 revision 的永久引用。SymbolId 仅是
当前符号表的编号，symbol 文本也允许重命名或转交。`setOpSymbol/setValueSymbol`
是底层符号绑定接口，不自动改写 declaredSymbols、generateGroups 或 regSymbol /
latchSymbol / targetImportSymbol；语义重命名须由调用者同步这些引用。符号转交时原
成员关系可自然保留，不能把这个接口改成隐式搬迁所有来源的操作。

跨 pass 的 resolved 分析用 `transform::DesignStamp::capture(graph)` 保存实例、revision
及 top GraphId，消费前调用 `matches(graph)`，过期则重建。SessionStore typed slot 通过
`requireSessionValue<T>()` 读取，缺失、空 slot 或类型错误均报错；克隆 session 中的
resolved 分析不能跳过新实例检查。

状态仍是原 `kRegister/kLatch` 声明。`core/state.hpp` 提供 stateLogicalType /
setStateLogicalType 和有序 InitSpec 的读写接口。InitSpec 支持 Constant、Fill、Readmem、
Random，保存范围、seed 及覆盖顺序；它不是另一份状态表。方言通过
`core/op_descriptor.hpp` 注册静态 OpDescriptor，声明名称、版本、effects、CSE/clone
许可和 verifier。未知 kind 不可被视为纯 op；PassManager 默认拒绝未声明支持的方言。

Graph 有两种内部状态，影响遍历性能：

| 状态 | 特点 | 触发方式 |
|------|------|----------|
| **可变态** | 支持读写（默认） | 创建后默认 |
| **冻结态** | 只读，遍历更快 | 调用 `freeze()` |

### 冻结与解冻

```cpp
// 冻结以优化遍历性能
graph.freeze();

// 执行写操作时自动解冻
graph.createValue(sym, 32, false);  // 自动回到可变态
```

### 使用建议

```cpp
// 性能敏感场景：多次遍历前先冻结
void analyze(Graph& graph) {
    graph.freeze();
    
    for (auto opId : graph.operations()) { /* ... */ }
    for (auto valueId : graph.values()) { /* ... */ }
    
    // 如需修改，直接调用写 API 会自动解冻
}
```
