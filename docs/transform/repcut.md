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
| `-cost-model` | `legacy` | 操作计算成本模型；支持 `legacy`、`verilator-v1`、`verilator-lowered-v1` 和实验性 `grhsim-v1` |
| `-initial-assignment` | `solver` | `solver` 保留原流程；`grhsim-terminal-v1` 从原图重新求解核心 ASC，再放置终端消费者 |
| `-initial-core-imbalance-factor` | `0.05` | 两阶段初解中核心求解的不平衡因子，须有限且非负；不改变参考求解的 `-imbalance-factor` |
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

## GrhSIM 两阶段初始分配

实验选项 `initial-assignment=grhsim-terminal-state-v1` 在下述终端 reader 规则上，
允许延后放置无反馈的寄存器 ASC。分类使用原始存储读写和 effect 返回值形成的 ASC
有向依赖图，先计算 SCC（强连通分量），再标记能到达环、memory、latch 或不支持的
sink 的节点。自保持的自环保守地归入核心；缺少状态元数据时不新增状态候选。
一个候选 ASC 必须全部由寄存器写入 sink 构成，且不能参与无写口 storage 的归属投票。
共享组合 piece、写入工作和状态通信仍按原成本完整计入。状态依赖现在从每个完整
ASC 的 sink 沿 value definition 反向回溯到实际的 register/latch/memory read，再
建立 source-state 到 sink-ASC 的边；普通 `values` 的直接 ASC reader 边仍只用于
通信和 evaluator 记账。

新增的状态链从下游端点往上游安排，每次只放置一个完整 ASC；就绪节点之间沿用原有
输入规模排序。它不合并寄存器链、不拆分 ASC，也不放宽下述结构预算。中间文件开启时，
`.hgr.state-regions.csv` 记录每个 ASC 的 SCC、sink 类型和分类结果。该回溯仍以完整
ASC 为分配单位，因而同一 ASC 内的多个结果可能继续形成保守耦合；结构账本必须结合
原始 sink 路径解释，不能仅按信号名字判断反馈。该选项保持实验性，默认仍为 `solver`；
仅结构预算通过不代表功能或性能验收通过。

`initial-assignment=grhsim-terminal-v1` 要求 `cost-model=grhsim-v1`、
`weight-mode=closure-aware` 和实际求解器后端，不能与 `assignment-replay` 同时使用。
它保持 ASC 的语义边界、storage 共置和原有重建规则，不按模块名、信号名或分区编号匹配。

普通 solver/refine 先产生参考方案，只提供结构预算和失败回退。新核心的标签从原始超图
独立求解，不继承参考标签：所有写者、有状态写入的 ASC、参与无写口 storage 归属投票的
ASC 都留在核心。只有输入合计至少 128 个 64-bit word、且输入 word 数超过独立 recurring
工作量的终端 reader 才延后放置；recurring 是排除常量、声明和直接别名/状态读取后的
结构成本，不是实测运行次数。

核心求解完成后，按精确共享 piece 并集、writer/reader 集合与通信拼包成本，为每个终端
reader 初次选择分区，不移动已经分配的核心 ASC。最多尝试 6 个接收容量上限；通信拼包
可随后续 reader 的放置重组，所以局部只剪枝单调增加的计算量，完整结果再核验全部约束。
预算由当前输入的参考方案推导：最大 recurring 不增加，总 recurring、通信调用及字节量
最多增加 0.5%，总 operation 数及最大单端发送/接收调用、字节量最多增加 1%。这些量是
结构约束，不能当作 walltime 预测。

当前构造器支持最多 64 个分区。无合适 reader、核心 ASC 不足、模型不支持、核心求解失败
或预算不满足时，保留原 solver/refine 方案并报告原因；不输出部分分配。启用中间文件时，
参考标签另存为 `.hgr.reference.partK`，核心图及其标签保存在 `.hgr.initial-core.hgr*`，
最终采用的完整标签仍是 `.hgr.closure-aware.partK`。

stats 的 `initial_assignment` 记录实际是否采用、原因、reader 数、尝试数和通信容量。
`final_assignment_metrics`、`closure_weight_stats` 及各分区的
`exact_referenced_closure_weight` 对应最终标签；`partition_refine_stats` 的 before/after
仍记录参考方案的局部调整，并以 `assignment_scope=solver_reference` 标明。
未采用新构造器时，`assignment_scope=final`，统计语义沿用原流程。

Python 对应 `initial_assignment="grhsim-terminal-v1"` 和
`initial_core_imbalance_factor=0.05`。通用 API、香山脚本及 Make 的默认仍为 `solver`。
香山 GrhSIM 流程可显式启用：

```bash
make build_xs_repcut_grhsim XS_REPCUT_INITIAL_ASSIGNMENT=grhsim-terminal-v1
```

该选项同时将未显式指定的成本模型选为 `grhsim-v1`；不会更改生成后的优化 pass 或
GrhSIM emitter 参数。已验证性能线使用
`XS_REPCUT_REG_TO_MEM_PROFITABILITY_FILTER=verilator-v1`，而普通 GrhSIM 入口该选项默认
仍为 `none`，因此分配器的性能结论不能直接视为全部默认生成配置的验证。Verilator
脚本拒绝该初始分配选项，保留原默认分区及生成路径。

