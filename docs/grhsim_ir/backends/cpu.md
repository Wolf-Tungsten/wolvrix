# GrhSIM CPU 后端

本文定义 CPU 后端的映射结构 `CpuBackendMapping`。[GrhSIM IR Overview](../overview.md)
定义通用模型和后端边界；本文中的三个分量仅属于 CPU 后端，不是其他后端必须采用的结构。

从模型输入到构建、运行和验收的步骤见 [CPU 单线程活动度仿真 Flow](../flows/cpu-st.md)。

## 1. `CpuBackendMapping`

```text
CpuBackendMapping
  data_layout: DataLayout
  partition_tree: PartitionTree
  schedule_plan: SchedulePlan
```

- `DataLayout` 为 I/O/S 对象和 graph value 选择 CPU 物理表示与存储位置；
- `PartitionTree` 把 `G` 的 op 组织为层次化分区；
- `SchedulePlan` 把分区映射到 NUMA node 和 CPU core，并规定执行顺序与跨 core 依赖。

三个分量共同映射同一个 `GrhSimModel`，但不改变其 op、value、对象或执行语义。这里的排列只是
数据结构的组成，不规定三个分量的生成顺序。

## 2. `DataLayout`

```text
DataLayout
  object_layouts: Map<ObjectRef, CpuDataLayout>
  value_layouts: Map<ValueId, CpuDataLayout>

CpuDataLayout
  cpu_type: TypeRef
  storage: CpuStorage

CpuStorage
  kind: object | partition_local | boundary
  owner: PartitionId?
```

`cpu_type` 引用 [cpu 方言](../dialects/cpu.md) 中的一种类型。cpu 方言的类型在取值集合之外
还定义了 CPU 内存中的物理表示（大小、对齐和存储结构），因此 `DataLayout` 不重复记录这些
信息，只为每个对象和 value 选择类型、指定存储位置。方言语义类型到 cpu 类型的典型对应：

| 语义类型 | cpu 类型 |
| --- | --- |
| `core.logic<1, false, 2-state>` | `cpu.bool` |
| `core.logic<w, s, 2-state>` | `w <= 64` 时按 `s` 取 `cpu.uint8/16/32/64` 或 `cpu.sint8/16/32/64` 中最小能容纳 `w` 位者；`w > 64` 时按 `s` 取 `cpu.uint<w>` 或 `cpu.sint<w>` |
| `core.logic<w, s, 4-state>` | `cpu.array<V, 2>`，`V` 是对应的 2-state 表示 |
| `core.real` | `cpu.f64` |
| `core.string` | `cpu.str` |
| `core.array<T, n>` | `cpu.array<U, n>`（`U` 是 `T` 对应的 cpu 类型） |

当前 `cpu.str` 的物理槽是 `pointer_bytes` 大小的字符串句柄，不是在字节 arena 中内嵌
`std::string`。C++ emitter 让句柄指向独立的类型化字符串对象：object、boundary 和 input
shadow 的字符串由模型持有；partition-local 字符串由 supernode 调用栈持有，生命周期覆盖
其全部 helper chunk。`init()` 清空持久字符串并重新绑定被 arena 清零覆盖的句柄，析构由
C++ RAII 完成。字符串 state 的初始化/提交与字符串数组尚未实现，emit 明确拒绝，不能走
字节 memcpy 或把句柄数组解释为 `std::array<std::string, N>`。

单结果 `core.compute.constant` 的两态 logic<1..64> 操作数在使用点直接发射为
按原类型截断/符号扩展的 C++ literal，不物化到 local/boundary arena，也不发出
常量变化通知。例如 `%one = constant logic<1,false> 1`、`%mask = constant
logic<8,false> 255` 作为 `regWrite` 的 enable 和 mask 时，消费者直接使用 true 和
255，编译器可消去恒定的 enable 分支与全位掩码运算。`logic<5,true>` 的 `5'h1d`
仍为 -3，`logic<1,false>` 仍按最低位截断，X/Z 沿用 two-state 投影为零的规则。
所有 compute unit、commit domain 和端口活动位在 `init()` 时已激活，首次求值
及重新初始化无需依赖常量生产者通知；常量后续不会变化。IR、mapping、任务顺序
和布局中的槽保持不变。宽常量仍走原存储路径，DPI inout 从 literal 初始化独立
可写临时值，再单独发布 result，不修改常量本身。

不可变 `core.compute.constant` 字符串例外：emitter 在使用点直接引用已转义的
`std::string` 常量表达式，不生成独立赋值或 local/boundary 字符串对象绑定；布局中的
槽仍保留，mapping 不变。例如 guarded DPI 的字符串构造位于原有 `if (enable &&
eventGuard)` 内，而非 guard 之前。动态字符串、DPI output/inout 的存储和保持语义不变。
所有 compute supernode 初始化时已激活，常量不再需要运行期变化通知；此优化不改变
调用次数、void 调用存活性、event 检测、history 采样或 compute/commit 分类。

当前 CPU emitter 对 two-state logic state 及其一维数组完整应用显式初始化步骤：
scalar `const`/`random`、array `const`/`fill`/`readmem`。数组 const 的 `value` 使用长度恰为
`count` 的 logic literal 字符串序列；fill/readmem 的 `start`、`count` 是非负元素索引和
元素数，缺省为 0 和剩余长度，显式 count=0 不写入。步骤按序覆盖，未覆盖的行、非法范围、
缺失值和不支持的类型在发射文件前报错，不能以 arena 的清零替代语义初值。

readmem 沿用 legacy 的发射期解析方式，共用注释/空白分词与十六进制 `@address` 解析器；
相对文件名基于发射进程工作目录解析。数据 token 按 hex/bin 和元素位宽解码，X/Z 在
two-state 目标中投影为 0，越宽高位截断。`@address` 是绝对行号，指定范围外的数据不写入；
重复地址按文件顺序覆盖，短文件不会改写未提供数据的行。发射出的静态地址/数据表在
`init()` 中直接写目标缓冲区，不在仿真时重新打开源文件。例如 8 行 byte 数组先
`fill {value: "8'h55"}`，再 `readmem {file: "data.hex", format: "hex", start: 2, count: 2}`，
文件内容 `aa @3 bb @0 ff` 仅把第 2/3 行改为 aa/bb，其余行仍为 55。

random 使用 legacy SplitMix64 算法，每个宽值按低字到高字写入目标并截断尾字，fill 每行
独立采样；不构造返回值宽数组。CPU backend 的无 seed RNG 初值为
`0x6a09e667f3bcc909`，每次 `init()` 重置；显式整数 seed 以其 64-bit 位模式初始化该步骤
的局部 RNG，不消耗共享流。此处规定的是 backend 的可复现随机策略，不声称随机序列与
其他 SV 仿真器一致。real/string state、嵌套数组仍明确拒绝，不能视为已支持。

位向量放入整数或宽整数时低位对齐：向量的第 0 位对应整数的最低位，超出位宽的高位
按符号性填充（无符号补 0，有符号补符号位）。4-state 数组的第 0 个元素保存 value，
第 1 个元素保存 known：某一位为 X 或 Z 时对应 known 位为 0，否则为 1。

该对应是默认选择而非约束：只要 cpu 类型的取值集合覆盖语义类型的取值集合，`DataLayout`
可以为同一语义类型选择不同的 cpu 类型。

