# GRH 的数据结构：列、池与 ID

本文解释 `core/grh.hpp` 中的实际存储结构。下面先用一张小图展开它的内存内容，
再说明追加、删除和 compact 怎样修改这些内容。公开 API 的完整用法见 [grh-api.md](grh-api.md)。

## 1. 用同一张小图贯穿说明

Value 是有类型的信号或计算结果，保存位宽、名字、定义者等信息；它不保存仿真运行时每一拍的数值。
Operation 是一次运算、声明或副作用，通过输入/输出 ValueId 与其他 operation 连接。
例如加法 operation 引用两个输入 value，并定义一个输出 value。

假设图中有四个 8 位 value 和两个加法 operation：

```text
a ──┬──► add0 ──► t ──► add1 ──► y
b ──┘                    ▲
a ───────────────────────┘

add0: t = add(a, b)
add1: y = add(a, t)
```

为方便阅读，后文用 V1～V4 表示四个 ValueId，用 O1、O2 表示两个 OperationId：

| 实体 | 名字 | ID 简写 |
| --- | --- | --- |
| value | a、b、t、y | V1、V2、V3、V4 |
| operation | add0、add1 | O1、O2 |

例如 V3 的完整 ID 是 `ValueId{index=3, generation=0, graph=当前GraphId}`。
**实体列的下标从 0 开始，ID.index 从 1 开始**，所以 V3 的数据在每个 value 列的下标 2。
index=0 表示无效 ID。只有同一个 Graph 的 ID 才能用于访问这张图。

后文还会出现多种编号，它们各自索引不同的表，不能混用：

| 编号 | 索引哪里 | 作用域与表示 |
| --- | --- | --- |
| ValueId / OperationId | 对应实体列，位置为 `index-1` | 带 GraphId、index、generation |
| SymbolId | 符号表，再由 symbolIndex_ 找到实体 | 当前 Graph 的名字编号 |
| 属性 record ID | `attrKeys_` / `attrTags_` / `attrPayloads_` 的同一下标 | 当前 Graph 的 uint32_t 编号 |
| StringId | `StringPool::textById_` | 当前字符串池的 uint32_t 编号 |
| SrcLoc record ID | SrcLoc record 表 | 当前 Graph 的 uint32_t 编号 |
| ShapeId | `ShapePool::ranges` | 当前 Graph 的 uint32_t 编号 |

池切片中的 offset 则是数组位置，不是上述任何实体或 record 的身份。
实体 ID 和 SymbolId 的 0 号无效；StringId 0 表示空文本；属性、SrcLoc 和 shape 的 0 号表示无记录。
compact 会重建实体、payload record、StringId 和 ShapeId；SymbolId 编号和名字预留保留。
generation 字段目前不能用来检测 compact 前遗留的旧 ID。

## 2. Graph 持有什么

```text
Design
└─ graphs_: unordered_map<string, unique_ptr<Graph>> 按图名独占各张 Graph
   └─ Graph
      ├─ symbols_: GraphSymbolTable       名字 ↔ SymbolId
      ├─ store_: shared_ptr<GraphStore>   实体、连接关系和属性的唯一存储
      │  ├─ values_: ValueColumns         每个 value 的固定字段
      │  ├─ operations_: OperationColumns 每个 operation 的固定字段和列表切片
      │  ├─ valueUsers_                   每个 value 的 use-list 切片
      │  ├─ topology_: TopologyPools      operand/result/use 等列表的实际元素
      │  ├─ GraphPayload                  属性、SrcLoc 和各类属性值的池
      │  ├─ liveValues_/liveOperations_   创建顺序 ID 列，延迟清除删除标记
      │  └─ inputPorts_/outputPorts_/inoutPorts_
      └─ metadata_/declaredSymbols_/generateGroups_ 等图级信息

symbols_ 和 store_ 共同引用同一个 StringPool，字符串字节只保存一份。
```

`GraphPayload` 是 `GraphStore` 的基类，用于组织池相关代码；它不是另一张图。
Graph 在构图时和 compact 后都读取同一个 GraphStore，没有额外的冻结存储。
Graph 的 `owner_` 借用所属 Design，GraphStore 的 `symbols_` 借用 Graph 的符号表；两者都不拥有目标。
Graph 禁止复制和移动；clone/import 会建立目标自己的实体编号和列目录。
块导入可保留部分固定类型列的页面、只读 payload 缓冲区和字符串页的所有权，
后续写入仍各自独立，见第 2.3、5.1 节。
`store_` 使用 shared_ptr 并不表示不同 Graph 共用一份可变存储。
`topology_` 是 `shared_ptr<TopologyPools>`，ValueColumns 和 OperationColumns 也持有它，
使切片引用的 vector 对象在列对象移动时仍位于同一个地址。
vector 内部的元素缓冲区仍会在扩容时移动，旧 span 不因此保持有效。

端口数组的元素是 `Port{std::string name, ValueId value}` 或
`InoutPort{std::string name, ValueId in, ValueId out, ValueId oe}`。
端口名字和 value 的符号是两个概念；绑定端口会设置相应角色标记，标记不是从 def/use 推断的。
inout 的 in/out/oe 三个 value 均标为 isInout，不能同时绑定为 input/output；转换角色前先移除旧绑定。
图级 `metadata_` 仍为拥有型 `vector<AttrKV>`，不进入 operation 的属性三列池。

### 2.1 ValueColumns：同一字段放在同一个数组

ValueColumns 包含以下列；表中的类型是元素类型。
各列下标 i 都描述同一个 value，即 index 为 i+1 的实体。

