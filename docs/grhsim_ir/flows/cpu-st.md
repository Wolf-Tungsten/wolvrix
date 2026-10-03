# CPU 单线程活动度仿真 Flow

> **状态（M5d-7）**：生产管线是 `scripts/wolvrix_xs_grhsim_ir.py` 的 A/B/C 三段序列；
> C 段（CPU mapping）是本文第 2 节末尾 C1–C8 的八 pass 序列，在 B8 语义封板后只运行
> 一次；C8 的 TU 计划让 `cpu.st.emit-cpp` 输出规模受控的多翻译单元（并行编译）。旧两
> 阶段 mapping 管线（`cpu.st.split-phase` → … → `cpu.st.build-schedule`）、其六个旧
> schedule 消费者 pass 与 `cpu.st.split-phases` 已在 M5d-6 删除（旧 emit 实现在 M5b
> 删除）；本文 §2–§5 描述当前形态。

本文规定 `cpu.st.*` 的端到端流程：从已验证的 `GrhSimModel` 构建 CPU mapping，生成
C++ 模型，再验证多时钟行为与运行性能。`st` 表示单线程，不表示单时钟。

模型语义以 [Overview](../overview.md) 和 [Core Dialect](../dialects/core.md) 为准；
mapping 字段、默认参数和 emitter 约束以 [CPU 后端](../backends/cpu.md) 为准。
本文负责组织各阶段的输入、输出和验收，不另建一份 schema，也不保存逐次实验日志。

## 1. 范围与产物

当前路线已实现事件/主体/内存写/输出四相位（P_event/P_general/P_mem/P_output）、
活动度传播、数据布局、调度和 C++ emit。运行时使用一个 NUMA node、一个 CPU core；
调用方改变输入后调用 `eval()`，不要求存在名为 `clock` 的端口，也不把一次 `eval()`
等同于一个硬件时钟周期。

输入是包含 `I/O/S/F/G/Init` 的合法模型。`Init` 必须完整定义状态初值；GRH lowering 和
前置变换在进入本 flow 前完成。产物包括：

- 携带完整 `CpuBackendMapping` 的模型及可重新加载的 JSON checkpoint。
- C++ 模型头文件、共享 runtime、driver、初始化和 task 源文件，以及生成目录内的 Makefile。
- 与模型、mapping、编译参数和实际可执行文件对应的功能与性能验证记录。

当前 C++ emitter 的主要逻辑路径为 two-state；mapping 支持某种物理类型不等于 emitter
已支持其全部操作。four-state、字符串状态等未实现路径必须显式拒绝。
runtime profile、多线程和 fullpass 不属于本 flow 已验收的能力；FST 波形自 M5d-8 起
以 declared-symbols 模式提供（见 `grhsim_ir/backends/cpu.md` 的波形小节）。

## 2. 构建流水线

GRH 入口使用 `lower_grhsim(..., logic_domain="2-state")`。对没有 defining op、仍被引用的
logic value，lower 显式生成同位宽、同有符号属性的 `core.compute.constant` 零值 producer，
与 legacy 二态仿真的零初始化一致。例如关闭 `RANDOMIZE_REG_INIT` 时，
`reg [7:0] debug; assign y = {debug, data};` 中没有其他赋值的 `debug` 会成为
`debug = core.compute.constant(constValue="8'h0")`，concat 继续读取该 value。
此 constant 无 operand，唯一 result 是原 undriven value；`constValue` 保存其定宽零字面量。
外部 input 和 inout 输入侧由 `core.input.read` 提供 producer，不补零；没有引用的 detached
value 继续跳过。四态或非 logic 的 undriven value 仍报错，不默认为二态零。

lower 还把 GRH 图的 `declaredSymbols` 与 `generateGroups` 原样携带为模型的只读
metadata（见 [Overview](../overview.md) 第 3.4 节；`keep_declared_symbols=False` 可关闭，
与 `keep_origins` 独立），并为每个能解析到存活 value/state/function 的声明建立
`declProvenance` 记录（direct 全覆盖 slice，数组声明保留形状；见 Overview 第 3.4.1 节）。
名字清单是来源注解，不参与仿真语义，cpu.st 各 pass 对它们没有维护义务；
`declProvenance` 持有实体下标，`compact()` 会重映射并丢弃已删除目标的 slice，
改写被声明实体的 pass 负责在删除前重定向。checkpoint 往返对两者保持不变。
生产流程（XS/HDLBits 入口脚本）一律显式启用 `keep_origins` 与
`keep_declared_symbols`，不得以关闭声明保留换取优化。

