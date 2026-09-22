# CPU 单线程活动度仿真 Flow

本文规定 `cpu.st.*` 的端到端流程：从已验证的 `GrhSimModel` 构建 CPU mapping，生成
C++ 模型，再验证多时钟行为与运行性能。`st` 表示单线程，不表示单时钟。

模型语义以 [Overview](../overview.md) 和 [Core Dialect](../dialects/core.md) 为准；
mapping 字段、默认参数和 emitter 约束以 [CPU 后端](../backends/cpu.md) 为准。
本文负责组织各阶段的输入、输出和验收，不另建一份 schema，也不保存逐次实验日志。

## 1. 范围与产物

当前路线已实现 compute/commit、活动度传播、输入与派生事件域、数据布局、调度和 C++ emit。
运行时使用一个 NUMA node、一个 CPU core；调用方改变输入后调用 `eval()`，不要求存在名为
`clock` 的端口，也不把一次 `eval()` 等同于一个硬件时钟周期。

输入是包含 `I/O/S/F/G/Init` 的合法模型。`Init` 必须完整定义状态初值；GRH lowering 和
前置变换在进入本 flow 前完成。产物包括：

- 携带完整 `CpuBackendMapping` 的模型及可重新加载的 JSON checkpoint。
- C++ 模型头文件、共享 runtime、driver、初始化和 task 源文件，以及生成目录内的 Makefile。
- 与模型、mapping、编译参数和实际可执行文件对应的功能与性能验证记录。

当前 C++ emitter 的主要逻辑路径为 two-state；mapping 支持某种物理类型不等于 emitter
已支持其全部操作。four-state、`memAssign`、字符串状态等未实现路径必须显式拒绝。
waveform/runtime profile、多线程和 fullpass 不属于本 flow 已验收的能力。

## 2. 构建流水线

GRH 入口使用 `lower_grhsim(..., logic_domain="2-state")`。对没有 defining op、仍被引用的
logic value，lower 显式生成同位宽、同有符号属性的 `core.compute.constant` 零值 producer，
与 legacy 二态仿真的零初始化一致。例如关闭 `RANDOMIZE_REG_INIT` 时，
`reg [7:0] debug; assign y = {debug, data};` 中没有其他赋值的 `debug` 会成为
`debug = core.compute.constant(constValue="8'h0")`，concat 继续读取该 value。
此 constant 无 operand，唯一 result 是原 undriven value；`constValue` 保存其定宽零字面量。
外部 input 和 inout 输入侧由 `core.input.read` 提供 producer，不补零；没有引用的 detached
value 继续跳过。四态或非 logic 的 undriven value 仍报错，不默认为二态零。

XiangShan 入口在 lower 成功后执行 GrhSIM IR 侧 `grhsim.reg-to-mem`、
`grhsim.canonicalize-compute` 和
[`grhsim.clone-shared-compute`](../passes/clone-shared-compute.md)、
[`grhsim.bitwise-predicates`](../passes/bitwise-predicates.md)，再进入下表的 CPU mapping；这不依赖 GRH 侧
reg-to-mem，也不修改 GRH。

完成首次mapping后，XiangShan入口执行
[`grhsim.pack-bit-registers`](../passes/pack-bit-registers.md)，依据同enable/mask、
event/history初值和quiescence投影类别把普通单写口bit寄存器打包为至多64位word。
该语义变换会使mapping失效。打包把原逐bit `core.state.read` 改写为
`core.compute.sliceStatic(packed, bit, bit)`，原 IR 中对这些bit的
`core.compute.concat` gather 因此退化为同一源值的连续切片拼接；随后立即重跑
`grhsim.canonicalize-compute`，将全覆盖顺序拼接折叠回源值、连续区间拼接就地改写为
单个 `sliceStatic`（详见下文该 pass 的代数规则），再完整重跑八个CPU mapping pass。
集成开关为 `XS_WOLF_GRHSIM_IR_PACK_BIT_REGISTERS=0/1`。读slice继续提供commit旧快照，
CPU emitter使用现有标量concat/read/slice/write路径；打包收益须由性能实测判断。

最终 mapping 前运行 [`grhsim.bitwise-muxes`](../passes/bitwise-muxes.md)，
将全部 operand/result 为 unsigned two-state bit 的 mux 转为按位选择 op，
让 emitter 使用按位算术表达已经求值的数据选择。禁用 bit 寄存器打包时，
此 pass 在 semantic pipeline 末尾执行。它保留 producers、依赖与共享关系，
不改变 state/commit/DPI 的执行条件。