`object_layouts` 覆盖 `I`、`O` 和 `S` 中的每个对象，`value_layouts` 覆盖 `G` 中的每个 value。
`F` 中的外部函数声明不是数据对象，不在 `object_layouts` 的覆盖范围；外部函数如何绑定到具体
实现由 CPU 后端自行定义。

`storage` 的三种取值分别表示：

- `object`：I/O/S 对象占用的长期存储，`owner` 为空；
- `partition_local`：只在一个分区内部使用的 value，`owner` 指向该分区；
- `boundary`：跨分区传递的 value，`owner` 指向产生该 value 的分区。

一个 value 是否跨分区，由 `G` 中的定义/使用关系和 `PartitionTree` 共同确定。`DataLayout`
必须据此选择 `partition_local` 或 `boundary`，不能改变 value 的生产者或使用者。

`DataLayout` 不定义初始化语义，也不保存初值。`Init` 始终是 `GrhSimModel` 的分量，初始化
语义（步骤种类、覆盖规则）完全由 core 方言定义（见 core 方言第 2.1 节），CPU 后端只负责
执行：实例创建时，对每个 `S` 对象按其语义类型依次求值 `InitSpec` 的各个步骤，得到完整
确定的语义初值，再按其 `cpu_type` 的物理表示逐层编码写入对象存储：

- 标量和宽整数：按本节约定的位序与填充规则写入；
- `cpu.array`：第 `k` 个元素编码到偏移 `k * stride` 处。

初始化只写 `S` 对象；`I` 由调用方设置，`O` 由 op 写入。

### 2.1 当前 `cpu.st.layout-data` 实现

该 pass 依赖最终 `EmitFunctions` 分区，产出 `DataLayout` stage，mapping 仍为
`complete=false`。CPU 类型表使用独立 `CpuTypeId`，不改写语义类型表；默认 registry
注册 `cpu` 类型方言，但禁止 CPU 类型出现在语义类型表中。当前 ABI 固定为 64 位指针。

`CpuDataLayout` 中的具体表为：

- `types`：按 ID 稠密存储 `kind/width/elementType/count/size/alignment`，结构相同的类型去重。
- `objects`：按 I、O、S 的顺序覆盖对象；每项保存 ObjectRef 与 slot，函数不占数据 slot。
- `values`：按 ValueId 稠密存储 slot；slot 含 CPU type、storage kind、owner 和字节 offset。
- `localFrames`：每个 compute supernode 的 frame 大小及对齐；所有 helper 接收同一个 frame。
- `runtime`：active word、edge-domain arm 和 `(domain,event value,edge)` slot 各占一字节，
  owner 分别为 word 或 domain；general 域无 arm。事件历史仍使用原有语义 state 存储。
- `objectBytes/boundaryBytes/runtimeBytes`：三个独立长期 arena 的字节数；前两者按 8 字节向上对齐。

所有 object slot 的 owner 为空，offset 相对于 object arena。value 的 owner 是生产者
supernode，`partition_local` offset 相对于该 supernode frame，`boundary` offset 相对于
boundary arena。跨 supernode、compute 到 commit、事件 value 和 DPI result 使用 boundary
持久存储。DPI 未被触发时需保留旧结果，因此即使其结果只在本 supernode 使用，也保守提升。
helper 拆分不改变 supernode 生命周期，不把跨 helper value 误建成 helper 栈内临时值。

例如 `%a = input.read`、`%b = not %a` 在同一 supernode，而另一 supernode 消费 `%b`：
`%a` 是 local frame slot，`%b` 是 boundary slot；把前两条 op 拆成两个 helper 时，`%a`
仍位于调用者共享 frame。`logic<65,true,4-state>` 映射为 `array<sint<65>,2>`，大小 32、
对齐 8；再包成 `array<array<T,3>,5>` 时大小为 480，元素 stride 逐层按对齐计算。

v1 使用 canonical layout；verifier 重新推导并逐项比较类型、覆盖、owner、lifetime、偏移、
frame 和 runtime slots，而不只是检查已有条目的 ID。所有大小计算检查 UInt64 溢出。
输入差分影子集合仍由后续 schedule 的 input fanout 确定，本阶段不声称完成调度运行态。

JSON 的 CPU payload 从 `[stage,root,partitions]` 扩展为
`[stage,root,partitions,layout]`，旧 stage 不输出 layout，旧 checkpoint 字节形态不变。
`layout` 为以下固定顺序数组，所有 ID 从 1 开始，空 owner/element/event value 编码为 0：

```text
[pointer_bytes, types, objects, values, frames, runtime, object_bytes, boundary_bytes, runtime_bytes]
type    = [id, kind, width, element_type, count, size, alignment]
slot    = [cpu_type, storage_kind, owner, offset]
object  = [object_kind, object_index, slot]
frame   = [owner, size, alignment]
runtime = [runtime_kind, owner, event_value, edge, offset]
```

枚举顺序见 C++ model 定义；未知枚举、缺失 layout 和 stage/payload 不一致均被拒绝。

## 3. `PartitionTree`

```text
PartitionTree
  root: PartitionId
  partitions: Map<PartitionId, Partition>

Partition
  parent: PartitionId?
  children: PartitionId[]
  ops: OpId[]
  attrs: PartitionAttrs

PartitionAttrs
  kind: root | phase | event_domain | supernode | node | active_word | emit_function
  phase: compute | commit | none
  event_gate: { source: input | derived, events: [{value: ValueId, edge: posedge | negedge}] }?
  active_id: UInt32?
  active_word: UInt32?
  helper_chunks: [{offset: UInt32, count: UInt32}]
```

`PartitionTree` 满足以下约束：

- `root` 是唯一没有 `parent` 的分区；
- 每个非根分区恰好有一个父分区，父子引用必须互相一致且不能形成环；
- 只有叶分区直接包含 `ops`，每个 `G` 中的 op 恰好属于一个叶分区；
- 一个非叶分区覆盖其全部后代叶分区中的 op，根分区覆盖整个 `G`。

CPU 后端可以把叶分区作为 node，把由若干子分区组成的非叶分区作为 supernode。node 和
supernode 是 `PartitionTree` 中的层次关系，不是新的 graph op。

value 仍然是 `G` 中的边。跨分区 value 可以由其生产 op 和使用 op 所属的叶分区推导，不在
`PartitionTree` 中重复保存。

### 3.1 单线程活动度分区

`cpu.st.split-phase` 建立 root 下唯一的 compute 和 commit 两枝，只有这两个分区携带
`phase` 标注。commit 包含 `core.state.regWrite/latchWrite/memWrite/memFill/memAssign/memWriteSeq`；
包括 `core.system.task`、`core.dpi.call` 在内的其他 op 属于 compute。

`cpu.st.form-event-domains` 在 commit 下建立 `event_domain` 分区，再把每个域的写口按原
op 顺序切为 `supernode` 叶分区。参数 `--max-op-in-commit-supernode N` 默认 4096，必须为正。
commit op 不定义 graph value，因此写口之间没有 value 依赖；这个稳定顺序不赋予不同写口
额外的优先级。同一状态的多个写口保留，覆盖语义仍由 core 方言定义。

