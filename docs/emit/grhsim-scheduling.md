# GrhSIM 当前调度方法

本文按当前代码实现描述 GrhSIM 的调度方法。实现分两层：

- `activity-schedule` pass 生成静态 schedule。
- `grhsim-cpp` emit 把静态 schedule 生成 C++ `eval()` 运行时代码。

对应实现位置：

- 静态 schedule：[activity_schedule.cpp](../../lib/transform/activity_schedule.cpp)
- C++ emit：[grhsim_cpp.cpp](../../lib/emit/grhsim_cpp.cpp)
- 选项定义：[activity_schedule.hpp](../../include/transform/activity_schedule.hpp)

## 读本文前先统一术语

`source` 这个词必须带上下文。本文不用 `source` 表示“没有依赖的图论起点”。
代码里存在枚举值 `ActivityOpClass::Source`，本文称它为 `source-class op`。

`source-class op` 只是 `activity-schedule` 的内部分类名。它当前包含
`kConstant`、`kRegisterReadPort`、`kLatchReadPort`、`kMemoryReadPort`。
其中 `kMemoryReadPort` 有地址、使能等 operand；这些 operand 仍然是依赖，
仍然会被 compute node builder 处理。也就是说：

```text
kMemoryReadPort 属于 ActivityOpClass::Source
!= kMemoryReadPort 没有 operand 依赖
```

## 总体模型

静态层生成两类最终 supernode：

- `computeSupernode`：组合计算、读端口、DPI/system task 等 compute 侧逻辑。
- `commitSupernode`：寄存器、latch、memory 的写入侧逻辑。

运行时每次 `eval()` 执行一个 fixed-point 循环。循环维护以下状态：

- `pending_eval_round` 是 `eval()` 局部循环门闩。round 入口将它复位为 false；
  round 末尾根据本轮产生的新活动重新赋值。
- `supernode_active_curr_` 是 compute supernode 的待执行集合。compute phase 会消费
  其中命中的 bit；compute value 变化和 commit state 变化都可能再写入新的 bit。
- `commit_activated_readers_` 是 commit phase 的本轮累加结果。它在 commit phase
  开始前清零，然后只由 commit/writeback 代码在“visible state 实际变化且存在
  reader compute supernode”时重新置 true。

compute batch 和 commit batch 都是生成出来的 C++ 方法。每个 round 按生成顺序调用
所有 compute batch，然后按生成顺序调用所有 commit batch；具体 body 是否执行由
batch 内部判断：

- compute batch 读取并消费 `supernode_active_curr_`，只执行 active bit 置位的
  compute supernode。
- commit batch 不用 `supernode_active_curr_` 决定是否扫描 commit supernode；它由
  event expression / update guard 决定具体 sink op 是否执行。

round 末尾用以下表达式更新 `pending_eval_round`：

```cpp
pending_eval_round =
    commit_activated_readers_ ||
    grhsim_any_active_flags(supernode_active_curr_);
```

## RepCut 包装层的端口通信

GrhSIM RepCut 的 `unit_to_unit` 宽端口通信默认逐个 64 字节块比较当前源与目标，
仅在内容不同时复制该块；不足 64 字节的尾块按实际长度处理。这里的 64 字节是
分块长度，不要求端口地址按缓存行对齐。端口使用普通 `std::array<std::uint64_t, N>`
存储，比较和复制都只访问本次通信的有效 word 范围。

完整块先读入 8 个源 word，以逐 word 异或及 OR 归约表示差异；仅在存在差异时
写回已读入的值。这样让编译器在比较和写回之间复用源数据，避免每个完整块调用
通用比较函数；外层保留紧凑循环，不展开整个大端口。尾块仍按实际字节数比较和复制。
该 C++ 表达不保证某种固定机器指令，实际效果须核对目标编译器的生成结果。

标量分区间连接同样比较当前目标值，只有不相等时才赋值；覆盖 bool 和 8/16/32/64 位
整数，不保存额外的历史状态。

规则适用于所有分区的标量及宽端口，不依赖模块名、信号名或 profile；发送方仍执行通信，
early/final update 的位置、复制顺序和 phase barrier 均保留。它直接比较当前目标值，
不保存上一轮源值，也不依赖 dirty 标记，因此初始化、重初始化和保持不变的输入均
使用同一条路径。Host 输入及顶层输出维持原有复制方式；Verilator 不生成
这些 helper，继续使用原来的标量赋值和宽端口复制。

这项优化减少未变化目标的写入，但增加目标读取和比较。当大量宽端口保持不变时，
它可以减少跨核写入代价；若每步几乎所有块都改变，额外比较可能抵消收益。实际选择
应以整个模拟器的 walltime 为准，不能将跳过的逻辑写入字节数视为硬件总流量降幅。

## RepCut 包装层的输入装载

每个分区的 `run_load_part_X_` 只把 Host 输入成员写入该分区自己的 top-input
字段；这些 load method 之间没有数据依赖。GrhSIM 生成的 `step()` 因而把
`load_step_fns_` 投递到与 eval/update 相同的 worker pool（默认 all-spin），并在进入
后续 eval phase 前等待这一批任务完成；N=1 仍在 Host 串行执行。这只并行化输入 fanout，不改变模型
计算、状态提交、通知或通信顺序。

Verilator 生成的 `step()` 保留 `run_host_phase_(load_step_fns_)`，继续由 Host
串行完成输入装载。这个分支差异位于 repcut package generator，而不是
`grhsim-cpp` 的模型代码，也不按分区名、信号名或 workload profile 做选择。

## 基本对象

### 可选的布尔生产者与 OR 归约融合

`bool_producer_reduce=1` 或 `WOLVRIX_GRHSIM_BOOL_PRODUCER_REDUCE=1`
启用实验性 emitter 规则，默认关闭；显式 attribute 优先于环境变量。
规则根据 IR 类型、使用关系、物理存储和已有 reader 集合选择，不使用模块名、
信号名、分区号、运行时 profile 或编译后反馈，不改变 Verilator 后端。

在普通 compute supernode 中，至少 32 个连续的持久 bool-slot AND 输出若参与
后续纯 OR 归约，且具有完全相同、完整覆盖下游的 deferred-reader 通知，便可
把输出计算、逐值变化检测和 OR 累计放进一个共享短循环。保留原生产者顺序与
每项输出写回，允许后项读取前项输出；循环返回 `changed` 和 `reduced`，分别
更新原 reader 累计标志和原归约位置的表达式。其他叶子继续在原归约点读取。

IR 检查要求输出地址互异、删除的 OR 中间值仅被该归约使用，且读取快照之后
没有覆盖相应物理 slot 的写入。输入/输出必须是 1-bit Logic 的持久 bool slot；
输入 port、直接状态视图、事件值、局部表达式和副作用均不能冒充可融合的生产者。
未融合的事件值叶子可以保留在原归约点读取，其边沿分类和写入完全保留。
不跨越有副作用或 reg-to-mem bypass 的操作，不合并即时 reader 通知。
不满足条件时保留原生成方式；fullpass 版本也保留原实现。

循环索引压缩同样只使用静态证明：目标地址连续不少于 32 项的区段以基址加
循环下标寻址，小间隙保留有序标量写回；两个输入索引各自跨度不超过 65535
时使用基址加 `uint16_t` 偏移，否则使用 `uint32_t`。没有长连续区段的组使用
完整三索引表。辅助函数不使用 `restrict`，因此有序的输入/输出别名仍然有效。
Clang 下禁止循环展开、向量化和交错展开，保持归约的中间值数量有界。

### 可选的当前全零 bool 赋值组保护

`grhsim-cpp` 的 `bool_zero_group_guard=1`（或环境变量
`WOLVRIX_GRHSIM_BOOL_ZERO_GROUP_GUARD=1`）启用实验性规则；默认关闭，显式 attribute
优先于环境变量。它不改变 activity schedule、模型字段或 Verilator 后端。

规则在普通 compute supernode 内按 IR 顺序识别至少 8 个连续的持久 bool-slot
赋值。赋值可以是 `kAnd`，也可以是 `kAssign` 包住已内联的 `kAnd`；只沿 emitter
已确认内联的同宽纯 bool alias 追溯，不重新求值有存储的快照。AND 有两个 1-bit
Logic operand：一个是共同 bool 状态字段（direct-state-read 或已内联的纯状态
读取），另一个是持久 bool 源 slot。比较实际 slot 编号，要求目标互异、整组源
与目标不相交。输入 port、任意表达式、事件值、宽值、带副作用标注的 op 不符合
资格；不同使能、其他 op、concat-prefix 声明和 supernode 边界终止分组。
fullpass 版本继续使用原来的实现。