| 列 | 元素类型 | 含义 |
| --- | --- | --- |
| `symbol_` | SymbolId | value 的名字编号 |
| `width_` | int32_t | 总位宽 |
| `isSigned_` | uint8_t | 是否有符号 |
| `type_`、`domain_` | ValueType、LogicDomain | 值的种类，以及二态/四态逻辑域 |
| `shape_` | uint32_t | 维度列表在 ShapePool 中的编号；0 表示标量 |
| `isInput_`、`isOutput_`、`isInout_` | uint8_t | 端口角色标记 |
| `definingOp_` | OperationId | 定义这个 value 的 operation；没有则为无效 ID |
| `srcLoc_` | uint32_t | SrcLoc 池中的编号；0 表示没有来源 |
| `alive_` | uint8_t | 实体是否仍然存活 |

`width_`、`isSigned_`、`type_` 和 `domain_` 使用 `detail::PagedColumn<T,256>`；
其余列使用 `std::vector<T>`。端口和 alive 标记分别保存，没有合并成 flags 字段。
ValueType 和 LogicDomain 也各占一列，枚举底层类型均为 uint8_t。

小图中与连接有关的列如下：

```text
数组下标          0       1       2       3
对应 ValueId      V1      V2      V3      V4
symbol_           a       b       t       y      ← 实际保存 SymbolId
width_            8       8       8       8
definingOp_       无      无      O1      O2
alive_            1       1       1       1
```

例如查 V3：`row = V3.index - 1 = 2`，从 `values_.width_[2]` 得到 8，
从 `values_.definingOp_[2]` 得到 O1。同一个实体的字段分散在各列的同一下标上。

代码中的 `ValueRow` 只是将同一下标的各列包装成一组引用，方便实现 mutation。
它不是另存的一份 value 对象。`getValue()` 才会按需构造 Value 对象并复制固定字段与 shape；
其中的 users 延迟读取原图，具体限制见第 8 节。

### 2.2 OperationColumns：固定字段加列表切片

`kind_` 使用 `detail::PagedColumn<OperationKind,256>`，其余列使用 std::vector；
例如 `operands_` 的完整类型是 `vector<PoolVector<ValueId>>`。

| 列 | 元素类型 | 含义 |
| --- | --- | --- |
| `kind_` | OperationKind | operation 的种类 |
| `symbol_` | SymbolId | operation 的名字编号 |
| `operands_` | PoolVector<ValueId> | 输入 value 列表的切片 |
| `results_` | PoolVector<ValueId> | 输出 value 列表的切片 |
| `operandUseIndices_` | PoolVector<size_t> | 每个输入在对应 value 的 use-list 中的位置，见第 4 节 |
| `attrs_` | PoolVector<uint32_t> | 属性 record ID 列表的切片，见第 5 节 |
| `srcLoc_` | uint32_t | SrcLoc 池中的编号 |
| `alive_` | uint8_t | 实体是否仍然存活 |

例如 `kind_ = [kAdd, kAdd]`，`symbol_ = [add0的SymbolId, add1的SymbolId]`。
OperationRow 同样只是这些列的引用集合，不拥有另一份 operands、results 或属性。

### 2.3 PagedColumn：普通平坦列与块导入共享页

PagedColumn 在普通构图期间使用一块连续的 `vector<T>`，不为每 256 行分别分配。
块导入的 `retainSourceColumns=true` 才让目标列使用固定大小的页目录：

```text
Page { storage: shared_ptr<vector<T>>, offset: size_t, owned: bool }
目标行 i → pages_[i / 256] → storage[offset + i % 256]
```

这是按页号直接索引的 O(1) 访问，不需要按来源分段做二分查找。
来源的原 vector 分配直接转由 shared_ptr 持有，来源图本身和目标页都可继续引用它；
销毁来源图不会释放仍被目标引用的缓冲区。

一个完整的目标页只有在选中的 256 个来源行物理连续、且处于同一个底层缓冲区时才共享。
**来源起点不必对齐 256 行。** 例如空目标导入来源的行 17～272：

```text
source：原有 vector，至少含 273 行
target.pages_[0] = { storage=source 的原缓冲区, offset=17, owned=false }
target[0] = source[17]；target[255] = source[272]
```

即使来源已经分页，只要跨页的物理区间仍属于同一个 owner 且 offset 连续，也可共享。
目标不满页的前缀、最后不足 256 行的尾部，以及不连续或跨 owner 的选行则复制到目标自己的页。
例如目标已有 3 行，再导入来源行 0～509：先复制 253 行补满目标页 0，
页 1 共享来源行 253～508，页 2 再复制来源行 509。

对目标共享页的第一次可变访问只复制该页，后续写入使用私有页。
来源仍为平坦列时，来源的写入、扩容等可变访问会取回独占缓冲区；若目标仍引用原缓冲区，
这条路径复制来源的整块平坦列。已分页列的 reserve 只预留页目录，不复制已保留的页面。
这些规则让共享的原始类型字段与后续 mutation 隔离，但不延长公开 span/视图的借用契约。
`stats().sharedRows` 累计成功共享的导入行数；`copiedRows` 累计导入边界复制和 writableAt 的页复制行数。
后者不包含首次分页时原目标尾页的复制，也不包含 writableFlat 的整列复制，不能作为总复制量；
前者也不是当前仍共享的存活行数。

## 3. PoolVector 与 TopologyPools：列表的描述和内容分开

