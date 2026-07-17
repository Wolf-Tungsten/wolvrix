# activity-schedule

## 功能概述

`activity-schedule` pass 为单个 graph 构建 GrhSIM 使用的静态 activity
schedule，并把结果写入 session。plain 调度路径是默认；local shared compute
clone、same-Kahn-level packing 和 post-DP refinement 均为默认关闭的 bounded
实验入口。

这个 pass 不会生成新的 wrapper/module，但会：

- 为缺失 symbol 的可分区 op 补内部 symbol
- 冻结 graph 并建立 def-use 信息
- 按 `ActivityOpClass::{Source,Sink,Compute,Declaration}` 分类 op
- 将 `ActivityOpClass::Source` 到 compute op 的 use 前置 clone
- 构造 compute node / commit node 中间模型
- 可选地只读探测 third common-owner 双边本地化机会并输出细分统计
- 可选地把严格受限的双消费者纯组合 op true-clone 到远端 consumer node，并完整重建中间模型
- 在 compute-node cluster DAG 上执行 plain coarsen 和连续分段
- 展开最终 `computeSupernode` / `commitSupernode` 调度模型
- 可选地只读探测把小型 terminal compute node 推入唯一 common-target supernode 的机会
- 将 schedule 写入 session，供 `grhsim-cpp` emit 使用

完整运行时术语和静态到运行时映射见
[GrhSIM Scheduling](../emit/grhsim-scheduling.md)。

## 路径语义

`-path` 的解析规则和其他 path-based pass 一致。

- 单段路径：直接按 graph 名选中目标 graph
- 多段路径：`<root>.<inst>...`

多段路径从 root graph 开始，逐层按实例 `instanceName` 查找，并通过实例的
`moduleName` 进入下一级 graph。

## 选项

`ActivityScheduleOptions` 定义在
[activity_schedule.hpp](../../include/transform/activity_schedule.hpp)。

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `-path` | 无 | 目标 graph / 实例路径，必填 |
| `-max-op-in-compute-supernode` | `128` | compute-node cluster coarsen 和连续分段的 op 数上限 |
| `-max-op-in-compute-node` | `8192` | 单个 compute node 吸收 op 的上限 |
| `-max-op-in-commit-supernode` | `4096` | commit cluster 打包上限；guard-event 模式下 atomic guard/ordered bucket 可超限，显式大于 4096 时顺序合并完整 4096-baseline cluster；关闭该模式时直接按请求值切分 |
| `-local-shared-compute-max-fanout` | `2` | local shared compute clone 的 distinct compute user op 上限 |
| `-local-shared-compute-max-width` | `64` | local shared compute clone 的 result value 宽度上限 |
| `-local-shared-compute-max-clones` | `4096` | local shared compute graph clone 硬上限 |
| `-local-shared-compute-max-cloned-op-ppm` | `5000` | clone 数占 baseline compute op 的 PPM 上限 |
| `-local-shared-compute-common-owner-policy` | `off` | third common-owner policy：`off/probe/strict` |
| `-local-shared-compute-common-owner-max-clones` | `4096` | strict common-owner graph clone 硬上限 |
| `-local-shared-compute-common-owner-max-cloned-op-ppm` | `5000` | strict common-owner clone 数占 baseline compute op 的 PPM 上限 |
| `-split-oversize-compute-node-max-ops` | `0` | 超大 compute node split 的 chunk 上限；为 0 时使用 `max-op-in-compute-supernode` |
| `-disable-coarsen` | `false` | 关闭 plain coarsen |
| `-disable-chain-merge` | `false` | 关闭 plain `out1` / `in1` chain merge；`siblings` 仍可执行 |
| `-enable-local-shared-compute` | `false` | 允许克隆局部共享 compute producer |
| `-disable-commit-guard-event-buckets` | `false` | 关闭 commit guard/event bucket 分组 |
| `-split-oversize-compute-nodes` | `false` | materialize 阶段拆分超过上限的单个 compute node |
| `-declared-value-compute-node-boundary` | `false` | 把带 declared symbol 的 value 作为 compute-node 截断边界 |
| `-final-terminal-pushforward-policy` | `off` | terminal common-target pushforward 策略：`off/probe/strict` |
| `-final-terminal-pushforward-max-node-ops` | `8` | 候选 terminal compute node 的最大 raw compute op 数 |
| `-final-terminal-pushforward-max-inputs` | `16` | 候选 cone 的最大 external input value 数 |
| `-final-terminal-pushforward-max-outputs` | `16` | 候选 cone 的最大 external output value 数 |
| `-final-terminal-pushforward-max-value-width` | `64` | 候选 external input/output logic value 的最大位宽 |
| `-final-terminal-pushforward-min-bae-gain` | `1` | 候选的最小 exact boundary activation edge 净收益 |
| `-final-terminal-pushforward-min-boundary-value-gain` | `1` | 候选的最小 exact boundary value 净收益 |
| `-final-terminal-pushforward-max-moves` | `128` | conflict-free projected move 数上限 |
| `-final-terminal-pushforward-max-moved-op-ppm` | `200` | selected move 涉及的 raw compute op 占最终 compute op 的 PPM 上限 |
| `-final-sibling-fusion-policy` | `off` | final compute sibling fusion 只读策略：`off/probe` |
| `-final-sibling-fusion-min-gain` | `4` | probe pair 的最小 exact compute BAE gain |
| `-final-sibling-fusion-max-pairs` | `256` | probe conflict-free projected pair 上限 |
| `-final-sibling-fusion-max-fused-op-ppm` | `5000` | selected pair 涉及的 raw compute op 占最终 compute op 的 PPM 上限 |
| `-kahn-level-pack-policy` | `off` | same-Kahn-level packing policy：`off/strict/bae-budget/balanced` |
| `-post-dp-refine-policy` | `off` | post-DP exact refinement policy：`off/strict/bae-budget/balanced/swap-probe` |
| `-export-compute-dag` | 无 | 导出 `wolvrix.compute-op-dag.v1` compute op DAG JSON |