对 `out_i = source_i & gate`，生成形式如下：

```cpp
if (gate || static_cast<bool>(out_0 | out_1 | /* ... */ out_n)) {
    // 原有逐项计算、新旧值比较、各自 reader 通知和写回，顺序不变。
}
```

只有 `gate == false` 且**当前**旧输出均为零时才跳过：所有 next 都为零，所有
changed 都为 false。存在旧值 1 时仍执行清零和精确通知；gate 为真则直接执行
原代码。不缓存历史状态，不假设上次 eval 后没有其他写者，不以采样、分区名或
信号名决定资格。这里仍沿用单模型无数据竞争的求值语义。专项回归为
`emit-grhsim-cpp-bool-zero-group-guard`，覆盖默认恒等性、选项优先级、正反两种
operand 次序、内联 alias/AND、状态快照/重叠/短组/副作用拒绝、DPI、使能翻转、重复 eval、不同 reader
集合、packed/direct deferred 通知、fullpass 和拆分 helper。

### GRH operation 和 GRH value

GRH operation 是图里的 op，例如组合运算、状态读端口、状态写端口、DPI 调用、
system task、声明 op。

GRH value 是 operation 的 result，或 graph 的 port value。静态依赖来自 value 的
def-use：一个 op 读取 value，就依赖这个 value 的 defining op。没有本 graph 内
defining op 的 value 通常来自 graph input、inout input 或外部边界。

### ActivityOpClass

`ActivityOpClass` 是 `activity-schedule` 的内部调度分类，不是硬件语义分类。

| Class | 当前包含 | 调度含义 |
| --- | --- | --- |
| `Source` | `kConstant`、`kRegisterReadPort`、`kLatchReadPort`、`kMemoryReadPort` | 本文称 `source-class op`。它的 result 在 compute 用户侧会被 clone；builder 可把它吸收到 compute node，或为它建立 owner compute node。这个名字不表示无 operand、无依赖。 |
| `Sink` | `kRegisterWritePort`、`kLatchWritePort`、`kMemoryWritePort`、`kMemoryFillPort` | 本文称 `sink-class op`。只进入 commit node / commit supernode。 |
| `Compute` | 普通组合 op、`kSystemTask`、`kDpicCall` 等 | 进入 compute node / compute supernode。是否有 side effect 由 builder 和 emitter 分别处理。 |
| `Declaration` | `kRegister`、`kMemory`、`kLatch`、`kDpicImport`、层级/XMR 类 op | 不进入 schedule；目标 graph 中不能残留层级类 op。 |
| `Unsupported` | 索引缺口或异常占位 | 不应作为正常 schedule 输入。 |

`kMemoryReadPort` 的完整语义是：

- 它被分类为 `ActivityOpClass::Source`。
- 它的 result 对 compute 用户会被 clone。
- clone 出来的 memory read 保留原 memory read 的 operand。
- clone 的 operand 依赖继续由 compute node builder 递归处理。
- 如果 operand producer 不能被吸收到同一个 compute node，该 operand 会成为
  `boundaryInputs`。

### Compute node

Compute node 是 `activity-schedule` 内部的中间调度原子，不是最终运行时
supernode。

一个 compute node 包含：

- `ops`：该 node 内的 source-class op 和 compute op。
- `boundaryInputs`：该 node 读取、但不由该 node 内部 op 产生的 value。
- `commonExpr`：共享表达式 owner 标记。
- `indivisible` / `intentGroup`：保护特定语义形态，例如 reg-to-mem intent。

Builder 会从 root value 反向追依赖，尽量把局部组合 producer 吸收到当前
compute node。以下情况会停止吸收，并把 operand 记为 boundary input：

- producer 已经属于另一个 compute node。
- producer 是共享表达式 owner，不能安全并入当前消费者。
- 当前 node 是不可分 intent group。
- 当前 node 达到 `maxOpInComputeNode`。
- producer 不是可本地共享的 compute op，或有副作用。
- producer 不在本 graph 内，或 classification 不可用于本地吸收。

### Commit node

Commit node 是 sink-class op 的中间分组，不参与 compute-node coarsen 和 DP。

一个 commit node 包含：

- `ops`：一组 sink-class op。
- `inputValues`：这些 sink-class op 读取的数据、地址、mask、条件、event 等 value。

Commit node 的 `inputValues` 会成为 compute node 构造的起始 value 集合，因为
commit phase 需要这些 value 在写状态前已经算好。

### Supernode

Supernode 是最终导出给 emitter 的静态执行单元。

| Supernode 类型 | 来源 | 运行时语义 |
| --- | --- | --- |
| Compute supernode | 一个或多个 compute node 展开后形成 | batch 每轮被调用；supernode body 只有对应 active bit 置位时才执行。 |
| Commit supernode | 一个 commit node 形成 | batch 每轮被调用；sink op 是否执行由 event expression / update guard 决定。 |

最终 supernode 不混合 compute 和 commit。Emitter 会检查这个不变量。

### Boundary value 和 activation edge

Boundary value 是跨 compute supernode 使用的 value：

```text
compute supernode A 产生 value v
compute supernode B 读取 value v
```

此时 `v` 对 B 是 boundary value。运行时如果 `v` 的缓存值实际变化，才需要激活
读取它的 compute supernode。

Activation edge 是“value 变化激活目标 compute supernode”的关系：

```text
value v changed -> set active bit of compute supernode S
```

同一个 value 在同一个目标 supernode 内有多个 use，只算一个 activation target。
同一个 value 扇出到多个目标 compute supernode，会产生多个 activation edge。

### Activity bit

`supernode_active_curr_` 是运行时的 compute activity bitset。它只表示
compute supernode 是否待执行，不表示 commit supernode 是否待执行。

写入来源：

- 第一次 `eval()` 激活所有 compute supernode。
- 外部输入变化激活读取该输入的 compute supernode。
- compute result value 实际变化激活读取该 boundary value 的 compute supernode。
- commit phase 写变 visible state 后激活读取该 state 的 compute supernode。

compute batch 会消费并清掉命中的 bit。commit batch 的扫描条件不使用这个 bit。

### State reader set

`state_read_supernodes` 是静态 schedule 导出的 state reader 表：

```text
state symbol -> compute supernode ids
```

Emitter 生成 `stateHeadSupernodesBySymbol`。Commit phase 写变 visible state 后，
根据这个表置位 reader compute supernode 的 activity bit。

### Event edge slot

Event edge slot 是运行时字段，用来记录本 fixed-point round 内某个 event value
是否出现边沿。它的生命周期是一个 round，不是一个完整 `eval()`：

- eval seed 会为直接 input event value 设置 slot。
- compute phase 中 event value 变化也会设置 slot。
- commit phase 和 side-effect event expression 读取 slot。
- round 末尾清空所有 event edge slot。

如果下一轮仍需要事件，必须在下一轮重新产生。

## Terminal priority mux 的 commit-demand 试验选项

长的优先级 `kMux` 链通常用于按优先级选择最终寄存器写入值。普通
`priority_mux_chains` 会在 compute supernode 中把链合成为一次优先级选择；这仍
会在每次 compute 激活时求值。GrhSIM 另有一个更保守的试验选项：

```text
priority_mux_commit_demand = 1
WOLVRIX_GRHSIM_PRIORITY_MUX_COMMIT_DEMAND=1
```

它只有在 `priority_mux_chains` 同时开启时生效，默认关闭，且不影响 Verilator
生成路径。规则不检查分区、模块或信号名字，而是逐条验证以下条件：

- 链在一个纯 compute supernode 内，结果是物化到稳定 value field 的标量值；
- 所有条件、候选值和 fallback 都是常量、输入 field 或稳定 value field，且不引用
  链内部的临时结果；
- 链根只有一个 value user，并且这个 user 是 commit 阶段的寄存器写入 data；
- 没有公开 output、waveform、inout 或第二个消费者观察该根值。

通过证明的链会从 compute body 中跳过，在 commit 写入的实际 event/update guard
内部重新求值。重新求值使用稳定 field/constant 引用，并关闭该临时结果的
change-propagation；状态写入、mask、event edge 和 reader activation 仍由原来的
commit emitter 处理。这样只有真正需要提交该寄存器的路径才执行长链，同时不会把
可能被其他观察者读取的组合值延迟到 commit。

当前实现还把这段冷链放进 `noinline` lambda，目的只是把长冷代码移出 commit
dispatch 热函数的指令流；它不是语义必需条件，若编译器或其他仿真器不支持该
GNU 属性，应禁用该试验选项。关闭该选项时生成源码必须与默认路径逐字节一致。