`PoolVector<T>` 描述一个列表，它自己不为列表分配独立的 vector。实际元素放在图级池中。
它保存四个字段：

| 字段 | 含义 |
| --- | --- |
| `pool_` | 指向装有元素的图级 `vector<T>` |
| `offset_` | 列表在该 vector 中的起始下标 |
| `size_` | 当前元素个数 |
| `capacity_` | 该列表在池中占用的预留元素个数 |

有效内容就是 `pool[offset .. offset+size)`。例如 `{offset=2,size=2,capacity=4}`
表示位置 2、3 是有效元素，位置 4、5 是预留空间。
当前实现使用指针和 size_t；在 64 位平台上这个描述符为 32 字节。
`pool_` 借用 vector 对象。描述符的 capacity_ 是这个列表在池中占用的槽数，
与整个 vector 自身的 capacity 不同。

TopologyPools 分别保存这些数组：

| 池字段 | 元素 |
| --- | --- |
| `operands` | 输入 ValueId |
| `results` | 输出 ValueId |
| `operandUseIndices` | 输入对应的 use-list 下标 |
| `users` | ValueUser，即 `{operation, operandIndex}` |
| `attrs` | 属性 record ID |
| `shapes` | ShapePool，内部另有维度数组和范围表 |

此外，`retainedEdges` 是 `vector<unique_ptr<vector<ValueId>>>`，持有单次使用来源的
operand/result 缓冲区。接管后，某个 PoolVector 的 pool_ 可以指向其中一个 vector，
不必指向目标的主 operands/results 数组。unique_ptr 保证这些 vector 对象的地址不受目录扩容影响；
后续列表增长仍遵循相同规则。接管与来源销毁的条件见第 7.1 节。

以小图的一份紧凑布局为例：

```text
operands 池： [ V1, V2, V1, V3 ]
                 └─ O1 ─┘ └─ O2 ─┘

results 池：  [ V3, V4 ]
                O1  O2
```

O1 的 operand 切片为 `{offset=0,size=2,capacity=2}`，
O2 的为 `{offset=2,size=2,capacity=2}`。
O1 的 result 切片为 `{offset=0,size=1,capacity=1}`。
这些 offset 是池数组的下标，不是 ValueId，也不是字节地址。
例如查 O1 的输入：先取 `operations_.operands_[O1.index-1]`，得到 offset=0、size=2，
再读取 `topology_->operands[0..2)`，得到 `[V1,V2]`。

### 列表增长时怎样处理

切片还有预留空间时，直接写入下一个位置。需要扩容时分两种情况：

- 切片位于池末尾：在原位置延长它占用的空间。
- 切片后面已有别的列表：将它搬到池末尾，保留原内容，并将容量至少加倍。

例如池为 `[V1,V2]`，A 的描述符为 `{offset=0,size=1,capacity=1}`，
B 为 `{offset=1,size=1,capacity=1}`。给 A 加入 V3 后，池成为 `[无效,V2,V1,V3]`，
A 变为 `{offset=2,size=2,capacity=2}`，B 的描述符不变。A 的旧位置暂时废弃，由 compact 回收。
实体 ID 不因这次搬迁改变，但指向池元素的旧 span 可能失效。

## 4. use-list 与 operandUseIndices：连接关系的两个方向

`operands` 回答“这个 operation 使用哪些 value”。use-list 回答相反的问题：
“哪些 operation 在什么输入位置使用这个 value”。

`ValueUser{O2,1}` 表示 **O2 的下标 1（第二个）operand 使用了该 value**；operand 位置从 0 开始。
小图的反向关系为：

| value | use-list |
| --- | --- |
| V1（a） | `[{O1,0}, {O2,0}]` |
| V2（b） | `[{O1,1}]` |
| V3（t） | `[{O2,1}]` |
| V4（y） | `[]` |

这些元素保存在 `TopologyPools::users`。`valueUsers_[V.index-1]` 是该 value 对应的切片。
其完整类型为 `vector<PoolVector<ValueUser>>`，与 ValueColumns 并列保存。
ValueUser 的两个字段分别是 `OperationId operation` 和 `uint32_t operandIndex`，
没有拆成两个独立数组，也没有为每个 value 单独分配元素 vector。

`operandUseIndices` 再保存一条反查路径：

```text
O1 的 operands：            [V1, V2]
O1 的 operandUseIndices：   [ 0,  0]
                            │   └─ V2.use-list[0] = {O1,1}
                            └───── V1.use-list[0] = {O1,0}

O2 的 operands：            [V1, V3]
O2 的 operandUseIndices：   [ 1,  0]
                            │   └─ V3.use-list[0] = {O2,1}
                            └───── V1.use-list[1] = {O2,0}
```

这里的 1 是 **V1 自己的列表内下标**。假设 `valueUsers_[0].offset=6`，
那么这条关系实际位于 `TopologyPools::users[6+1]`，并非全局池的下标 1。

普通 mutation 完成后，两个方向必须满足：令 `v = opOperands(op)[j]`、`k = operandUseIndices(op)[j]`，
则 `valueUsers(v)[k] == ValueUser{op,j}`。
块导入的 `setOperationBlockEdges()` 暂时只写正向边和结果定义，反向关系要到
`rebuildUseLists()` 后才恢复；这段期间的限制见第 7.1 节。

因此，删除 O1 对 V1 的使用时，可直接找到 V1.use-list[0]。
实现用该 use-list 的末尾 `{O2,0}` 填入位置 0，再把 O2.operandUseIndices[0] 改成 0。
删除不需要搜索整个 use-list。修改 operand 位置时，也要同步更新 ValueUser.operandIndex。
这里的常数时间只指移除一条 use；`eraseOperand()` 还会移动 operand 列表，并重建该 operation 的 uses。