## Plain 调度路径

当前主路径如下：

1. `buildActivityOpData(...)` 收集可调度 op，并按 operand def-use 建 op-level DAG。
2. `cloneSourceUsesForCompute(...)` 复制 source-class op 到 compute 用户侧；source-class
   value 到 commit 的 use 保留原始 value。
3. 如果发生 clone，pass 重新 `freeze()` 并重建 `ActivityOpData` / `opClasses`。
4. `buildComputeNodeRewrite(...)` 先按 sink event key / guard key 构造 `commitNodes`，
   再从 commit input、output/inout 根和无 result compute op 出发构造 `computeNodes`。
   guard-event 模式同时按实际 cap 不超过 4096 的 baseline cluster 给每个 sink op
   分配 graph-global commit locality group；更高 cap 只合并 execution cluster，不合并
   该 locality group。关闭 guard-event 模式时 locality group 等于直接 event chunk。
5. 若开启 local shared compute clone，先用该 rewrite 发现 bounded 候选；true-clone
   op/value 并替换远端 consumer uses 后，重新 `freeze()`，从头重建
   `ActivityOpData` / `opClasses` / compute rewrite。baseline rewrite 的 node/ID 不复用。
6. `buildComputeDag(...)` 基于 compute node 的 `boundaryInputs` 建 compute-node DAG；
   如果产生 cycle，会把相关多 op compute node 拆回 singleton 后重建。
7. `materializeComputeNodeSchedule(...)` 从每个 compute node 的 singleton cluster 开始，
   在 cluster DAG 上执行 plain coarsen。
8. 对 coarsen 后的 cluster topo 序列做连续分段；可选 Kahn-level packing 会从
   baseline 分段出发交换同 level slots、重建 view/value edges 并重跑同一 DP，
   可选 post-DP refinement 再做 bounded exact move/swap。
9. 分段上限为
   `max-op-in-compute-supernode`。