该选项仍属于实验性功能，不能用来替代完整的 Difftest/功能回归。专门的测试入口
是 `emit-grhsim-cpp-priority-mux-commit-demand`，覆盖开关关闭时的默认恒等性、
单消费者寄存器写入、时钟沿提交以及非时钟沿不提前改变状态。

## 静态 schedule 生成

### 1. 定位并准备目标 graph

`activity-schedule` 用 `path` 定位目标 graph：

- 单段 path：直接按 graph 名查找。
- 多段 path：按 `<root>.<inst>...` 逐层解析实例。

进入主流程前，pass 会：

1. 给缺失 symbol 的可分区 op 补内部 symbol。
2. 拒绝目标 graph 中的层级类 op。
3. `freeze()` graph，建立 def-use 信息。
4. 收集可分区 op，按 operand def-use 建 op-level topo DAG。
5. 按 `ActivityOpClass` 分类所有 op。

### 2. Clone source-class use

`cloneSourceUsesForCompute(...)` 只处理下面这种 use：

```text
ActivityOpClass::Source op result -> ActivityOpClass::Compute op operand
```

对每个这样的 use，pass 创建一个同 kind、同 attr、同 operand 的 clone op，并让
compute 用户改读 clone result。非 compute 用户不改写：

| 原 use 类型 | clone 后使用哪个 value |
| --- | --- |
| source-class result -> compute op | clone value |
| source-class result -> sink-class op | 原始 value |
| source-class result -> 其他非 compute 用户 | 原始 value |

这样做的目的，是让 compute 用户侧的状态读、常量读等入口可以被局部吸收到
compute node，或单独建立 owner compute node。它不改变 source-class op 自身的
operand 依赖。

对 `kMemoryReadPort`，clone 后仍然有地址、使能等 operand；这些 operand 随后仍按
普通依赖处理。如果发生 clone，pass 会重新 `freeze()` 并重建 topo / classification。

### 3. 构造 commit node

Commit node 由 sink-class op 形成。

流程：

1. 收集所有 `ActivityOpClass::Sink` op。
2. 为每个 sink-class op 计算 normalized event key。
3. 如果开启 `commitGuardEventBuckets`，把 update guard 也纳入分桶 key。
4. 在 `commitGuardEventBuckets` 模式下，event 内按 guard first-seen order 先以
   `min(maxOpInCommitSupernode, 4096)` 打包；单个 guard bucket 或 ordered write group
   保持原子，允许自身超过该上限。
5. 该模式请求 `maxOpInCommitSupernode > 4096` 时，只把相邻的完整
   4096-baseline cluster 按原顺序继续合并到请求上限，不重新切分 baseline cluster；
   关闭该模式时直接按请求值切分 event bucket。
6. 每个最终 cluster 形成一个 commit node。

Commit node 的 `inputValues` 包括 sink-class op 写入需要的 value，例如条件、
data、mask、地址、event 相关 value。后续 compute node 构造会从这些 input value
反向追依赖。

### 4. 构造 compute node

Compute node builder 的 root 包括：

- 所有 commit node 的 `inputValues`。
- graph output port value。
- inout port 的 output/oe value。
- 没有 result 的 compute op，例如部分 side-effect op。

处理某个 operand 时，builder 看该 operand 的 defining op：

| Defining op 情况 | 处理 |
| --- | --- |
| 没有本 graph 内 def | 记录为 boundary input。 |
| `ActivityOpClass::Source` | 尝试吸收到当前 compute node；如果不能吸收，则创建/复用 source-class owner compute node，并把该 value 作为 boundary input。该 source-class op 的 operand 继续递归处理。 |
| `ActivityOpClass::Sink` | 报错；compute 侧不应依赖 sink-class result。 |
| 可本地共享的 compute op | 根据共享度、最早消费者、可达性、容量等规则决定吸收或建立 owner compute node。 |
| 不可本地共享或带副作用 compute op | 建 owner compute node；当前 node 通过 boundary input 读取它的 result。 |
| declaration / unsupported | 作为 boundary input 或报错，取决于上下文。 |

构造完 compute node 后，pass 根据 `boundaryInputs` 建 compute-node DAG：

```text
producer compute node -> consumer compute node
```

如果 DAG 有 cycle，pass 会尝试把 cycle 中的多 op compute node 拆成 singleton，
然后重建 DAG。最多尝试 1024 次。

### 5. Compute-node coarsen

Materialize 阶段先创建初始 cluster：

```text
一个 compute node -> 一个 cluster
```

然后在 compute-node cluster DAG 上做 coarsen。当前 coarsen 受
`maxOpInComputeSupernode` 约束：两个 cluster 合并后的 op 数不能超过该值。

Coarsen pipeline 每轮按以下 stage 执行：

| Stage | 候选 | 排序/限制 |
| --- | --- | --- |
| `out1` | 当前 cluster 只有一个后继，尝试合并到后继 | 按 producer -> consumer activation weight 降序。 |
| `in1` | 当前 cluster 只有一个前驱，尝试合并到前驱 | 按 predecessor -> current activation weight 降序。 |
| `siblings` | 前驱集合完全相同的 cluster | 按确定性顺序批量合并。 |

`out1` 和 `in1` 受 `enableChainMerge` 控制；`siblings` 属于 coarsen pipeline，
不受 `enableChainMerge` 控制。每个 stage 使用 DSU 批量合并，合并后重新检查
topo 合法性。

性能保护：当 cluster 数不少于 100000，且连续 3 轮每轮减少的 cluster 数都小于
1024 时，coarsen 提前停止。

### 6. DP 连续分段

Coarsen 后，pass 在 topo-ordered cluster 序列上做连续分段 DP。这里不是任意 DAG
partition；它只决定 topo 序列中相邻 cluster 如何切成 compute supernode。

约束：

- 一个 segment 的 op 数不超过 `maxOpInComputeSupernode`。
- 如果单个 cluster 已经超过上限，DP 不会把它和其他 cluster 合并。

目标函数：

```text
cost(segment) = incoming_boundary_activation_edges + 1
```

含义：

- `incoming_boundary_activation_edges`：segment 读取、但 producer 不在该 segment
  内的 boundary activation edge 数。
- `+1`：轻量 segment 数惩罚。
- 同成本时偏向更长 segment。

DP 输出的 segment 会 flatten 成 compute node 列表，再展开成 compute supernode。
每个 compute supernode 内部还会按真实 op 依赖做 local topo sort。

### 7. 最终 materialize

最终 schedule 的顺序是：

1. 生成所有 compute supernode。
2. 追加所有 commit supernode。
3. 从最终 `supernode_to_ops` 重新构造关系。

写入 session 的 key：

| Session key | 含义 |
| --- | --- |
| `supernode_to_ops` | 每个 supernode 包含的 op。 |
| `op_to_supernode` | 每个 op 所在 supernode。 |
| `commit_locality_group_by_op` | 每个 commit op 的 canonical pre-merge locality group；非 commit op 为 invalid。 |
| `commit_locality_group_order` | `level-id` 下 canonical commit locality group 的伪 baseline topo 顺序。 |
| `dag` | supernode DAG。 |
| `supernode_kind` | compute 或 commit。 |
| `compute_nodes_by_supernode` | compute supernode 由哪些 compute node 展开得到；commit 对应空列表。 |
| `value_fanout` | 跨 supernode value 到目标 supernode 的 fanout。 |
| `topo_order` | supernode topo order。 |
| `state_read_supernodes` | state symbol 到 reader compute supernode 的映射。 |
| `summary_stats` | 统计信息 JSON。 |

最终 DAG 的边来自跨 supernode operand def-use。Commit supernode 不作为 value
producer 产生出边。

## Runtime model

Emitter 读取静态 schedule 后，会构造运行时模型。下面名字来自
`grhsim_cpp.cpp` 的 `EmitModel` 或 generated class。

### Static-to-runtime mapping

| 名字 | 来源 | 运行时用途 |
| --- | --- | --- |
| `activeIdBySupernode` | `topo_order` | 给 topo order 中的 supernode 分配 bit 位置；运行时 activity 语义只用于 compute supernode。 |
| `computeSupernodeIds` | `supernode_kind` + `topo_order` | compute phase 中 batch 方法覆盖的 supernode。 |
| `commitSupernodeIds` | `supernode_kind` + `topo_order` | commit phase 中 batch 方法覆盖的 supernode。 |
| `boundaryFanoutByValue` | `value_fanout` 过滤出 compute target | 某个 boundary value 实际变化后，要置位哪些 compute active bit。 |
| `inputHeadSupernodesByValue` | compute supernode 的 input operand scan | 外部输入变化后，要初始激活哪些 compute active bit。 |
| `stateHeadSupernodesBySymbol` | `state_read_supernodes` | visible state 变化后，要激活哪些 reader compute active bit。 |
| `commitInputValues` | commit supernode 的 input operand scan | 外部输入只喂给 commit 时，也要启动一轮 commit scan。 |

