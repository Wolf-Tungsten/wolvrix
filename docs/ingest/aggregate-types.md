# ingest 聚合类型支持（数组与结构体）

本文是 ingest 对 SystemVerilog 聚合类型（数组、结构体）处理的规范。它对应
`pdocs/grh-ir-array-support/` stage-04 / stage-08 的实现，目标是：在不违反 GRH 类型系统
（同质数组）的前提下**尽可能保留源级聚合结构**，不再把一切平化成单向量。

## 1. 类型映射决策树

对变量 / net / 端口的声明类型，按以下优先级判定 GRH 形态：

1. **同质数组**（packed 数组、unpacked 数组、二者嵌套，元素为位类型）：**全维进 shape**。
   - shape = 各维 extent 的规范序列：unpacked 维为外层（leading）、packed 维为内层
     （trailing），元素 = 最内层位类型。
   - 例：`logic [3:0][7:0] a [0:5]` → 总位宽 192、shape=[6,4]、element=8bit；
     纯 packed `logic [3:0][7:0] b` → 总位宽 32、shape=[4]、element=8bit。
   - shape 是纯结构视图，不改变任何 op 语义（packed-first：扁平语义即正确语义）。
   - SignalInfo/TypeResolution 的 `width` 字段语义：数组 = 元素位宽（最内层位类型），
     标量 = 总位宽；GRH value 的 width 恒为总位宽（width × ∏shape）。
2. **结构体：字段级拆分（field-wise split，SoA）——struct 永远不成为 GRH value**。
   声明 / net / 端口 / 嵌套中的 struct 一律递归拆成叶子字段信号，命名
   `c__DOT__a`（DOT 编码），数组维度迁移到每个字段信号上（详见 §3）。
3. **数组状态的存储形态**：带 shape 的变量在时序域被写或为数组视图时，统一落成
   kRegister + shape + 端口族（kRegisterReadArrayPort / WritePort / WriteMaskPort /
   WriteArrayPort / WriteArrayMaskPort / FillPort，多索引、部分索引=子数组）。
   "reg 还是 mem" 不再是 ingest 的决策——regLatch/mem 分类全部由 grhsim 后端按
   规模 / 访问模式推导。
4. **数组端口不再平化**：端口 value 带 shape；struct 端口同样字段级拆分为叶子端口。
5. **动态 / 关联数组、队列**：报错。union 报错（字段重叠与拆分不兼容）。
   Real/String 的任何数组化报错。

### 1.1 同型元素构造

字段拆分后的 struct 数组 concat 若每项恰为一个同型外层元素、目标几何为 `[N]+S`，
直接产生 `kArrayCreate`；SV MSB 优先次序转换为规范化低元素优先次序。
数组 of struct assignment pattern 的逐元素组装同样使用该 op；packed/unpacked 的
索引方向继续沿用原来的归一化规则。异宽、跨元素边界或其他目标几何的拼接仍为 `kConcat`。
新 op 的结果类型由实际元素推导，不先生产扁平整数再恢复 shape。

## 2. 数组状态的分类阈值

- unpacked 数组变量：恒为数组状态（memoryRows>0）。
- 纯 packed 数组变量：仅当 **shape.front() > 1 且元素宽 > 1** 时为数组状态；
  低于阈值时折回为总位宽标量（shape 清空），与 stage-04 之前的扁平行为逐位等价。
- **struct 叶子字段**：阈值为 **shape.front() > 1**（不再要求元素宽 >1）——叶子
  元素访问是成员链访问，要数组视图；1-bit 元素的叶子数组同样进数组状态。
- 折回的叶子保留 `elementWidth`（SignalInfo.elementWidth），其元素选择退化为
  普通位切片（element width × 剩余维宽）。

## 3. struct 字段级拆分规则

- **拆分**：`splitStructType` 沿类型树递归 FieldSymbol（Packed/UnpackedStructType），
  收集路径上的全部数组维度（unpacked + packed）迁移到每个叶子信号。
- **命名**：叶子信号名 = `根名__DOT__字段__DOT__子字段`（如
  `robEntries__DOT__ftqIdx__DOT__value[352]`）。
- **维度迁移顺序**：外层维度在前、字段自身维度在后；unpacked 维永远前导。
- **own/outer packed 边界**：每个 struct 层级记录 packed 维边界——只有叶子**自身
  声明的** packed 维可降级为元素位宽；外层（包围 struct 的数组维）永远是 shape 维。
  例：`struct packed {logic [4:0] ssid;}[7:0]` → ssid 叶子 shape=[8]、element=5；
  `struct packed {logic flag;}[31:0][3:0]` → flag 叶子 shape=[32,4]、element=1。
- **嵌套**：嵌套 struct 继续拆（`struct { struct { x, y } f, 标量 b } c[n]` →
  `c__DOT__f__DOT__x[n]`、`c__DOT__b[n]`）。
