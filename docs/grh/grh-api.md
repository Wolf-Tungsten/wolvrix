# GRH API 使用指南

GRH（Graph RTL Hierarchy）是基于 SSA 的 RTL 中间表示，本文档介绍其 C++ API 的使用方法。

## 目录

- [1. 概述](#1-概述)
- [2. Design 操作](#2-design-操作)
- [3. Graph 操作](#3-graph-操作)
- [4. Graph 的存储与 compact](#4-graph-的存储与-compact)

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

### 3.5 来源位置

`setOpSrcLoc()` / `setValueSrcLoc()` 接收拥有型 SrcLoc；已有文本缓冲区时，
用 `SrcLocView` 配合 `setOpSrcLocView()` / `setValueSrcLocView()`，可避免先复制字符串。
setter 在返回前将内容导入本图的池，因此输入只需在调用期间有效。

```cpp
SrcLocView loc;
loc.file = "example.fir";
loc.line = 12;
loc.pass = "ingest-firrtl";
graph.setOpAndValueSrcLocView(op, result, loc); // 检查两个实体，只导入一条共享 record
```

`copyOpSrcLoc(destination, sourceGraph, sourceOp)` 和
`copyValueSrcLoc(destination, sourceGraph, sourceValue)` 直接复制来源记录；
跨图时导入文本，来源没有记录时清空目标记录。全空 SrcLoc/SrcLocView 则表示一条字段全空的记录。
读取 `opSrcLoc()` / `valueSrcLoc()` 会返回拥有字符串的 optional<SrcLoc>。

同一对图批量复制来源时，给两个 copy 接口传入同一个 `Graph::SrcLocImportContext`：

```cpp
Graph::SrcLocImportContext locations;
target.copyValueSrcLoc(dstValue, source, srcValue, locations);
target.copyOpSrcLoc(dstOp, source, srcOp, locations);
// 后续 value/op 继续复用 locations；重复来源 record 和文本直接命中整数映射。

target.compact();
// compact 可能重编号实体，须先按目标符号重新取得 ID。
dstOp = target.findOperation("copied_op");
target.copyOpSrcLoc(dstOp, source, srcOp, locations);
// 目标池 epoch 已变化，locations 会在这次导入中自动重建映射。
```

context 保存来源 record ID → 目标 record ID、来源 StringId → 目标 StringId 两类缓存。
它用两图 GraphStore 的 weak_ptr 所有权身份及 compact epoch 检查映射是否仍然有效；
普通追加或修改实体的来源可继续复用缓存，任一图实际 compact、切换图对或重新创建图时自动失效。
同图复制仍直接复用 record，来源无记录仍清空目标。缓存不改变 copy 接口的这些语义。

context 不拥有图，也不保存借用的文本视图；已复制文本由目标图拥有，来源销毁后仍可读取。
调用时两图和实体 ID 必须有效，同一个 context 不应并发写入。compact 后即使继续复用 context，
也必须重新查找被重编号的实体 ID。缓存的内部结构见 [SrcLoc 池](grh-storage.md#62-srcloc-池)。

### 3.6 块导入

以下接口供 hier-flatten 的内部块导入使用。它们分开处理实体列、属性与连接，
调用者负责来源 ID → 目标 ID 的重定位表和旧遍历顺序；它们不会导入完整的 Graph metadata 或端口表。
来源与目标必须是不同的图。

| 接口 | 输入和效果 |
| --- | --- |
| `prepareBlockImport(source, payloads, locations)` | 准备属性原型与稠密 StringId 映射，保留来源字符页和数值 payload 缓冲区，并为来源位置导入提供同一映射 |
| `appendValueBlock(source, values, symbols, output, locations, threads=1, retainSourceColumns=false)` | 等长的来源 value、目标预留 SymbolId 和调用者输出缓冲区；追加类型、shape、来源等列，写出新 ValueId；不复制端口标记、定义或 users；可选择保留来源类型列页 |
| `appendOperationBlock(source, ops, symbols, output, locations, threads=1, retainSourceColumns=false)` | 等长的来源 operation、目标预留 SymbolId 和输出缓冲区；追加 kind、symbol、来源，连接和属性先为空；可选择保留来源 kind 页 |
| `appendOperationBlockAttributes(source, sourceOps, targetOps, payloads)` | 按等长 operation 映射复制属性；目标不得重复且属性必须为空，每个目标取得独立的属性 record |
| `appendOperationBlocksAttributes(blocks, executor)` | 每个 OperationBlockImport 提供 source 指针、等长 sourceOps/targetOps 和已准备 context 指针；所有块先按顺序预分配 record 与切片，再用同一 BlockExecutor 填写独立范围并等待完成 |
| `setOperationBlockAttr(ops, key, value)` | 导入一次公共属性值的 payload，然后分别设置各 operation 的 record；覆盖同名 key 时保持普通 setter 的语义 |
| `setOperationBlockEdges(ops, operandOffsets, operands, resultOffsets, results)` | 为连接为空、互不重复的 operation 设置扁平边列表；立即设置结果定义，暂不写反向 uses |
| `setOperationBlockResults(ops, resultOffsets, results)` | 先为连接为空的 operation 设置 results 和定义，operands 留空 |
| `setOperationBlockOperands(ops, operandOffsets, operands)` | 为 operands 仍为空的 operation 设置 inputs，可保留先前已发布的 results；暂不写反向 uses |
| `consumeBlockTopology(source, blocks, mappedValues)` | 独占消费已发布来源的拓扑缓冲区，原位重定位边并绑定目标切片，随后删除来源 Graph；要求先完成全部来源读取、发布目标 results，并由调用者保证来源可单次消费 |
| `rebuildUseLists()` | 遍历全部存活 operation，重建 users 和 operandUseIndices，恢复普通 mutation 所需的不变量 |

先按旧创建路径的顺序预留目标名字。已绑定父侧 value 的端口不放进 `sourceValues`，
而是直接填入调用者的重定位表。来源的 instance/DPI 等边界须把 operation 列表切成连续 run：
`P1、instance C、P2` 的追加次序仍是 `P1 run、C 的 runs、P2 run`。
ID 按目标物理列尾部顺序生成，删除留下的 tombstone 不会在此回收。
属性内的语义符号引用仍由调用者改写；同类型端口的定向合并、类型/shape view 与 output alias
所需的边界 assign 也仍由 flatten 按原规则处理。
全局 flatten 计划保持这些事件的顺序，先发布 result 定义并完成定向端口 union/find，
然后将每条 operand 一次重定位到最终父侧代表；result ID 保持已发布的定义身份。

下面省略名字预留和重定位表的构造；所有 vector 的长度在调用前已经确定：

```cpp
Graph::BlockImportContext payloads;
Graph::SrcLocImportContext locations;
target.prepareBlockImport(source, payloads, locations);
target.appendValueBlock(source, sourceValues, valueSymbols, targetValues, locations);
target.appendOperationBlock(source, sourceOps, opSymbols, targetOps, locations);
target.appendOperationBlockAttributes(source, sourceOps, targetOps, payloads);

// operands/results 已全部通过 value 重定位表改写为目标 ID。
target.setOperationBlockEdges(targetOps, operandOffsets, operands,
                              resultOffsets, results);
target.rebuildUseLists();
```

offset 数组长度必须为 `ops.size()+1`，首项为 0，非递减，末项为对应扁平边数组的长度。
例如两个 operation，`operandOffsets=[0,2,3]`、`operands=[V1,V2,V3]`，
得到输入 `[V1,V2]` 和 `[V3]`；`resultOffsets=[0,1,2]`、`results=[V4,V5]`
分别定义 V4 和 V5。结果不能已有 defining operation，也不能在同一批中重复定义。
这些数字是输入数组内的位置，和属性 payload 的虚拟 offset 是不同的索引。
结果定义同时遵循普通 `addResult` 的来源规则：result 没有 SrcLoc record、operation 有 record
时，result 继承 operation 的同一记录；已有 result record 不覆盖，包含字段全空但确实存在的记录。
例如 op 为 L7、result 无来源时得到 L7；result 原为 L9 时仍为 L9。
`setOperationBlockEdges()` 和 `setOperationBlockResults()` 都执行此规则。

单次使用且允许销毁的来源可在属性等全部导入后改用拓扑接管：

```cpp
// 已追加实体和属性；mappedValues[sourceValue.index] 是最终目标代表，0 号不用。
target.setOperationBlockResults(targetOps, resultOffsets, results);
Graph::TopologyBlock block{sourceOps, targetOps};
target.consumeBlockTopology(source, std::span(&block, 1), mappedValues);
// source 已从其所属 Design 删除，后续不得再使用 source 或来源借用视图。
target.rebuildUseLists();
```

每个 TopologyBlock 的 sourceOps/targetOps 等长，两侧 operation 在全部块中各自不得重复。
目标 operands 必须为空，results 数量必须与来源匹配。接管将原 operands/results vector
缓冲区及来源已接管的 edge 缓冲区交给目标，按原 offset/size 绑定切片；operands 改为映射后的 ID，
results 使用已发布的目标 ID。调用成功时来源 Graph 的引用、实体 ID 和借用视图立即失效。
API 不检查设计中是否还有其他使用者；flatten 只对定义使用一次且无 blackbox 保留要求的图采用此路径，
并要求 `preserveFlattenedModules=false`。来源需要保留或多次使用时，使用前面的边列表导入。

从设置块边到 `rebuildUseLists()` 完成之间，不得查询导入边的 users，或执行依赖它们的
`replaceAllUses`、erase 等 mutation。调用者在异常退出时也须重建反向关系，才能交还普通 Graph API；
这些低层接口不提供整批导入的事务回滚。既有 top instance 的旧边以及普通 API 新建的边界
assign 仍保留各自的有效 backlink，flatten 只按这个边界继续处理它们。

两个 context 应成对复用。BlockImportContext 检查来源/目标的存储身份、compact epoch 和
来源 mutation epoch；来源经可变存储修改、任一图 compact 或更换图对后，须再次 prepare，
否则属性导入会拒绝过期 context。目标普通追加允许继续复用。compact 后仍须重新取得实体 ID。
SrcLocImportContext 可独立用于第 3.5 节的 copy 接口；块导入额外使用共享的稠密 StringId 映射，
后续新来源字符串超出映射范围时仍走普通文本导入。

`threads` 默认是 1；内部只让 worker 填写预先扩容后的不相交列区间。
符号绑定、来源/shape 映射与池准备按确定顺序完成，不允许调用者并发修改同一个 Graph 或 context。
hier-flatten 的 `HierFlattenOptions::numThreads=0` 自动选择最多 10 个硬件线程，
实际较小的块仍串行填写。线程数不改变实体和名字顺序。
全局计划使用一个 BlockExecutor 处理多块属性和最终 operand 映射，每批等待完成后才发布下一阶段。
`retainSourceColumns=true` 使 value 的 width/isSigned/type/domain 四列和 operation kind 列
可共享完整的 256 行目标页；来源区间须物理连续且属于同一缓冲区，来源行起点无需按页对齐。
不满页或不连续选行复制到私有页。目标共享页写入时只复制该页；平坦来源随后写入时可能复制整列。
SymbolId、来源记录、shape、端口标记与定义仍使用目标自己的编号和列。
数值 payload 保留来源分段缓冲区，StringId 列表则整块重定位；
这不会移动整个 GraphStore，也不延长公开借用视图的有效期。
具体 offset 和所有权示例见 [PagedColumn](grh-storage.md#23-pagedcolumn普通平坦列与块导入共享页)、
[PayloadPool](grh-storage.md#51-payloadpool虚拟-offset-与共享缓冲区)
及[块导入边界](grh-storage.md#71-块导入的列payload-和重建边界)。

## 4. Graph 的存储与 compact

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

Graph 始终使用同一份列式 GraphStore，构图和 compact 后不会切换到另一套 frozen 图存储。
部分固定类型列内部可在平坦 vector 与共享页目录之间切换。
实体 ID 在修改期间保持稳定，删除保留 tombstone；`compact()`（或兼容名 `freeze()`）
回收垃圾，并按上述契约重编号。`frozen()` 表示自上次 compact 后未修改；后续写入
直接修改相同的列，不再执行 thaw。

生产 ingest prefix 提前 compact 的范围与旧 flatten source-freeze gate 对齐：来源不是任何 top，
至少被一个可 inline 的 `kInstance` 引用，并且所有指向它的 instance/blackbox 引用都有完整的
input/output/inout named port maps。每组名字须无重复、数量和实际端口匹配、覆盖全部端口；
没有该类端口时允许省略对应 map。其余图继续原路径，top 端口和 positional ABI 不因 prefix 被排序。

| 内部协调接口 | 效果 |
| --- | --- |
| `compactForFlatten()` | 对尚未 compact 的安全来源调用 compact，成功后记录当前 mutation epoch 的一次性 receipt；已经 compact 时直接返回，不新建 receipt |
| `consumePreparedFlattenFreeze()` | 当图仍 compact 且 receipt epoch 匹配时返回 true；无论返回什么都会清除 receipt，不修改图内容或 JSON |

这两个接口不替调用者判断 named-map 安全条件。receipt 是 Graph 内部的非序列化状态，
不写 metadata，也不经 store/load 保存。图存储 mutation 会使 compact 状态或 epoch 不匹配；
普通 `compact()` / `freeze()` 则先清除 receipt，即使已经 compact 也是如此。
flatten 在原 source-freeze 边界消费有效 receipt，将该图计入原日志的 `frozen`，表示同一次准备工作
已提前完成；没有有效 receipt 的 compact 图计入 `already-frozen`。重复消费返回 false，
positional 跳过和失败后的原 inline 诊断边界保持不变。receipt 不证明实际耗时，性能仍需独立测量。

`getOperation()` 复制字段、连接列表和属性。`getValue()` 复制固定字段与 shape，
但其 `users()` 首次读取仍会查询原 Graph，须在图修改或销毁前读取并复制所需 users。
`opOperands/opResults/valueUsers` 返回 span，`opAttrs/opAttrRange` 返回借用的 `AttrRange`。修改图或 compact 前应先
消费借用，跨修改的遍历须持有必要的副本。属性通过 `key()`、`value()` 读取，
`value().get<T>()` 返回可选的标量或借用视图，`toOwned()` 显式生成拥有型值。

```cpp
for (const auto attr : graph.opAttrs(op)) {
    if (attr.key() == "eventEdge") {
        if (const auto edges = attr.value().get<std::vector<std::string>>()) {
            for (std::string_view edge : *edges) consume(edge);
        }
    }
}
graph.compact();
// ID 可能已变化；通过符号重新查找需要继续修改的实体。
```

池布局、切片搬迁、来源/shape 去重和回收规则见 [grh-storage.md](grh-storage.md)。