XiangShan 入口在 lower 成功后先执行全图优化段（M5d-3 起的目标 A 段顺序）：
`grhsim.canonicalize-compute`（归一化）、`grhsim.reg-to-mem`（恢复标量化表/数组）、
[`grhsim.comb-pack`](../passes/comb-pack.md)（全图同构组合 lane 打包）、
[`grhsim.pack-bit-registers`](../passes/pack-bit-registers.md)（bit 寄存器打包，改用原始
event_edges 与读写安全分析），然后 `grhsim.simplify(scope=whole)` 全图不动点化简，
最后 [`grhsim.select-state-stores`](../passes/select-state-stores.md)（M5d-4，A7）
完成语义存储分类——每个状态获得 `regLatch`/`mem` 归属与对应 NBA 提交契约（见
[Overview](../overview.md) 第 3.5 节）；这些 pass 不修改 GRH。

随后是分区段 B（M5d-5 起接线，仍为纯语义层，不建立任何 CPU mapping）：
`grhsim.classify-event-inputs`（B1）、`grhsim.lower-edge-detect`（B2）、
`grhsim.extract-output-cones`（B3）、`grhsim.migrate-timeslot-tasks`（B4）完成事件/输出/
time-slot 结构降级；[`grhsim.split-phases`](../passes/grhsim-split-phases.md)（B5）按
A7 分类完成类感知相位归属（`mem` 类数组的写归 P_mem，`regLatch` 类状态的写——含小数组
上的 mem op——归 P_general），`grhsim.simplify(scope=phase)`（B6）逐分区独立化简
（保留分区接口与副作用根、禁止跨阶段 CSE、共享状态位需求取全分区并集），
[`grhsim.clone-shared-compute`](../passes/clone-shared-compute.md)（B7）按边界感知成本
模型只克隆能消除预测超节点边界的共享计算（归位决议 1），最后
`grhsim.verify --seal semantic`（B8）封板语义层（总相位归属、无 event_edges 残留、
P_mem 操作数产自 P_general）。**B8 之后不再有任何语义改写**：旧管线的"后置化简 +
第二轮 mapping"往返段已拆除，CPU mapping（下表 C1–C8 八个 `cpu.st.*` pass）在封板
模型上只运行一次，单向推进、不回改语义。
集成开关新增 `XS_WOLF_GRHSIM_IR_PHASE_SIMPLIFY=0/1`（B6 调试开关）；`--used-bits` 与
`--bitwise-predicates` 旋钮随独立调用段的移除而成为空操作（两者都只在
`grhsim.simplify` 内部固定点中运行）。

既有化简子 pass 经统一入口 [`grhsim.simplify`](../passes/simplify.md) 以固定子序列
迭代至不动点（A6 全图 + B6 分区），支持 whole/phase 两种 scope。子序列包含
[`grhsim.const-fold`](../passes/const-fold.md)、`grhsim.canonicalize-compute`、
`grhsim.bitwise-predicates`、[`grhsim.bitwise-muxes`](../passes/bitwise-muxes.md)、
[`grhsim.mux-chain-fold`](../passes/mux-chain-fold.md) 与
[`grhsim.used-bits`](../passes/used-bits.md)（位需求分析、窄化及死锥清理）；各子 pass
的匹配与拒绝规则见其各自文档。phase scope 下 canonicalize-compute 不删除结果被分区外
op 消费的 op（越界消费者永不重接，删除会使其悬空），mem 写参数等分区接口值由此保持
完整。

`grhsim.canonicalize-compute` 删除同完整 TypeId 的两态 logic 赋值链并重接所有
消费者。`core.compute.assign` 唯一 operand 是源值，唯一 result 是赋值结果；
op 不得带 objectRefs 或 parameters。例如：

```text
x:u8 -> assign y:u8 -> assign z:u8 -> regWrite(enable, z, mask, clk)
                                 => regWrite(enable, x, mask, clk)
```

`regWrite` 的 operands 依次为 enable、写入数据、位掩码和事件值。重写只将数据
从 z 接回 x；原 history 对象、初始化和事件边沿参数不变。若赋值连接不同位宽、
signedness 或 logic domain，则保留转换；string、real、四态值也不消除。同类型
宽 logic 可消除，因为不存在截断或扩展。赋值环保持，不为环构造常量或任意根。
链解析不依赖 operation 的存储顺序，留下的操作顺序、名称、origin、对象和参数
保持，删除后重建 dense ID。该 pass 为 SemanticTransform，修改会使既有 mapping
失效；重新建立的依赖与状态 alias 证明仍保证 commit 读取提交前快照。再次运行
无变化时保持 revision 不变。

