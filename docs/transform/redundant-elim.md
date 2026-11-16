# redundant-elim (内部 Pass)

## 功能概述

`redundant-elim` 是一个内部 pass，由 `simplify` pass 调用，用于内联简单的赋值操作并消除冗余的临时变量。

## 详细说明

该 pass 执行多种局部优化来消除冗余的运算和值，包括赋值内联、公共子表达式消除（CSE）和逻辑优化。

### 优化类型

#### 1. 赋值内联（Assign Inlining）

将简单的 `kAssign` 操作内联到其使用者中：

```
%tmp = kAssign %src
%result = kAdd %tmp, %b
```
优化为：
```
%result = kAdd %src, %b
```

#### 2. 常量传播到输出

将驱动输出端口的常量直接绑定到输出：

```
%const = kConstant { value = 8'hFF }
%out = kAssign %const
```
优化为：
```
%out = kConstant { value = 8'hFF }
```

#### 3. 单操作数连接消除

消除单操作数的 `kConcat`：

```
%result = kConcat %a
```
优化为直接使用 `%a`。

#### 4. 逻辑或优化

检测 `A || !A` 模式并替换为常量 1：

```
%not_a = kLogicNot %a
%result = kLogicOr %a, %not_a
```
优化为：
```
%result = kConstant { value = 1'b1 }
```

#### 5. NOT/XOR 折叠

将 `NOT(XOR(...))` 转换为 `XNOR`：

```
%xor = kXor %a, %b
%result = kNot %xor
```
优化为：
```
%result = kXnor %a, %b
```

#### 6. 公共子表达式消除（CSE）

识别并消除重复的计算：

```
%t1 = kAdd %a, %b
%t2 = kAdd %a, %b  // 重复
```
优化为：
```
%t1 = kAdd %a, %b
// %t2 被替换为 %t1
```

#### 7. 同源连续 slice 的 concat 合并

`simplify` 默认包含这条规则。它在活动调度前恢复 word 数据流，避免先保存多个 bit 中间值、再在 concat 中重新拼接。只改 concat 的输入，保留它的结果、位宽、signedness、端口和名称；原 slice 的其他用户保持不变，后续 DCE 决定哪些 slice 已无用途。

`kConcat` 的 operands 从高位到低位排列。`kSliceStatic` 的唯一 operand 是被截取的源，`sliceStart` 是低位、`sliceEnd` 是高位，均包含端点。例如，32-bit `x`：

```text
concat(slice(x, 31:24), slice(x, 23:16), slice(x, 15:8), slice(x, 7:0))
  -> concat(x)

concat(prefix, slice(x, 23:16), slice(x, 15:8), suffix)
  -> concat(prefix, slice(x, 23:8), suffix)
```

完整源片段直接复用同一 SSA value；部分片段生成一个 unsigned static slice。各 operand 的位宽不变，concat 保留原结果类型，因此 signed 源也不会使 concat 意外发生符号扩展。此规则仅重新排列已有位，不解释 X/Z，适用于二态和四态简化。

合并条件：Logic 类型、合法正位宽、concat 输入总宽度与输出一致、slice 在源范围内且结果宽度准确、相邻 slice 的 source ID 相同且位序连续。间隙、重叠、重复位、反序、不同源、动态 slice 和循环依赖均不合并。不要求 slice 只有一个用户，不移动或复制寄存器、memory 或 effect 的读取。

寄存器 bank 可能表示为 `concat(x31[31], ..., x31[0], x30[31], ..., 32'b0)`；本规则先恢复 `concat(x31, x30, ..., 32'b0)`，之后由正常调度和 emitter 处理。规则不依赖 CPU 型号、信号名或运行 profile。

回归：`transform-concat-slices`，覆盖 signed/unsigned、跨 word 的部分/完整片段、多用户/可观测符号、二态和 X/Z 输入、不合法位区间及不连续负例。

## CSE 候选条件

要成为 CSE 候选，运算必须：

1. 无副作用（不是存储器、实例等）
2. 只有一个结果值
3. 结果不是端口值
4. 结果是临时符号（以 `_val_` 开头）
5. 所有操作数都有效

## 与 Simplify Pass 的关系

该 pass 通常不直接调用，而是通过 `simplify` pass 间接使用：

```bash
# 通过 simplify 间接使用
wolvrix --pass=simplify input.sv
```

## 注意事项

- 这是内部 pass，不对外暴露为独立的 transform 选项
- CSE 使用哈希表进行高效的重复检测
- 属性值在比较时会被排序以确保一致性
- 输出端口的常量驱动会被特殊处理以保留端口名称
