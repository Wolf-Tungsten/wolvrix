# xmr-resolve

## 功能概述

`xmr-resolve` pass 解析跨模块引用（Cross-Module References, XMR），通过添加端口和实例连接来实现层次引用的解析。

## 详细说明

SystemVerilog 支持通过层次路径（如 `top.sub.mod.signal`）访问其他模块中的信号。该 pass 将这种引用转换为标准的端口连接，使设计能够被扁平化和综合。

### 支持的 XMR 类型

| 类型 | 说明 |
|------|------|
| XMR 读（`kXMRRead`） | 读取其他模块中的信号值 |
| XMR 写（`kXMRWrite`） | 写入其他模块中的信号 |

### 解析策略

1. **XMR 读解析**：
   - 在目标模块中创建输出端口或存储器读端口
   - 通过层级向上传播，在每个父模块中创建相应的输出端口
   - 在实例操作中添加输出连接

2. **XMR 写解析**：
   - 在目标模块中创建输入端口或存储器/寄存器/锁存器写端口
   - 通过层级向上传播，在每个父模块中创建相应的输入端口
   - 在实例操作中添加输入连接

## 端口命名

创建的端口名称格式：
```
__xmr_r_<path>    // 读端口
__xmr_w_<path>    // 写端口
```

路径长度超过 64 字符时会使用哈希值。

## 使用示例

```bash
# 基本使用
wolvrix --pass=xmr-resolve input.sv

# 通常与扁平化结合使用
wolvrix --pass=xmr-resolve --pass=hier-flatten input.sv
```

## 支持的存储器类型

XMR 可以指向以下存储元素：

- **寄存器（Register，标量）**：创建 `kRegisterReadPort` 或 `kRegisterWriteMaskPort`
- **锁存器（Latch）**：创建 `kLatchReadPort` 或 `kLatchWritePort`
- **寄存器（Register，数组，shape 非空）**：创建 `kRegisterWriteArrayMaskPort`（读需要显式地址）

## 注意事项

- XMR 写的目标如果已存在驱动，会触发警告并替换
- 写入输入端口会断开原始输入连接
- 不支持对 inout 端口的 XMR 写操作
- 存储器 XMR 读需要显式地址操作数