## 5. 属性：一层 ID 列表，加三列 record，再加值的池

一条属性有 key 和 value。例如给 O1 添加：

```text
originModules = ["top"]
```

它经过三层引用：

```text
O1.attrs 切片
    └─ TopologyPools::attrs 中的一个元素：属性ID A1
         ├─ attrKeys_[A1]     = originModules 的key编号
         ├─ attrTags_[A1]     = StringList
         └─ attrPayloads_[A1] = {offset=0,count=1}
                                  └─ stringListPayload_[0] = "top" 的StringId
                                       └─ StringPool 中的字节 "top"
```

再看一份带实际下标的内容示意（不是可编译表达式）。假设 O1 有 `eventEdge=["posedge"]` 和
`originModules=["top"]` 两个属性，此前的追加与搬迁使它们位于：

```text
operations_.attrs_[0] = {offset=4, size=2, capacity=2}  ← O1 的切片
topology_->attrs[4] = 7                              ← eventEdge 的 record ID
topology_->attrs[5] = 3                              ← originModules 的 record ID

attrKeys_[3]     = originModules 的 key 编号
attrTags_[3]     = StringList
attrPayloads_[3] = {offset=10, count=1}
stringListPayload_[10] = 12                          ← StringId
StringPool.textById_[12] = "top"
```

此处 **4 是属性 ID 数组的位置，3 是 record 的位置，10 是字符串列表池的位置，12 是字符串编号**。
它们相互独立。O1 的 record 列表 `[7,3]` 按 key 文本排序：eventEdge 在 originModules 前面，
record 编号本身不要求递增。上例中的位置用于解释布局，不要求构图时恰好分配到这些数字。

三列都按同一个属性 ID 索引，0 为无效属性。三列元素分别是 uint32_t、AttrValueKind（底层 uint8_t）、uint64_t，
合计每条 13 字节，另加列数组自身的容量开销。
`AttributeRecord` 是读取这三列时返回的临时描述，不是池里逐条持有的对象。

不同属性类型使用 payload 的方式如下：

| 类型 | payload 内容 | 变长内容保存位置 |
| --- | --- | --- |
| bool | 0 或 1 | 无 |
| int64_t、double | 该标量的 64 位表示 | 无 |
| string | StringId | StringPool |
| vector<int64_t> | offset、count 各 32 位 | `int64Payload_` |
| vector<double> | offset、count 各 32 位 | `doublePayload_` |
| vector<bool> | word offset、bit count 各 32 位 | `bitPayload_`，每个 uint64_t 保存 64 个 bool |
| vector<string> | offset、count 各 32 位 | `stringListPayload_`，元素为 StringId |

列表的 offset 放在 payload 的低 32 位，count 放在高 32 位；offset 的单位是对应池的元素，
bool 列表的 offset 单位则是 uint64_t word。
例如上面的 `{offset=10,count=1}` 实际是一个 `uint64_t` 数字 `(uint64_t(1)<<32)|10`，
没有额外分配一个切片对象。

key 编号也分两种：内置 key 用最高位置 1 的编号查预定义名字表，动态 key 用普通 StringId。
内置 key 编号不能直接当 StringId 去访问 textById_。
每个 operation 的属性 ID 列表按 **key 文本**排序，而不是按编号排序。
覆盖已有 key 时保留该属性 record，改写 tag/payload；旧列表内容留待 compact 回收。

### 5.1 PayloadPool：虚拟 offset 与共享缓冲区

四类变长属性池分别使用 `PayloadPool<int64_t>`、`PayloadPool<double>`、
`PayloadPool<uint64_t>` 和 `PayloadPool<StringId>`。它们不是 TopologyPools 中的
`vector`，也不是给每条属性新建一个 vector。一个 PayloadPool 保存总元素数 `size_`
和按 offset 排序的 `segments_`；每个段是：

```text
Segment { offset: size_t, values: shared_ptr<vector<T>> }
```

offset 是本池虚拟元素空间的起点。读取 `{offset,count}` 时，先找到起点不大于 offset
的最后一个段，再用 `local = offset - segment.offset` 访问其 vector。
**每条列表必须完整落在一个段内**；一次 append/appendGenerated 把整条列表放入同一缓冲区，
share 则保留来源的段边界。不能把跨段范围当成一个连续 span；`slice()` 对这种范围返回空视图。
这里以 `upper_bound` 二分查找段目录，代价随段数增长；它不同于第 2.3 节固定页列的直接索引。
属性 key/tag/payload 三列仍各自是单个 vector；实体列与连接池的所有权分别见第 2、3 节。

例如，来源的 int64 池和目标原有内容如下：

```text
source.size = 5
source.segments = [(0, [10,11,12]), (3, [20,21])]
source 的某条列表 = {offset=3,count=2} → [20,21]

target.size = 4
target.segments = [(0, [1,2,3,4])]
```

`base = target.share(source) = 4` 后，目标成为：

```text
target.size = 9
target.segments = [(0, [1,2,3,4]), (4, [10,11,12]), (7, [20,21])]
导入后的列表 = {offset=4+3,count=2} = {7,2}
查找 offset=7 → 段起点7 → 段内下标0 → [20,21]
```

