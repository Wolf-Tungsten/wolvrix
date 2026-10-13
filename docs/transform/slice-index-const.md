# slice-index-const

## 功能概述

`slice-index-const` pass 将位偏移为常量的动态位切片（`kSliceDynamic`）转换为静态位切片（`kSliceStatic`），以便后续优化。
数组访问 `kSliceArray` 保留原操作，即使所有下标都是常量；其 shape 信息供后续数组寻址使用。

## 详细说明

动态位切片操作在编译时位偏移已知的情况下，可以转换为等效的静态位切片操作。这种转换允许后续 passes 进行更多的常量传播和优化。

### 转换条件

满足以下条件时，动态切片会被转换：

1. 索引值可以计算为编译时常量整数
2. 索引值非负且在有效范围内
3. 切片宽度明确指定且为正数
4. 结果位宽与切片宽度匹配

### 支持的常量表达式

pass 可以计算以下类型的常量表达式：

- 常量字面量（如 `8'hFF`、`32'd100`）
- 赋值传播（`kAssign`）
- 静态切片（`kSliceStatic`）
- 连接操作（`kConcat`）
- 算术运算（`kAdd`、`kSub`、`kMul`、`kShl`、`kLShr`）

## 转换效果

转换前：
```
%result = kSliceDynamic %base, %index { sliceWidth = 8 }
```

转换后（假设 `%index` 计算为 16）：
```
%result = kSliceStatic %base { sliceStart = 16, sliceEnd = 23 }
```

## 使用示例

```bash
# 基本使用
wolvrix --pass=slice-index-const input.sv

# 通常与 simplify pass 结合使用
wolvrix --pass=simplify --pass=slice-index-const --pass=simplify input.sv
```

## 优化收益

- **简化后续分析**：静态切片更容易进行位范围分析
- **启用更多优化**：为 `comb-loop-elim` 等 passes 提供更多优化机会
- **减少运行时开销**：消除动态索引计算的硬件开销

## 注意事项

- 该 pass 仅处理非负整数索引
- 对于复杂的索引表达式，常量计算可能受限
- 建议在常量折叠 pass 之后运行以获得最佳效果