mux 链折叠之后、第二轮 mapping 之前运行
[`grhsim.used-bits`](../passes/used-bits.md)：对全部二态 logic 值与寄存器状态做反向
"实际使用位"不动点分析，删除结果不可观测的死锥（纯计算/读 op、无活读者状态的写口与
状态本体），并把只被低 `[0,k)` 位观测的 op 锥与寄存器收窄到实际宽度（宽值跨越 64 位线时
从多字 helper 路径降级为标量路径）。sink（输出、DPI/system、memory 端口、事件）按全宽
保守处理；语义保持论证见 pass 文档。

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

按下表顺序执行。前八步只生成或推进 CPU mapping，不改写语义 op、value 或 `Init`；
最后一步只读消费完整 mapping。不得通过 session 隐藏状态传递后端决策。

| 阶段 | Pass | 主要产物 |
| --- | --- | --- |
| 相位 | `cpu.st.split-phase` | root 下唯一的 compute/commit 两枝 |
| 事件域 | `cpu.st.form-event-domains` | commit 事件域及域内写口 supernode |
| 计算节点 | `cpu.st.build-compute-nodes` | 拓扑有序的 compute node |
| 活动度单元 | `cpu.st.merge-compute-supernodes` | coarsen + DP 聚合的 compute supernode |
| 活动字 | `cpu.st.pack-active-words` | active ID、每字 8 个 supernode、helper ranges |
| 函数 | `cpu.st.pack-emit-functions` | compute/commit 的生成函数边界 |
| 布局 | `cpu.st.layout-data` | object、local、boundary 和运行态槽位 |
| 调度 | `cpu.st.build-schedule` | task 顺序、执行条件、fanout、round seeds、input shadows |
| 发射 | `cpu.st.emit-cpp` | C++ 模型与 Makefile，要求输出目录为空 |

最终分区树为：

```text
root
  compute
    emit_function
      active_word
        supernode
          node -> ops
  commit
    event_domain
      emit_function
        supernode -> ops
```

这些是同一 `PartitionTree` 的层次，不是独立的 Domain/Word/Function graph 实体。
supernode 决定活动度粒度；函数决定代码组织，不能为了减少函数数而扩大调度粒度。
TU 归属不进入 mapping；当前 emitter 为每个 task 输出一个源文件，未来 TU 打包仍属 emit 决策。

只有 Schedule stage 的 mapping 才是 `complete=true`。各阶段校验对应的不变量；
semantic revision 改变后旧 mapping 失效，必须重新生成，不能只补最后一个 pass。

## 3. 相位和事件域

commit 分类包含 `core.state.regWrite`、`latchWrite`、`memWrite`、`memFill`、
`memAssign`、`memWriteSeq`。分类并不放宽 emitter 的支持范围，例如 `memAssign` 当前仍被拒绝。
其他 op 属于 compute，包括 `core.system.task` 和 `core.dpi.call`。

事件域的 canonical key 仅由 `(edge, ValueId)` 集合构成，排序去重后相同的写口归入同域。
update condition、data、mask 和 history StateId 不进入 key。全部事件值由 input.read 产生
时 source 为 input，否则为 derived；同时引用输入和派生事件的域也属于 derived。
无边沿写口和 latch 放入无 event gate 的 general 域。

例如一个带异步复位的寄存器写口：

```text
operands   = [enable, next, mask, clk, reset]
objectRefs = [q, clkHistory, resetHistory]
event_edges = [posedge, posedge]
```

`enable` 是数据更新条件，`next` 是写入数据，`mask` 选择写入位；`clk/reset` 是事件值，
两个 history 各保存该 op 对应事件的旧值。域 key 只含 `(posedge, clk)` 与
`(posedge, reset)`；守卫是两个边沿的析取，再与 enable 组合决定是否写数据。
即使 enable 为假，仍须采样两个 history。

不得预设 XiangShan 只有一个边沿域，也不得假设同一 state 只有一个写口。域统计应来自
实际 IR；多个写口完整保留，合法性和覆盖规则遵守 core 方言。`memWriteSeq` 的有序三元组
在同一个 op 内处理，分区或函数打包不得拆散它。

DPI 保持 compute 侧的 `callCond && eventGuard`：有无返回值不决定其相位，没有结果的调用
不能被删除，没有 event 时 event guard 为真。history 采样不受 callCond 限制；未触发时结果保持。

## 4. 分区、布局和调度的衔接

compute node 由依赖关系和容量边界形成；coarsen/DP 合并后仍须保证展开顺序为合法 DAG。
word 只存在于 compute 枝，active ID 连续分配。helper ranges 覆盖一个 supernode 的展开 op
序列，不是模型 OpId 范围，也不是新的调度单元。