10. 展开 compute segment 为 `computeSupernode`，追加 commit node 形成
   `commitSupernode`，然后重建最终 `dag`、`value_fanout`、`topo_order` 和
   `state_read_supernodes`。

## Local shared compute true clone

local shared compute clone 只处理 allowlist 内的 cheap pure compute op：单个 Logic
result、两个 distinct compute user op 且分属两个 compute node，source op 已经 local
于其中一个 consumer。clone 目标 node 必须非 intent/indivisible，且 result 宽度、node
容量、clone count 和 compute-op PPM 都在显式预算内。

source op 的每个 operand 还必须已经是目标 node 的 local value 或既有 boundary
input。这个约束保证删除 shared result boundary 时不会引入新的 operand boundary。
结果不能是 declared/output/inout，也不能有非 compute user 或 side effect/intent
attribute。clone 使用新 op/value，保留原 op/value，不写入 source-clone 使用的
canonical value map。

所有候选在 graph mutation 前先复制 kind、operands、attrs、source location 和 result
metadata；apply 后完整 refreeze/rebuild，并校验 commit partition、intent groups、owner
locality、cycle splitting 和 compute-node cap。`false` 路径不执行发现或二次 rebuild。

`-local-shared-compute-common-owner-policy=probe` 在 baseline compute rewrite 上只读扫描
source owner 位于第三个 common-expression node 的候选，细分 user/node owner、singleton、
intent/indivisible、双边 operand locality、双边 capacity 和 projected removed pairs。它不创建
op/value、不替换 operand、不重新 freeze，也不增加 session key 或 summary stats 字段。

`strict` 复用相同 exact probe gate，并要求 `enable-local-shared-compute=true`、普通
local-owner `max-clones=0`，以隔离两类 candidate。它按 op cost、operand bits、result
width 和 topo/op id 稳定排序，在独立 hard/PPM budget 内处理两个 target 的累计 cap、
source/target node role 与 source/user op role，再一次性 snapshot 和 apply。较早 consumer
保留 original，较晚 consumer 改用 clone；apply 后完整 refreeze/rebuild并验证 graph
增量、metadata、exact users、owner locality、commit/intent/cycle split/cap/topology。candidate
rebuild 会先验证当前 sink 全集、每个 commit node 的 ordered inputs，以及按 event/cap 得到的
normalized partition 都与 baseline 相容，再复用 baseline commit node 的 exact op/input order；
最终 exact commit validator 保持不变，避免 compute clone 顺带改变 commit code layout。

plain coarsen 每轮按以下顺序尝试合并：

- `out1`：producer 只有一个 compute successor
- `in1`：consumer 只有一个 compute predecessor
- `siblings`：拥有相同 predecessor 集合的 sibling clusters

所有合并都受 `max-op-in-compute-supernode` 约束，并在批量合并后重新做 topo check。
`out1` / `in1` 受 `enableChainMerge` 控制；`siblings` 属于 plain coarsen 基础路径。

连续分段只决定 topo 序列中相邻 cluster 如何切成 compute supernode，不做任意 DAG
partition。分段成本是：

```text
incoming_boundary_activation_edges + 1
```

同成本时偏向更长 segment。

## Final terminal common-target pushforward

`final-terminal-pushforward-policy=probe` 在 final schedule 上只读寻找完整的小型
terminal compute node/cone `C`：`C` 当前位于 source compute supernode `S`，它的所有
external output value 都只被同一个 target compute supernode `T` 使用。候选的所有
external input value 必须由 `S` 中不属于 `C` 的 remaining op 定义；move 后 `S`、`T`
都必须非空，且 `T + C` 不能超过最终 compute supernode op cap。`C` 自身不能包含
state/memory/event/intent、side-effect、declared/port result 或不满足纯组合 allowlist 的
op；external input 的 producer 可以是其它类别，但必须仍由 `S` 中的 remaining op 定义。

probe 对每个候选按最终 schedule 的 value fanout 机械重算两项净收益：

```text
net BAE = removed output activation edges - newly introduced input activation edges
net boundary values = outputs losing all external fanout - inputs becoming external
net logical bytes = removed boundary ceil(width/8) - added boundary ceil(width/8)
```

