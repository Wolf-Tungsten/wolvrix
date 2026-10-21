# simplify

## 功能概述

`simplify` pass 是一个综合性的优化 pass，迭代地执行常量折叠、冗余消除和死代码消除，以简化设计并提高性能。

## 详细说明

该 pass 按子 pass 的依赖关系迭代执行，直到没有需要重跑的子 pass 或达到最大迭代次数。

### 子 Passes

| Pass | 功能 |
|------|------|
| `const-fold` | 常量折叠，计算编译时常量表达式 |
| `redundant-elim` | 冗余消除，内联简单赋值并消除重复运算 |
| `dead-code-elim` | 死代码消除，移除未使用的运算和值 |

### 迭代流程

```
dirty = {const-fold, redundant-elim, dead-code-elim}
for iter in 0..<maxIterations:
    按 const-fold → redundant-elim → dead-code-elim 的顺序运行 dirty 子 pass
    根据各子 pass 的变化更新 dirty
    if dirty is empty: break
```

常量折叠内部使用工作队列：一次扫描加入可折叠操作，折叠结果重定向之前将其
用户入队。同一操作不会重复待处理，成功折叠的操作不再入队；操作擦除延迟到
队列排空后。即使操作存储顺序是 `out = sum + 1`、`sum = 2 + 3`，也能在
一次常量折叠调用中得到 `out = 6`。队列按图独立，不假设存储顺序是拓扑顺序。

| 已执行子 pass 的变化 | 后续需要运行的子 pass |
| --- | --- |
| 常量折叠有变化 | 冗余消除、DCE |
| 常量折叠后的切片或比较简化有变化 | 另需重跑常量折叠，处理新暴露的常量输入 |
| 冗余消除有变化 | 常量折叠、冗余消除、DCE |
| DCE 有变化 | 冗余消除：删除用户可能使赋值、concat 或 NOT/XOR 满足单用户条件 |
| 子 pass 无变化 | 不使其他子 pass 失效 |

DCE 在首轮裁剪图，之后积累删除需求，在常量折叠和冗余消除稳定后执行；到达
`maxIterations` 上限时也执行待处理的 DCE。这样中间每次别名替换不必立即
重算全图活性。DCE 已完成一次全图活性闭包，单独删除死操作不会产生新常量输入，因此不触发
下一轮常量折叠或 DCE。常量池仍在每次 `const-fold` 调用时重建，不跨外层迭代缓存。

冗余消除的 CSE 同样使用图内工作队列，合并结果前将被替换值的用户入队，
使逆序存放的重复表达式链在一次调用中完成合并。每次替换输入都更新用户的
版本标记，哈希表命中时校验代表操作的版本；已擦除或签名过期的条目不能作为
合并目标，无需为每次命中重新构造代表操作的签名。类型和 shape 检查照常执行。

日志保留 `simplify iteration=... changed=...`，并列出实际执行的子 pass、
各自的 changed 值和耗时；没有出现在该轮日志中的子 pass 已跳过。

## 配置选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `-max-iter` | 8 | 最大迭代次数 |
| `-x-fold` | `known` | X 值处理模式 |
| `-semantics` | `4state` | 语义模式 |

### X 值处理模式

控制常量折叠时如何处理包含 X/Z 值的情况：

| 模式 | 说明 |
|------|------|
| `strict` | 严格模式，操作数包含 X/Z 时不进行折叠 |
| `known` | 已知模式，折叠结果包含 X/Z 时不传播（默认） |
| `propagate` | 传播模式，即使结果包含 X/Z 也传播，仅发出警告 |

### 语义模式

| 模式 | 说明 |
|------|------|
| `2state` | 二值逻辑（0/1） |
| `4state` | 四值逻辑（0/1/X/Z）（默认） |

## 使用示例

```bash
# 基本使用
wolvrix --pass=simplify input.sv

# 增加迭代次数
wolvrix --pass=simplify:-max-iter=16 input.sv

# 使用二值语义
wolvrix --pass=simplify:-semantics=2state input.sv

# 严格 X 处理
wolvrix --pass=simplify:-x-fold=strict input.sv

# 组合配置
wolvrix --pass=simplify:-max-iter=16:-x-fold=strict:-semantics=2state input.sv
```

## 优化效果

- **减少运算数量**：常量折叠消除了运行时计算
- **简化数据流**：冗余消除减少了中间变量
- **降低资源使用**：死代码消除移除了未使用的逻辑

## 注意事项

- 迭代次数过多可能增加编译时间而收益递减
- 二值语义会改变设计的 X/Z 行为
- 建议在综合流程的早期运行此 pass
- 可以通过日志级别查看每次迭代的优化统计