event gate 只包含 operands 尾部的事件值及 `event_edges`。按 edge、ValueId 排序并去重，
不含 updateCond、data、mask 或 event-history StateId。例如
`(posedge a, negedge b, posedge a)` 与 `(negedge b, posedge a)` 归入同域；`posedge a` 与
`negedge a` 属于不同域。所有事件值都由 `core.input.read` 产生时 source 为 input，否则为
derived。latch 和无边沿写口进入无 `event_gate` 的 general 域，每轮扫描。

`cpu.st.build-compute-nodes` 从 G 构造 producer/use 索引并做拓扑排序，再逆序吸收单一消费
node 内的纯计算锥。共享值、commit 消费者和容量上限形成边界。与 legacy 的主要区别是
不 clone source op、不改写 operands：mapping pass 必须维持每个 op 的唯一归属和语义版本。
`--max-op-in-compute-node N` 默认 128；node 内的 ops 和 compute 子树展开顺序必须满足
value 的先定义后使用，组合环被拒绝。

`cpu.st.merge-compute-supernodes` 移植 out1/in1/sibling coarsen 和 DP 分段。out1/in1 候选
按跨边界不同 value 的数量优先，sibling 必须具有完全相同且非空的 predecessor 集合。
每批合并后检查 quotient DAG，拒绝成环的批次。DP 的目标为每段不同输入 activation value
的数量加 1，默认 uniform weight，与 legacy 主路径一致；同成本优先较长的段。
`--max-op-in-compute-supernode N` 默认 128。单个 node 已超限时保持独立，不切割前一 pass
生成的叶分区；后续 helper 分块负责代码组织。所有 DAG 和 DP 临时表在 pass 返回后消失。

`cpu.st.pack-active-words` 按 compute 顺序分配从 0 开始的 `active_id`，每 8 个 supernode
打包到一个 `active_word` 分区，word index 同样从 0 开始，最后一字允许不足 8 个。
`--helper-max-estimated-lines N` 默认 2048；较大 supernode 的 `helper_chunks` 指定其按树序
展开的 op 范围。例如 supernode 展开为 `[op5, op9, op11]`，`[{0,2},{2,1}]` 表示两个 helper
分别负责前两个和最后一个 op，不表示模型 OpId 范围。非空 ranges 必须连续完整覆盖该
supernode，只有 compute supernode 可以携带 active ID 和 helper ranges。

`cpu.st.pack-emit-functions` 沿完整 word 或域内 commit supernode 打包，不能拆 word、
跨域或插入 TU 分区。参数 `--batch-max-ops`、`--batch-max-estimated-lines`、
`--target-batch-count` 默认为 2048、8192、64；target count 为 0 时只使用显式体积限制，
否则以总 ops/lines 除以 target count 放大下限。单个不可拆单位超限时独占函数。
line estimate 目前为 `4 + operand_count + result_count + sum(result_logic_words)`，是映射期
的确定性估计；实际发射体积和编译性能需在 emitter 落地后验证。

最终树形态为：

```text
root
  compute -> emit_function -> active_word -> supernode -> node -> ops
  commit  -> event_domain  -> emit_function -> supernode -> ops
```

`kind` 标注树层次的用途，不增加独立的域、word 或函数实体。`CpuBackendMapping.stage` 的分区阶段
依次为 `SplitPhase / EventDomains / ComputeNodes / ComputeSupernodes / ActiveWords / EmitFunctions`，
之后为 `DataLayout / Schedule`。
六个分区 pass 每次只推进一个层次，DataLayout 由第 2.1 节的后续 pass 生成，SchedulePlan
由第 4.1 节的 pass 生成。只有 Schedule stage 的 mapping 为 `complete=true`，表示映射数据
齐全；`cpu.st.emit-cpp` 消费完整映射生成模型，但完整映射本身不能替代生成代码的运行验收。

### 3.2 持久化

CPU mapping 由模型持有，backend 为 `cpu`，schema 为 `cpu.st.v1`。在 streaming
`wolvrix.grhsim.v1` 的 mapping 行中，第五项为 schema 专属 payload：

```text
[backend, schema, complete, parameters, [stage, root, partitions, layout?, schedule?]]
partition = [id, parent, kind, phase, children, ops, gate]
          | [id, parent, kind, phase, children, ops, gate, activity]
gate = [] | [source, [[value, edge], ...]]
activity = [active_id, active_word, [[helper_offset, helper_count], ...]]
active_id = [] | [UInt32]
active_word = [] | [UInt32]
```

ID 延续已有整数编码，parent 的 0 表示无父分区。枚举按 C++ 定义的顺序从 0 编码，未知值
在读取时拒绝。旧的非 CPU mapping 行仍为四项，空 mappings 的旧 checkpoint 可直接读取。
load 和 clone 为 mapping 绑定新模型 identity；semantic mutation 清除整个 mapping。
无活动度标注时省略最后的 activity 项，旧的 phase/domain checkpoint 保持可读和稳定往返。

## 4. `SchedulePlan`

```text
SchedulePlan
  numa_nodes: NumaNodeSchedule[]
  inputFanout: Map<ValueId, ActivationTargets>
  computeSupernodeFanout: Map<ValueId, ActivationTargets>
  commitStateFanout: Map<StateId, ActivationTargets>
  quiescenceProjection: Bitmap<StateId>
  roundSeeds: PartitionId[]
  inputShadows: [value, cpu_type, offset][]
  inputShadowBytes: UInt64

ActivationTargets
  activate: PartitionId[]
  arm: PartitionId[]

NumaNodeSchedule
  numa_node: NumaNodeId
  cores: CpuCoreSchedule[]

CpuCoreSchedule
  core: CpuCoreId
  tasks: ScheduledTask[]

ScheduledTask
  id: TaskId
  partition: PartitionId
  waits_for: TaskId[]
  execution: ActivityDrivenCompute | DomainGatedCommit | AlwaysScanCommit
```

单线程活动度方案的 task 引用函数分区，只有一个 NUMA node 和 core，`waits_for` 为空。
compute 函数内部按 active word/bit 执行 supernode；边沿 commit 函数由所属域的 arm 标志
门控，general commit 每轮扫描。event guard 始终负责最终语义判断。

边沿域门控要求其每个 history 的所有写者都采样同一个 event ValueId。若 history
被不同 event 采样，或还存在普通 state 写者，相关边沿域的全部 task 使用现有
`AlwaysScanCommit`，但保留 event_gate、任务顺序、逐 op guard 和 history 采样。
该选择物化到 schedule 并由 verifier 重算校验，不由 emitter 隐式决定。同源共享
history 仍可门控；这里不按表达式相似性推断不同 ValueId 等价。

例如 A/B 两个 posedge 写口共享 H，按 A 后 B 采样：A=1、B=0、H=0 时，A 的
guard 成立，B 的 guard 不成立，但最终 H=B=0。下一次仅 data 改变时，A 的
guard 仍成立；只依赖 event 或 H 的变化 arm 会漏执行。B 也必须执行采样，
覆盖 A 暂存的 H=1，不能因为自己的 guard 为假或 H 已等于 B 就跳过。

