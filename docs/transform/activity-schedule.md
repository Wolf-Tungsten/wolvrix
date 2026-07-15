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
| `-max-op-in-commit-supernode` | `4096` | 单个 commit supernode 最多包含的 sink op 数 |
| `-local-shared-compute-max-fanout` | `2` | local shared compute clone 的 distinct compute user op 上限 |
| `-local-shared-compute-max-width` | `64` | local shared compute clone 的 result value 宽度上限 |
| `-local-shared-compute-max-clones` | `4096` | local shared compute graph clone 硬上限 |
| `-local-shared-compute-max-cloned-op-ppm` | `5000` | clone 数占 baseline compute op 的 PPM 上限 |
| `-local-shared-compute-common-owner-policy` | `off` | third common-owner opportunity policy：`off/probe`；`probe` 只输出 info log |
| `-split-oversize-compute-node-max-ops` | `0` | 超大 compute node split 的 chunk 上限；为 0 时使用 `max-op-in-compute-supernode` |
| `-disable-coarsen` | `false` | 关闭 plain coarsen |
| `-disable-chain-merge` | `false` | 关闭 plain `out1` / `in1` chain merge；`siblings` 仍可执行 |
| `-enable-local-shared-compute` | `false` | 允许克隆局部共享 compute producer |
| `-disable-commit-guard-event-buckets` | `false` | 关闭 commit guard/event bucket 分组 |
| `-split-oversize-compute-nodes` | `false` | materialize 阶段拆分超过上限的单个 compute node |
| `-declared-value-compute-node-boundary` | `false` | 把带 declared symbol 的 value 作为 compute-node 截断边界 |
| `-kahn-level-pack-policy` | `off` | same-Kahn-level packing policy：`off/strict/bae-budget/balanced` |
| `-post-dp-refine-policy` | `off` | post-DP exact refinement policy：`off/strict/bae-budget/balanced` |
| `-export-compute-dag` | 无 | 导出 `wolvrix.compute-op-dag.v1` compute op DAG JSON |

## Plain 调度路径

当前主路径如下：

1. `buildActivityOpData(...)` 收集可调度 op，并按 operand def-use 建 op-level DAG。
2. `cloneSourceUsesForCompute(...)` 复制 source-class op 到 compute 用户侧；source-class
   value 到 commit 的 use 保留原始 value。
3. 如果发生 clone，pass 重新 `freeze()` 并重建 `ActivityOpData` / `opClasses`。
4. `buildComputeNodeRewrite(...)` 先按 sink event key / guard key 构造 `commitNodes`，
   再从 commit input、output/inout 根和无 result compute op 出发构造 `computeNodes`。
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

## Session 输出

对 `path=<target>`，pass 写入以下 key：

- `<target>.activity_schedule.supernode_to_ops`
- `<target>.activity_schedule.op_to_supernode`
- `<target>.activity_schedule.dag`
- `<target>.activity_schedule.supernode_kind`
- `<target>.activity_schedule.compute_nodes_by_supernode`
- `<target>.activity_schedule.value_fanout`
- `<target>.activity_schedule.topo_order`
- `<target>.activity_schedule.state_read_supernodes`
- `<target>.activity_schedule.summary_stats`

`summary_stats` 是 plain schedule 的结构统计 JSON，只包含当前调度结构统计字段。

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