该 pass 还按拓扑次序共享精确相同的两态 logic `core.compute.*` 纯计算，键包含
op、结果 TypeId、归一化后的 operands 及完整 parameters。例如两条
`add(a,b) → xor(result,b)` 链可共用一条；`sub(a,b)` 与 `sub(b,a)` 保持独立，
不同 sliceStart/sliceEnd 的切片保持独立。只编码整数、bool、string 参数，其他
参数类型保留原 op；string 参数按长度分隔，不能因文本包含分隔符发生误合并。
input/state/memory read、DPI、system、output 和 commit 均不共享。赋值环、其他
计算环及依赖计算环的纯 op 不进入 CSE；不推断循环不变量。诊断包括
`identity_assigns_removed`、`algebraic_identities_removed`、
`common_expressions_removed` 和 `rewritten_uses`。

归一化也在这个 GrhSIM IR pass 中执行代数恒等式，不由 emitter 改写计算语义。
对无 parameters、同完整 TypeId 的两态 1–64 位 operands/result，二元操作的
operands 依次是左值 `a`、右值 `b`：`add(a,0)`、`sub(a,0)`、`mul(a,1)`、
`div(a,1)`、`and(a,all-ones)`、`or(a,0)`、`xor(a,0)` 接回 `a`；可交换操作
也识别另一侧的常量。`mul/and` 遇零接回零，`or` 遇全 1 接回全 1。
有符号 1 位除法保守保留；模除及除零不套用恒等式。逻辑 `logicAnd/logicOr`
只在同类型 1 位条件下套用布尔恒等式，不能将宽整数的布尔结果接回宽整数。
例如 `u5 x && u5(1)` 的结果仍须经过布尔化，而 `u1 x && u1(1)` 可接回 `x`。

`mux` 的三个 operands 是条件、真分支、假分支。两分支和 result 同完整 TypeId、
条件为两态 logic 时，常量条件选定分支，或两个相同分支接回该分支；分支可以是
宽 logic。常量条件识别限 1–64 位。`and(a,a)`、`or(a,a)` 也支持同类型宽 logic。
常量要求只有一个 `value` 或 `constValue` 参数；两者并存或附带其他参数时保守
保留。先按字面量自身符号扩展/截断到结果位宽，再按两态语义将 X/Z 投影为零。

恒等式在已有赋值链解析后的拓扑遍历中处理，后继看到的是替换后的 ValueId，
因此 `assign(0) -> mul(x,alias) -> add(y,product)` 可一次接回 `y`。已知可交换的
`add/mul/and/or/xor/xnor/eq/ne/caseEq/caseNe/logicAnd/logicOr` 在两输入同类型时
按 ValueId 排序构造 CSE 键，`add(a,b)` 与 `add(b,a)` 可共享，非交换运算保持顺序。
删除与重接发生于 IR，后续依赖分析和 mapping 使用化简后的图；语义 revision
使旧 mapping 失效。状态共享若再暴露相同表达式，重复现有归一化过程。

拓扑遍历之前还折叠同源连续切片的 `core.compute.concat`：全部 operand 都是
同一 two-state logic 源值的 `sliceStatic` 结果、位段按 MSB 优先顺序首尾相接，
且拼接宽度等于结果宽度时，`concat(slice(x,w-1),…,slice(x,0))` 在源值与结果
同完整 TypeId 下接回源值（`concat_identity_folds`）；否则当结果为 unsigned
two-state 时把 concat 原 op 就地改写为单个 `sliceStatic(x,lo,hi)`，op/result
标识不变并继续参与后续 CSE（`concat_range_folds`）。位段有空缺、逆序、跨源、
四态源或有符号结果的拼接保守保留。该形态主要由 pack-bit-registers 把逐 bit
读改写为 word 切片后产生，故流水线在打包后重跑本 pass。诊断键相应增加
`concat_identity_folds` 与 `concat_range_folds`。

按下表顺序执行。C1–C8 只生成或推进 CPU mapping，不改写语义 op、value 或 `Init`；
最后一步只读消费完整 mapping。不得通过 session 隐藏状态传递后端决策。C1 是唯一的
mapping 初始化点：不要求任何前置 mapping，重复运行丢弃并重建；其余 pass 各推进一个
stage，单向不回退。