### Materialized value slot locality

Emitter 保持 state storage 的稳定布局，并按关联 state anchor、首次读取顺序、读取次数
和 graph order 排列 materialized value 的 typed slots。Compute supernode 继续以节点内
register/latch read/write state 的最大 slot offset 作为 anchor。

Commit operand 的 canonical 路径要求同时存在完整有效的
`commit_locality_group_by_op` 和 `commit_locality_group_order`。Emitter 先只按实际
schedule 扫描 compute reads，再按 canonical group topo order 扫描一次 commit ops；
组内保持实际 schedule 的稳定 op 顺序。每个 group 同时聚合其 register/latch write
target 的最大 state offset，并把该 anchor 应用于组内 operand。这样
`firstReadSequence`、`totalReads` 和 state anchor 都不再依赖 high-cap merged commit
supernode 在实际 topo 中的位置。

canonical/显式 4096 baseline 下 group 和 order 对应实际 commit supernode；默认 8192
execution schedule 会合并相邻的完整 baseline cluster，但各子组仍使用自己的 baseline
anchor 与 baseline topo 顺序，因此 execution partition 不会引起全局 value-slot 改号。

这两个 session key 是成对的可选兼容输入。任一 key 缺失，map 未覆盖全部 graph op、
commit group 不稠密，order 不是 group 的完整排列，或 commit op 未在实际 schedule 中
恰好出现一次时，emitter 对整张 value-slot 排列表回退到原有 schedule scan 和 commit
supernode-level anchor，不混用部分 metadata。当前 transform 仅在
`final_topo_policy=level-id` 时导出非空 canonical order；其他 topo policy 因此使用
legacy 路径。

`value_fanout` 中指向 commit supernode 的 target 不进入 `boundaryFanoutByValue`。
Commit supernode 每轮由 commit phase 扫描，不通过 boundary value activation 决定是否扫描。

### Runtime variables

| 变量 | 生命周期 | 写入者 | 读取者 | 含义 |
| --- | --- | --- | --- | --- |
| `first_eval_` | class field | 初始化/reset 置 true；`eval()` 结束置 false | `eval()` 开始 | 第一次 eval 需要激活所有 compute supernode。 |
| `pending_eval_round` | `eval()` 局部变量 | eval seed；每轮开头清 false；每轮末尾重算 | `while` 条件 | 是否还要进入下一个 fixed-point round。 |
| `supernode_active_curr_` | class field bitset | eval seed、compute value change、commit state change | compute batch；round 末尾 `any_active` | 待执行的 compute supernode 集合。 |
| `commit_activated_readers_` | class field；按 round 使用 | commit phase 前清 false；commit/writeback 路径按条件置 true | round 末尾；perf trace | 本轮 commit phase 是否因为 visible state 实际变化而激活 reader compute supernode。 |
| event edge slots | class field；按 round 使用 | eval seed、compute value change | commit event expression、side-effect event expression | 本 round 内的边沿事件。 |
| `prev_*` input fields | class field | `eval()` 结束更新 | 下一次 `eval()` 开始 | 外部输入变化检测 baseline。 |

`commit_activated_readers_` 的精确规则：

- 每轮 commit phase 开始前清 false。
- compute phase 不写它。
- 只有 commit/writeback 路径在 visible state 实际变化且存在 reader compute supernode
  时，才把它置 true。
- 它被置 true 的同时，会把 reader compute supernode 的 bit 写入
  `supernode_active_curr_`。
- round 末尾读取它，决定是否需要下一轮。

## Runtime eval

### Eval seed

每次 `eval()` 开始时，生成代码先决定是否需要进入 fixed-point round，并初始化
compute activity：

1. `initial_eval = first_eval_`。
2. `pending_eval_round = initial_eval`。
3. 如果是第一次 eval，置位所有 compute supernode 的 active bit。
4. 如果不是第一次 eval，比较当前外部输入和 `prev_*`：
   - 输入喂给 compute supernode：置位 `inputHeadSupernodesByValue` 对应的 compute
     active bit，并设置 `pending_eval_round = true`。
- 输入只喂给 commit supernode：不置 compute bit，只设置
     `pending_eval_round = true`，让 commit phase 有机会扫描 event/update 条件。
5. 对直接 input event value，更新 event edge slot。

### 一个 round 的精确时序

每个 fixed-point round 的代码等价时序如下：

```cpp
while (pending_eval_round) {
    pending_eval_round = false;

    // 按生成顺序调用所有 compute batch。
    // batch 内只执行 active bit 命中的 compute supernode。
    run_all_compute_batches_in_generation_order();

    // 初始化本轮 commit phase 的累加器。
    commit_activated_readers_ = false;

    // 按生成顺序调用所有 commit batch。
    // batch 内按 event expression / update guard 判断 sink op 是否执行。
    run_all_commit_batches_in_generation_order();

    pending_eval_round =
        commit_activated_readers_ ||
        grhsim_any_active_flags(supernode_active_curr_);

    clear_event_edge_slots_for_this_round();
}
```

分步骤说明：

| 顺序 | 动作 | 读写关系 |
| --- | --- | --- |
| 1 | `pending_eval_round = false` | 只清本轮循环控制变量。 |
| 2 | 按生成顺序调用所有 compute batch | batch 读取并清除命中的 `supernode_active_curr_` bit；compute result 实际变化时，按 boundary fanout 置位目标 compute bit；不写 `commit_activated_readers_`。 |
| 3 | `commit_activated_readers_ = false` | 开始收集本轮 commit phase 的结果。 |
| 4 | 按生成顺序调用所有 commit batch | commit batch 是否被调用不由 compute activity 决定；sink op 写变 visible state 且存在 reader 时，置位 reader compute bit，并把 `commit_activated_readers_` 置 true。 |
| 5 | 重算 `pending_eval_round` | 如果 `commit_activated_readers_` 为 true，或 `supernode_active_curr_` 仍有任何 bit 置位，则下一轮继续。 |
| 6 | 清 event edge slot | event edge 是 round-local 信号，下一轮必须重新产生。 |

`commit_activated_readers_` 在第 3 步初始化，在第 4 步由 commit/writeback 路径
更新，在第 5 步被读取。

### Compute batch

Compute batch 方法每轮都会被调用。它内部按 active bit 过滤：

1. 读取当前 active word 中属于本 batch 的 bit。
2. 清掉这些 bit，避免同一个 compute supernode 无条件重复执行。
3. 对每个命中的 compute supernode，按 supernode 内 local topo order 执行 op。
4. 如果某个 result value 实际变化：
   - 若它是 event value，更新 event edge slot。
   - 若它有 `boundaryFanoutByValue`，置位目标 compute active bit。

同一个 active word 内有局部优化：如果当前 compute supernode 激活了同一 word 中
active id 更大的目标，生成代码可以把该 bit 放入局部 `activeWordFlags`，让后续
supernode 在同一 batch/round 内执行。其他目标写回 `supernode_active_curr_`，
由后续 batch 或下一轮处理。

Compute batch 不设置 `commit_activated_readers_`。

### Commit batch

Commit batch 方法每轮都会被调用。它内部不看 `supernode_active_curr_` 来决定是否
调用 commit supernode；sink op 是否真正执行，由 event expression 和 update guard
决定。

Commit sink 的效果：

1. 判断 event expression 和 update condition。
2. 如果条件命中，执行对应写入路径。
3. 写 visible state，或通过 generated helper 把 pending write commit 到 visible
   state。
4. 只有 visible state 实际变化时，才激活 reader compute supernode。
5. 如果存在 reader compute supernode：
   - 置位 reader 的 `supernode_active_curr_` bit。
   - 设置 `commit_activated_readers_ = true`。

因为 commit phase 位于 compute phase 之后，commit 激活的 reader compute supernode
不会在同一 round 的 compute phase 中执行；它会让 round 循环再进入下一轮。

### Eval 收尾

当 round 循环退出后，`eval()` 收尾：

1. flush deferred system task text。
2. `refresh_outputs()`，发布最终 output/inout out/oe。
3. 如果启用 waveform，dump waveform。
4. 把当前 input/inout input 保存到 `prev_*`。
5. `event_baseline_initialized_ = true`。
6. `first_eval_ = false`。

## 关键不变量

### `ActivityOpClass::Source`