两项净收益分别受 `min-bae-gain` 和 `min-boundary-value-gain` gate；node op、input/output
数量与 logic width 也受各自显式上限约束。通过结构 gate 的候选按稳定顺序选择，并受
move 数、moved-op PPM 和 touched-supernode conflict 预算约束。logical bytes 是 value-width
proxy，不代表 emitter slot、alignment 或 ELF bytes。

`probe` 不修改 graph、schedule、active ID、session payload 或 emitter layout；它只输出
候选/reject funnel、projected gain 和预算统计。`strict` 在排序、conflict 和预算选择前，
先过滤掉 `added BAE / boundary values / logical bytes` 任一非 `0` 的候选，即只接受 `C`
的 input 在 baseline 已经直接激活 `T` 的 zero-add 候选。strict 从 `S` 稳定过滤 `C`，
再把原 op 顺序不变地插到 `T` 的最早 external-output consumer 前；两个 supernode 的
其它 op 相对顺序保持不变。

strict 只支持 `final-topo-policy=level-id` 和未发生 oversize split 的 schedule，也不能与
`final-fanin-pullback-policy=strict` 同时启用。重建 derived schedule 后，supernode/kind/op
partition、capacity、commit、stable-splice 精确 op/node vector、DAG/topo、state-read、
compute-commit、value fanout/source 必须逐项闭合；actual compute/total BAE、
boundary-value 和 logical-byte gain 必须与 probe 投影完全一致。C++ native default 仍为
`off`，结构收益仍须由 SimTop walltime 裁决。

`post-dp-refine-policy=swap-probe` 只读枚举因目标 segment 满载而受阻的 equal-load
cluster swap。候选必须保持 pair topology 和 exact quotient DAG support key，且降低 exact
compute BAE；预算内的无冲突候选只应用到内部副本以复算指标和验证约束，不修改导出的
activity schedule。

`final-sibling-fusion-policy=probe` 在 final schedule 上构造每个 compute supernode 的
完整 value/input activation signature：一部分是 `value_fanout` 反向得到的 schedule
incoming `ValueId` 有序集合，另一部分是直接 operand 中的 graph input/inout `ValueId`
有序集合。只有处于同一 `level-id` Kahn level、两部分 signature 都完全相同、且两个
supernode 的 raw op 总数不超过 `max-op-in-compute-supernode` 的节点才可能进入 exact
pair 空间。graph input signature 只用于证明激活等价；projected compute BAE gain 仍只
等于共同 schedule incoming `ValueId` 的数量，空 schedule incoming signature 不进入候选。

首版 probe 保守拒绝任何包含 state-read/reg-to-mem-intent、event-sensitive side effect
或未分类无 def operand 的 compute supernode。只比较相同 state symbol 不足以证明当前
emitter 下同时执行：native direct-state 优化会把激活从 read source 重写到 consumer，
memory reader 还可能按 constant row 细化；同理 pure-event bypass 会区分 posedge/negedge
predicate。probe 分别报告 `state_activation_nodes`、`event_activation_nodes` 和
`unclassified_activation_nodes`，不把这些节点作为 activation-equivalent 候选。

同一 signature 桶内的可行 pair 按
`(topoDistance, storageDistance, min(supernodeId), max(supernodeId))` 升序贪心选择，
已配对节点不再参与后续 pair。实现只保留每个节点当前最近的可行 topo 后继，并通过
lazy priority queue 和 active-op minimum tree 更新冲突项，避免物化桶内的二次方 pair
集合。形成的无冲突 pair 再按 gain 降序、canonical signature 桶顺序和桶内 pair 顺序
应用 pair 数与 fused-op PPM 预算。日志区分所有组合的 `raw_exact_pairs`、capacity 组合
上界 `cap_eligible_pairs`、因 pair overlap 未入选的 `rejected_overlap_pairs`、桶内最近
距离配对后的 `exact_eligible` 以及最终 `selected`，并给出 gain/op/distance 分布。
每个 selected pair 另行输出 `lhs/rhs` supernode、对应 baseline active ID、gain、op 数和
topo/storage distance，供 runtime fire profile 与 active-mask packing 离线复核；这些
逐 pair 日志不写入 session。