| 阶段 | Pass | 主要产物 |
| --- | --- | --- |
| C1 | [`cpu.st.build-general-nodes`](../passes/build-general-nodes.md) | 从零初始化 mapping：root + Event/General/Mem/Output 四平铺分枝，General 分枝的 node（锥吸收） |
| C2 | [`cpu.st.merge-general-supernodes`](../passes/merge-general-supernodes.md) | General 超节点；超节点序号 = General 分枝子节点顺序，就此固定 |
| C3 | [`cpu.st.layout-named-stores`](../passes/layout-named-stores.md) | 物理类型表与七个具名 store（零分类决策，只消费 A7 `storeClass`） |
| C4 | [`cpu.st.build-event-activation-map`](../passes/build-event-activation-map.md) | 每个 event act 的 P_event 激活映射：含该 act 事件 op 的**非 sink** 超节点位图（bit i = C2 序号 i；V2-M2 起 sink 超节点不再配位图） |
| C5 | [`cpu.st.build-mem-write-plan`](../passes/build-mem-write-plan.md) | Mem 相写的 per-mem 优先级与 mem 类状态的 General 相读者表 |
| C6 | [`cpu.st.pack-general-functions`](../passes/pack-general-functions.md) | 超节点 helperChunks；General 分枝尾随 EmitFunction 叶子（只记 `supernodeRange` 区间）；Event/Mem/Output 各塌缩为唯一 EmitFunction |
| C7 | [`cpu.st.build-phase-schedule`](../passes/build-phase-schedule.md) | 三张 fanout、timeslot 触发映射、单核 task 序列 |
| C8 | [`cpu.st.plan-translation-units`](../passes/plan-translation-units.md) | emit TU 计划（块流 + 单元装箱，记录规模上限）；终态 `TranslationUnits` |
| 发射 | `cpu.st.emit-cpp` | 每单元一个 .cpp + 共享头 + 多源 Makefile，要求输出目录为空 |

最终分区树为：

```text
root
  phase Event    -> emit_function -> ops         ; 锥拓扑序，edgeDet 殿后
  phase General  -> supernode+ -> node+ -> ops   ; 超节点直挂 General 分枝，子节点顺序即序号
                 -> emit_function+               ; 尾随叶子，attrs.supernodeRange 记序号区间
  phase Mem      -> emit_function -> ops         ; op-id 序
  phase Output   -> emit_function -> ops         ; 拓扑序
```

这些是同一 `PartitionTree` 的层次，不是独立的 Domain/Word/Function graph 实体。
supernode 决定活动度粒度；EmitFunction 决定代码组织，不能为了减少函数数而扩大调度粒度。
归位决议 2：超节点序号在 C2 固定、与函数打包解耦——C6 不再重新挂载超节点，只在区间上
记录函数边界。TU 归属由 C8 的 `translationUnits` 计划承载（M5d-7）：块流（Core/Init/
Event/GeneralScan/Supernode/Mem/Output/Dump）按估计行数装箱为规模受控的单元，emit
逐单元输出一个源文件，生成的 Makefile 承载完整源文件清单（`make -j` 即并行编译）。

`PhaseSchedule`（C7 后）与 `TranslationUnits`（C8 后）都是 `complete=true` 的终态；
emit 只接受 `TranslationUnits`。`CpuMappingStage` 枚举数值保持稳定（旧 checkpoint
兼容），但流水线顺序不再是数值顺序——`GeneralFunctions`（数值 11）排在
`MemWritePlan`（14）之后、`PhaseSchedule`（15）之前，`TranslationUnits`（16）为终态；
所有 stage 比较走 `cpuMappingStageRank`/`cpuMappingStageAtLeast`
（`include/grhsim/ir/model.hpp`）。`SplitPhases` 枚举仅为旧 checkpoint 解码保留，不再
由任何 pass 产生；stage 早于 `SplitPhases` 的旧 checkpoint 被 verify 拒绝。各阶段校验
对应的不变量；semantic revision 改变后旧 mapping 失效，必须重新生成，不能只补最后一个
pass。

## 3. 相位与事件

相位归属完全在语义层完成（见 [Overview](../overview.md) 第 3.6 节）：B2–B4 标记
event/output 锥，[`grhsim.split-phases`](../passes/grhsim-split-phases.md)（B5）按 A7
存储分类完成总归属，B8 封板。C 段只消费 `SimOp::phase`，不重新归因；C1 发现 `none`
op 即报错并指向 B5。

