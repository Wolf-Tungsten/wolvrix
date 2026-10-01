# `grhsim.simplify`

统一化简基础设施（M5d-2）：把既有化简子 pass 按固定顺序组合，迭代至稳定或轮数上限。
它是全图优化（A6）与分区化简（B6）共用的入口；[`clone-shared-compute`](clone-shared-compute.md)
**不**加入此不动点——它是语义层最后一个改写 pass，放在化简之后避免与 CSE 反复抵消。

## 子流程

每轮按以下顺序执行（各子 pass 的匹配/拒绝规则见各自文档）：

```text
grhsim.const-fold             # 常量运算折叠（就地改写为 constant）
grhsim.canonicalize-compute   # 赋值链/CSE/恒等式/concat 折叠 + 等价状态合并
grhsim.bitwise-predicates     # 1-bit logicAnd/logicOr -> and/or
grhsim.bitwise-muxes          # 1-bit mux -> bitSelect
grhsim.mux-chain-fold         # 优先 mux 链 -> prioritySelect
grhsim.used-bits              # 位需求分析、窄化及死锥清理
```

## 参数

```text
--scope whole|phase       默认 whole
--phase event|general|mem|output   仅随 --scope phase 使用，限定单个计算分区
--max-rounds N            正整数，默认 8
```

- `whole`：每轮对全图执行一遍子流程。
- `phase`：每轮按 Event -> General -> Mem -> Output 逐分区执行子流程，各分区只改写本
  分区的 op。`--phase` 进一步限定单一分区（定向调试用）。

生产接线（M5d-5）：A6 以 `--scope whole` 运行于全图优化段；B6 以 `--scope phase` 运行于
分区段（`grhsim.split-phases` 之后、`clone-shared-compute` 之前）。

## scope 边界（phase 模式的保留规则）

phase 模式保留分区接口、副作用根和跨分区引用，禁止跨阶段 CSE：

- 只有 `op.phase` 等于当前分区的 op 可被改写或删除；其它 op 一律是不可变根。
- 跨阶段 CSE/别名本就被 `canonicalize-compute` 的相位屏障禁止（Event/Output 锥自封）；
  phase 模式进一步只让本分区的 op 进入 CSE 表与赋值链来源表。
- 被任何分区外 op 引用的状态不合并、不窄化、不删除；其写口 data/mask 保持全宽需求。
- used-bits 分析始终全图运行，共享状态的位需求天然取**所有分区需求的并集**；分区外
  op 被视为 opaque 全使用汇（operand/result 全宽、引用的状态全宽），局部需求因此永远不
  能单独窄化或删除共享状态。
- canonicalize-compute 不删除结果被分区外 op 消费的 op（越界消费者永不重接，删除会使
  其悬空）；mem 写参数等分区接口值由此完整保留（M5d-5 补齐的相位作用域护栏）。

## 终止条件与变化汇总

整轮（whole：一遍子流程；phase：四个分区各一遍）无任何变化即收敛；达到
`--max-rounds` 仍未收敛时正常返回（模型始终合法），并发出 warning。诊断汇总包含
`simplify_scope/simplify_rounds/simplify_converged` 头行和每个有变化的子步骤的累计计数
（`simplify_step=<name>` + 该 pass 的原有计数键）。

## 来源更新规则

子 pass 按 M5d-1 契约维护 `declProvenance`：

- 就地改写（const-fold、bitwise-predicates、bitwise-muxes）：目标实体不变，零维护。
- canonicalize-compute：被折叠/合并的 value 的 slice 重定向到等价存活 value
  （direct→alias）；等价状态合并时两组声明的 slice 指向同一存活 state（direct→merged）。
- mux-chain-fold：链根 result 就地存活；内层 link 没有等价存活体，其 slice 由
  `compact()` 安全网丢弃（记录留作空锚点）。
- used-bits：窄化重建的 value/state 的 slice 重定向到窄化替代体并把覆盖宽度截到存活
  前缀（kind 不变，声明区间可能变为部分覆盖）；死锥删除的实体由 `compact()` 丢弃 slice。

化简结果通过 verifier 结构校验与 JSON 字节稳定往返（`grhsim-simplify-tests` 锁定）。
