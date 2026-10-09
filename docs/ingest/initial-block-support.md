# ingest: initial 块行为支持说明

本文档说明 `ingest` 对 Verilog/SystemVerilog `initial` 块的当前支持边界，以及它在 GRH 中的落地形式。

## 1. 总体原则

- `initial` 主要被当作**初始化信息来源**，而不是时序写端口来源。
- 能静态归约成初始化值的内容，会写入 `kRegister`（标量或带 shape 的数组寄存器）的初始化属性。
- 不能归约成初始化值的 `initial` 赋值，不会变成 `kRegisterWritePort`。

## 2. 支持矩阵（当前行为）

| SV 形式 | 当前支持 | GRH 落地 |
|---|---|---|
| `logic a = 1;`（声明初始化） | 支持 | `kRegister(symbol=a, initValue=...)` + `kRegisterReadPort(regSymbol=a)` |
| `initial a = 1;`（简单常量） | 支持 | 记录为 `Register initValue`，不是 write port |
| `initial a = $random;` | 支持 | 记录为 `initValue="$random"` |
| `initial a = b;`（非常量 RHS） | 不支持 one-shot 写语义 | 不生成 `kRegisterWritePort`，该赋值最终被忽略并告警 |
| `initial $readmemh(...)` / `$readmemb(...)` | 支持 | 写入数组 `kRegister`（shape 非空）的 `initKind/initFile/initStart/initLen` |
| `initial $display/...` 等系统任务 | 支持（任务级） | 生成 `kSystemTask`，`procKind="initial"`；通常无 `eventEdge` |

## 3. `initial` 赋值为何没有 `updateCond/data`

`updateCond` / `data` 是 `kRegisterWritePort` 或 `kLatchWritePort` 的操作数字段。  
当前 ingest 设计下，`initial` 赋值不降为 write port，而是：

1. 能提取初始化值时，转成 `initValue` 属性；
2. 不能提取时，不保留 one-shot 写语义（会被丢弃）。

因此像下面这类代码：

```systemverilog
logic a, b;
initial begin
  a = b;
end
```

`a` 不会有 write port，自然也不存在该 write port 的 `updateCond` 和 `data`。

## 4. 两条处理路径

### 4.1 初始块可求值路径（优先）

- ingest 会尝试对 `initial` 块做 AST 常量求值。
- 成功时，将寄存器/存储器最终值写入 `registerInits` / `memoryInits`。
- 这一路径保留的是“初始化结果”，不是过程性赋值序列。

### 4.2 语句级回退路径

- 若整块不可求值，退回逐语句 lowering。
- 对可识别的简单赋值（字面量、`$random`）仍会记为 `initValue`。
- 其他赋值会先进入 write-intent，但因 `initial` 没有边沿事件，后续 write-back 阶段会跳过，并给出告警：
  - `Skipping sequential write without edge-sensitive timing control; initial/final assignments are ignored`

## 5. 与 always_ff/always_latch 的区别

- `always_ff` / `always_latch`：会生成 `kRegisterWritePort` / `kLatchWritePort`，因此有 `updateCond`、`data`、`mask`、`eventEdge`。
- `initial`：以初始化属性为主，不生成上述写端口。

## 6. 当前限制与建议

- 目前不建模“仿真时刻 0 的一次性过程赋值”（one-shot write）。
- 若希望稳定进入 IR，请尽量使用：
  - 声明初始化（如 `logic x = 0;`）
  - 或可静态提取的 `initial` 初始化形式（如字面量 / `$random` / `$readmemh`）。
