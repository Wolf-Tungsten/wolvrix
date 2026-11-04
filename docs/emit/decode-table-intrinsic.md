# 译码表 intrinsic

本功能保留 Chisel 译码表的语义，让 GrhSIM 生成软件匹配函数。开关为
`--wolvrix-sim-primitives decode-table`，默认 `none`。同名入口位于 XiangShan utility：
`ListLookup`、`DecodeLogic`、`decoder`。原来的调用形式和字段连接保持不变。

## 语义

`ListLookup` 使用首项优先；输入与某行 mask/value 匹配后，返回这一行所有字段。
没有匹配时返回默认值。动态输出、不支持的数据类型和未知输入宽度使用原库路径。
Bool 列保持 Bool 类型，其余 UInt 列按原接口推导宽度。

`TruthTable` 保存输出的 value/care，并合并所有命中项的约束。相交表项的约束相互
冲突时保留原库路径。没有行命中时使用默认 value/care；有命中时按该输入的约束计算。
未约束的输出位确定性填零。A/B 不必复制 Espresso/QMC 对这些位的选择。

输入 `?` 仍是通配符；非法指令并不自动成为 DontCare。当前实现针对 GrhSIM 二态
语义，不宣称四态等价。

## 自包含 transport 和 GRH

Chisel 使用无时钟、恒真 enable 的 `circt_dpi_call`，中转函数名为：

```text
wolvrix_decode_table_v1_i<N>_o<M>_b<L>
```

输入参数为 `value` 和常量表载荷，输出参数为 `decoded`。N/M 是逻辑位宽；DPI
标量承载宽度取 1/8/16/32/64 中能容纳逻辑宽度的最小值，更宽使用 packed bit vector。
L 是载荷字节数。载荷 UTF-8 字节按大端字节顺序编码为 L×8 位 UInt，格式为：

```text
WDT1|P或T|N|M|字段宽度列表|默认value|默认care|mask,value,out,care;...
```

数值字段为小写十六进制，宽度为十进制；字段零位于打包输出的最高位。例如：

```text
WDT1|P|3|4|4|f|f|1,1,3,f;7,7,5,f
```

表示 3 位输入、4 位输出，默认 15。输入最低位为 1 时，第一行返回 3；因此输入 7
同样返回 3，不能被后面的精确匹配覆盖。

导入后生成：

```json
{"kind":"kDecodeTable","in":["instruction"],"out":["controls"],
 "attrs":{"decode.table":{"t":"string","v":"WDT1|P|3|4|4|f|f|1,1,3,f;7,7,5,f"}}}
```

唯一动态操作数是输入，表不是数据依赖。输出是单个普通组合 SSA 值，字段拆分仍由
现有 slice 表达。没有状态、时钟或副作用；未知版本、非恒定表、错误签名、条件 enable
和 clocked transport 均拒绝。普通 DPI 不按此规则推断为纯函数。

常量折叠、CSE/DCE、来源序列化、局部表达式、跨组物化和结果变化检测已接入。
普通常量输入的表求值继续参与常量折叠。曾加入的“动态输入上检查每行输出与默认
输出相同，从而提前消除依赖”在当前 CSR-only 消融中关闭；`constantOutput` 分析
函数保留供测试，优化 pass 不再对动态输入调用它。其他常量传播、CSE/DCE 不变。
SystemVerilog emitter 对该 GRH 操作明确拒绝；普通硬件 RTL 应使用默认 Chisel 路线。

## C++ 形态

有效输入不超过 16 位，且完整结果数组按 64 位字存储不超过 1 MiB 时，生成静态 LUT。
重复输出记录合并为结果池和紧凑索引，不存在运行时初始化或堆分配。

输出只有 1 位、有效输入最多 6 位的表恢复为单字位图；Clang/x86 下不足 32 位的
标量输入采用非 volatile 空 asm 寄存器值约束，没有 memory clobber。该 lowering
修复独立 CSR 模型的重叠宽读取。上一轮按
[TNO0415](../../../pdocs/grhsim_opt_thj/TNO0415_decode_table_csr_ablation_setup_20261103.md)
移除此专用 lowering 做消融，保留常量表依赖消除。
消融结果见 [TNO0417](../../../pdocs/grhsim_opt_thj/TNO0417_decode_table_csr_ablation_results_20261104.md)：
撤回 CSR lowering 的 D 与组合版 C 为 4 胜 4 负，配对中位数约零、95% 区间跨零，
没有可靠改善；D 仍在 8/8 组中慢于旧 intrinsic B，不能把此前回退主要归咎于 CSR
约束。当前根据 [TNO0418](../../../pdocs/grhsim_opt_thj/TNO0418_decode_table_csr_only_setup_20261104.md)
恢复 CSR lowering，关闭动态恒定表提前折叠，形成 E 版做互补消融。
完整结果见 [TNO0420](../../../pdocs/grhsim_opt_thj/TNO0420_decode_table_csr_only_results_20261104.md)：
E/C 配对均值 −0.558%，八组更快；E/B 四胜四负，配对中位数 −0.026%、区间跨零。
关闭恒定表提前折叠恢复了当前配置下的性能，CSR 单独没有明确整机收益。末两轮
出现整体运行状态漂移，全部样本保留，不能把全部均值差异归为代码收益。