## 后端计算成本模型

`cost-model` 与 `weight-mode` 正交：前者给单个 GRH 操作计价，后者决定这些成本如何分配到
ASC（不可拆分的原子逻辑锥）、共享 piece（逻辑片段）和分区。各模型均支持两种 weight mode。
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
新增模型需独立算子 fixture、默认兼容性和跨设计验证。不接受无版本的 `gsim`/`grhsim` 别名。

### GrhSIM 静态模型 `grhsim-v1`

这是独立的、显式启用的模型，不复用或修改 Verilator 的计价公式。ASC 构造、
storage 共置、共享 closure 分摊及 solver/refinement 算法保持原样；只替换传入这些
步骤的操作成本。默认值不变，性能收益需要在目标设计上单独验证。

记 `G(W)=ceil(W/64)`。GrhSIM 使用原生标量及 `uint64_t` 数组；主要规则是：

| 操作 | 相对成本 |
| --- | --- |
| bitwise、assign、state read、静态切片 | `G(O)` |
| add/sub | 标量 `1`，宽值 carry chain `3*G(O)` |
| 比较 / XOR reduction | 标量比较 `1`；宽比较或 XOR reduction `2*G(I)` |
| AND/OR reduction、逻辑非 | `G(I)` |
| 逻辑与/或 | `1 + sum(G(width) for non-bool operands)` |
| mux / concat | `1+G(O)` / `G(O)+sum(G(operand width))` |
| replicate / 动态 shift | `2*G(O)`；常量 shift 为 `G(O)` |
| 动态切片 / memory read | `2+2*G(O)` / `2+G(O)` |
| state/memory write，全零 / 全一 mask | `1` / `2+3*G(data width)` |
| state/memory write，部分或动态 mask | `2+5*G(data width)` |
| mul / div/mod | 标量 `3` / `8`；宽值 `3*G(O)^2` / `64*G(O)^2` |

write 项近似覆盖暂存、变更比较、提交拷贝及激活记账，部分 mask 再增加逐字合并，
不使用 SV 的逐 bit 写入估算。storage 声明和常量保留最小正权重 `1`；DPI、system
effects 等外部成本仍显式回退 legacy。

上述基础算子成本另加中间值存储项：组合结果若被两个不同的操作使用，按可能的
supernode 边界增加 `2*G(result width)+1`，近似旧值比较、持久存储与变化标记；
若只有一个 consumer、但该 consumer 是 state/memory write，则增加 `G(result width)`，
表示跨 compute/commit 阶段保存数据。常量、assign 别名和直接 state read 不加此项。
同一操作在两个 operand 位置重复使用一个值，不会被误算成两个 consumer。
该项只依赖 GRH def-use，不运行 emitter/编译器，也不读取 profile。
多个 consumer 可能最终合并到同一 supernode，所以这是结构代理，不是对最终
materialized 值的精确计数；单 consumer 也可能因 batch 大小限制形成边界。

系数是机制启发式，不是机器周期标定；
它不预测运行时活跃频率、编译器融合、batch 分组、分支误预测或两沿负载差异，
也不根据模块名、信号名、分区编号或测得的耗时拟合。

Python 使用 `cost_model="grhsim-v1"`；香山 GrhSIM 流程显式设置
`XS_REPCUT_BACKEND=grhsim XS_REPCUT_COST_MODEL=grhsim-v1`。
Verilator 的默认配置与所有已有 cost model 的数值规则不变。

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

在 Host 和 worker 都永久轮询时，阶段派发清理 worker 异常槽不再获取逐 worker
互斥锁：首次派发已有线程启动握手，后续派发已 acquire 上一阶段的 `completedFast`，
随后 `workFast` 的 release/acquire 发布本阶段任务和异常槽复位。异常仍由完成握手
传回 Host，并保留停止、join 与重新初始化路径。CV 或混合等待策略保留原有锁路径。
当 worker 自旋而 Host 可能回退到 CV 时，派发还会在 mutex 下复位 `completed`，
避免上一阶段残留的完成标志使 Host 提前越过屏障。
此调度实现由 Verilator 和 GrhSIM 共用，不改变模型内部计算、跨分区复制或阶段屏障语义。
运行时会输出一行 `[runtime-config]` JSON，记录实际 Host lane、CPU 映射、等待策略及配置来源。

all-spin 假设进程 affinity 中至少有 N 个专用 CPU，并会持续占满这些 CPU；它以 walltime 为优化
目标，不以 task-clock 或功耗为目标。析构时 Host affinity 会恢复为构造仿真器前的完整掩码。

## GrhSIM RepCut 后端

同一 RepCut 分区设计也可调用
`Session.emit_grhsim_repcut_package(design=..., output=..., top=...)` 生成全 GrhSIM
仿真包。每个分区分别执行 activity schedule、生成独立 GrhSIM 模型；adapter 将
各模型 runtime header 隔离在独立编译单元，不在热路径加入虚函数或运行时后端判断。
端口直接引用各模型自身存储，宽端口采用 `std::array<uint64_t, ceil(width/64)>`。

