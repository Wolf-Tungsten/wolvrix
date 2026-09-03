# repcut

## 功能概述

`repcut` pass 对指定路径对应的单个 graph 执行 RepCut 超图分区，并将该 graph 重建为一个 wrapper graph，内部实例化若干个分区子图。

该 pass 只作用于一个 graph，不会递归分区其内部实例指向的子图。

## 路径语义

`repcut` 使用 `-path` 指定目标。

支持两种形式：

- 单段路径：直接按 graph 名选中目标 graph
- 多段路径：`<root>.<inst>...`

多段路径解析规则：

1. 第 1 段是 root graph 名
2. 从第 2 段开始，每一段都按当前 graph 中实例的 `instanceName` 匹配
3. 找到该实例后，读取它的 `moduleName` 跳转到下一级 graph
4. 最终得到被分区的目标 graph

例如：

- `SimTop.logic_part` 表示从 `SimTop` 中，沿实例 `logic_part` 进入其 `moduleName`

## 拆分后的命名规则

假设 `repcut` 最终作用到 graph `SimTop_logic_part`，并分成 3 个分区，则会创建：

- `SimTop_logic_part_repcut_part0`
- `SimTop_logic_part_repcut_part1`
- `SimTop_logic_part_repcut_part2`

如果名称已被占用，会自动追加后缀，如 `_1`。

同时，原 graph 名 `SimTop_logic_part` 会被重建为 wrapper graph。该 wrapper 内部的实例名是：

- `part_0`
- `part_1`
- `part_2`

对应的 `moduleName` 分别是各个 `*_repcut_part*` graph。

## 行为概述

`repcut` 会：

1. 对目标 graph 建立 RepCut 分区输入
2. 根据写端口逻辑锥、读端口使用关系、storage 归属等信息进行切分
3. 为每个分区生成一个新的 graph
4. 删除原 graph
5. 用同名 wrapper graph 重建该 graph
6. 在 wrapper 中实例化各分区 graph，并补齐跨分区连接

这意味着“外部看到的模块名不变，内部结构变成 wrapper + part graphs”。

## 主要选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `-path` | 无 | 目标路径，必填 |
| `-partition-count` | `2` | 分区数，必须 `>= 2` |
| `-imbalance-factor` | `0.015` | 分区不平衡因子 |
| `-work-dir` | `.` | 中间文件输出目录 |
| `-partitioner` | `mt-kahypar` | 分区后端 |
| `-mtkahypar-preset` | `deterministic-quality` | mt-kahypar 预设；`repcut` 会强制使用确定性 preset |
| `-mtkahypar-threads` | `0` | 线程数，`0` 表示后端默认 |
| `-weight-mode` | `baseline` | 求解器 vertex weight 模式；严格支持 `baseline` 或 `closure-aware` |
| `-keep-intermediate-files` | false | 是否保留中间文件 |

`baseline` 保留原有 RepCut vertex weight 口径。`closure-aware` 包含三步：

1. singleton piece 的计算权重全额计入其 ASC，shared piece 在关联 ASC 间做守恒、确定性分摊；
2. shared piece 的计算权重加入 KM1 edge，显式计入跨分区重复物化成本；
3. Mt-KaHyPar 返回后，按 exact ASC-piece occupancy 对最大 closure-load 分区做有界、确定性的 whole-ASC
   refine。refine 不移动 full closure 超过 nominal target 的 overweight ASC，也不允许向其 owner 分区增加正边际负载；
   总 closure load 和 communication proxy KM1 的回退预算均为 1%。

refine 的接受顺序是：先比较按降序排列的完整 per-part exact load vector，再比较总 closure load，
最后比较 communication proxy KM1。搜索最多执行 4 轮、接受 64 次移动，并在每个 source 分区中只枚举
`sourceLoss` 最大的 128 个 ASC；因此它是有界的确定性启发式，不是全局最优证明。

overweight owner 的规则是 post-refine guard，不是求解器级的独占约束：raw partition 中已经落在该 owner
上的普通 ASC 可能保留。stats 中的 before/after 精确负载用于审计实际结果，不能仅凭 `overweight=true`
声称该分区只含一个 ASC。

该模式不会拆分 ASC，也不会改变 storage 归属或 graph 重建语义。所谓 exact load 仅覆盖 ASC 引用的
piece closure；Phase E 另行归属的 storage/source 和重建 adapter 继续通过 `estimated_node_weight_sum`、
最终 graph ops 等指标观测。

启用 `-keep-intermediate-files` 时，solver 原始分区保存在常规 `.hgr.partK` 文件，refine 后实际用于
重建的分区另存为 `.hgr.closure-aware.partK`。最终 info stats 会记录 `closure_weight_stats`、
`partition_refine_stats`，以及每个 part 的 `exact_referenced_closure_weight`。

## 示例

### 示例 1：直接按 graph 名分区

```bash
wolvrix --pass=repcut:-path=top_logic_part:-partition-count=8 input.json
```

这是单段路径，用 graph 名直接选择目标。

### 示例 2：按实例路径分区

```bash
wolvrix --pass=repcut:-path=SimTop.logic_part:-partition-count=32 input.json
```

这是按实例路径选择目标 graph 的形式。

### 示例 3：比较两种权重模式

```bash
wolvrix --pass=repcut:-path=SimTop:-partition-count=32:-weight-mode=closure-aware input.json
```

Python binding 使用对应的 `weight_mode` 命名参数：

```python
session.run_pass(
    "repcut",
    design="design.main",
    path="SimTop",
    partition_count=32,
    weight_mode="closure-aware",
)
```

XiangShan Make 流程通过同名实验变量选择模式：

```bash
make build_xs_repcut_verilator XS_REPCUT_WEIGHT_MODE=closure-aware
```

未设置 `XS_REPCUT_WEIGHT_MODE` 时使用 `baseline`。

## 前置条件

- 推荐在 `xmr-resolve` 之后运行
- 当前实现要求 graph 中已经没有未处理的 `kSystemTask` / `kDpicCall`

## 注意事项

- 多段路径按实例名解析，不按模块名解析
- `-weight-mode` token 区分大小写，不接受 `candidate`、`closure_aware` 等别名
- `-mtkahypar-preset=quality` / `highest-quality` 会映射到 `deterministic-quality`，`default` 会映射到 `deterministic`；`large-k` 不再适用于 `repcut`
- 该 pass 只分区目标 graph 一层，不会继续递归分区其内部实例
- 若目标 graph 原来是 top graph，重建后会重新标记为 top；否则不会改变 top 集合
- 分区数上限当前为 `4096`
- `closure-aware` 的 exact post-refine 当前只在 `partition-count <= 64` 时启用；更大的 K 仍使用
  closure-aware HGR 权重，但 stats 会记录 refiner 的 skip reason
- dense post-refine 还受 512 MiB 估算工作集上限保护；超过上限时保留 solver raw partition，并在 stats
  中记录 skip reason
- 重建后的 wrapper graph 会保留原 graph 名，因此从父层进入该 graph 的既有实例路径不需要改写