`ActivityOpClass::Source` 是调度内部分类。`kMemoryReadPort` 属于这个分类，同时仍有
地址、使能等 operand。这些 operand 参与 compute node 构造和 boundary 判定。

### Batch 调用和 body 执行

每轮都会按生成顺序调用所有 compute batch 和所有 commit batch。

compute batch 内部由 active bit 过滤 compute supernode body。commit batch 内部由
event expression / update guard 过滤 sink op。

### `commit_activated_readers_`

`commit_activated_readers_` 每轮 commit phase 前初始化为 false。commit/writeback
路径在 visible state 实际变化且存在 reader compute supernode 时将它置 true。
compute phase 不写这个变量。

### Commit supernode 扫描

Commit supernode 每轮由 commit phase 扫描。Compute result 变化只写 compute
activity bit。`value_fanout` 里即使有 commit target，也不会进入
`boundaryFanoutByValue`。

### Event edge slot

Round 末尾会清 event edge slot。下一轮如果需要事件，必须重新由输入变化或 op
result 变化产生。

## 选项语义

| 选项 | 语义 |
| --- | --- |
| `maxOpInComputeSupernode` | compute-node coarsen 和 DP 分段的 op 数上限；不是 emit 文件大小上限。 |
| `maxOpInComputeNode` | 单个 compute node 吸收 op 的上限。 |
| `maxOpInCommitSupernode` | commit cluster 的 sink-class op 打包上限，native default 为 8192；`commitGuardEventBuckets` 模式下单个 atomic guard/ordered bucket 可超限，请求 cap 大于 4096 时只做 4096-baseline cluster 的顺序合并；关闭该模式时直接按请求值切分。 |
| `enableCoarsen` | 是否执行 compute-node cluster coarsen。 |
| `enableChainMerge` | 是否执行 `out1` / `in1`；`siblings` 仍属于 coarsen pipeline。 |
| `commitGuardEventBuckets` | commit 分桶是否把 update guard 纳入 key。 |
| `splitOversizeComputeNodes` | 是否在最终 materialize 时切开超大 compute node。 |
| `splitOversizeComputeNodeMaxOps` | 超大 compute node split 的 chunk 上限；为 0 时使用 `maxOpInComputeSupernode`。 |
| `exportComputeDagPath` | 导出 compute op DAG JSON。 |

Emitter 的 `direct_single_writer_state_reads` 和
`pure_event_compute_word_bypass` 默认开启。两者都按 attribute、环境变量、C++ 默认值的
顺序解析；attribute 名分别对应环境变量
`WOLVRIX_GRHSIM_DIRECT_SINGLE_WRITER_STATE_READS` 和
`WOLVRIX_GRHSIM_PURE_EVENT_COMPUTE_WORD_BYPASS`。将 attribute 或环境变量显式设为
`0` 可回滚到旧生成路径。

### 仅复用已有 reader 的布尔状态视图

实验性 emitter attribute `direct_bool_state_views` 对应环境变量
`WOLVRIX_GRHSIM_DIRECT_BOOL_STATE_VIEWS`；attribute 优先，默认关闭。规则不按模块、
分区或信号名选择，也不使用编译后反馈；不影响 Verilator 后端。

对单写者 1-bit 普通寄存器 `q`，若某个 compute supernode 内的未物化读取经纯
`kAssign` 路径得到 `q`，随后以 `kNot` 或 `kLogicNot` 产生跨 supernode 的持久
中间值 `t = !q`，可以让纯 compute 使用者直接计算 `!q`，省去 `t` 的 slot
写回、新旧值比较和相关通知。这里“未物化”表示没有独立保存的旧状态快照，
不能把已保存的值当作当前 `q` 重新读取。

收益门槛要求 **所有使用者的 active ID 已在 `q` 的原有 reader 激活集合中**。
检查只读该集合，候选之间不相互扩展资格，也不增加或删除 register→reader 边。
因此 commit 改变 `q` 时已有通知足以唤醒使用者；同一 compute phase 内 visible
state 固定，不需要再保存一份反相信号。缺少任意 reader 就保留原路径。

多写者、latch/memory、reg-to-mem intent、可见输出/波形/事件值、物化读取快照、
commit 使用者和有副作用的路径均不匹配。布尔 eager lowering 继续把视图视为
纯 bool；bool-pack 不得把 `!q` 错当成原始 `q` 的可直接打包字段。
引用变化仍可能改变既有状态字段布局，因此“通知集合不变”不保证 commit
机器码、缓存行为或 walltime 不变，需要整体测量。

专项回归 `emit-grhsim-cpp-direct-bool-state-view` 检查默认恒等性、attribute/env
优先级、完整/部分/缺失 reader 集合、局部 alias、快照/副作用等拒绝情况及
commit 文本恒等性；执行 O0/O3、ASan/UBSan、重复初始化、使能/边沿保持、多写者
优先级、128-bit concat、packed/fullpass 和真正开启的 runtime profile 路径。

### 标量寄存器的小激活集合提交

实验性 emitter attribute `commit_scalar_eager_max_activation_writes` 对应环境变量
`WOLVRIX_GRHSIM_COMMIT_SCALAR_EAGER_MAX_ACTIVATION_WRITES`，按 attribute、环境变量、
默认值的顺序解析。默认 `0` 关闭；允许 `0..3`，其他取值报错。它不改变 schedule、
写使能条件、写入顺序或固定点终止条件，只选择部分标量寄存器提交的 C++ 生成方式。

门槛计算的是 reader 激活集合经局部/全局拆分和连续字节合并后的标记写入次数，
不是 reader 数量或寄存器位宽。例如 16 个 reader 的 active bit 若能合并为一次
掩码写入，其成本记为 1；相距较远、需要分别更新的 4 个标记可能记为 4。
只有不超过门槛的普通标量寄存器（最多 64 bit）匹配；宽寄存器、latch、memory、
reg-to-mem intent storage 和高成本激活集合保留原路径。已有 lockstep 状态分组不变。

在原有 event/update guard 内，原路径仅在 `state != next_value` 时写状态、累计
`touchedWriteCount` 并激活 reader。新路径保留同一个比较，将 `next_value` 无条件
写回标量状态，再用比较结果生成全零或全一掩码更新 active bit。`touchedWriteCount`
仍只累计真正的变化，`commit_activated_readers_` 也只在状态实际变化且存在 reader
时置位；未变化的状态不会因此激活额外 reader。部分掩码和多个写者仍按原顺序执行。
这里的“无条件写回”仅指已满足原写入 guard 的提交块内部，不表示每轮强制写所有状态。

此选项以少量额外的无效 store/OR 换取更少的变化分支，不保证运行更快；应在固定
workload 上与关闭状态交错测量最终 walltime。它不影响 Verilator 后端。

### 相同 reader 集合的标量提交分组

实验性 emitter attribute `commit_scalar_same_reader_groups` 对应环境变量
`WOLVRIX_GRHSIM_COMMIT_SCALAR_SAME_READER_GROUPS`，按 attribute、环境变量、默认值
的顺序解析，默认关闭。它不依赖上一选项，也不按模块、分区或信号名匹配。

在已有 event/update guard 的发射顺序中，将连续 2..8 个完整 reader active-ID
集合相同的寄存器写入分成一组。仅匹配单写者、全掩码、最多 64 bit 的普通寄存器，
且 next 值必须已经保存为 compute 标量结果。直接状态引用、宽寄存器、部分掩码、
多写者、无 reader、latch、memory 和 reg-to-mem intent storage 均回退；已有
lockstep 分组保持优先，不能与该分组合并。遇到不匹配项或不同 reader 集合就断组，
不跨 guard、event、supernode 或其他可见副作用移动写入。

组内先读取各自 next 值并计算精确 `changed_i = (state_i != next_i)`，再以
`changed_0 | changed_1 | ...` 判断整组是否变化。全都不变时不写状态也不激活；
任一变化时按原顺序写回该组所有状态，并仅写一次共享 reader 激活集合。原路径
激活集合的并集与这个共享集合完全相同，因此不会漏激活或多激活 reader。
`touchedWriteCount` 与 batch-trigger profile 仍分别按各 `changed_i` 累计，
不是按整组大小累计；固定点 continuation 标记也只在实际变化时置位。

该规则减少逐寄存器跳转和重复 active read-modify-write，同时在部分成员变化时
引入有限的冗余状态 store；是否更快仍需测量。此选项与小激活集合提交同时开启时，
先处理本分组，未分组的写入才考虑逐寄存器 eager。Verilator 生成路径不变。

### 实验性逐字宽拼接

