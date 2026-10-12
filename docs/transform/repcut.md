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
| `-cost-model` | `legacy` | 操作计算成本模型；支持 `legacy`、实验性 `verilator-v1` 和非默认 `verilator-lowered-v1` |
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

## 后端计算成本模型

`cost-model` 与 `weight-mode` 正交：前者给单个 GRH 操作计价，后者决定这些成本如何分配到
ASC（不可拆分的原子逻辑锥）、共享 piece（逻辑片段）和分区。两者的四种组合均支持。
操作成本贯穿 HGR 顶点权重、共享 piece 复制惩罚、exact closure refinement 和分区静态统计。
通信 proxy 公式和 ASC/storage 共置规则不变；closure-aware 边权含共享计算成本，因此其数值会随模型改变。

`legacy` 保留历史位宽/扇出规则，仍是默认。`verilator-v1` 根据当前 Verilator 的 scalar/wide
表示和 helper 实现给出相对工作量：它不是周期数或生成 C++ 字节数，尚未完成跨设计或 walltime 验证。
模型不随 emitter 自动切换，也不使用香山分区耗时拟合。未覆盖的操作显式回退 legacy。

`verilator-lowered-v1` 是更激进但仍然静态的实验模型：它额外识别当前 SV emitter
已经实现的常量左移和连续 constant-mask memory/register write lowering，分别按固定
shift/part-select 近似计价。它不需要运行 Verilator，也不是完整 C++/机器码成本模型；
默认不启用，必须显式传入，验证无收益时可以直接移除而不影响 `legacy` 和
`verilator-v1`。

记 `L(W)=1`（`W<=64`），否则为 `ceil(W/32)`；`O` 为最大结果位宽，`I` 为最大操作数位宽。
首版主要规则如下，系数是机制启发式，不是已标定硬件常数：

| 操作 | 相对成本 |
| --- | --- |
| add/sub、shift | `2*L(O)` |
| bitwise、assign、replicate | `L(O)` |
| AND/OR/NAND/NOR reduction、逻辑非 | `L(I)` |
| XOR/XNOR reduction、比较 | `2*L(I)` |
| 逻辑与/或 | `1 + sum(L(operand width))` |
| mux / concat | `1+L(O)` / `operand count+L(O)` |
| 静态 / 动态切片 | `L(O)` / `2+2*L(O)`；array slice 额外 `+1` |
| write，全零 / 全一常量 mask | `1` / `1+L(data width)` |
| memory write，部分常量 / 动态 mask | `1+mask ones` / `1+2*data width` |
| register/latch write，其他 mask | 回退 legacy，等待后端合并行为验证 |

写口操作数必须按类型解释：register write 是 `[enable, data, mask, events...]`，
latch write 是 `[enable, data, mask]`（无 event），
memory write 是 `[enable, address, data, mask, events...]`。
例如 memory write 的 `[1-bit en, 8-bit addr, 256-bit data, 256-bit mask]` 使用 256-bit data 计价，
不会误将 8-bit address 当作 data。动态 memory mask 的逐 bit 成本来自当前 SV emitter，
不预测运行时全一快速路径的命中率；窄动态切片按输出宽度计价，不按整个宽输入扫描。

mul/div/mod、memory read、storage、常量和 effects 等未覆盖类型保留 legacy 成本。
新规则不再叠加历史扇出系数，但 fallback 仍保留旧公式。新规则的单操作成本饱和到 `UINT32_MAX-1`，
新模型的 piece 总和溢出时明确报错；legacy 保持历史累加行为。不同模型的绝对权重不能直接比较。
常量 mask 使用 sized literal 解析，另支持无尺寸 `'0`；`'1`、未知位或无法解析的 mask 暂按动态规则保守计价。

最终 stats 的 `cost_model` 标记选用模型；各分区的 `cost_model_legacy_fallback_op_count`
统计其原始节点集中使用回退计价的操作数（不是动态执行次数，也不含重建后新增的 adapter）。
`estimated_node_weight_sum` 和 `exact_referenced_closure_weight` 使用相同选定模型，但节点范围不同。