后两个 vector 的元素没有复制；两图各自的段描述符持有相同缓冲区的 shared_ptr。
来源随后追加 `[30]` 时，共享尾段不能原地扩容，因此来源新增 `(5,[30])`，目标仍有 9 个元素。
目标追加 `[40]` 同样新增自己的 `(9,[40])`，不会修改来源。来源图销毁后，目标仍持有
`[10,11,12]` 和 `[20,21]` 的所有权。只有最后一个持有者释放后才释放这些缓冲区。
没有被共享的尾段可以直接增长；这种增长仍可能使本图先前借用的 span 失效。

块导入直接保留 int64、double 和 bool word 池的缓冲区。StringId 列表的元素属于来源字符串池，
所以不能直接沿用其数字：实现按段遍历来源列表池，将所有元素通过 StringId 映射重写后
整块追加到目标 StringId 列表池。来源中的同一列表切片在不同实例间复用这次重定位结果。
bool 的 offset 仍以 word 为单位，count 仍为 bit 数，不因分段而改变。

## 6. 字符串、符号、来源和 shape

### 6.1 StringPool 保存文本，SymbolTable 赋予名字语义

StringPool 包含四部分：

- `StringArena`：按块分配的字符空间，拥有文本字节；扩容不会移动已有块。
- `textById_`：StringId → string_view，指向 arena 中的字节。
- `idsByHash_`：文本的完整 hash → StringId 链头。哈希表只保存整数键。
- `hashNext_`：与 textById_ 等长，保存同一 hash 下的下一个 StringId，0 表示链尾。

查找或插入时只计算一次文本 hash，再遍历对应链并比较完整文本。
例如两个不同字符串恰好得到 hash H，`idsByHash_[H]=12`、`hashNext_[12]=7`，
就依次比较 textById_[12] 和 textById_[7]，不会因为 hash 相同而合并不同文本。
扩容哈希表只需处理整数，不重新扫描已经保存的字符串字节。

StringArena 的页记录为 `{shared_ptr<char[]> bytes, capacity, used}`。
`StringPool::retain(source)` 先保留来源全部页的 shared_ptr，再合并文本索引，产生稠密的
`source StringId → target StringId` 数组。目标已有的同文本使用原目标 ID；新文本的
string_view 直接指向已保留的来源页，不再复制字符。比如来源 S3 的 `"file.fir"` 已在目标 T8，
则 `remap[3]=8`；来源 S7 的新文本可登记为 T11，`remap[7]=11`，字节仍在被两图持有的页上。
StringId 0 映射为 0，内置属性 key 的最高位编号则原样保留，不索引这张数组。
任一图在共享尾页之后追加文本时都会分配自己的新页，不能写入仍被另一图持有的尾页。
因此来源和目标可独立增长，来源销毁也不会使目标文本悬空。

GraphSymbolTable 共用这个 StringPool，但维护自己的 SymbolId 索引：

```text
文本 ──► StringPool.lookup ──► StringId
                                └─ SymbolTable.symbolsByStringId_[StringId] ──► SymbolId
SymbolId ──► SymbolTable.textById_ ──► StringPool 的同一份文本字节
SymbolId ──► GraphStore.symbolIndex_ ──► {value或operation, 实体index}
```

StringId 只表示“这段文本存在哪里”；SymbolId 表示“这个名字已在符号表登记”。
它们的数字不要求相等。例如属性字符串 `"tmp"` 可以已有 StringId，但没有 SymbolId，
因此不会妨碍之后创建名为 tmp 的实体。此时 `symbolsByStringId_[tmp的StringId]`
尚不存在或为无效 SymbolId；登记该名字时才为这个数组位置填入 SymbolId。
符号表没有第二张文本哈希表。

符号表还允许预留名字而暂不绑定实体。此时有 SymbolId，但 symbolIndex_ 没有实体记录。
compact 保留这些预留，内部名字生成器也会避开它们。
文本 hash 和实体绑定的索引使用连续哈希表，StringId → SymbolId 使用数组，
避免每个条目单独分配哈希节点。

### 6.2 SrcLoc 池

每条 SrcLocRecord 保存以下字段：

```text
file、origin、pass、note：StringId
line、column、endLine、endColumn：uint32_t
```

全部字段相同的来源只保存一条 record，value/op 的 srcLoc_ 列保存它的编号。
例如 add0 和 add1 来自同一条 FIRRTL 语句时，可以都引用 loc1。
调用 `opSrcLoc()` 才会将这些 ID 还原为拥有字符串的 SrcLoc 返回给调用者。
`copyOpSrcLoc()` / `copyValueSrcLoc()` 直接复制或导入来源 record，供跨图复制使用，
不先构造拥有字符串的中间 SrcLoc；同图复制直接复用来源 record ID。
跨图导入会重写四个 StringId，再按全部字段去重；目标拥有导入后的字节，来源图销毁后仍可读取。
来源没有 SrcLoc 时，复制会清空目标的来源记录。

跨图批量复制可以复用 `Graph::SrcLocImportContext`。它缓存一次来源图到目标图的映射，
供 `copyOpSrcLoc(..., context)` 与 `copyValueSrcLoc(..., context)` 共用：

| 内部字段 | 内容与作用 |
| --- | --- |
| `source_` / `target_` | 来源、目标 GraphStore 的 weak_ptr；用共享所有权身份识别图的存储，不依赖裸地址或实体 ID |
| `sourceEpoch_` / `targetEpoch_` | 两图来源池的 compact epoch；对应 Graph 的私有 `srcLocEpoch_` |
| `records_` | 来源 SrcLoc record ID → 目标 record ID 的数组；0 表示尚未导入 |
| `strings_` | 来源 StringId → 目标 StringId 的整数映射，供不同 record 共享文件名、pass 等文本的导入结果 |
| `blockStrings_` | 可选的共享只读稠密 StringId 映射，由 BlockImportContext 提供；命中时直接下标读取 |

