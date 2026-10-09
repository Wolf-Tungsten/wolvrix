# memory-init-check

## 功能概述

`memory-init-check` pass 验证数组寄存器（带 `shape` 属性的 kRegister）初始化属性的一致性，
确保合并的数组寄存器具有兼容的初始化配置。

## 详细说明

在硬件设计中，数组状态可以通过多种方式初始化（如文件加载、常量填充等）。当多个数组寄存器实例被合并时，它们的初始化配置必须一致。该 pass 检测配置不一致的情况。

### 检查的属性

| 属性 | 类型 | 说明 |
|------|------|------|
| `initKind` | `string[]` | 初始化类型（如 `"file"`、`"const"`） |
| `initFile` | `string[]` | 初始化文件路径 |
| `initStart` | `int64[]` | 初始化起始地址 |
| `initLen` | `int64[]` | 初始化长度 |

### 验证规则

1. **完整性检查**：
   - `initKind` 和 `initFile` 必须同时存在且长度相同
   - `initStart` 和 `initLen` 必须存在且与其他数组长度匹配

2. **一致性检查**：
   - 同一符号的存储器实例必须具有相同的初始化配置
   - 所有数组的长度必须一致

## 使用示例

```bash
# 基本使用
wolvrix --pass=memory-init-check input.sv

# 在综合流程中使用
wolvrix --pass=simplify --pass=memory-init-check input.sv
```

## 错误信息

```
# 属性不完整
array register init attributes are incomplete (missing initKind/initFile)

# 数组长度不匹配
array register initKind/initFile size mismatch

# 初始化配置不一致
array register init attributes differ for merged register '<symbol>'
```

## 注意事项

- 该 pass 仅进行检查，不会修改 IR
- 发现错误会导致 pass 失败
- 建议在存储器优化 passes 之后运行
- 对于无初始化属性的存储器，pass 会跳过检查