三张表分别在输入差分、compute value 写站点、commit state 写站点消费。`activate` 只能指向
compute supernode，`arm` 只能指向 commit 边沿事件域。`commitStateFanout` 中的 state 真变化激活
其读者；其中属于 E 的成员才驱动下一轮 fixed-point 求值。无扇出的 input/value 没有表项；输入表的
键同时确定需要变化检测的输入。state 表的 key 是所有被读取的状态，E 成员身份记录于
`quiescenceProjection` 位图，即使某个被读状态没有激活目标，也保留其空表项。
`roundSeeds` 指定每次 G 应用入口必须激活的 compute supernodes（仅 system/DPI 属主）；输入影子类型及字节偏移已物化。

`NumaNodeId` 标识目标 CPU 的一个 NUMA 内存域，`CpuCoreId` 标识该内存域中的一个 CPU 执行核。
二者都是 CPU 后端的目标资源 ID；这里的 CPU core 与 GrhSIM `core` 方言无关。每个
`CpuCoreId` 只能属于一个 `NumaNodeSchedule`，task 在执行期间不能迁移到其他 core。

`tasks` 的数组顺序就是同一 CPU core 上的执行顺序。不同 core 各自执行自己的 task 序列，
因此可以并行；一个 task 必须等其全部 `waits_for` task 完成后才能开始。`waits_for` 用于表达
跨 core 或跨 NUMA node 的必要顺序，同一 core 内的顺序不需要重复写入。

一个 task 执行 `partition` 覆盖的全部 op。被 task 引用的分区必须互不包含，并共同覆盖根分区：
每个叶分区恰好被自身或它的一个祖先分区所覆盖。这样既可以把 node 作为执行单元，也可以把
包含多个 node 的 supernode 整体分配给一个 CPU core，而不会重复执行 op。

task 从所在的 `CpuCoreSchedule` 继承 CPU core 和 NUMA node 归属。`DataLayout` 可以根据该归属
区分同 core、同 NUMA node 和跨 NUMA node 的 value，但 value 的生产者和使用者仍由 `G`
决定。

所有 task 序列与 `waits_for` 合成的顺序必须无环，并覆盖 `G` 中跨 task value 的生产者到
使用者关系。该顺序还必须满足方言规定的对象访问约束。

### 4.1 当前 `cpu.st.build-schedule` 实现

该 pass 依赖 DataLayout stage，产生一个 NUMA node 0、core 0。task ID 从 1 开始，按分区树
DFS 顺序访问函数，先 compute、再按域排列 commit；每个函数恰好一个 task，`waitsFor` 为空。
输入和 compute 的 source 使用 ValueId，state 的 source 使用 StateId；各表按 source ID 排序，
activate 按 activeId 排序去重，arm 按 domain ID 排序去重。不建立 state x domain 稠密矩阵。

- `inputFanout` 激活 input.read 本身所在的 supernode 及直接 compute consumers，并 arm 引用
  该 input value 的边沿域，包含同时使用 input 与 derived event 的混合域。无 consumer 的
  input.read 不建影子。一个输入对象有多个 read value 时各自覆盖，不遗漏其生产者。
- `computeSupernodeFanout` 只激活跨 supernode consumers，不建立自激活边；派生 event value
  的任意变化都 arm 对应域，而不只是域要求的边沿方向。例如 posedge 域也须观察下降沿，
  否则历史停在 1，下一次上升沿会被漏掉。input value 的 arm 只在输入表中生成。
- `commitStateFanout` 覆盖所有被 compute 分区读取的状态，而不仅是 overview 第 4.2 节的
  输出/边沿状态依赖闭包 E。E 由反向遍历 value producer、遇到 state.read/memRead 后穿过该
  状态的所有写口继续传播得到，单独物化为 `quiescenceProjection` 位图；边沿历史本身影响判定，
  也必须入闭包；其 next 值仅依赖当前 event，不能误把未观察写口的数据口加入 E。
- state fanout 激活 state.read/memRead 和 task/DPI history consumers；commit history 的变化
  arm 所属域。E 之外的 state reader 同样由 commit fanout 按真变化激活，不再每轮播种；
  `quiescenceProjection` 只决定 pending 记录的收敛标记（非 E 状态更新不增加轮次）。
- 所有 system function/task/DPI 的 supernode 仍保守进入 `roundSeeds`，避免 `$time`、外部状态
  或无 event 调用因缺少输入变化而永久停在首轮值。该集合不是 fullpass；后续若收窄，须证明
  调用生命周期与外部状态语义不变，并单独做性能验证。
- `inputShadows` 与 inputFanout 逐项对应，采用 value 的 CPU 类型，在独立 arena 内分配对齐
  偏移，arena 末尾按 8 字节对齐，大小算术检查 UInt64 溢出。

例如 `q1 <= d; q2 <= q1` 与输出 `q2`：E 包含 q2、q1，以及两个写口的边沿历史；反向闭包
不会因为 q1 不直接连接输出就遗漏它。若另有不影响任何输出或边沿的 `dead <= hidden`，其
写口的 history 仍影响边沿判断而入 E，但 dead、hidden 不会仅因这条写口有 event 就入 E；
二者的读者仍经 commit fanout 按真变化激活，只是不改变轮次收敛。

运行时消费这些表时必须遵守：

1. 每次 G 的 compute/commit 从同一旧状态快照求值；history 更新不受 data enable 限制。
2. commit 结束后比较 E 的最终状态与轮初状态，而不是冻结 history 到 eval 结束。
   task/DPI 在 compute 产生的 history 更新也属于本次 G 的状态更新，不能遗漏。
3. 当前轮 arm 与下一轮 arm 分开消费；轮末不能清掉刚由 state fanout 生成的下一轮 arm。
4. 所有 SN 初始激活、所有边沿域初始 arm；每次 G 另加 roundSeeds，完整保留 event guard 精判。

v1 verifier 检查模型结构；JSON 在 layout 后追加 schedule，形态为：

```text
schedule = [numa_nodes, input_fanout, compute_fanout, state_fanout, round_seeds, input_shadows, shadow_bytes, projection_bits, projection_words]
numa     = [numa_id, cores]
core     = [core_id, tasks]
task     = [task_id, function_partition, waits_for, execution]
fanout   = [source_id, activate_partition_ids, arm_domain_ids]
shadow   = [value_id, cpu_type_id, offset]
```

Schedule stage 要求外层 complete 为 true，其余阶段要求 false；stage、payload 和 complete
不一致会被拒绝。完整 JSON、clone 和 semantic mutation 保持既有 identity/revision 约束。

## 5. 验证

### CPU C++ 阶段计时

模型默认关闭运行时计时。`set_runtime_profile_enabled(true)` 清空计数并启用新一轮测量；
传入 false 暂停计数但保留数据。`init()` 清空计数并保持启用状态。
`cpu_runtime_profile()` 返回只读快照引用，`dump_runtime_profile()` 在至少一个 eval 完成后
向 stderr 输出一行 `[grhsim-cpu-phase]` 数据；XiangShan 的既有 `EMU_RUNTIME_PROFILE=1`
会调用这些启用/导出接口，不需要修改 workload。

计数项为完成的 `evals` 和进入的收敛 `rounds`。所有时间项使用 steady_clock 纳秒：

- `eval_ns`：成功返回的完整 eval 区间，包括输入同步、round seed、arm 交接和输出复制。
- `compute_ns`：连续 ActivityDrivenCompute task 段，包含外层 activity guard 和实际调用。
- `commit_ns`：连续 commit task 段，包含 domain guard 和实际调用。
- `publish_ns`：每轮 cpu_publish 区间。