其他表递归取候选项 mask 的交集，按共同约束位的值分组，组内保留原顺序。
不超过 4 项或没有共同约束位时使用叶子匹配。优先表命中即返回；约束表保留所需合并。
宽值按机器字处理，相同表按内容共享函数。报告保留 LUT 字节数、树深、节点和最大叶子长度。

## 验证和复现

局部入口：`scripts/decode_table_study.py`，输入是实际 elaboration 导出的原表。
包含原库 Verilator、普通 GrhSIM、intrinsic 与独立解释器比较；小输入空间穷举，
大表用两组固定种子和逐行定向样本。验证只比较原表 care 位：

```text
(actual XOR expected) AND care == 0
```

`kernel` 测量摘取的生成计算主体和端口读写，`full` 包含正常 eval 调度及变化检测。
每个 kernel 必须先与本版本正常 eval 的完整输出核对，不将提取后的成本冒充整机成本。

整机入口：`scripts/grhsim_decode_table_study.py build|coverage|measure --output ...`。
build 完整生成同一快照 A=none/B=decode-table；measure 使用 node030 CPU96，专用
单模型单线程，DiffTest 开启，先比较两个种子的有效轨迹，再进行 8 对 AB/BA C100000。
编译与测量分开，正式计时不带诊断。运行记录和 NUMA 检查沿用专用单线程工具。

局部完整工作流：

```bash
python3 scripts/decode_table_replay.py --manifest <B的simulation_primitives.json> --output <新案例目录>
python3 scripts/decode_table_evidence.py --build <完整A/B目录>
python3 scripts/decode_table_measure.py --case <单张表案例目录> --output <新测量目录>
python3 scripts/decode_table_results.py --build <完整A/B目录> --local <局部汇总目录> --comparison comparison-v2
```

局部测量同样固定 node030 CPU96，复制 binary/library 并核验实际 NUMA 页面。
每个版本独立校准重复次数，每次使用整数个相同输入周期，记录每次操作 ns；
正式 8 对交替顺序，包含 kernel 和 full。`decode_table_trace_inputs.py` 可从有效
DiffTest commit slot 导出主译码输入，保留顺序及融合计数；不恢复未记录的融合指令，
也不能把它等同于全部推测译码输入。

2026-11-03 的 TLConfig/CoreMark 实验覆盖 24 张表、70 次 elaboration 替换，完整
模型保留 23 张／62 个操作。两组 C100000 有效 DiffTest 轨迹一致；8 对正式单线程
测量中 A 平均 99.255 s、B 97.229 s，配对均值为 -2.041%，8/8 对 B 更快。
这只是该配置与负载的结果。36 个 PreDecode 调用因单参数 WireInit 推迟输入宽度
推导，保留原实现；宽度未知不表示最终 RTL 不是 32 位。

详见 [语义与 NEMU 调研](../../../pdocs/grhsim_opt_thj/TNO0402_decode_table_intrinsic_contract_20261103.md)、
[完整覆盖与回退来源](../../../pdocs/grhsim_opt_thj/TNO0403_decode_table_full_coverage_20261103.md)、
[整机配对结果](../../../pdocs/grhsim_opt_thj/TNO0406_decode_table_whole_walltime_20261103.md)、
[24 张表的局部实证与最终交付](../../../pdocs/grhsim_opt_thj/TNO0407_decode_table_local_results_and_delivery_20261103.md)。

局部测量共 1,968 对，包含无收益和退化：恒零的索引偏移表仍保留动态依赖，
小型 CSR 权限逻辑使用查表后也可能更慢。默认开关与整机覆盖未因这些结果改变；
不把独立表的局部倍率等同于整机倍率。

上述数字是调整前结果。对应的后端调整见
[TNO0409](../../../pdocs/grhsim_opt_thj/TNO0409_decode_table_lowering_adjustments_20261103.md)。
复测入口 `decode_table_tune.py build|measure` 从经身份核验的同一份 RTL 重建调整版，
同场测量 A=原 RTL、B=上一版 intrinsic、C=调整版各 8 次。局部重建入口为
`decode_table_tune_local.py`，它使用相同的 24 张表、冻结 RTL 和验证向量；此局部
对照的 A/B 分别是旧 intrinsic 和新 intrinsic，不能与原硬件 A/B 标签混用。

此次调整的完整复测见
[TNO0413](../../../pdocs/grhsim_opt_thj/TNO0413_decode_table_tuned_whole_walltime_20261103.md)。
同场整机 A/B/C 平均分别为 98.793/96.785/97.381 秒；C 相对 B 慢 0.617%，
八组均较慢。虽然随机输入的完整局部 eval 中恒零索引表约 4.724→3.032 ns，
CSR 权限表约 8.858→4.547 ns，这些局部修复没有带来本轮整机额外收益。
完整模型的 CSR 输入原本已在寄存器打包，新约束也可能阻碍部分优化；同时常量
消除改变了调度与布局。没有消融实验，不能把整机差异完全归因于某一项修改。
默认开关仍为 none；调整代码作为可审阅实验保留，旧 intrinsic 的冻结 binary
继续作为本次更快的性能对照，不宣称这次调整应更新性能基线。