写 op 集合为 `core.state.regWrite`、`latchWrite`、`memWrite`、`memFill`、`memAssign`、
`memWriteSeq`：reg/latch 写与 `regLatch` 类状态（含小数组）上的 mem 写在 General 相，
`mem` 类数组的写在 Mem 相。其他 op 归 General（含 `core.system.task` 和
`core.dpi.call`），事件锥与输出锥分别归 Event/Output。

事件在 B2 已降级为聚类形态：每个 `(event, edge)` 去重聚类对应一个
`core.event.edgeDet`（P_event，`prevEvent` 保存上一轮事件值），消费 op 以
`event_acts`（聚类下标数组）引用判定结果；C4 按聚类生成 P_event 激活映射（只覆盖
含事件 op 的非 sink 超节点）。写口守卫是命中聚类的析取，再与 enable 组合决定是否写
数据；即使 enable 为假，prevEvent 仍逐轮采样。

不得预设 XiangShan 只有一个事件聚类，也不得假设同一 state 只有一个写口；多个写口完整
保留，合法性和覆盖规则遵守 core 方言。`memWriteSeq` 的有序三元组在同一个 op 内处理，
分区或函数打包不得拆散它。

DPI 保持 `callCond && event guard` 语义：有无返回值不决定其相位，没有结果的调用不能被
删除，没有 event 时 guard 为真；未触发时结果保持。

旧管线的 `cpu.st.split-phase`/`cpu.st.form-event-domains`（compute/commit 两枝与 commit
事件域分区）已在 M5d-6 删除。

## 4. 分区、布局和调度的衔接

C1 的 node 由依赖关系和容量边界形成；C2 的 coarsen/DP 合并后仍须保证展开顺序为合法
DAG。超节点序号在 C2 固定（General 分枝子节点顺序），C3–C7 与 emit 共用同一序号空间。
helper ranges（C6 写入的 `helperChunks`）覆盖一个 supernode 的展开 op 序列，不是模型
OpId 范围，也不是新的调度单元。

C6 的函数打包只聚合 C2 序号的连续区间（`supernodeRange`），不重新挂载超节点。
`target_batch_count` 是函数数量的软目标，不是编译规模硬保证：当前非零值会按总
ops/lines 除以目标数放大阈值，0 则不启用该调整。默认值为 64。改变此参数必须单独记录
并重新验证编译，不能仅凭函数数接近 legacy 就认定 emit 结构或编译成本已一致。

DataLayout 在 C3 生成：状态归 store 只消费 A7 的 `storeClass` 注解（`regLatch`/`mem`），
未分类状态报错并指向 `grhsim.select-state-stores`。`regLatch` 类状态（含数组）进
regLatch store（emit 声明 `regLatchStore`/`regLatchStoreNext` 双缓冲，publish 整块
memcpy 提交），`mem` 类数组进 mem store。输入端口、跨超节点 value 和 General→Mem 写
参数使用 boundary 持久存储。布局完整性不能代替 emitter 的类型支持检查。

C7 的单核 task 序列为：P_event（`AlwaysScanCommit`）→ P_general 各 EmitFunction 叶子
（`DataGated`，任务执行叶子的 `supernodeRange` 区间；V2-M2 前名为 `EventDataGated`）
→ P_mem（`AlwaysScanCommit`）
→ P_output（`EvalEnd`，round 循环外）；每个 EmitFunction 叶子对应一个 task，
`waitsFor` 为空。激活关系由三张 fanout 表表示（目标按 C2 序号排序去重；V2-M2 起目标
一律收窄为**非 sink** 超节点——sink 超节点不经 dataActiveFlag 激活）：

| 激活表 | 触发源与用途 |
| --- | --- |
| `inputFanout` | General 相输入 read value 的任意变化，激活所属超节点；纯事件输入无条目（由 P_event 覆盖） |
| `computeSupernodeFanout` | boundary value 真变化，激活跨超节点非 sink 消费者（Mem 相写消费者除外，P_mem 每轮运行；sink 消费者过滤后无条目） |
| `commitStateFanout` | reg/latch 状态真变化，激活 General 读者超节点并决定继续迭代；General 相、目标为 regLatch 类数组的 memRead 读者同标量一样经本表激活 |

activate 目标只指向 General 超节点；mem 类数组的读者不在此表，由 C5 写计划的读者表
激活。posedge 聚类也必须观察下降沿，以便 prevEvent 回到 0；不能只在所需边沿方向上
做事件判定。

## 5. 运行时 Flow

初始化应用全部 `Init` 步骤（写 regLatchStore 当前值后由 initGlue 同步 next 缓冲），
激活全部 General 超节点（dataActiveFlag 全 1）。每次 `eval()` 固定外部输入，然后执行：