Emitter attribute `wordwise_wide_concat` 对应环境变量
`WOLVRIX_GRHSIM_WORDWISE_WIDE_CONCAT`，默认 `false`，显式 attribute 优先。
仅作用于既有 direct-wide-concat 路径：结果至少 96 bit、至少四个操作数，
且不是 event 值。进一步要求操作数宽度和等于结果宽度、表达式为纯读取，
并排除操作数依赖树与结果物化存储的别名关系。

异步 `kMemoryReadPort` 也属于该规则允许的读取：其 operand 0 是地址，
`memSymbol` 指向只读访问的状态内存，结果为该行数据。拼接结果存储与状态
内存分离，因此组成目标字期间可重排这些读取；地址表达式仍递归接受相同的
纯表达式、非 event 及无目标别名检查。此放行只用于拼接规则，不扩大其他
调度/提交优化共用的纯操作集合。保留原有越界读取为零及宽度截断行为。

旧路径先清零结果数组，再将各操作数按位 OR 到目标字；实验路径在局部
`uint64_t` 中组成每个目标字，最后只写回一次。输入分块及结果尾字都保留
宽度掩码，跨字片段使用小于 64 的无符号移位；宽操作数表达式只求值一次。
不修改 supernode、执行 guard、值布局、变化传播或调度固定点。规则按结构
应用于所有满足条件的拼接，不使用模块、分区或信号名称。该 C++ 后端选项
不影响 SystemVerilog/Verilator；是否降低机器指令和 walltime 由实际编译及测量判断。

### 实验性宽拼接的无循环 bool 打包

Emitter attribute `wordwise_wide_concat_bool_pack` 对应环境变量
`WOLVRIX_GRHSIM_WORDWISE_WIDE_CONCAT_BOOL_PACK`，默认 `false`，attribute 优先；
只有同时开启 `wordwise_wide_concat` 才生效。按 IR 和存储元数据识别同一目标字内
连续至少八个状态 bool 字段读取，不依据模块、信号名、分区或编译后反馈。
允许寄存器/latch bool 字段及已内联的同宽纯别名；物化读取仍使用原值槽路径，
不绕过快照去读当前状态。排除物化值槽、输入、常量、event、内联计算、动态
memory 访问和 reg-to-mem intent storage。

生成的 constexpr 偏移表在目标 C++ 编译时验证：八个 bool 子对象的字节地址必须
连续（顺序或逆序），且目标 ABI 为 1-byte、0/1 表示、小端 bool。满足时用 `memcpy`
读取恰好八字节，借助无符号掩码和乘法压成八个位；非连续、重复槽、剩余不足八位
或不支持的 ABI 保留逐位展开读取。无运行期索引循环，不读 padding 或额外字段。
不改变存储布局、读写时序、激活集合、调度和 Verilator 路径。

### 实验性 producer dirty-word 宽拼接

Emitter attribute `producer_dirty_words` 对应环境变量
`WOLVRIX_GRHSIM_PRODUCER_DIRTY_WORDS`，默认 `false`，显式 attribute 优先。
它只在 `wordwise_wide_concat` 已开启时生效，并按 IR 结构处理所有满足条件的
宽拼接，不按模块、分区、信号名或运行时 profile 选择；关闭时不增加模型字段，
生成结果保持原路径。

对结果至少 96 bit、至少四个操作数的纯拼接，逐个目标 64-bit 字检查依赖。
候选字必须只读常量和至少八个不同的标量 register/latch 状态字段，且结果槽
持久化并由该 SSA value 独占。输入、event、memory、宽状态、带副作用、
reg-to-mem intent、物化的中间快照，以及无法证明为状态根的依赖保留原路径；
同一拼接中的其他字仍可单独符合条件。该静态门槛用于避免简单拷贝也承担通知
成本，不是对运行时收益的保证。

emitter 根据依赖建立状态字段到目标字 dirty bit 的映射。跨字字段会通知所有
读到它的候选字。只有状态写入实际变化时才置位；成组提交复用已有变化判断，
合并对同一位图字的通知。每个拼接独占自己的位图字，消费端先取出并清除标记，
再通过 `countr_zero` / `switch` 枚举实际需要重建的字，跳过未变化的字。

dirty bit 存在模型实例中并在初始化时置为全脏，因此首次计算和语义上需要的
重建不依赖历史状态。该规则只减少 producer 对未变化目标字的重复写入，不改变
compute/commit 调度、激活条件或 Verilator 后端。模型新增的私有成员会改变
C++ 对象布局，启用时需要完整重新编译；仍需用完整 workload
验证生成代码、Difftest 和 walltime，不能仅凭候选字数推断收益。

### 实验性局部宽右移的单比特需求计算

Emitter attribute `local_shift_demanded_bit` 对应环境变量
`WOLVRIX_GRHSIM_LOCAL_SHIFT_DEMANDED_BIT`，默认 `false`，显式 attribute 优先。
按完整 IR 使用集合匹配 compute supernode 内的局部 `kLShr`：operand 0 是待移位
数据，operand 1 是移位量，结果宽度大于 64 bit；所有消费者必须在同一 compute
supernode 内、位于其后，且均为读取同一个常量位置的单比特 `kSliceStatic`。
`kSliceStatic` 的 operand 0 为宽值，`sliceStart` 为所取位号，结果宽度为 1。
多个消费者读取同一位也可匹配；不同位、完整宽值输出、跨块/commit 使用、event、
需变化跟踪的宽值和算术右移均不匹配。不依据分区、模块、信号名称或 profile。

例如 `t = logical_right_shift(x, n, 128); y = t[0]` 原先构造两个 64-bit 字，
候选在 `t` 的原定义处生成 `const bool t = grhsim_lshr_bit<128, 0>(x, n)`，
消费者读取这个 bool 快照。不是把源读取推迟到消费者，也不修改 IR/状态布局。
普通单比特赋值、变化传播和激活路径保持不变。移位量仍用原 `grhsim_index_words`
处理，过大、负数或宽移位量的高字非零时返回零；跨字和非整字宽度遵循原 helper
语义，包括输入数组存在未清零 padding 的情形。该开关只影响 GrhSIM C++ 生成，
不修改 SystemVerilog/Verilator 路径。

### 实验性 demanded-bit 源值快照缓存

Emitter attribute `demanded_bit_source_cache` 对应环境变量
`WOLVRIX_GRHSIM_DEMANDED_BIT_SOURCE_CACHE`，默认 `false`，显式 attribute 优先；
只有同时开启 `local_shift_demanded_bit` 才生效。该规则处理的是同一个宽值被多次
用于局部 demanded-bit 读取的情况：在一个 compute supernode 内，完整 use-list 必须
证明同一已物化源至少被使用 128 次，且首次使用之后没有对同一 canonical storage
的后续写入，才在首次读取位置复制一次源值并复用该局部快照。输入、event 值和
不满足物理槽稳定性证明的值保留原路径；缓存不跨 supernode，也不改变状态布局、
写入、通知、调度或通信。

128 次是后端成本模型的静态门槛，用于让一次宽值复制能够摊薄重复的宽位读取和
避免小组因寄存器压力或代码布局回退。规则只依赖 IR 类型、use-list 和存储别名，
不按分区、模块、变量名、profile 或编译后结果选择；不影响 SystemVerilog/Verilator。
它可能减少局部 compute 时间，但不保证最终 Host/walltime 收益，必须以完整生成和
交错运行测量裁决。

### 实验性 deferred-activation 标志打包

Emitter attribute `packed_deferred_activation` 对应环境变量
`WOLVRIX_GRHSIM_PACKED_DEFERRED_ACTIVATION`，默认 `false`，显式 attribute 优先。
规则只改变 compute supernode 内已有 deferred-activation 组的临时变化标志表示，
不重分组、不跨 supernode，也不改变 schedule、存储布局或 flush 位置。

一个组表示一组需要共同激活的下游目标，其 Boolean 标志是所有相关源值的变化
条件的 OR。原路径可能在一个源值变化后分别更新多个组的标志。候选将每个组
映射到 `uint32_t` 的一个独立 bit，按照同一个变化条件一次 OR 入对应的 bit 集合；
flush 时用单 bit 测试恢复原 Boolean 条件。没有变化时 OR 零，不会额外激活目标。
例如源 `a` 对应组 0、2，源 `b` 对应组 1、2，原先四次 Boolean OR 变成
`mask |= -uint32_t(bool(a_changed)) & 5u` 和
`mask |= -uint32_t(bool(b_changed)) & 6u`；第 2 组仍严格等于
`a_changed | b_changed`，而不是所有组一起激活。