段切换只读一次时钟并累加前一段，不改变原调度顺序。相邻时钟读取和记账存在小额扰动，
阶段时间不是纯 task CPU 时间；三个阶段的和应不大于完整 eval 时间，差值包含未分桶工作。
失败 eval 可能留下部分 round/阶段计数，此时应丢弃测量并重置模型，不能用于性能结论。
关闭计时不读时钟，但仍有条件分支和代码布局变化；其性能影响需在相同输入下实测。
这些诊断计时不能直接作为未插桩仿真提速的证据。

### Backend mapping 校验

`CpuBackendMapping` 必须满足：

- `DataLayout` 完整覆盖当前 `GrhSimModel` 的 I/O/S/value；每个 `cpu_type` 都能解析到 cpu
  方言中的类型，且其取值集合覆盖对应对象或 value 的语义类型；
- `partition_local` 和 `boundary` 引用的分区存在，且与 `G` 的 value 定义/使用关系一致；
- `PartitionTree` 是一棵覆盖全部 op 且不重复归属 op 的树；
- phase 子树不互含，compute/commit op 分类纯净；事件域只出现在 commit 下，各写口 canonical
  event key 必须与域标注一致，只有叶分区含 op；不完整 mapping 按已生成的 stage 校验；
- compute 分区按树序展开后，每个 operand 的 producer 必须已执行；active ID 连续，word
  与 8 对齐，函数只包含完整 word 或同域 commit supernode，helper ranges 连续覆盖；
- 激活表的 `activate` 只引用 compute supernode，`arm` 只引用 commit 事件域；
- NUMA node 和 CPU core ID 唯一，每个 CPU core 只属于一个 NUMA node；
- task ID 唯一，`waits_for` 只引用当前计划中的 task；task 所引用的分区互不重叠并完整覆盖
  `PartitionTree`；
- core 内序列与 `waits_for` 共同构成无环顺序，并满足 graph 数据流和方言对象访问语义；
- `CpuBackendMapping` 不增加、删除或改写 `GrhSimModel` 的 `Init` 条目。

`GrhSimModel` 或其所引用的方言版本发生变化后，原有 `CpuBackendMapping` 失效，必须重新生成
并验证。

### CPU C++ 宽比较与移位量

两态比较结果虽然是 1 位，helper 选择仍由操作数位宽决定。超过 64 位时，CPU emitter
调用 pointer-based `grhsim_compare_extended_words(lhs, lhsWords, lhsWidth, rhs, rhsWords,
rhsWidth, signedMode)`：两个 pointer 指向低字在前的 uint64_t 数据，`Words` 是缓冲区长度，
`Width` 是逻辑位宽，`signedMode` 仅在双方操作数均 signed 时成立。helper 屏蔽末字 padding，
按需逐字符号/零扩展到共同宽度，不分配扩展后的宽数组。标量操作数先放进 uint64_t 局部变量，
不能将 bool/uint32_t 存储地址重新解释为 uint64_t 指针。

例如 signed 8 位 `-1` 与 signed 129 位 `-1` 比较相等；signed 8 位 `-1` 与 unsigned
129 位 `255` 比较也相等，因为 mixed signedness 使用零扩展后的 8'hff。

`shl/lshr/ashr` 的移位量使用现有 `grhsim_index_words(amount, resultWidth)` 饱和转换。
512 位移位量若高字非零，即使低字为 0，也返回 `resultWidth`，触发超范围移位规则：
逻辑移位归零，负数算术右移填充符号位。不得只读取移位量低 64 位。

### CPU C++ 宽状态写入

两态、1–64 位的 staged 标量写入先读取有效值：dirty 时使用当前 shadow，否则使用
轮初 visible state。完整赋值和 masked 写入都先规范化到目标位宽；masked 写入基于有效值
合并。新值与有效值相同时不登记 pending，也不写 shadow。第一次变化才登记 state、offset、
size、fanout 范围和 projection 标记，并直接写入调用方持有的 shadow，不复制已被完整覆盖的旧值。
后续变化复用同一 pending 条目，visible state 仍只在 publish 边界更新。

例如 visible=0，同轮依次写 1、0，第二次必须与 shadow=1 比较并写回 0；不能因新值等于
visible 就忽略第二次写入。已有 pending 条目保留到 publish，由最终值比较决定是否激活读者。
事件 history 仍在原采样位置更新，不受 data enable 或是否触发写边沿影响。批量 history、
memory cell、宽值和已证明可 direct-commit 的写口继续使用各自的路径。

宽寄存器、锁存器和内存 cell 的 mask 写入复用 legacy 原地 helper，目标指向 staged state。
每个 word 计算 `(staged & ~mask) | (data & mask)`，保留先前 disjoint-mask 写口在同轮的修改。
例如 129 位状态的两次写入分别选中 bits 0-63 与 64-128 时，publish 后同时包含两次更新，
不能将第二次写入直接赋值为完整 data。标量 staging 以 state 为粒度，内存 staging 以 cell 为粒度。

内存元素使用 `cpu_at<Element>(cpu_stage_cell(...), 0)`；stage helper 返回指定 cell 的 shadow
地址，同轮第一次触及该行才复制 visible cell 并登记 pending，后续写口复用该行。
dirty key 为每个数组分配的独立基址加行号；pending 保存 cell 字节偏移和长度，不扫描/复制整数组。
`memFill` 逐行使用同一路径，因此 fill 与 masked/sequence 写口混用仍保留先后覆盖顺序。
写入地址越界时不登记 pending，但仍采样 event history。
`memWriteSeq` 的整个有序写入列表必须先命中显式 event，
之后才逐三元组检查 enable/address/data；event history 的采样不依赖某个 enable 成立。

### CPU C++ 状态发布

event history 采样和一般 state 写入使用 shadow/pending，在 publish 阶段比较最终
字节内容。后写必须基于本轮已有 shadow，不能始终基于 visible object。例如 visible=0，
同轮先写 7 后写 0：最终 shadow=0，publish 不激活读者。任何提前排除不变写入的优化
都必须保留这种覆盖顺序，并实测其比较/分支/代码体积成本，不能只按队列数量判断收益。

每个 memRead 在 compute 时缓存实际读取 cell 的绝对字节偏移，包括地址为 local value 的情况。
memory pending 发布时仅激活缓存偏移与变更 cell 相同的读端口所属 supernode；地址输入变化仍由
原 compute/input fanout 激活读端口。缓存、dirty bits 和 pending 在 init 时一起清除。

### CPU C++ 读取与变化发布

对已有 commit-state fanout 的 state.read，若所有 value 使用者都在 compute，且不作为域 event，
emitter 可将 value 解析为 state 引用并省去读取复制。state 发布的激活集合补入这些直接使用者，
按活动字合并 mask。直接作为 commit operand 的读取仍保留 boundary/local 快照；不能把
`q1 <= d; q2 <= q1` 的第二个输入改为 direct-commit 后的 q1。此优化不修改 IR 或 layout。

一个内联 supernode 或 helper chunk 中，具有相同 activate/arm 集合的逻辑结果共享 changed 标志。
每个结果比较并写回，块尾一次性以 mask 发布变化；宽位原地 helper 的返回标志也合并到该组，
不引入额外数组快照。分组不会越过 supernode/chunk 边界；DPI 调用与其结果发布保持原语义。

