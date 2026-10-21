# const-fold (内部 Pass)

## 功能概述

`const-fold` 是一个内部 pass，由 `simplify` pass 调用，用于执行常量折叠优化。它计算编译时可以确定的常量表达式，并将其替换为常量值。

## 详细说明

常量折叠通过评估编译时已知的表达式来简化设计。该 pass 支持多种运算类型和配置选项。

### 支持的运算类型

| 类别 | 运算 |
|------|------|
| 算术 | `kAdd`, `kSub`, `kMul`, `kDiv`, `kMod` |
| 比较 | `kEq`, `kNe`, `kCaseEq`, `kCaseNe`, `kWildcardEq`, `kWildcardNe`, `kLt`, `kLe`, `kGt`, `kGe` |
| 位运算 | `kAnd`, `kOr`, `kXor`, `kXnor`, `kNot` |
| 逻辑运算 | `kLogicAnd`, `kLogicOr`, `kLogicNot` |
| 归约运算 | `kReduceAnd`, `kReduceOr`, `kReduceXor`, `kReduceNor`, `kReduceNand`, `kReduceXnor` |
| 移位运算 | `kShl`, `kLShr`, `kAShr` |
| 数据选择 | `kMux` |
| 位操作 | `kConcat`, `kReplicate`, `kSliceStatic`, `kSliceDynamic`, `kSliceArray` |
| 其他 | `kAssign`, `kSystemFunction` |

### 优化步骤

1. **常量收集**：识别并收集所有常量运算
2. **工作队列折叠**：评估可折叠的表达式；替换结果前将用户入队，直到队列排空
3. **切片简化**：优化冗余的切片操作
4. **无符号比较简化**：优化无符号比较运算
5. **死常量消除**：所有常量创建结束后移除未使用的常量，避免池中条目指向已删除的值

工作队列在图内去重，成功折叠的操作不再处理，最后统一擦除操作并保留声明符号。
切片或比较简化发生在队列处理之后，通过 `needsRevisit()` 通知 `simplify`
再次执行常量折叠；仅常量折叠或死常量删除不要求自身再扫一遍。

## 配置选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `xFold` | `Known` | X 值处理模式 |
| `semantics` | `FourState` | 语义模式 |

### X 值处理模式

控制常量折叠时如何处理包含 X/Z 值的情况：

| 模式 | 说明 |
|------|------|
| `Strict` | 严格模式，操作数包含 X/Z 时不进行折叠 |
| `Known` | 已知模式，折叠结果包含 X/Z 时不传播（默认） |
| `Propagate` | 传播模式，即使结果包含 X/Z 也传播，仅发出警告 |

### 语义模式

| 模式 | 说明 |
|------|------|
| `TwoState` | 二值逻辑（0/1），强制使用 Strict X 处理模式 |
| `FourState` | 四值逻辑（0/1/X/Z）（默认） |

## 与 Simplify Pass 的关系

该 pass 通常不直接调用，而是通过 `simplify` pass 间接使用：

```cpp
// simplify pass 内部使用
pm.addPass(std::make_unique<ConstantFoldPass>(foldOptions));
pm.addPass(std::make_unique<RedundantElimPass>());
pm.addPass(std::make_unique<DeadCodeElimPass>());
```

## 注意事项

- 这是内部 pass，不对外暴露为独立的 transform 选项
- 使用 slang 库的 `SVInt` 进行精确的 Verilog 数值计算
- 支持四值逻辑（0/1/X/Z）的完整语义
- 常量池用于去重相同的常量值
