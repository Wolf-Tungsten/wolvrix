# dead-code-elim (内部 Pass)

## 功能概述

`dead-code-elim` 是一个内部 pass，由 `simplify` pass 调用，用于移除未使用的运算操作和值。

## 详细说明

死代码消除通过识别和移除对设计输出没有贡献的运算和值来优化设计。该 pass 使用工作列表算法进行迭代消除。

### 死代码判定标准

运算被视为死代码的条件：

1. **无副作用**：不是存储器、寄存器、锁存器、实例、系统调用等具有副作用的操作
2. **结果未被使用**：所有结果值都没有使用者
3. **非端口值**：结果不是输入、输出或双向端口
4. **非声明符号**：如果启用了声明符号保护，结果不是声明的符号

### 有副作用的操作

以下操作类型被视为有副作用，不会被消除：

- `kRegisterWritePort`, `kRegisterWriteMaskPort`, `kRegisterWriteArrayPort`, `kRegisterWriteArrayMaskPort`, `kRegisterFillPort`
- `kLatchWritePort`
- `kInstance`, `kBlackbox`
- `kSystemFunction`, `kSystemTask`
- `kDpicImport`, `kDpicCall`

### 消除算法

1. **初始化**：计算每个值的使用次数
2. **工作列表**：将死运算加入工作列表
3. **迭代消除**：
   - 从工作列表取出运算
   - 删除运算，减少其操作数的使用计数
   - 如果操作数使用计数归零且满足死代码条件，加入工作列表
4. **值清理**：删除未使用的值

## 与 Simplify Pass 的关系

该 pass 通常不直接调用，而是通过 `simplify` pass 间接使用：

```bash
# 通过 simplify 间接使用
wolvrix --pass=simplify input.sv
```

## 优化效果

- 移除未连接的中间结果
- 消除优化过程中产生的临时变量
- 减少设计的逻辑复杂度

## 注意事项

- 这是内部 pass，不对外暴露为独立的 transform 选项
- 端口值和声明的符号受到保护，不会被删除
- 迭代过程确保级联的死代码被完全消除
- 可以配置 `keepDeclaredSymbols` 选项保护特定符号