pair 的 combined raw-op cap 直接使用 `max-op-in-compute-supernode`，与当前 schedule 的
合法 supernode 上限保持一致；它不是独立的固定常量。

probe 还单独扫描 storage 相邻且 topo 相邻的 exact pair，报告
`adjacent_cap_eligible` 和逐层拒绝原因。plain DP 对正 segment penalty 的最优分段通常
使该集合为空；该计数独立于实验 `min-gain`，只用于验证结构性质，不裁剪主集合。
当前没有 `strict` 策略，probe 不修改 graph、schedule/session key 或 summary stats。
后续若实现非相邻融合，需要先
解决 active-ID、batch/code layout 与 value-slot locality 稳定性，不能直接把 projected
pair 应用到导出 schedule。

## Session 输出

对 `path=<target>`，pass 写入以下 key：

- `<target>.activity_schedule.supernode_to_ops`
- `<target>.activity_schedule.op_to_supernode`
- `<target>.activity_schedule.commit_locality_group_by_op`
- `<target>.activity_schedule.commit_locality_group_order`
- `<target>.activity_schedule.dag`
- `<target>.activity_schedule.supernode_kind`
- `<target>.activity_schedule.compute_nodes_by_supernode`
- `<target>.activity_schedule.value_fanout`
- `<target>.activity_schedule.topo_order`
- `<target>.activity_schedule.state_read_supernodes`
- `<target>.activity_schedule.summary_stats`

`summary_stats` 是 plain schedule 的结构统计 JSON，只包含当前调度结构统计字段。

`commit_locality_group_by_op` 是按 `op.index - 1` 索引的 `uint32_t` vector。
非 commit op 使用 invalid ID。默认 4096 cap 下每个 group 与实际 commit supernode
一一对应；guard-event high-cap schedule 合并完整 baseline commit node 时，组 ID 仍保持
pre-merge 4096 边界，供 emitter 稳定 value-slot locality。fixed partition 或 graph clone
rebuild 始终从当前 graph 的 sink partition 重建该映射，不复用旧 OperationId。

`commit_locality_group_order` 是 canonical locality group ID 的排列。`level-id`
策略在最终 materialize 后构造 `compute supernode + canonical commit group` 伪
baseline DAG：compute-to-compute 边沿用最终 DAG，每个 group 的 incoming 边则从
组内 sink operand 的 defining compute supernode 重新建立，再复用 level-id topo。
因此 high-cap execution merge 不会改变该 group 顺序。group ID 对应 pre-merge
baseline commit node ID 顺序。

当前 canonical order 只定义于 `final_topo_policy=level-id`。`level-op` 和
`ready-op` 仍导出该 key，但值为空；emitter 会把空 order 视为不完整 metadata，
对整张 value-slot 排列表使用 legacy 路径。

## Compute DAG 导出

`-export-compute-dag=<path>` 在 `buildComputeNodeRewrite(...)` 之后、最终 materialize
之前导出 compute 侧 DAG。导出文件是 `topo-graph-partition-harness` 的输入协议，
不暴露任何旧策略的内部权重模型。

导出语义：

- JSON `format` 固定为 `wolvrix.compute-op-dag.v1`
- `options.node_granularity` 固定为 `op`
- node 对应一个 compute op
- `op_id` 记录原始 GRH operation id
- `topo_pos` 是导出时重新排序后的 op-level topo 位置
- edge 对应从 `src` op result value 到 `dst` op operand 的依赖
- `options.edge_weight` 固定为 `value_bitwidth_words`
- `edges[].values[]` 记录该 op pair 上的 distinct value，每个 value 只带 `id` 和 `width`
- `edges[].weight = ceil(sum(edges[].values[].width) / 64)`，最小为 1
- 不导出 commit node / sink op