扩展其他仿真器时，在 `repcut_cost_model.hpp/.cpp` 添加独立、有版本的模型及对应选项校验，
复用 `estimateRepcutOperationCost` 接口与现有汇总流程；不要把新后端映射成 Verilator 别名。
新增模型需独立算子 fixture、默认兼容性和跨设计验证。当前不接受 `gsim`/`grhsim`，避免误称已支持。

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

通用 RepCut pass 未设置 `weight_mode` 时仍使用 `baseline`；当前 XiangShan Make
流程将 `XS_REPCUT_WEIGHT_MODE` 默认覆盖为 `closure-aware`。

### 示例 4：显式选择实验性 Verilator 成本

```python
session.run_pass(
    "repcut", design="design.main", path="SimTop", partition_count=32,
    weight_mode="closure-aware", cost_model="verilator-v1",
)
```

```bash
make build_xs_repcut_verilator XS_REPCUT_WEIGHT_MODE=closure-aware XS_REPCUT_COST_MODEL=verilator-v1
```

CLI 对应 `-cost-model=verilator-v1`。Python 省略 `cost_model` 或传 `None` 时不发送该选项，
沿用 C++ 默认 `legacy`；当前 XiangShan Make 流程将 `XS_REPCUT_COST_MODEL` 默认覆盖为
`verilator-v1`。

当前 XiangShan 主线脚本在 RepCut 完成后默认继续执行全分区的
`memory-write-coalesce`，以及带 Verilator 成本门槛的
`reg-to-mem(intent=false, profitability_filter=verilator-v1)`。这些是流程级默认值，
不改变 `RegToMemOptions::profitabilityFilter` 在通用库/API 中的默认 `None`；可分别通过
`XS_REPCUT_MEMORY_WRITE_COALESCE=0`、`XS_REPCUT_REG_TO_MEM=0` 或
`XS_REPCUT_REG_TO_MEM_PROFITABILITY_FILTER=none` 关闭/回退。

评估新模型时显式使用：

```bash
make build_xs_repcut_verilator \
  XS_REPCUT_WEIGHT_MODE=closure-aware \
  XS_REPCUT_COST_MODEL=verilator-lowered-v1
```

## Verilator RepCut 运行时

`emit_verilator_repcut_package` 生成的仿真器使用 `XS_EMU_THREADS=N` 选择运行时并行度，设
RepCut 分区数为 `K`：

- `N=0` 或 `N=1`：Host 串行执行；
- `2 <= N < K`：保留 N 个后台 worker 和 condition variable 调度；
- `N=K>=2`：默认使用 Host 参与的 all-spin。Host 接管一个逻辑 lane，只创建 `N-1` 个后台
  worker，Host 和 worker 均永久轮询，因此总 runnable 实体数仍为 N。

`N=K` 默认 Host lane 为 `min(9, N-1)`；这保留了 K=32 XiangShan 实验使用的 lane 9，同时
也覆盖较小 K。以下环境变量只允许在 `N=K>=2` 时设置：

| 环境变量 | 默认值 | 说明 |
|------|--------|------|
| `WOLVI_REPCUT_HOST_LANE` | `min(9, N-1)` | Host 接管的逻辑 lane，必须小于 N |
| `WOLVI_REPCUT_HOST_SPIN_US` | `infinite` | `0..1000000` 微秒或 `infinite`；有限自旋后回退到 condition variable |
| `WOLVI_REPCUT_WORKER_SPIN_US` | `infinite` | `0` 或 `infinite` |

显式设置 `HOST_SPIN_US=0`、`WORKER_SPIN_US=0` 可保留 Host 参与但恢复全 CV 等待；设置
`HOST_SPIN_US=50`、`WORKER_SPIN_US=0` 可恢复 Host 先自旋 50 us、worker 使用 CV 的实验配置。
运行时会输出一行 `[runtime-config]` JSON，记录实际 Host lane、CPU 映射、等待策略及配置来源。

all-spin 假设进程 affinity 中至少有 N 个专用 CPU，并会持续占满这些 CPU；它以 walltime 为优化
目标，不以 task-clock 或功耗为目标。析构时 Host affinity 会恢复为构造仿真器前的完整掩码。

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