函数打包只能聚合完整 word 或同域 commit supernode。`target_batch_count` 是函数数量的
软目标，不是编译规模硬保证：当前非零值会按总 ops/lines 除以目标数放大阈值，0 则不启用
该调整。默认值为 64。改变此参数必须单独记录并重新验证编译，不能仅凭函数数接近 legacy
就认定 emit 结构或编译成本已一致。

DataLayout 在最终分区后生成。跨 supernode、compute 到 commit、事件值和需保持的 DPI 结果
使用持久存储；其余可用 partition-local 槽。布局完整性不能代替 emitter 的类型支持检查。

boundary 偏移按三层分配。最前层是事件门控端点输入的致密段：compute（非 EventDomain
子树的 EmitFunction）中，凡 unit 含带非空 `event_edges` 参数的 `core.system.task` /
`core.dpi.call`（判据只看 op 名与参数，不看 task 编号或模块名），该 unit 直接引用的
两态、宽度 ≤8 位的 boundary 值按消费 task 聚成一组，组间按组大小降序（同大小按 task
分区 id 升序）、组内按 value id 升序从偏移 0 起致密分配；致密段预算 16 KiB，超预算的组
整组回落到旧层（不拆组）。第二层是每周期被 commit task 读取的 edge-commit 写口 boundary
操作数，第三层是其余 boundary 值（value id 顺序）。致密化只重排偏移，每个值偏移唯一、
boundaryBytes 按对齐规则照常累计；生产者写回与消费者读取都经
`layout.values[v].offset` 取址，天然一致。该集合是模型与分区树的确定函数，是 canonical
布局的一部分；`verifyCpuDataLayout` 同时接受致密化前的旧 canonical 形式，使旧 checkpoint
经重跑 `cpu.st.layout-data`（reemit remap 路径）升级，完整 SV 流程产物则始终为致密形式。
`cpu.st.layout-data` 诊断输出 `densified_boundary_values` / `densified_bytes` /
`densified_groups` 三个计数。

SchedulePlan 的单核 task 序列先 compute 后 commit，每个函数对应一个 task，`waitsFor` 为空。
执行条件与激活关系分开表示：

| 执行条件 | 行为 |
| --- | --- |
| `ActivityDrivenCompute` | 调用函数后按 word/bit 派发活动 supernode |
| `DomainGatedCommit` | 域 arm 为真才进入函数，内部仍逐 op 精判 event guard |
| `AlwaysScanCommit` | general 域每轮进入，保留原写口守卫 |

| 激活表 | 触发源与用途 |
| --- | --- |
| `inputFanout` | 输入 read value 的任意变化，激活生产者/消费者并 arm 引用它的域 |
| `computeSupernodeFanout` | compute value 真变化，激活跨 supernode 消费者或 arm 派生事件域 |
| `commitStateFanout` | E 中的状态最终真变化，激活状态读者及相关 history 消费者，并决定继续迭代 |

`activate` 只能指向 compute supernode，`arm` 只能指向 commit 事件域。commit 不使用
per-write/per-supernode 的 compute 活动位，不能将域级门控扩展成稠密的 state × domain 表。
posedge 域也必须观察下降沿，以便 history 回到 0；不能只在所需边沿方向上传播 arm。

`commitStateFanout` 的 key 集合是从输出和边沿判定反推的状态闭包 E。遍历状态读后还要穿过
该状态的写口，不能遗漏间接依赖，也不能把所有状态都加入 E。非 E 状态读者和具有外部观察
语义的 system/DPI 单元通过 `roundSeeds` 保守逐轮执行。`inputShadows` 与输入表逐项对应。

## 5. 运行时 Flow

初始化应用全部 `Init` 步骤，清除 pending/dirty/read-address 缓存，激活全部 compute 单元，
并 arm 全部边沿域。每次 `eval()` 固定外部输入，然后执行：

```text
加载并归一化输入
对 inputFanout 所列输入做差分，更新 shadow，产生 activate / 当前轮 arm
重复执行 G：
  注入 roundSeeds
  compute：按 task、word、supernode 顺序求值并传播变化
  commit：域级 arm 筛选后精判写口守卫，登记状态更新
  publish：应用最终更新，比较 E，激活读者并产生下一轮 arm
  将下一轮 arm 转交当前轮，清空下一轮缓冲
  若 E 未变化，刷新对外输出并返回
超过收敛轮数上限则报告错误
```

当前 C++ emitter 的收敛保护上限为 100000 轮。活动字中，后序 bit 可以在同字局部 flags 中
立即激活；当前或前序 bit 留在全局待后续扫描，不能把它们清掉。

history 是普通状态更新的一部分：每轮 G 的守卫读取该轮旧 history，采样随该轮 publish
生效，**不是冻结到整个 eval 结束**。当前轮 arm 与下一轮 arm 分开消费，轮末清理不能丢失
publish 刚产生的唤醒。DPI/system task 在 compute 中登记的 history 也参与同一发布过程。

