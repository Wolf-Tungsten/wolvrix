# GrhSIM CPU Dialect

本文定义 `cpu` 方言的类型。`cpu` 方言是 CPU 后端的配套方言，目前只定义类型，不定义 op
和外部函数声明。方言的一般结构见 [GrhSIM IR Overview](../overview.md) 第 2 章。

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