首次遇到非零来源 record 时，分别映射 file/origin/pass/note，再按全部字段在目标池去重。
例如来源 L7 与 L8 都引用文件名 S3，第一次导入 L7 时保存 `S3 → T5`；导入 L8 可直接使用 T5，
再次复制 L7 则直接读取 `records_[7]`，不会重复导入它的四段文本。
StringId 与 record ID 都只在各自池中有效，因此两类映射必须同时绑定来源和目标的存储身份。

普通修改只追加来源 record、修改实体所引用的 record，不使旧编号失效，缓存可继续使用。
任一图实际完成 `compact()` 时会递增自己的 epoch；`freeze()` 走同一路径。
下次非零跨图导入若发现任一存储身份或 epoch 改变，会清空两张映射并重新绑定。
同时清除旧 `blockStrings_`；块映射存在时，后续新追加、超出其范围的来源字符串仍走 `strings_`
和普通 intern，不能把旧映射误用于新 StringId。
已 compact 的图再次调用 `compact()` 会直接返回，不递增 epoch。同图复制和无来源的复制
直接复用 record 或写入 0，不查询这些缓存。

context 只保存 weak_ptr 和整数映射（包括可共享的映射数组），不延长两图或字符串池的生命周期，也不借用字符串字节。
复制后的文本由目标池拥有，来源图销毁后仍可读取目标。context 可保留给后续另一对图使用，
存储所有权身份检查会使旧映射失效；同一 context 的缓存写入需要串行使用。
缓存自动失效不改变实体 ID 的契约：compact 后，调用者仍须重新取得 source/destination 实体 ID。

前端若已经持有来源文本，可以传入 `SrcLocView`：它的坐标字段与 SrcLoc 相同，
四段文本改为 string_view，只在调用期间借用。`setOpSrcLocView()`、`setValueSrcLocView()`
会将文本导入本图的池，调用者随后可以销毁输入。
同一运算及其结果使用同一来源时，`setOpAndValueSrcLocView(op,value,view)`
先检查两个实体，再只 intern 一次，两个 srcLoc_ 列位置写入同一个 record ID。
传入全空 view 会设置一条字段全空的来源记录，与传入全空 SrcLoc 的语义相同。

### 6.3 ShapePool

ShapePool 用 `dims` 保存 int64_t 维度，用 `ranges[ShapeId] = {offset,count}` 定位一个维度列表。
例如 `dims=[2,4,8]`、`ranges[1]={0,2}`、`ranges[2]={2,1}` 分别表示 shape `[2,4]` 和 `[8]`。
ShapeId 0 表示空 shape，也就是标量。哈希索引按维度内容去重，相同 shape 共用编号。
具体地，连续哈希表 `ids` 将维度 hash 映射到一个 ShapeId，`hashNext[ShapeId]`
链接相同 hash 的候选编号；查询时仍比较完整维度内容，hash 相同不代表 shape 相同。

shape 只描述数组维度，不直接表示位宽。例如总位宽 64、shape `[2,4]` 的 Logic value
有 8 个元素，每个元素 8 位；总位宽仍保存在 value 的 width_ 列中。

## 7. 删除和 compact 如何作用于上述结构

删除 value/op 时，将 alive_ 设为 0，并从连接关系中移除相应定义或 use。
它的列位置和废弃池空间暂时保留，其他存活实体的 ID 不变。
存活 ID 列也标记这个空位，下一次枚举时只过滤 ID 列，避免每次删除都移动整个数组。

compact 则准备一份 **同样布局**的新列和池：

1. 按存活顺序建立旧实体 index → 新 index 的映射。
2. 复制存活字段、列表和 payload，重写其中的 value/op ID，并重建 use-list。
3. 保留符号编号及全部名字预留，按名字排序端口。
4. 全部准备成功后替换旧存储，释放旧列和池。

例如先将 O1.operands[1] 从 V2 改为 V1，使 b 不再被使用，并确认 b 不是端口，
就可以删除 V2。compact 后，原来的 V1、V3、V4 变为 V1、V2、V3。旧 ID 的数字可能指向另一个实体，
所以 compact 后必须重新查找需要继续使用的实体。generation 在 compact 后重置为 0。
`freeze()` 调用同一个 compact；`frozen()` 仅表示上次 compact 后未修改，仍允许继续写入。

生产 prefix 只提前 compact 旧 flatten 可安全准备的来源：不是任何 top、至少被一个 instance
内联引用，且所有指向它的 instance/blackbox 都有完整、唯一并覆盖实际端口的 named maps。
无相应端口时可省略该组 map。top、positional 引用或不完整 map 的来源不在提前 compact 集合，
因此 prefix 不会为了排序端口而改变它们的可见次序或 positional ABI。

`compactForFlatten()` 在实际 compact 成功后，将 Graph 私有的
`optional<uint64_t> preparedFlattenFreeze_` 设为当前 `mutationEpoch_`；已经 compact 的图不新建记录。
这是非序列化的一次性 receipt，不属于 GraphStore 的列、metadata 或 JSON。
flatten 仍在原 source-freeze gate 下处理来源；对 compact 的图调用
`consumePreparedFlattenFreeze()`，仅当 receipt epoch 匹配且图仍 compact 时返回 true，随后无条件清空。
有效 receipt 使原日志将这项提前完成的工作记入 `frozen`，其余 compact 来源记入 `already-frozen`。
第一次消费后再消费为 false；图存储 mutation 使 epoch/compact 条件失效，
普通 compact/freeze 也会先清空 receipt。positional 跳过和失败后进入原 inline 诊断的规则保留。
该记录用于保持工作归属和诊断格式，不是持久化分析结果或性能测量。