例如派生时钟 `g = clk & en` 从 0 变为 1，会在 compute 中 arm 使用 g 的域；该域精判
`!gHistory && g` 后写状态，并采样 gHistory=1。下一轮 G 看到的是新 history，不会把同一
电平再次误判为上升沿。随后 g 的下降也须唤醒该域采样 history，为下次上升做好准备。

## 6. Emit 约束与优化边界

以下优化可以改变 C++ 形态，但不能改变上述 G 转移、E 或调用语义：

- compute-only 的安全 state-read 别名可省去复制，但必须补齐直接消费者的状态激活关系；
  直接作为 commit operand 的值保留旧快照，例如 `q1 <= d; q2 <= q1` 不得读取提前更新的 q1。
- 同一 supernode/helper chunk 内相同 fanout 的逻辑结果合并 changed，再统一发布 mask。
  宽位 helper 沿用 legacy 的指针、调用者 out-buffer 和原地计算，不增加整块旧值快照。
- 标量 direct commit 只用于经证明不会被其他 commit 观察的私有单写者；其余保留 deferred
  publication。不能从“通常只有一个写口”推导这一优化。
- 内存按 cell 暂存和发布，多写口复用同轮该行 shadow，保留 mask 和有序覆盖；fill 与
  sequence 写也使用同一路径。读端口缓存实际地址，发布时只激活匹配行的读者所属单元。
- history batching、稳定历史跳过和 inactive-edge sampling 必须保持各 op 独立的 history
  与守卫；不能用一个代表 history 替代整组状态。

fullpass 不是当前默认路线。只有在功能正确、已有 profile 将差距归因到激活检查/传播后，
才可单独设计并验证快速路径；不能以忽略多时钟、混合边沿或派生事件来换取单时钟结果。

### 6.1 srcloc 溯源链路

生成的 C++ 需要能回查 RTL 源码位置。链路为：ingest 把 slang AST 的 `SourceLocation`
写入 GRH op 的 `SrcLoc`（`grh.hpp`，JSON `loc` 字段随 store/load 保留）；GRH transform
在改写时继承原 op 的 srcloc，新建 op 用 `origin="transform"` 标记来源 pass；
`grh_to_grhsim` 在 `keep_origins`（Makefile 变量 `XS_WOLF_GRHSIM_IR_KEEP_ORIGINS`，默认 1）
开启时把 `SrcLoc` 拷成 grhsim IR 的 `Origin`（JSON origins 表持久化）；grhsim pass 经
`replaceOperation` 自动保留 origin，新建 op 必须显式继承被改写 op 的 origin。

`cpu.st.emit-cpp` 默认（`--srcloc-comments true`）在每个 op 的生成代码前输出一行
`// @<file>:<line>:<col> op=<opType> name=<opName>`；无源码位置的 op 输出
`// @generated pass=<pass> note=<note>`。常量、别名读、静态字符串等不产出代码的 op
不输出该行。shape-twin/branch-block share 在文本折叠前把行注释换成 `\x02` 哨兵，
哨兵不进分组 key（分组不受影响），共享函数体恢复 host 任务的注释——被折叠成员任务的
代码见共享体，注释位置一一对应但源码位置以 host 为准。

## 7. 验证与验收

每阶段先验证结构，再验证生成物行为，最后才做性能结论：

1. 检查 phase/event 覆盖、拓扑顺序、supernode/word/function 边界和 helper ranges。
2. 检查布局、三张 fanout、E 闭包、roundSeeds 和 task 覆盖；fresh-session JSON roundtrip
   不得依赖旧 session 的隐藏状态。
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
make test_grhsim_cpu_mapping
make test_grhsim_cpu_schedule
make test_grhsim_cpu_emit
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

- [相位和事件域](../../../lib/grhsim/backend/cpu.cpp)
- [node、supernode、word 和函数打包](../../../lib/grhsim/backend/cpu_partition.cpp)
- [数据布局](../../../lib/grhsim/backend/cpu_layout.cpp)
- [调度与激活关系](../../../lib/grhsim/backend/cpu_schedule.cpp)
- [C++ emitter](../../../lib/grhsim/backend/cpu_emit.cpp)
- [CPU mapping 回归](../../../tests/grhsim/test_cpu_mapping.cpp)
- [调度回归](../../../tests/grhsim/test_cpu_schedule.cpp)
- [生成代码回归](../../../tests/grhsim/test_cpu_emit.cpp)
- [Legacy 活动度调度](../../transform/activity-schedule.md)
- [Legacy GrhSIM 调度](../../emit/grhsim-scheduling.md)