### CPU C++ 标量直接提交

唯一写者的 1..64-bit、2-state 标量可使用 direct commit，但完整 object-ref pool
中的引用必须全部归类为该唯一 regWrite/latchWrite 的目标或 compute 的 state.read。
任何 history、其他 commit/DPI/system ref、未知引用、多写者、memory 或宽值均回退。
compute 已读取旧状态且 commit 没有其他观察者，因此该写站点计算的 masked/normalized
值就是本次 G 的最终值，物理字节提前写回不可被中途观察。`q1 <= d; q2 <= q1`
仍从 compute 已保存的旧 q1 value 计算 q2，不从已更新的对象重新读取。

发生真变化时，小型 helper 复用原 state target 表，置 compute active bits 和
**next-arm**，仅 E 成员累积下一轮条件。原 publish 将该条件与其他 pending 的最终
变化合并并清除；init 也清除。非 E 状态更新不得增加轮次，既有 roundSeeds 保留。
direct commit 不改变 phase/task/domain/event 策略，也不影响 DPI 真实调用与历史采样。

专门非 E fixture 通过 void DPI 观察每次 G 的旧状态，要求每次 eval 恰一次调用，
覆盖 signed 5-bit、unsigned 64-bit、bool、零/部分/全 mask、重复 eval 和 init。
生成标记还检查多写者及普通写口写 history 的回退，防止提前记录中间变化。

### CPU C++ 提交端口变化武装

满足以下条件的 direct-commit 写口（regWrite/latchWrite）按变化武装（change-armed）：
enable/data/mask 三个操作数都是 boundary 存储的 2-state 1..64-bit logic，不是
state.read 别名，且生产者仅限 `core.compute.*`、`core.state.read`、`core.state.memRead`
（这些写站点都经过 computeGroup 的变化发布路径）。每个 task 内合格写口按 op 顺序
编号，每 8 个组成一个 `cpu_pflags` 活动字节；`init()` 全部置 1，首轮必做完整求值。

机制依据：写口目标状态只有唯一写者（direct commit 前提），两次求值之间 `current`
不变；操作数是 boundary 持久值，只随 compute 的变化发布改写。因此若某个边沿求值时
全部操作数与上次求值相同，则 `(current&~mask)|(data&mask)` 的重算结果必然等于
`current`，写入与 `cpu_direct_state_changed` 通知都不会发生，跳过求值不可观察。
常数操作数（如常量 enable）的写站点从不产生变化事件，相应端口在首轮后不再被武装，
同样安全。

武装端在 compute 侧：`computeGroup` 的分组键加入端口武装签名，使带端口目标的 value
即使原无 fanout 也获得 `cpu_changed` 比较；组尾在原 fanout 激活之外追加
`cpu_pflags[word] |= -changed & mask`。常量操作数没有写站点，天然不武装。

消费端在 commit 任务的 main path：事件采样仍按原 op 顺序无条件发射（共享 history
的覆盖顺序不变），合格写口的写块移到按 `cpu_pflags` 字节分组的武装段，位于其他
写口之后；不同写口的目标状态互不相交、boundary 在 commit 期间稳定，该重排不
改变任何状态内容。每个端口仅在武装位置位时求值，且仅当其边沿守卫（共享快照或
内联边沿表达式，latch 为 true）成立才消费该位：边沿未触发时保留武装位到下一次
执行。stable-history 整任务跳过与 inactive-edge 采样路径不消费武装位。被跳过的
端口状态更新、pending 记录与轮次收敛结构（`again`/E）与原语义逐位一致。

武装位持续直到被消费，跨 round、跨 eval 有效；同一 eval 内 compute 的新变化在
下一轮重新武装。发射器诊断给出 `port_arm_ports/tasks/values/words`。

### CPU C++ 共享边沿端口块的一次消费

提交端口仍以持久 `cpu_pflags` 记录 enable/data/mask 变化。现有最多八字节扫描
块内，若所有端口使用同一个非空缓存边沿快照，则先检查该快照一次。如果全部
armable 端口在整个 task 内也共享该快照，则检查进一步提到所有扫描块的入口。
无法按整个 task 共享时仍逐块判断，混合块保留原路径。边沿为假
时不读取/消费端口位，边沿为真时按原次序检查武装位、enable 和实际值变化。
本块端口的边沿已经相同，无需逐端口再次检查或累积 consumed。字节中八个位
都有端口时消费后直接置 0；不足八个时仅清除实际端口掩码，保留其他位。

例如某字节的五个端口都使用 `posedge(clk)`，有效掩码为 `0x1f`。下降沿时全部
武装位保留；上升沿时按原序评估已武装端口，末尾执行 `pflags &= ~0x1f`。
enable 为 false 的端口也消费该武装位，后续 enable 变化仍会重新武装。不同事件
或不能缓存的 guard 继续逐端口消费，不合并。事件快照在原 history 采样之前计算，
而 pflags 只由 compute 写入，因此边沿成立时清除这些位与逐位消费等价。
history 采样、状态写入、通知、padding 位和跨 eval 的持久规则保持。
诊断 `shared_edge_blocks/ports` 表示共享检查的块数和端口数。

### CPU C++ 活动扫描字打包

逐轮稠密扫描统一加字粒度空测试预过滤：调度循环中共享同一 8 字节 `cpu_flags`
桶的相邻单字节任务检查、domain-arm 逐槽交接（`cpu_flags[s]=cpu_next_arms[s]` 加
清零）、以及 commit 任务 `cpu_pflags` 武装段的逐字节走查，都先在桶范围上用
`cpu_word8`（定长 `std::memcpy` 装入 `std::uint64_t`，字节序与空测试无关）检测
是否全零。全零证明桶内每个字节检查都是无操作：没有任务会执行、没有槽需要拷贝
或清除（交接桶测试 `next_arms|flags`，两侧都零时逐槽写本身就是恒等）、没有端口
字会消费武装位；非零则进入桶并逐字节执行原逻辑，顺序与语义逐位不变。

打包只覆盖同桶相邻且不少于 2 项的组，组内保持原发射顺序；多 word 任务、无
条件任务、孤立槽位与不足 8 字节的尾组仍按原标量形式发射（尾组用收窄的定长
`memcpy`，不越界）。预过滤只读取活动数组，不引入队列、不改变武装/消费/发布
路径。发射器在写完模型后报告 `dispatch_packed_checks`、
`handoff_packed_slots`、`port_arm_walk_packed_words`。

### CPU C++ 边沿静默跳过

compute supernode 的全部外部效应都来自边沿守卫的 system/DPI op（守卫项
`(!hist && event)` / `(hist && !event)` 及其内嵌历史采样）时，该 supernode 在
入口满足所有守卫项 `hist == event` 的激活是 provably inert：两个方向的边沿守卫
都为假，内嵌历史采样是 `current == next` 的无操作（本轮内该 supernode 尚未执行，
历史态不是 dirty，守卫读取的 `cpu_objects` 值正是采样 helper 要比较的 current；
别名历史根本没有采样发射，`stage()` 直接返回，其守卫经 `object()` 解析读同一个
代表元）。frame 清零、局部字符串与全部 frame-local 计算都是每次激活的临时量，
没有跨 supernode 可见性。发射器把合格 supernode 的函数体包进
`if((hist0)!=(event0) || ...){ ... }`，每次激活先求值；活动字节读取/清除、位
消费与归还、播种、轮次与收敛结构完全不动，被跳过的执行与执行惰性函数体逐位
一致：没有 pending 记录、没有 fanout、没有状态变化。