### 7.1 块导入的列、payload 和重建边界

`Graph::BlockImportContext` 把一个来源定义的 payload 重定位结果留给多个实例使用：

| 字段 | 实际内容 |
| --- | --- |
| `source_` / `target_` | 两端 GraphStore 的 weak_ptr 所有权身份，不延长图的寿命 |
| `sourceEpoch_` / `targetEpoch_` | 两端 compact epoch，发现池重编号时作废映射 |
| `sourceMutation_` | 来源的 mutation epoch，发现来源属性等通过可变存储发生变化时重新准备 |
| `prepared_` | 当前映射是否已成功准备完成 |
| `strings_` | `shared_ptr<const vector<StringId>>`，来源 StringId 为下标、目标 StringId 为元素 |
| `attributes_` | 以来源属性 record ID 为下标的 AttributeRecord 原型；key、字符串值及各类列表 offset 已重定位 |

`prepareBlockImport()` 保留字节页和数值 payload 段、重定位 StringId 列表，并准备这些原型；
它也将同一张 `strings_` 数组交给 SrcLocImportContext 的 `blockStrings_`。
已准备 context 在同一对图上可复用；来源 mutation、任一端 compact 或换图时需要重新准备。
`appendOperationBlockAttributes()` 会拒绝过期 context，调用者应先再次调用 prepare。
目标普通追加不改变已导入的 offset/ID，因此不使该映射失效。

**属性 record 不跨 operation 共享。** 每个目标 operation 得到独立的 key/tag/payload record，
但这些 record 可以指向同一段只读列表。例如 O1/O2 的 originModules 都指向 `{7,1}`，
但它们的属性 record 可以分别是 A10/A11；之后覆盖 O1 只改 A10，O2 的 A11 保持原值。
`setOperationBlockAttr()` 同样只导入一次公共值的 payload，再为各 operation 设置独立 record。

多块属性导入 `appendOperationBlocksAttributes()` 先按块顺序计算 record 和属性 ID 切片的
前缀范围，一次扩容目标三列及 ID 池，再由同一个 BlockExecutor 填写互不重叠的范围。
worker 不分配 record、不修改字符串映射；所有 worker 完成后才继续语义属性的名字改写。
全局 flatten 计划在一次 pass 中创建一个 executor，属性和 operand 重定位复用线程，离开 pass 前 join。

value/op 的逻辑行仍按旧创建顺序追加；SymbolId、ValueId、OperationId 和切片引用
都使用目标编号。普通块导入复制字段，开启 `retainSourceColumns` 后可保留第 2.3 节的
类型列和 kind 页面。两条路径都不将整个来源 GraphStore 搬入目标。
比如来源依次是 `P1、instance C、P2`，目标 operation 的追加顺序仍是
`P1 run、C 的 runs、P2 run`，不能因为 P1/P2 属于同一来源就把它们移到相邻位置。
已映射到父侧的端口 value 不再克隆；同类型端口以旧目标 value 为定向合并的代表。
shape/type view 和 output alias 的边界 assign 仍按旧顺序保留，包括名字、来源和属性。

flatten 的全局计划为实例进入、普通 operation run、instance/DPI 边界和退出保存有序事件，
预留名字及分配实体时逐项消费事件，以检查实际顺序与原遍历一致。
端口等价关系使用定向 union/find：合并到原父侧代表，不按 rank 任意换代表，普通内部 value
直接保留自己的目标映射。result 的 ID 和 valueDef 先发布；所有端口合并完成后，
每条 operand 最后只通过已确定的代表重定位一次。普通块提交还会先压缩代表路径。

`setOperationBlockEdges()` 一次追加一个 run 的 operands/results，并立刻设置 valueDef；
需要延迟 operand 重定位时，先用 `setOperationBlockResults()` 发布定义，
最后再用 `setOperationBlockOperands()` 填入 inputs。
结果定义还保持普通 `addResult()` 的来源继承：value 的 srcLoc_ 为 0、operation 的 srcLoc_
非 0 时，将 operation 的同一 record ID 写入 value；value 已有任何记录时都不覆盖。
例如 `op.srcLoc=L7、result.srcLoc=0` 得到 L7，`result.srcLoc=L9` 则保留 L9。
一条字段全空的来源记录仍是非 0 record，不能当成没有来源。
例如两个新 operation 的 operandOffsets 为 `[0,2,3]`、operands 为 `[V1,V2,V3]`，
则第一个 operation 的输入为 `[V1,V2]`，第二个为 `[V3]`；resultOffsets 用相同规则解释。

来源仅被使用一次且允许删除时，flatten 也可调用 `consumeBlockTopology()` 接管其拓扑：

1. 先完成来源位置、属性、语义符号引用和 node inventory 等全部来源读取；目标 result 已发布。
2. 按来源 ValueId.index 准备最终目标代表表，0 号留空；每个 TopologyBlock 配对等长的来源和目标 op 列表。
3. 将来源主 operands/results 的原 vector 缓冲区移入目标 `retainedEdges`，同时接过来源已有的 retainedEdges。
   在原切片位置把 operands 从来源 ID 改为最终目标 ID，并用已发布的目标 result ID 覆盖 results。
   例如来源 inputs `[S1,S2]`、映射 `S1→V7、S2→V9`，接管后同一缓冲区的该切片为 `[V7,V9]`。
