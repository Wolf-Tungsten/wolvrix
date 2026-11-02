# hier-flatten

## 功能概述

`hier-flatten` pass 将层次化的模块设计扁平化为单个模块，通过内联所有实例操作来实现。

## 详细说明

该 pass 递归地将子模块的实例内联到父模块中，创建一个扁平化的设计表示。这在综合优化、形式验证和某些仿真流程中非常有用。

### 扁平化过程

1. **实例发现**：识别所有模块实例操作（`kInstance`）
2. **递归内联**：深度优先地内联子模块的运算和值
3. **符号重命名**：使用层级分隔符（`$`）保持名称唯一性
4. **端口连接处理**：将子模块的端口连接转换为内部的运算操作数

内联前先冻结可安全压缩的非顶图源模块，回收 builder 中已删除操作和值的
存储，并使用连续的只读存储供所有实例共享。冻结会重编号 op/value ID，
因此发生在保存端口 span、实例列表和映射之前；活操作的相对顺序、结果、
类型、shape、属性、源码位置和图级 metadata 保留。源图对象继续保留到
既有的成功清理阶段，`-preserve-modules` 和失败路径仍保留相应源图。

冻结会按名字排序端口。只有某个源图的全部实例和黑盒引用都完整具名连接时，
该源图才参与压缩；任一引用依赖位置顺序时保留原 builder，继续使用原端口
顺序。空端口族可省略名字列表。压缩用于降低生成过程的存储成本，最终
C++ 执行成本仍由生成操作和后端代码决定。

克隆时按源图 ValueId 的 index 保存映射，并校验 graph 和 generation；
删除值留下的空洞视为未映射。多个子模块输入或 inout 输入共享一个 ValueId
时保留首次连接，具名连接按实例的端口名字列表确定顺序。名称映射只记录
`regSymbol`、`latchSymbol`、`targetImportSymbol` 引用及 generate group 成员
需要的操作；只有源图含 generate groups 时才记录值名映射。前向引用仍在
克隆后统一回填，未解析引用保留原诊断。

FIRRTL 节点 key 的实例路径限定使用每次递归内联独立的缓存。缓存持有
不可变路径及源/目标 key 字符串，不借用属性存储或递归 push/pop 的路径；
每个操作仍经过原有节点属性读取、normalize、schema、root result 和数量
上限检查。

### 符号保护模式

declaredSymbol（declared 的 value/op）在任何模式下都会保留为层次路径名并维持 declared 身份；未声明的子图符号一律改用内部名（`_val_N`/`_op_N`）。`symProtect` 模式目前只影响一种情形：父图侧**未声明**的端口映射值是否改名为子图的层次端口名。

| 模式 | 说明 |
|------|------|
| `all` | 父图未声明端口值改名为层次端口名（默认） |
| `hierarchy` | 同 `all` |
| `stateful` | 不改名父图端口值 |
| `none` | 不改名父图端口值 |

## 配置选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `-preserve-modules` | false | 保留被扁平化的模块定义 |
| `-sym-protect` | `all` | 符号保护模式（见上表） |

## 使用示例

```bash
# 基本扁平化
wolvrix --pass=hier-flatten input.sv

# 保留原始模块定义
wolvrix --pass=hier-flatten:-preserve-modules input.sv

# 仅保护状态元素
wolvrix --pass=hier-flatten:-sym-protect=stateful input.sv

# 不保护任何符号
wolvrix --pass=hier-flatten:-sym-protect=none input.sv
```

## 内联命名约定

扁平化后的符号名称遵循以下格式：

```
<instance_path>$<original_symbol>
```

其中 `<instance_path>` 由逐层实例名以 `$` 连接（如 `u_m1$u_leaf`）。例如：`u_m1$u_leaf$w` 表示实例路径 `u_m1.u_leaf` 下的 `w` 信号。顶图符号保持原名，作为层次路径之根；端口映射冲突时父图 declared 名优先，子图端口名不保留。名称冲突时追加 `_N` 后缀去重。

generate 块内声明的符号在 ingest 期已含作用域分量（`gen_loop$0$sig`），扁平化时实例前缀叠加在其前（`u_m1$gen_loop$0$sig`）；对应的 generate 副本分组（`generateGroups`）随之传播：组的 `scope` 叠加实例前缀（`u_m1$gen_loop`），`name` 保持裸声明名，成员重映射到改名后的符号；成员被丢弃的组会收缩，空组不写入主图。同一模块的多实例产生各自独立的组。

## 注意事项

- 递归实例（循环层次结构）会被检测并跳过
- 黑盒模块（如果未被转换）不会被内联
- 大量内联可能导致 IR 膨胀，影响内存使用
- 建议在扁平化之前运行 `multidriven-guard` 检测潜在的驱动冲突