- **整体操作逐字段展开**：整体赋值 = 逐字段连接（handleStructAssignment）；
  整体相等 = 逐字段 kEq 归约；整体 mux = 同 cond 逐字段 kMux；`'{...}` 字面量 =
  逐字段构造（含数组-of-struct 的逐索引模式，如 ROB 复位）；数组-of-struct 的
  元素范围选择（`arr[base +: n]`）= 逐叶子数组的范围切片。
- **扁平布局重建**：需要 packed 位布局的场合（cast/算术/$bits/整体端口连接/
  XMR/调试引用）按声明顺序 kConcat 现场重建——布局是类型的纯函数。
- **初始化**：`wire struct x = '{...}` 与变量声明初始化经 handleStructRootInitializer
  逐字段展开。
- **端口与实例**：struct 端口拆为叶子端口（`io__DOT__bits__DOT__...`），实例连接
  逐叶进行（输入 = 扁平连接 + 静态切片；输出 = 逐叶 result 绑定到父侧叶子）。

## 4. 访问 lowering

| 源形态 | lowering |
|--------|----------|
| 数组状态元素读 | kRegisterReadArrayPort 多索引（1..rank，部分索引=子数组） |
| 数组状态元素写 | kRegisterWriteArrayPort / WriteArrayMaskPort 多索引；行内位/段写合成 mask |
| 数组状态整体读 | kRegisterReadPort（数组 value 单定义） |
| 数组状态整体写 | kRegisterWritePort / FillPort |
| net/组合数组元素读 | kSliceArray 多索引（常量索引保留数组访问） |
| 成员访问（读/写） | 成员链解析到拆分叶子信号 + 剩余索引（无专用位段路径） |
| 折回叶子元素选择 | 位切片（element width × 剩余维宽） |

索引归一化：所有维度（unpacked 的 left/right/downto/upto、packed 的 lane 方向）在
ingest 内部归一化为 0..extent-1 下标，不进入 GRH。

整体 struct 赋值的每个叶子写值按 **选中 LHS 的 SV 类型** 定宽，不能使用底层
存储的总位宽代替。例如 `Entry [0:0][3:0] entries` 中 `Entry.setIdx` 为 7 位，
`entries[0][0] <= rhs` 必须先得到 7 位的 `rhs.setIdx`，再写回对应片段：

```text
entries__DOT__setIdx_next = concat(old[27:7], rhs.setIdx[6:0])  // 21 + 7 = 28
```

该叶子的 shape 即使因前导 extent=1 被折回标量，选中元素仍为 7 位。
提前把写值补到存储宽度会产生 `concat(old[27:7], zero[20:0], rhs[6:0])`，
总共 49 位，截回 28 位后会清除其他元素。静态索引、动态索引及嵌套 struct
成员赋值均须保持同一规则。

逐层读取折回叶子时，每次消费一个维度也必须同步缩小当前总位宽。
例如 192 位的 `[0:0][3:0][7:0]` 叶子（元素为 6 位）依次变为
`192 → 192 → 48 → 6` 位；最后一维的 lane 步长为 6，不能继续使用最初的
192 位计算出 `192 / 8 = 24`。这一规则同时适用于语句表达式和实例连接表达式。

## 5. 支持矩阵

| 形态 | 处理 |
|------|------|
| 标量位向量 | 原样 |
| packed 数组（标量元素） | 全维进 shape（packed 变量按 §2 阈值） |
| unpacked 数组 | 全维进 shape + 数组状态 |
| 多维混合（unpacked×packed） | 同维进 shape（unpacked 前导） |
| packed/unpacked struct | 字段级拆分（SoA） |
| struct 数组（packed/unpacked） | 字段级拆分，维度迁移到叶子 |
| union | 报错 |
| 动态/关联数组、队列 | 报错 |
| Real/String 数组 | 报错 |
| 含非位字段的 struct | 报错 |

## 6. slang 兼容性（wolvrix/keyword-member-names 分支）

firtool 会以 SV 关键字作为字段名（Chisel 源信号名，如 `from`/`until`）。vendored
slang 分支 `wolvrix/keyword-member-names` 在语法上接受这些位置的关键字名字：
声明名（parseDeclarator/parseDeclarators）、点分名段（parseNamePart）、成员访问
（parseMemberAccess）、结构化赋值模式键名（首键与后续键）。原理与上游
Unique/And/Or/Xor 成员名特例一致：这些语法位置只接受名字，关键字无歧义。

已知环境级限制（非 ingest 缺陷，S7 管线口径待决）：
- `xs_assert_v2` 裸调用（未声明的函数）需要 assert_dpi.sv 式 DPI import 声明
  （xs-bugcase 用法）或以 `-D SYNTHESIS` 剥离断言块（综合形态）。
- 巨型稀疏数组（如 difftest Mem1R1WHelper 的 ~1.1e12 项）超出 slang 的 int32
  数组维界限（evalRange int32 强转）——Verilator 风格写法，slang 不可详细化。