4. 目标 PoolVector 绑定这些缓冲区中的原 offset/size，然后从所属 Design 删除来源 Graph。

接管不复制整条 edge 数组，但仍逐条重定位有效边。只处理传入块的有效切片；废弃池槽可留待 compact 回收。
调用方必须保证来源可独占消费；API 检查来源是已发布的其他图、operation 不重复、
目标 operands 为空且 result 数量匹配，不会替调用方证明设计中仅有一个使用者。
flatten 只在关闭 `preserveFlattenedModules`、定义使用次数为 1 且没有 blackbox 要保留该定义时采用此路径。
成功返回后，来源引用、ID、span 及指向它的计划记录不得再解引用；目标保留的列页、文本和 payload
由各自 owner 继续持有。多次使用或需要保留的来源走独立目标 edge 池的导入路径。

bulk operands 的反向 users/backlink 尚未写入：禁止查询这些 uses，或对 imported edges
执行 erase/replaceAllUses 等依赖 users 的 mutation。既有 top instance 自己的旧边仍有完整
backlink，可以继续按旧边界算法删除；新建边界 assign 也维护自己的 uses。
全部 run 就位后调用 `rebuildUseLists()`：先计数和前缀分配 users，再按存活 operation 及
operand 的顺序同时填写 users 和 operandUseIndices。失败退出也须重建。
这些内部接口不提供整批导入回滚；完成重建后才恢复普通 Graph API 的双向连接不变量。

## 8. 这些结构对应哪些读取接口

- `opOperands()`、`opResults()`、`valueUsers()`、`valueShape()` 返回池的借用 span。
- `opAttrs()` / `opAttrRange()` 返回属性 ID 切片的视图；元素通过 `key()`、`value()` 读取。
- `symbolText()` 返回字符串池中的 string_view；compact 更换池后，旧文本视图也失效。
- `getOperation()` 按需复制字段、连接列表和属性，返回拥有型快照。
- `getValue()` 拥有固定字段、名字文本、shape 和 SrcLoc 字符串，但 `Value::users()` 首次调用时仍会查询原 Graph。若需保留 users，应在图修改或销毁前读取并复制。

即使内容已经复制，快照中携带的实体 ID 仍受 compact 重编号影响。

属性的 `get<T>()` 返回 optional：标量得到标量，`get<std::string>()` 得到 string_view，
`get<std::vector<int64_t>>()` 得到 span，bool/string 列表得到可索引、可遍历的视图。
`toOwned()` 显式复制成 AttributeValue；`setAttrView()` 将现有视图导入目标图的池，导入后可销毁来源图。

**任何修改或 compact 后，都应重新取得借用的 span 和属性视图。** 需要跨修改持有内容时先复制。
图级 metadata 保留原有接口，不属于每个 operation 的属性池。

## 9. 自包含 SemanticTree

F28 将原六类来源树替换为计算 Node 的组织结构。每个 Graph 可持有一棵
`std::optional<SemanticTree>`，内部为独立 ID 域的 Path，叶子只有一种具名 Node。
根是 Path，编号为 1；Node 的 `SemanticId` 与 `SemanticPathId` 分别计数，0 无效。
Path 只存父路径与名字，Node 存父路径、名字及实际 op/anchor 成员范围。
没有 NodeKind、角色、source shape 或子节点；数组形状、状态及 effect 语义由实际 IR 表达。

所有名称共享字符池，成员使用池中的 graph-local `{index,generation,next}` 记录，
不为每个 Node 独立分配 `std::vector`。`tree.operations(id)` / `tree.anchors(id)`
提供轻量遍历；`Graph::semanticNodeOperations` / `semanticNodeAnchors` 可取得实际 typed refs。
op→Node 与 value→Node 的紧凑 Range 是同一成员关系的反向索引，由 Graph setter
同步维护、去重。一个 op 可以属于多个 Node，IR 仍只执行一次。

Node ID 是 Graph 内对象身份。路径裁剪、存储重排、flatten 保持顶图 Node ID；
跨 Graph 复制返回显式 Node/Path 映射，不再使用 `copiedRoot + oldSID - 1`。
clone 得到独立成员池，compact 在实体 ID 重排后重建引用；erase 移除实际成员引用。
输入、别名、零宽及空数组可以是空 op Node，路径裁剪不会据此删除 Node。
普通值替换或类型 view 通过实际引用和 Graph 成员 API 保持 anchors 与实现 op 关系。

`hier-flatten` 直接把子图叶子复制到目标根，使用实际实例/端口/op/value 映射。
结束时所有 Node 直接连根；限定名逐段转义后用 `.` 连接。
`namesFlattened` 记录名字已限定，避免 JSON 往返后重复编码。

`validateSemanticTree` 校验 Path 拓扑、名字、所有实际成员存活与双向索引一致。
JSON v5 只保存 Path、Node、显式 ID、名字和实际 op/value 引用，详见
[GRH JSON 规范](grh-json-spec.md)。旧 `semanticBindings` 不参与新模型生产或写出；
v4 不能自动转换成实际计算 Node，应从正常 ingest 重新生成。
F28 首阶段支持 ingest→xmr-resolve→hier-flatten；尚未迁移的后续 pass 明确拒绝新树。