仅匹配 4..32 个组，且组内源成员总数至少是源值并集大小的两倍；现有分组器
最多生成 16 个组。此静态门槛避免对低重叠情况增加掩码操作，不使用分区、模块
或信号名称。未被已有组覆盖的直接目标继续走原路径；event 值、完整 compute
pass、commit 路径及已有 profile/bitmap 校验语义保持不变。逻辑计数仍按原组
记录，不应当作打包后物理指令数。该规则可能减少重复 OR 和同时存活的标志，
也可能增加位提取开销或依赖链，因此是否更快必须实测；不影响 Verilator。

### 实验性直接累计 deferred 最终激活掩码

Emitter attribute `direct_deferred_activation` 对应环境变量
`WOLVRIX_GRHSIM_DIRECT_DEFERRED_ACTIVATION`，默认 `false`，显式 attribute 优先。
它沿用已有组的完整源值/目标集合，仅选择最终目标集中的部分组：每组所有
目标必须同属一个对齐的 8-byte active-flag 区间，或全是同一个局部活动字中
尚未执行的前向目标；不能混合局部与全局目标，也不能拆开一个组。

同一区间至少有 3 个完整组，且组内源成员总数至少为源值并集大小的两倍，
才建立直接累加器；每个 supernode 最多 4 个，按局部/全局及地址的稳定顺序
选取。累加器宽度为覆盖目标所需的 1/2/4/8 byte，按该宽度对齐，并检查
访问不越出活动数组。中间空洞位保持零，最终 OR 不会清除已有激活。

源值变化时，将其涉及组的**最终目标位**直接 OR 到局部累加器；原 flush
位置再把该累加器 OR 到真实活动字。相较 `packed_deferred_activation`，
没有先编码成组编号、最后逐组解码再生成目标掩码的步骤。没有被选择的组
仍走原 Boolean/flush 路径，未被组覆盖的直接目标也保持原处理。
一个 supernode 存在直接累加器时不再启用其 packed-group 路径；没有匹配
直接累加器时才允许回退到显式开启的 packed-group 规则。

组的目标位互不重叠，所以任选该组一个最终 bit，就能精确恢复其 Boolean
变化条件；batch-trigger profile 和 bitmap 更新继续使用此条件。事件结果、
fullpass、commit、执行 guard、固定点和模型存储布局不变，不扩大活跃集合。
该选项不匹配模块、分区或信号名称，也不影响 Verilator；是否减少真实机器码
和 walltime 仍需实测，不能从累加器数量或源码更新条数直接推断。

为避免相同最终掩码被反复展开，直接累加器内部还会按源值的完整目标掩码
建立 equal-target cohort（目标集合相同的源值组）。至少两个源值匹配时，
先累计 Boolean 变化，再在原 flush 位置生成一次该掩码。每个累加器最多
`min(4, 所选原组数−1)` 个 cohort，按重复次数从多到少选择、掩码作为稳定
平局次序；未入选的源仍直接累计。每次变化条件仍在原执行位置求值，最终
位集合保持完全一致。cohort 的掩码先合入累加器，再恢复原组的 profile/
bitmap 条件，避免诊断遗漏。该因式分解不扩大激活集合，也不引入运行时
查表或模型状态；逻辑成本计数依旧不能代表最后的物理指令数。

### 实验性共享 deferred 变化归约

Emitter attribute `shared_deferred_activation` 对应环境变量
`WOLVRIX_GRHSIM_SHARED_DEFERRED_ACTIVATION`，默认 `false`，显式 attribute 优先。
它在一个 compute supernode 内，按源值所属的**完整 deferred 组集合**寻找
共同来源；只考虑 1..64 bit Logic 源值，不跨 supernode，不匹配分区、模块、
信号名称或 profile。对于 `S` 个来源共同参与的 `G` 个组，要求 `S >= 3`、
`G >= 2`，且 `S * (G - 1) - G >= 4`，即至少减少四次源码 Boolean OR。
门槛仅计入仍有独立标量变化通知的 materialized producer：排除初始化常量、
存储别名、直接状态视图、被已有 lowering 吸收的操作/循环以及特殊副作用路径。
保留原依赖组，不把消除的来源计作可节省的重复工作。

例如两组分别由 `a..f` 加上各自独有的 `u` / `v` 激活，原本需要把
`a..f` 的变化条件各累计两次。开启后先累计一次 `shared = a_changed | ... |
f_changed`，再在原有 flush 前分别合入两组。`u` / `v` 仍各自累计；因此
`u` 单独变化不会误激活另一组。每个来源的变化条件仍在原执行位置求值，
保留条件执行、原来的直接通知和全部目标集合；没有变化时不会额外激活。

只增加 supernode 局部临时量，不改变模型存储或通信布局。存在 direct
累加器或选择 packed-group 表示时沿用对应路径；未匹配这些表示的 supernode
仍可使用共享归约。fullpass、commit 和事件路径保持原语义；合回各组发生在
predicate materialization、profile/bitmap 观察和最终激活之前。
已有逻辑分组统计仍描述原始依赖关系，不能直接当作优化后的物理指令数。

这是 GrhSIM 专属的代码生成选项，不改变 Verilator。减少源码重复归约并不
保证 walltime 下降：编译器可能自行消除其中一部分，新的局部依赖链和代码
布局也会影响结果。应使用同一完整优化配置的新跑 AB/BA 判断收益。

### 实验性完整宽值赋值的 XOR 差异归约

Emitter attribute `wide_assign_xor` 对应环境变量
`WOLVRIX_GRHSIM_WIDE_ASSIGN_XOR`，默认 `false`，显式 attribute 优先。
它仅改变运行库的 `grhsim_assign_words_full<N>`：每个字仍先读取 `src[i]`，
再写回 `dst[i]`，把 `changed |= (old != next)` 改为 64-bit unsigned
`difference |= old ^ next`，最后返回 `difference != 0`。因此返回值仍精确
表示至少一位变化，所有字仍被写回，源目标为同一个数组时返回 false。
没有改变窄值、两字特化或需要截断/补零的其他赋值 helper。

此规则不按分区、模块、信号名或 profile 选择，也不改变调度、激活条件、
模型布局和 Verilator。它为编译器提供另一种差异归约表示，但**不保证生成
无分支代码**；编译器可能重新识别为逐字比较并提前跳出。是否减少机器码、
栈访问或 walltime 必须实测，不能用 C++ 中最后只判断一次作为机器级保证。

### 实验性宽值赋值的低成本条件激活

Emitter attribute `wide_assign_eager_max_activation_writes` 对应环境变量
`WOLVRIX_GRHSIM_WIDE_ASSIGN_EAGER_MAX_ACTIVATION_WRITES`，合法范围 `0..3`，
默认 `0`（关闭），显式 attribute 优先。它只作用于普通组合计算中的物化
宽值赋值：先按原 helper 完成一次赋值和变化检测，再按返回的 Boolean
结果生成精确的条件激活掩码，不再外包一层 `if (changed)`。

预算计数的是活动标志的写入次数，而非信号个数：同活动字中的后继位合并
为一次局部 OR，相邻全局活动字按既有 1/2/4/8-byte chunk 合并为一次写入。
只有估算为 `1..budget` 次写入时才选中；大扇出、激活表和超过三次写入的
形态继续走原分支。该成本估算沿用条件激活 lowering 的规则，不按位宽、
模块名、分区名或采样结果筛选。profile/bitmap 元数据可能另有写入，预算
并不表示最终机器指令数或全部诊断开销。

赋值 helper 只求值一次，`changed=false` 时 OR 零，不新增下游活跃位。
原来的直接目标集合、局部/全局位置、执行 guard、batch 边界、固定点及
模型布局不变；不将这些目标改为 deferred group，不改 commit 或事件值。
helper 的实现本身不变；该选项独立于 `wide_assign_xor`，也不影响 Verilator。
它可能减少条件跳转，也可能增加无变化时的标志写入、寄存器压力和溢出，
应以普通 walltime 配合动态诊断判断收益，不能保证机器级无分支。

### 实验性长 deferred predicate 物化

Emitter attribute `deferred_activation_materialize_min_sources` 对应环境变量
`WOLVRIX_GRHSIM_DEFERRED_ACTIVATION_MATERIALIZE_MIN_SOURCES`，为非负整数，
默认 `0`（关闭），显式 attribute 优先。正数表示分组变化归约的最小源数。
筛选普通 compute deferred group：源数达到门槛，源全部为 1..64-bit Logic，
且至少一个 8-bit 活动字包含两个或更多目标位。packed/direct accumulator
已接管的组保持原路径；fullpass 不生成此改写。筛选不依赖名称或 profile。

