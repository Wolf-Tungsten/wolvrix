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

## 基本对象

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