```text
加载并归一化输入
对 inputFanout 所列输入做差分，置位对应超节点的 dataActiveFlag
重复执行轮次：
  P_event：边沿判定写 eventActStore、采样 prevEvent；按 C4 激活映射把
    命中 act 的非 sink 超节点置位 dataActiveFlag（V2-M2 起无 eventActiveFlag）
  P_general：按 task、C2 序号顺序，按超节点类别点火并传播变化
    （非 sink：dataActiveFlag 点火并清零；SinkEvent 簇：eventActStore 签名
    组合门控；SinkEscape 簇：每轮无条件；regLatch 类写在超节点内 NBA 提交
    regLatchStoreNext；memRead 读当前值）
  P_mem：按 C5 计划逐写原地提交 mem 类数组（同地址后写覆盖先写）
  P_publish：regLatchStoreNext 整块提交回 regLatchStore；状态真变化经
    commitStateFanout 把读者激活进 dataActiveFlagNext，并决定是否继续迭代
  若 E 未变化，退出轮次循环
P_output：收敛后执行一次（EvalEnd，round 循环外），刷新对外输出
超过收敛轮数上限则报告错误
```

当前 C++ emitter 的收敛保护上限为 100000 轮。

prevEvent 是普通状态更新的一部分：每轮 P_event 的边沿判定读取上一轮 prevEvent，采样随
本轮生效，**不是冻结到整个 eval 结束**。dataActiveFlagNext 与当前轮标志分开消费，轮末
清理不能丢失 publish 刚产生的唤醒。

例如派生时钟 `g = clk & en` 从 0 变为 1，P_event 判定 posedge 后写 eventActStore 对应
act 位，引用该聚类的 SinkEvent 写簇在 P_general 经签名组合门控点火写状态；g 的下降沿
同样被判定（prevEvent 回到 0），不会把同一电平再次误判为上升沿，也为下次上升做好准备。
产生门控使能的 latch 写落入 SinkEscape 簇、每轮无条件点火，使"EN 先稳定、Q 边沿后判定"
在同一 eval 的轮次循环内闭环（不再存在 eventActiveFlag 双重门的循环等待）。

## 6. Emit 约束与优化边界

emit 可以改变 C++ 形态，但不能改变上述 G 转移、E 或调用语义。当前 emitter 的关键结构：

- General 相 regLatch 类数组写（memWrite/memFill/memAssign/memWriteSeq）在所属超节点内
  NBA 提交 regLatchStoreNext：merge 基为 next 行（同轮多写口读改写叠加）、对 cur 行做
  真变化检测，经 commitStateFanout 把读者激活进 dataActiveFlagNext，与标量 reg/latch
  路径一致；memRead 对 regLatch 类数组读 regLatchStore 当前值；init 写 cur 后由
  initGlue 同步 next。
- mem 类数组写在 P_mem 按 C5 优先级原地提交（同地址后写覆盖先写）；常量地址的读者按
  精确 staticRow 激活，动态地址保守地在任意写时激活。
- 宽位 helper 沿用 legacy 的指针式、调用者提供 out-buffer 的原地计算 ABI，不增加整块
  旧值快照。
- dumpState 对 regLatch 类大数组（count>64）与 memStore 一样走 fnv1a 哈希。
- M5d-7 多 TU：emit 按 C8 计划把块函数分配到多个 .cpp；超过规模的函数再拆块成员
  （超节点 `sn_<i>__c<j>` 用 C6 helperChunks；相位/init/dump/扫描各有块函数），跨块
  局部值经类内嵌套 spill 帧（`SnFrame<i>`/`EventFrame`/`OutputFrame`）按引用传递
  （driver 栈上分配，调用方提供缓冲）；相位 driver 与 eval 的执行顺序不变——并行的是
  C++ 编译，不是模拟器执行。DPI import 声明不进公共头，由引用它的单元各自声明
  （测试台可自由提供 extern "C" 定义）。

fullpass 不是当前默认路线。只有在功能正确、已有 profile 将差距归因到激活检查/传播后，
才可单独设计并验证快速路径；不能以忽略多时钟、混合边沿或派生事件来换取单时钟结果。

## 7. 验证与验收

每阶段先验证结构，再验证生成物行为，最后才做性能结论：

1. 检查相位覆盖、拓扑顺序、超节点序号/函数区间边界和 helper ranges。
2. 检查 named-store 布局、三张 fanout、事件激活映射、mem 写计划和 task 覆盖；
   fresh-session JSON roundtrip 不得依赖旧 session 的隐藏状态。
