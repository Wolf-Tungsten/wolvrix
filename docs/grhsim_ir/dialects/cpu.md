# GrhSIM CPU Dialect

本文定义 CPU 后端配套的物理类型和计算 op。物理类型只出现在 mapping 中；计算 op 的
operands/results 仍使用 `core.logic` 语义类型。方言的一般结构见
[GrhSIM IR Overview](../overview.md) 第 2 章。当前 schema fingerprint 为
`wolvrix.grhsim.cpu.v2`，不定义外部函数声明。

## 1. 类型

`cpu` 方言的每种类型定义一个取值集合，以及该取值集合在 CPU 内存中的物理表示（大小、
对齐和存储结构）。这些类型只出现在 CPU 后端映射中（见 [CPU 后端](../backends/cpu.md)
第 2 节），不能作为 I/O/S 对象或 value 的类型。

下文记类型 `T` 的大小为 `size(T)`、对齐为 `align(T)`；`align_up(x, a)` 表示把 `x` 向上
取整到 `a` 的倍数。

### 1.1 标量类型

| 类型 | 取值 | 大小（字节） | 对齐（字节） |
| --- | --- | --- | --- |
| `cpu.bool` | 0 或 1 | 1 | 1 |
| `cpu.uint8` / `cpu.sint8` | 8 位无符号 / 有符号整数 | 1 | 1 |
| `cpu.uint16` / `cpu.sint16` | 16 位无符号 / 有符号整数 | 2 | 2 |
| `cpu.uint32` / `cpu.sint32` | 32 位无符号 / 有符号整数 | 4 | 4 |
| `cpu.uint64` / `cpu.sint64` | 64 位无符号 / 有符号整数 | 8 | 8 |
| `cpu.f32` | IEEE 754 单精度浮点数 | 4 | 4 |
| `cpu.f64` | IEEE 754 双精度浮点数 | 8 | 8 |
| `cpu.str` | 字符串 | 指针大小 | 指针大小 |

同位宽的有符号和无符号整型存储表示相同（二进制补码），仅取值解释不同。`cpu.str`
存放为指向宿主字符串对象的指针，其大小和对齐与目标平台的指针相同（64 位平台上
为 8 字节）。

### 1.2 `cpu.uint<width>` / `cpu.sint<width>`

宽整数。`width` 为大于 64 的整数位宽；`width <= 64` 时使用第 1.1 节的定长标量。取值集合
为 `width` 位无符号 / 有符号整数。物理表示为单个 C23 `unsigned _BitInt(N)` 对象
（`N = ceil(width/64) * 64`，低 `width` 位承载取值，高于 `width` 的 padding 位恒为 0；
signedness 只在运算点解释，存储恒为无符号）。大小 `8 * ceil(width/64)` 字节，对齐
8 字节——与等长 `std::array<std::uint64_t, ceil(width/64)>` 完全相同，因此裸字节路径
（整 store memcpy、FST 按字转储、按字节哈希）可直接复用其 u64 字镜像（生成头以
static_assert 钉死该布局）。

### 1.3 `cpu.array<element_type, count>`

`count` 个 `element_type` 类型元素组成的定长数组。`element_type` 必须是 `cpu` 方言的
类型，`count` 为正整数。元素连续存放，第 `k` 个元素的偏移为 `k * stride`，其中
`stride = align_up(size(element_type), align(element_type))`。数组大小为
`stride * count`，对齐与 `element_type` 相同。

## 2. `cpu.compute.maskedOr`

```text
%result:core.logic<W> = cpu.compute.maskedOr(%mask:core.logic<N>,
                                          %data:core.logic<N*W, shape=[N]>)
```

一个 result，所有 operands/result 均为 two-state logic，无 object refs。signedness
仅解释 bit pattern，不改变 OR 的结果。支持两种编码，N>=2、W>=1，均不限制在 64 位内：

1. **Dense**：两个 operands，无 parameters。`mask` 为 N-bit scalar；`data` 为
   shape `[N]`、总位宽 N*W 的数组。result 总位宽 W，shape 可保留原有的 singleton/Vec。
2. **Views**：任意数量的原有 value operands，唯一 parameter `views` 为 int vector。
   每四个整数描述一项 `[selectOperand, selectBit, dataOperand, dataOffset]`。
   operand 编号从 0 开始；bit/offset 是 canonical packed bit 偏移，最低位为 0。
   选择范围为 1 bit，数据范围为 W bits，必须在相应 operand 的总位宽内。
   operand 可以是任意 shape 的数组、packed scalar 或独立计算结果。

精确定义：`result = OR_{i=0..N-1}(mask[i] ? data[i] : W'b0)`。
bit i 对应 canonical array index i；没有命中时返回零，多个命中时按位 OR。例如：

```text
maskedOr(4'b0101, [8'h12, 8'h80, 8'h24, 8'h40]) = 8'h36
maskedOr(4'b0000, [8'h12, 8'h80, 8'h24, 8'h40]) = 8'h00
```

Views 的定义为 `OR_t(operand[s_t][b_t] ? bits(operand[d_t], o_t, W) : 0)`。
它允许不完整覆盖、置换、重复选择位/数据项、多个 mask 和多个数据源。例如：

```text
%r:logic<8> = cpu.compute.maskedOr(%m:logic<4>, %a:logic<32,shape=[4]>, %x:logic<8>)
  {views=[0,0,1,8, 0,2,2,0]}
// r = (m[0] ? a[1] : 0) | (m[2] ? x : 0)
```

`views` 是编译期描述，不是运行时 gather；不要求创建 mask 或 data 临时数组。

它可以表示 Chisel Mux1H 降级后的 OR 结构，但不带 one-hot 假设；不能只选择最低的
命中项。W=1 时等价于 maskedAny。该 op 是纯 value 计算，读取的是 `%data` 的既定值，
本身不新增状态访问，也不授权跳过任意 producer、外部调用或随机采样。

[`cpu.st.recover-masked-or`](../passes/recover-masked-or.md) 在 CPU 映射前恢复这种结构。
CPU emitter 按 64-bit word 遍历 mask 的置位 bit；W=1 时遇到 true 可结束，W>1 时
合并全部选中元素。Views 按 selector 来源和 word 分组；连续布局使用偏移表达式，
置换使用静态 offset 表，离散数据源使用 switch 读取既有 SSA 值。W>64 使用原生
`unsigned _BitInt` accumulator，沿用现有 wide ABI。
已有 row-backed 数组直接索引，其他数组值从既有 packed 表示提取元素。仅供 maskedOr
使用、且无需 boundary/frame 快照的 whole-state read 可以作为稳定状态数组的 view，
避免复制整个数组。对已有 boundary/frame 值必须读取该快照，不能重读较新状态。
若 value 的 shape 与底层状态/输入的行布局不同，则通过 packed 值提取，不能直接索引底层行。
64-bit 分段不生成移位 64 位的掩码；只对非零 mask 调用 `countr_zero`。

不支持此 CPU op 的后端应显式拒绝该模型，或先展开为等价 core 图。