两个后端共用上述 load / early eval / early update / normal eval / final update
阶段调度器、CPU 绑定、Host 参与及 all-spin/CV 策略；后端在生成时确定，不混用。
GrhSIM 包不依赖 Verilator，默认 `make` 构建 `libgrhsim_<top>.a`；
`grhsim_<top>.hpp` facade 提供公共端口和 `init()/eval()`，可以接入现有 GrhSIM 客户端。
`init()` 重置模型状态和计时统计；第一个 `eval()` 建立输入边沿基线，初始高电平不视为
上升沿。常规同步设计应先执行时钟低电平的 eval，再翻转时钟。

生成的 GrhSIM Makefile 保留编译器默认优化，不按 Clang 版本或目标架构自动追加
`-fno-slp-vectorize`；`GRHSIM_COMPILER_FLAGS` 默认为空。历史 SSE2 shuffle 和
bool change-reduction 的复现用例继续保留，编译器验证失败时直接报告，后续在 Clang
层定位和修复，不自动关闭 SLP。更换优化选项后须清理旧 PCH/对象并重新编译。
Verilator 的生成/编译路径不变。

当前边界：

- 输入必须是 RepCut 重建后的单层 wrapper 和可由 GrhSIM 发射的叶子分区；原设计不被修改。
- 固定宽度 logic 端口受支持；inout 不支持。顶层端口名在 C++ 中保留 sanitizer 后的名字，
  关键字和名称冲突会报错。分区内部端口使用私有规范化名字，因此不受该命名约束。
- 保留原有跨分区效果依赖检查；不支持 early→early 或 normal→early 的额外依赖链。
- 不支持分区 waveform 和 GrhSIM 模型内部 perf/profile；框架级分区/阶段计时及
  `WOLVI_REPCUT_TIMING_JSONL` 仍可使用。请求 `set_runtime_profile_enabled(true)` 明确报错。
- DPI 延续 GrhSIM 的原生 ABI。当前香山输入的标量 bit/整数/string 导入可接入；不能将
  任意 Verilator 的 packed-wide/open-array DPI stub 直接作为 GrhSIM stub。外部 DPI
  实现自身须满足已有的并发调用约束。
- 当前每个模型的文件句柄表独立，因此 package 明确拒绝
  `$fopen/$ferror/$fdisplay/$fwrite/$fflush/$fclose`，直到支持共享文件上下文；
  例外是 `$fdisplay/$fwrite` 使用直接 32 位常量标准流描述符
  `1` / `0x80000001`（stdout）或 `0x80000002`（stderr），它们不依赖私有句柄表。
  `$display/$write` 不受此限制，但不同分区输出的先后顺序不保证。

Playground 的 `XS_REPCUT_BACKEND` 默认为 `verilator`；
`make build_xs_repcut_sim XS_REPCUT_BACKEND=grhsim` 与
`make run_xs_repcut_sim XS_REPCUT_BACKEND=grhsim XS_EMU_THREADS=32` 选择 GrhSIM。
也可直接使用 `build_xs_repcut_grhsim` / `run_xs_repcut_grhsim`；产物位于独立的
`build/xs/repcut-grhsim/{package,work,partitioned-emu}`，不覆盖原 Verilator 产物。
GrhSIM 管线暂默认使用 `legacy` cost model 和 `none` reg-to-mem 收益筛选，避免把
Verilator 专用成本估计当作 GrhSIM 成本；现有 Verilator 默认值保持不变。

## GrhSIM 划分前事件场景分析

`analyzeRepcutGrhsimScenarios()`（`transform/repcut_grhsim_scenarios.hpp`）是只读的原始
GRH 分析接口，不需要先分区、生成 activity schedule 或发射 C++。调用方声明输入变化场景、
已知事件根的上升沿/下降沿场景，以及各写入场景经过 commit 后在哪些场景对后续计算可见。
场景是可重叠的 bit 集合，不是执行次数或实测概率。

例如，外部驱动每次 `eval()` 翻转一次时钟时，可以声明上升沿为 bit 0、下降沿为 bit 1，
并声明上升沿 commit 的状态在下一下降沿计算中可见。这样寄存器写入的事件 mask 与
寄存器读取所激活的组合计算 mask 不会被混为一谈。该可见性关系由调用方提供，接口
本身不假定单时钟、交替两沿或固定的时钟相位关系。

接口只穿透一位 `assign`/`not`/`logic-not` 时钟别名，并正确反转极性；其他派生事件、
未声明的输入和未声明的 commit 可见性保留为 `kRepcutUnknownScenario`。此最高位表示
不确定性，不是额外一个实际执行场景；只有未知位的操作也不能据此判定在已知场景中
不活跃。当前传播使用整个 value 的 may 集合，尚未建模位级条件、后端内联、分组共同
激活或动态数据门控，不能把其计数解释成精确的半周期执行量。

该分析目前供独立 GrhSIM 初解实验提取结构特征；尚未接入生产默认分配入口，也不改变
Verilator 权重、划分结果或生成流程。

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