3. 运行生成 C++ 回归，覆盖 inline/helper、宽窄值、init/reset、DPI、掩码/多写口和读旧写新。
4. 使用多时钟记分板和 Verilator 对照，覆盖双域同时触发、混合边沿、派生/门控时钟、latch、
   异步复位、重复 eval 和 memory read-before-write。现有 `cpu_cdc`、`cpu_dual_ram` 是此类门禁，
   不代表模拟了硬件亚稳态，也不能替代 ingest 集成验证。
5. HDLBits 全量通过后，用 XiangShan CoreMark/NEMU 做完整 50k 对拍，比较全部有序采样和终态，
   不以局部样本或“未报错”代替最终退出结果。
6. 功能通过后比较普通 O3 可执行文件的串行同参耗时。记录模型/二进制指纹、packing、编译参数、
   image、NEMU、seed、waveform 和进度输出设置；测量不得与构建或其他仿真重叠。

legacy 是代码结构与性能的参考，不是可以直接套用的规模假设。对照分区数量级、实际表达式/
临时存储/变化发布、helper 的数据搬运三方面，并分别检查编译时间与运行时间。
50k 耗时不超过同条件 legacy 的 105% 是性能目标，**不是目前已满足的保证**。
功能通过、完整 mapping 或本轮三处 emit 修复，都不能替代该性能验收。

## 8. 项目执行入口

下列目标属于集成仓库 `wolvrix-playground/Makefile`，不是 wolvrix 子仓库根目录的命令。
先在集成仓库根目录加载项目环境；构建、安装、测试和仿真一律使用 Makefile 入口，不手工
拼接 CMake、编译器、链接器或 Python 构建命令。临时输出放项目内 `ptmp/`。

```sh
mkdir -p ptmp/cpu-st-tmp ptmp/cpu-st-ccache
export TMPDIR="$PWD/ptmp/cpu-st-tmp"
export CCACHE_DIR="$PWD/ptmp/cpu-st-ccache"
make test_grhsim_cpu_phases
make test_grhsim_cpu_stores
make test_grhsim_cpu_phase_emit
make py_install
make run_all_hdlbits_grhsim_ir_tests SKIP_PY_INSTALL=1
```

生成新的 XS 模型时选择独立、尚不存在或为空的输出目录；要复用 flat checkpoint，显式设置
`XS_WOLF_GRHSIM_IR_RESUME_FROM_FLAT_GRH_JSON=1` 和其输入路径。`XS_WOLF_GRHSIM_IR_CPU_TARGET_BATCH_COUNT`
是 packing 覆盖项，未设置时使用默认值；用于 A/B 时必须记录，不能静默改动。

```sh
make xs_wolf_grhsim_ir XS_GRHSIM_IR_BUILD=ptmp/cpu_st \
  XS_WOLF_GRHSIM_IR_EMIT_CPP_DIR=ptmp/cpu_st/model XS_LOG_DIR=ptmp
make xs_wolf_grhsim_ir_build_emu XS_GRHSIM_IR_BUILD=ptmp/cpu_st \
  XS_WOLF_GRHSIM_IR_EMIT_CPP_DIR=ptmp/cpu_st/model VM_BUILD_JOBS=8
```

构建结束并确认无遗留构建/仿真进程后，以下两个 run-only 目标按顺序执行，不并行：

```sh
make run_xs_wolf_grhsim_ir_emu XS_GRHSIM_IR_BUILD=ptmp/cpu_st \
  XS_SIM_MAX_CYCLE=50000 XS_WAVEFORM=0 XS_WAVEFORM_PATH= \
  XS_PROGRESS_EVERY_CYCLES=1000 XS_LOG_DIR=ptmp RUN_ID=cpu_st_50k
make run_xs_wolf_grhsim_emu XS_GRHSIM_BUILD=build/xs/grhsim \
  XS_SIM_MAX_CYCLE=50000 XS_WAVEFORM=0 XS_WAVEFORM_PATH= \
  XS_PROGRESS_EVERY_CYCLES=1000 XS_LOG_DIR=ptmp RUN_ID=legacy_cpu_st_50k
```

第二条命令要求指定目录中已有匹配配置的 legacy 可执行文件。复测使用新的 RUN_ID，保留
上一候选及其日志；某个进程观察超时不等于构建失败，不得因此重复启动同一任务。

## 9. 实现索引