合格性按 supernode 静态判定（全部 op 须通过）：边沿守卫的
`core.system.task`/`core.dpi.call`（事件不在本 supernode 内产生、且是可见状态或
boundary 读取——与 compute 守卫提升相同的不变量测试）贡献去重后的
（别名解析 history，event）项；其余 op 只能是纯 frame-local 计算（不写
boundary/object/shadow、无 fanout、无端口武装目标、不更新 `cpu_read_offsets`）。
含无 `event_edges` 的电平调用、`core.output.write`、带返回值的 DPI、boundary
写回的 supernode 不合格；任一守卫历史若在模型范围内被本 supernode 的边沿项之外
引用（含别名前像）或为 batched history，同样不合格——共享历史按程序序覆盖
pending 值，外部采样者会让被跳过的采样变得可观察。检查项为每个去重项的
`(state(hist))!=(value(event))`，发射在 frame 清零之前。发射器诊断报告
`compute_quiescence_units`/`compute_quiescence_terms`。

### CPU C++ 宽位运算活动度

compute word 调度沿用 legacy 局部活动字节结构：读取 `cpu_flags[wordOffset]` 到
`cpu_active_word` 并清空全局字节，然后依次检测/消费 supernode 位。当前 word 中严格
靠后的 fanout 位 OR 到局部字节；当前/先前位及其他 word 保留全局写入。同一激活集合
按 word offset 合并掩码，helper chunk 通过引用共享局部字节。word 退出时将未消费局部位
OR 回全局，不覆盖已经排队的回边。例如当前 bit2 激活 bit1 和 bit5，bit5 本次即可执行，
bit1 留在全局等待后续扫描。域 arm、DPI guard/history 和 commit 发布保持原语义。

带 `computeSupernodeFanout` 的 two-state 宽值（大于 64 位）`and/or/xor/not` 使用
`cpu_bitwise_words_changed<Operation>`：输入为 lhs/rhs 指针及各自 word 数、结果位宽、
输出指针及其 word 数；not 的 rhs 为空且 word 数为零。每个结果 word 按 legacy
指针 helper 的零扩展规则计算，先屏蔽结果宽度之外的位，再比较旧 word 并原位写回。
返回 bool 表示任意 word 真变化，只有返回 true 才发射 mapping 已有的 fanout 激活。
例如 129 位结果的最后一个 word 仅最低位有效；旧 padding 非零也会被清除并报告变化。

输出为已初始化的持久 boundary 槽，不创建宽值副本或返回值数组；逐 word 运算支持
输出与任一输入的精确指针别名。无 fanout 的局部输出仍调用原 legacy helper，不读取
旧局部缓冲区。该变化不修改共享 legacy runtime、mapping、DPI/event 或状态提交。
宽加减也使用 `cpu_arithmetic_words_changed<'+'/'-'>` 单遍计算最终 word，保持 legacy
进位/借位和零扩展规则。宽移位使用 `cpu_shift_words_changed<'L'/'R'/'A'>`，参数依次为
输入指针、输入 word 数、移位量、结果位宽、输出指针、输出 word 数；移位量仍由
`grhsim_index_words` 饱和到结果位宽。左移逆序、右移顺序写出，算术右移先读取原符号位，
随后直接合成补符号及末字截断后的 word，不预清零输出，也不保存整块旧值。
例如 129 位算术右移 129 位，符号为 1 时输出 `[UINT64_MAX,UINT64_MAX,1]`；
重复计算相同输出返回 false。仅有 fanout 的持久输出使用变化检测，其他输出保留原路径。

### CPU C++ 多时钟回归

发射器在同一个边沿域 commit task 内按采样等价类共享私有事件历史，适用于
`DomainGatedCommit` 和 `AlwaysScanCommit`。每个 history 单独检查：全模型的
object ref 必须全部是本 task 中 `regWrite/memWrite/memFill/memWriteSeq` 的
posedge/negedge 采样，且每次采样使用同一个、不经 state-read alias 的 boundary
ValueId。history 与 event 类型相同，均为 unsigned two-state 1-bit；history 具有
唯一常量初始化记录，publication 目标恰为本域 arm。同一 history 可以被本 task
重复采样；被观察、被其他操作写入、跨 task 采样、采样不同事件或随机初始化的
history 保留独立存储，不妨碍同 task 内其他 history 共享。

共享键为 `(event ValueId, history TypeId, 规范化初始常量)`。例如同一 task 的三个
posedge 操作引用历史 `[h0=0,h1=0,h2=1]` 且采样同一 `clk`，发射后前两个守卫
读取 h0，第三个仍读取 h2；只暂存 h0 和 h2。每次 task 执行均无条件采样历史，
因此相同初值的成员在所有 publication 边界保持相等。guard 始终读取 visible 值，
代表的 shadow 更新不会提前暴露；代表沿用原域唤醒目标。IR state、layout 元数据和
schedule 本身不变，发射器重定向私有存储访问并省略冗余 stage；原存储 arena 大小
保持不变。重复 init 只向代表写入相同常量，不改变随机数序列。诊断
`history_shared_states/tasks` 给出省略的状态数和受影响 task 数。

混合示例：同 task 中 `h0=0` 被两个写端口采样 `clk`、`h1=0` 被一个写端口采样
`clk`、`h2=0` 还被输出读取。前两者可共享 h0（代表元的两次原始采样仍保留），
h2 保持独立。若 h0 的两次采样分别使用 `clk_a` 和 `clk_b`，则 h0 也保持独立；
不能对 `h0←clk_a; h0←clk_b; h0←clk_a` 跨越中间写入去重。

For tasks with shared private histories, repeated edge predicates are evaluated
once into task-local `const bool cpu_edge_snapshot_N` values before payload
writes. The key is the ordered list of `(event ValueId, resolved history StateId,
edge polarity)`. For example, two writes using `!h0 && clk` reuse one snapshot;
a write using `!h2 && clk` or `h0 && !clk` retains a distinct predicate. Singleton
predicates remain inline. All commit operands retain pre-commit boundary values,
and private histories change only at publication, so the snapshot stays valid
through intervening payload writes. It is recomputed on every task invocation.
Write order, unconditional history sampling, stable-history and inactive-edge
exits, and publication remain unchanged. Only predicates whose every history
passes the sampling proof can be cached; other predicates in the same task
retain the existing path.

CPU emitter 可以在同一边沿事件域的 commit 函数内部批量暂存私有 event history，
包括因其他 history 冲突而使用 `AlwaysScanCommit` 的边沿域。
资格为：history 仅有一个 object ref、state/event 类型相同且均为一个字节的 2-state
1-bit logic、projection 为真、fanout 只有本域 arm、批内对象字节连续。至少四个 history
组成短 pattern 批次；其余仍逐 op stage。DPI/system task 和 general commit 不参与。

各 guard 仍读取独立旧值，批量 shadow 写在原函数末尾完成，publish 时对范围比较并
按共同目标激活。没有合并 state、初值或不同域，也没有新增 task/调度条件。单值用
memset，多值用至多八字节 pattern 的原地 memcpy；复用既有 pending ABI。
emitter 的 history_batch 诊断给出实际覆盖和回退数量，不能仅凭该数量判断性能收益。