选中后保留原来逐源的 Boolean OR 归约，在 flush 前增加一次局部
`const volatile bool materialized = any_changed`，随后只读取一次到普通
`const bool ready`，所有掩码、profile 和 bitmap 使用 `ready`。此处 volatile
只限制编译器跨越该值读写重排/传播表达式，不是原子操作、CPU 内存栅栏或
线程同步；临时量只属于该次函数调用。目标是避免编译器把最终多位常量
掩码分散进整条归约链，生成大量 CMOV 和额外栈访问。代价是每组选中时
增加一次局部 store/load，所以必须实测，不能假定多位掩码一定受益。

变化检测、目标集合和激活条件完全不变；单独变化、无变化或多个变化都
保持原有谓词。此选项不改 RTL、存储布局、batch 边界、commit 或 Verilator，
与宽值条件激活选项独立。初始实验门槛为 32，不是默认推荐值。

### 实验性纯 bool 读取的非短路计算

`bool_eager_logic` / `WOLVRIX_GRHSIM_BOOL_EAGER_LOGIC` 默认关闭。
开启后，二元逻辑 AND/OR 的两侧必须都是 1-bit 值，且已是无副作用的
bool slot 读取或物化临时量，才以 bool 转换后的 `&`/`|` 代替 `&&`/`||`。
普通 compute 中的局部存储引用也可以满足条件：emitter 根据别名声明的
`ValueSlotScalarKind::kBool` 记录其纯读取属性，不根据用户信号名或分区名
判断。直接端口、混合位宽和未证明纯度的内联表达式保持原路径。

例如 `auto &a = value_bool_slots_[i]` 与已物化的 bool `b`，可生成
`static_cast<bool>(a) & static_cast<bool>(b)`。这保持逻辑结果，但总是读取
两侧；不能将该规则扩展到有副作用、可能越界或代价未知的表达式。
目标是降低不必要的短路控制流，不保证编译器最终无分支，也不保证每个
分区都加速。局部引用扩展不改变默认开关、存储布局、调度或 Verilator。

### 实验性标量零选择项的编译器提示

`scalar_leaf_mux_unpredictable` /
`WOLVRIX_GRHSIM_SCALAR_LEAF_MUX_UNPREDICTABLE` 默认关闭，attribute 优先于环境变量。
只在普通 compute 的标量 `kMux` 上生效：三个 operand 依次是条件、真值、假值；
条件必须为已经计算好的 1-bit 读取，两条数据分支中一条是确定的零常量，
另一条是已计算好的 1–64-bit 标量读取，且结果直接供给标量按位 `kOr`。
“已计算好”由 IR 类型和当前局部值/存储映射判断，包括输入、存储引用和已声明的
临时量，也包括展开成标量左值读取的寄存器/锁存器读取及其局部同宽 `kAssign`
别名；不会沿别名跨过算术、动态访存或另一层选择。未物化的复杂内联条件、
算术、动态访存不扩大到该规则。未知常量、宽值、
独立选择及仅参与逻辑 OR 的选择保持原路径。不依赖用户命名或分区名单。

例如 `OR(MUX(c, x, 0), MUX(d, 0, y))` 在满足上述条件时，用
`grhsim_sel_u64(c, x, 0) | grhsim_sel_u64(d, 0, y)` 表达选择项。
helper 在支持时使用 `__builtin_unpredictable` 提示条件不适合分支预测，
否则回退到原 `grhsim_mux_u64`。两者都先计算函数实参，不改变截断/符号扩展、
求值次数、状态或调度。等长 helper 名称避免扰动基于表达式长度的物化阈值。
该选项不保证生成无分支指令，也不保证 walltime 改善；与 `bool_eager_logic`
独立，不改变该选项或 Verilator 后端的默认行为。

### 实验性宽输入按读取位激活

`input_bit_activation` / `WOLVRIX_GRHSIM_INPUT_BIT_ACTIVATION` 默认关闭，
显式 attribute 优先于环境变量。它在 IR 上分析每个 compute supernode
（共同调度的一组操作）对外部宽输入的全部直接读取，收窄该输入对这个
supernode 的初始激活条件。适用于超过 64 bit 的 Logic 输入和 inout 输入侧，
不依赖模块名、信号名、分区编号或运行时 profile。

只有全部读取都能证明为固定位置、完整落在输入逻辑位宽内的切片时才收窄。
切片可以是单 bit 或多 bit。`kSliceStatic` 的唯一 operand 是源值，
`sliceStart` 是起始位；`kSliceDynamic` 的两个 operand 分别是源值和起始位，
起始位必须可证明为常量。读取宽度按 Logic result 的位宽计算，与宽输入的
切片生成规则一致。分析包含最终被内联的操作，同一 supernode 的全部读取
范围取并集；重叠范围只记录一次，跨 64-bit 字的范围逐字生成掩码。
例如，一个 supernode 只读取 193-bit 输入 `a` 的第 3、140 位时，`a` 对它的
激活条件相当于 `((a[0] ^ prev_a[0]) & (1ULL << 3)) != 0 ||
((a[2] ^ prev_a[2]) & (1ULL << 12)) != 0`；其他位变化仍更新输入快照，
但不再因这个输入额外激活该 supernode。相同输入字的 XOR 在输入分组内共享，
相同谓词的目标合并使用原来的活动位图生成逻辑。

例如从 193-bit 输入的第 63 位起读取 67 bit，掩码依次覆盖第 0 字的最高位、
第 1 字的全部位和第 2 字的最低两位。读取覆盖整个逻辑输入时不会增加
掩码判断，不能把存储字的填充位当作可省略的有效输入位。

整值读取、动态未知索引、读取范围越界或未证明的依赖保持整输入比较。
含副作用标记、DPI、system task、commit、memory/latch 读取或 reg-to-mem
intent 的 supernode 整体回退；只有保守 fanout、没有直接读取证据的依赖
也保留原条件。不会沿别名推断更窄的掩码。`input_fullpass_specialization`
或 `posedge_fullpass_specialization` 开启时暂不收窄。

规则只决定某个输入是否贡献激活，不能清除其他输入、状态变化或后续
固定点轮次提供的活动位；首次 eval 仍全量计算，输入变化仍保留原来的
`pending_eval_round` 行为。复用现有 previous-input 快照，不增加模型状态，
不改变 compute/commit 函数、分区、通信或 Verilator 后端。关闭时生成代码
保持原样；开启后多出的掩码判断是否划算，需要以普通 walltime 验证。

### 实验性 RepCut 通信变化标记与输入旧快照精简

`input_dirty_tracking` / `WOLVRIX_GRHSIM_INPUT_DIRTY_TRACKING` 默认关闭。
RepCut package 接受值为 `0` 或 `1` 的 emitter attribute，显式 attribute 优先于
环境变量。该选项只作用于 GrhSIM package；Verilator 路径不读取它。普通单模型
emitter 没有 package 提供的内部端口映射时，仍生成原来的 `eval()`。

package 统一选择至少 128 个 uint64 word、恰好有一个完整宽度内部发布点、
且没有输入端口别名的输入。顶层输入、常量、部分发布、多写者以及别名保持
普通路径，不依赖分区编号、模块名、信号名或运行时测量。通信 helper 复用
原本逐 8-word 块比较/复制的结果；发现变化时设置该输入的标记，否则不写标记。
每个标记占独立 64 B 缓存行，adapter 在 eval 后清零。标记与输入通过现有
phase barrier 一起交接，不增加原子操作或新的同步阶段。

模型以标记替代被选输入的整数组变化检查，逐位激活和未掩码激活仍使用原来的
规则。进一步只保存后续增量激活实际读取的旧值 word：所需 word 为所有输入
激活掩码的并集，最多占完整数组的四分之一时才启用，相邻 word 合并为固定长度
复制。事件分类需要读取旧值的输入保持完整快照。完整的当前输入存储、所有
compute/commit 逻辑和通信载荷都保留；精简的是用于比较的旧值副本。

adapter 的 `eval_tracked()` 与模型的 `eval_with_input_changes()` 是 package
内部协议。相邻两次 eval 之间只进行一次完整发布，标记必须等价于本次输入与
上次 eval 输入的整值比较；同一输入的标记只由它的发布线程设置，并由所属 eval 线程在阶段交接后
清零。对外 facade 的调用方式仍为 `init()` / `eval()`，由 package 自动维护
此协议。模型首次 eval 固定跟踪或普通模式，此后切换模式会抛出 `logic_error`；
`init()` 后可以重新选择，防止普通 eval 误用精简后的旧快照。未选择的输入在
跟踪模式内仍按原来的整值比较和快照规则处理。