- [mapping 校验（verifyCpuMapping）](../../../lib/grhsim/backend/cpu.cpp)
- [C1/C2/C6：四平分枝初始化、node、超节点与函数区间](../../../lib/grhsim/backend/cpu_partition.cpp)
- [C3：named-store 布局](../../../lib/grhsim/backend/cpu_layout.cpp)
- [C4/C5/C7：事件激活映射、mem 写计划与调度](../../../lib/grhsim/backend/cpu_schedule.cpp)
- [C8：TU 计划](../../../lib/grhsim/backend/cpu_emit_plan.cpp)
- [C++ emitter](../../../lib/grhsim/backend/cpu_phase_emit.cpp)
- [相位与分区回归](../../../tests/grhsim/test_cpu_phases.cpp)
- [named-store 回归](../../../tests/grhsim/test_cpu_stores.cpp)
- [生成代码回归](../../../tests/grhsim/test_cpu_phase_emit.cpp)
- [Legacy 活动度调度](../../transform/activity-schedule.md)
- [Legacy GrhSIM 调度](../../emit/grhsim-scheduling.md)

## 演进（M5d-6；M5d-7 追加 C8）

M5d-6 把 CPU mapping 定为一次最终 mapping 的 C 段七 pass（第 2 节末表）。删除内容：

- 旧两阶段线的八个 mapping pass（`cpu.st.split-phase`/`form-event-domains`/
  `build-compute-nodes`/`merge-compute-supernodes`/`pack-active-words`/
  `pack-emit-functions`/`layout-data`/`build-schedule`）与六个旧 schedule 消费者
  （`grhsim.demonitor-redundant`/`demonitor-edge-completion`/`migrate-boundary-ops`/
  `migrate-boundary-ops-ec`/`fuse-expr-chains`/`fold-residue`）；
- `cpu.st.split-phases`（归因职责由 B5 在语义层完成，mapping 初始化由 C1 承担）及
  M5d-5 的按 op 类型兼容 shim——Mem 分枝播种、mem 写计划收集、boundary 采样与写参数
  槽位命名全部回到按 `op.phase == Mem`；
- `CpuDataLayout`/`CpuSchedulePlan` 的 legacy payload 字段（只留 types/namedStores 与
  numaNodes/三张 fanout/eventActivation（V2-M2 前为 eventBitmaps）/memWritePlan/timeslotTriggers）与
  `CpuPartitionAttrs` 的 `eventGate`/`activeId`/`activeWord`（新增 `supernodeRange`）。

关键决议：超节点序号在 C2 固定（General 分枝子节点顺序），与 C6 函数打包解耦
（归位决议 2）；C6 的 EmitFunction 变为尾随叶子，只以 `supernodeRange` 记录序号区间
（铺满 [0,N)），Event/Mem/Output 分枝照旧各塌缩成唯一 EmitFunction；C6 移到 C5 之后、
C7 之前，`CpuMappingStage` 数值不变、顺序以 `cpuMappingStageRank` 为准；C3 前置降为
`GeneralSupernodes`，零分类决策、只消费 A7 `storeClass`；mem 写调度按相位——General 相
regLatch 类写经 NBA next 缓冲在超节点内提交，Mem 相 mem 类写在 P_mem 原地提交。

M5d-7 追加 C8 与多 TU emit（第 4.2 节）：TU 归属进入 mapping（`translationUnits`
payload），emit 从单源文件改为按单元输出，头文件只含声明（含帧结构与块函数声明），
生成的 Makefile 承载源文件清单支持并行编译。

`verifyCpuMapping` 按 rank 逐级校验：四分枝覆盖全部 op 不重不漏、各分枝相一致性、
Event/Output/General 序列 use-before-def、Mem 序列 op-id 升序、超节点 `eventActs` 与
扫 op 重算一致；`LayoutNamedStores` 起校验 named stores 结构，位图/写计划/fanout/task/
trigger 按模型与分区树重算要求完全一致；`GeneralFunctions` 起校验 `supernodeRange`
区间连续铺满 [0,N)；`TranslationUnits` 起按记录的规模上限重放 TU 计划并要求完全一致。
JSON 保持 v2 可选尾字段的位置化追加（TU 计划是 mapping 行的第六项）。

语义边界预测 helper `predictGeneralBoundaries`
（`include/grhsim/pass/general_boundaries.hpp`）静态模拟 C1 的锥吸收规则，不建立 mapping
即给出预测边界集，供 B7 `clone-shared-compute` 使用；两者实现保持一致，B7 的定向测试
（`grhsim-split-phases-tests`）校验预测边界集与 C1 实际 node 边界完全一致。