批次的 `[offset, offset + count)` 范围由 pattern 完整覆盖，因此生成代码使用
`cpu_stage_bytes_overwrite` 登记 dirty/pending 后直接取得 shadow 指针，省略 visible
范围的预复制；普通 `cpu_stage_bytes` 仍保留预复制语义。publish 继续对最终 shadow
与 visible 做 memcmp，所以批次与同轮其他写入产生相同的最终值时不会虚假激活读者。

`DomainGatedCommit` 函数可从实际 op/event operands 合成一个有限的必要条件：
所有 posedge 当前值及所有 negedge 当前值取反的 OR。条件为假时，payload 的全部
edge guard 必为假，函数只按原 op/event 顺序采样 history，并执行同样的私有批次。
条件为真时保留原逐 op guard/写入/采样。最多合并八个去重的 (ValueId, edge) 项，
超过上限、无事件或不支持的 op 回退；general/AlwaysScanCommit 不参与。
该路径不跳过 history、不冻结历史、不改 arm 或 E，不改变任何外部调用的执行。

同一 (ValueId, edge) 项含至少 16 个独立、unsigned two-state 1-bit history 且每个
槽均为一字节时，可以进一步只读检查 visible history：按 offset 去重排序，形成不跨
间隙的连续区间，平均每段至少四字节才启用。posedge 项仅在当前值为真且任一区间有
旧值 0 时可能触发，negedge 项仅在当前值为假且有旧值 1 时可能触发。单段直接
`std::memchr`，多段使用 constexpr offset/size 表及短路循环，不分配或复制 history。
稀疏/小组保留原电平必要条件。这个 OR 只决定是否走上述 sampling-only 分支，各
history 的初值和完整 guard 仍独立；只有上述已证明等价的历史才能使用代表，
不能读取 pending shadow。
回归覆盖尾字节命中、混合边沿、不同初值、派生时钟、区间间隙、固定电平下数据变化
和重复 init，并保留 shared-history 的 AlwaysScanCommit 测试。

`DomainGatedCommit` 还可在原 body 之前检测整个 task 是否无效果：仅限
`regWrite/memWrite/memFill/memWriteSeq`，全部 event 为 posedge/negedge，全部 history
均为 unsigned two-state 1-bit、一字节存储且各自只有一个 object ref。至少 16 个 history、
至多八个不同的当前事件 ValueId 才启用。共享、被观察、被普通写口写入的 history
以及 general/AlwaysScanCommit、DPI/system task 均不使用该检测。

条件为每个 history 的 visible 值等于其当前事件值。成立时全部边沿守卫为假，且历史
采样也不改变值；私有性排除了需要覆盖的其他 pending 写者，因此可直接返回而不暂存。
任一 history 不同就完整保留原 guard/采样路径，不能据此冻结 history 或合并不同初值。
已证明等价的共享历史按代表 offset 去重，避免重复扫描同一个值。
按当前 ValueId 分组，连续历史字节以 memchr 查找相反布尔值，非连续历史以 constexpr
offset 表逐字节比较，不扫描间隙、不创建副本，不改变 E、domain arm 或 fixed-point 规则。

例如同一 task 的两个 posedge history 为 `[1,0]`、当前时钟为 1 时，检测必须失败，
第二个守卫仍需触发；两者采样后均为 1，下一轮相同电平时才可跳过。如果下一次时钟
下降到 0，即使 posedge 守卫都为假，也必须采样成 0，不能跳过该更新。

CPU emitter 测试含三个独立多时钟 DUT，各自对照 Verilator 逐次 eval：

- `cpu_chain`：双时钟、派生 gated clock、异步复位、latch、多 mask 写及写回原值。
- `cpu_cdc`：posedge A / negedge B 计数器及双向两级采样，独立记分板检查同次 eval
  双域触发时必须读旧值，覆盖计数回绕、不同频率、无输入变化的重复 eval 和异步复位。
- `cpu_dual_ram`：双 posedge 域 masked memory write 与同步读捕获，独立记分板检查
  read-before-write、异步复位、空闲域和全地址观测。排除 RTL 未定义的同时同地址双写。

这里的 IR fixture 为手工构造，用于隔离 CPU mapping/emitter 语义，SV 供 Verilator
参考；不是 ingest 端的自动等价证明，也不模拟硬件 CDC 亚稳态。全链路集成另由
HDLBits 和 XiangShan 回归覆盖。

### CPU C++ DPI 与系统任务

`core.dpi.call` 留在 compute，按 `callCond && eventGuard` 发射实际 `extern "C"` 调用；
没有 event 时 `eventGuard=true`，没有结果时仍发射调用。调用条件为假或未命中边沿时，
boundary 中的旧返回值/output/inout 保持不变。所有 event history 均在调用条件之外采样，
沿用当前 staged history/publish 路径，不改变 phase、event metadata 或 schedule。
调用条件按 legacy 的非零判真规则处理；宽逻辑条件用归约 OR，不能只检查最低位。

声明消费 `DpiSignature`，ABI 对齐 legacy：标量 input 按值，string input 为 `const char*`，
宽 input 为 `const std::array<uint64_t,N>&`，output/inout 指向调用者临时存储。inout 先按
声明类型归一化并装载输入，调用后依次发布返回值、output 和 inout，真变化时激活 mapping
中的 fanout。宽结果屏蔽 padding；窄 signed 结果保留逻辑位宽的符号扩展，包括 signed 1-bit。
Real 端口和 DPI 值使用 double。此 ABI 不是通用 svdpi/open-array ABI；unpacked array 参数
显式拒绝。重复 symbol 的不兼容声明、调用数量/类型不匹配在写出文件之前报告错误。
DPI C 声明只写入 compute task 源文件，不污染公共模型头文件。这沿用 legacy 的翻译单元
边界，允许 harness 继续包含其原生接口声明；本机 scalar ABI 保持原始位模式，不承诺跨 ABI
或通用 svdpi 兼容。

例如声明参数顺序为 `(out output, acc inout, x input)` 且有返回值时：

```text
operands = [callCond, x, accBefore, clk]
object_refs = [function, clkHistory]
event_edges = [posedge]
results = [returnValue, outValue, accAfter]
C call = function(&outTemporary, &accTemporary, x)
```

系统任务复用 legacy runtime 的 `grhsim_make_task_arg`/`grhsim_format_task_message`，参数只在
调用守卫命中时构造。支持 display/write/strobe/fdisplay/fwrite、info/warning/error 和
fatal/finish/stop；strobe 延后到当前 eval 收敛时输出，terminal task 刷新输出并退出进程。
标准 stdout/stderr 句柄支持 1/2 及 0x80000001/0x80000002，其他文件句柄目前显式报错。
initial 无 timing 的 task 受首个 eval 门控；initial 带 timing 的 task 在首次实际触发后
标记完成。final process、其他任务名和数组任务参数仍被 emit 明确拒绝，不能静默跳过。

## 6. 参考

- [GrhSIM IR Overview](../overview.md)
- [Core Dialect](../dialects/core.md)
- [CPU Dialect](../dialects/cpu.md)
- [Pass System](../passes/overview.md)
