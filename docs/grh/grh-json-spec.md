# GRH JSON 规范（StoreJson）

StoreJson 输出 GRH 设计的紧凑 JSON 表示，统一 CLI 与测试的入口。默认写入 `grh.json`（位于 `StoreOptions::outputDir`，缺省为当前工作目录），并通过 `StoreOptions::jsonMode` 控制排版：
- `Compact`: 单行最紧凑格式。
- `PrettyCompact`（默认）：保持顶层/Graph 缩进，但 `vals`/`ports`/`ops` 元素各占一行。
- `Pretty`: 全量缩进换行（每个字段独立一行）。

## 顶层结构
- `graphs`: 按图名升序排列的数组，每个元素为单个 Graph 描述。
- `aliases`: 可选，别名对象。键为 alias，值为目标 Graph 名称（symbol）。
- `declaredSymbols`: 用户定义模块名数组（必需，缺失视为错误）。
- `tops`: 顶层图名数组，顺序遵循 `Store` 解析后的顶层列表（包括 `topOverrides`）。

## Graph 结构（按字段顺序输出）
- `symbol`: 图名称（必需，缺失视为错误）。
- `declaredSymbols`: 模块内部声明的符号名数组（必需）。
- `generateGroups`: 可选，generate 块内声明的副本分组数组（仅非空时写出，缺失视为空集合）。每项 `{ scope, name, symbols }`，均为符号文本：
  - `scope`: 无轮次的作用域路径（嵌套时以 `$` 连接块名，如 `gen_loop`、`outer$inner`）；hier-flatten 后叠加实例路径前缀（如 `u_inst$gen_loop`）。
  - `name`: 裸声明名（flatten 不改动该字段）。
  - `symbols`: 按 elaboration 顺序排列的副本符号名（如 `gen_loop$0$sig`；flatten 后为 `u_inst$gen_loop$0$sig`）。
- `vals`: 值列表，保持创建顺序。每个值包含：
  - `sym`: 符号名（必需，缺失视为错误）。
  - `w`: 位宽。
  - `sgn`: 是否有符号。
  - `in` / `out` / `inout`: 输入/输出/inout 标记（`inout` 不可与 `in/out` 同时为真）。
  - `def`: 可选，定义该值的 Operation 符号。
  - `users`: 使用者数组 `{ op, idx }`，描述操作与操作数索引。
  - `loc`: 可选，源码位置与扩展调试信息 `{ file, line, col, endLine, endCol, origin, pass, note }`。
- `ports`: 端口对象。
  - `in`: 输入端口数组，每项 `{ name, val }`。
  - `out`: 输出端口数组，每项 `{ name, val }`。
  - `inout`: inout 端口数组，每项 `{ name, in, out, oe }`。
- `ops`: 操作列表，保持创建顺序。每个操作包含：
  - `sym`: 符号名（必需，缺失视为错误）。
  - `kind`: 操作类型字符串（来自 `wolvrix::lib::grh::toString(OperationKind)`）。
  - `in` / `out`: 操作数与结果符号数组，保持插入顺序。
  - `attrs`: 可选属性对象，键为属性名，值为属性负载。
  - `loc`: 可选，源码位置与扩展调试信息 `{ file, line, col, endLine, endCol, origin, pass, note }`。

## 存储单元 Port 操作
- 存储声明类操作使用 `sym` 作为声明名，`attrs` 仅承载类型信息。
- `kRegister`
  - `in`: 为空
  - `out`: 为空
  - `attrs.width`: int
  - `attrs.isSigned`: bool
- `kRegisterReadPort`
  - `in`: 为空
  - `out`: 单个 result（读值）
  - `attrs.regSymbol`: string
- `kRegisterWritePort`
  - `in`: `[updateCond, nextValue, mask, event0, event1, ...]`
  - `out`: 为空
  - `attrs.regSymbol`: string
  - `attrs.eventEdge`: string[]
- `kLatch`
  - `in`: 为空
  - `out`: 为空
  - `attrs.width`: int
  - `attrs.isSigned`: bool
- `kLatchReadPort`
  - `in`: 为空
  - `out`: 单个 result（读值）
  - `attrs.latchSymbol`: string
- `kLatchWritePort`
  - `in`: `[updateCond, nextValue, mask]`
  - `out`: 为空
  - `attrs.latchSymbol`: string
- `kMemory`
  - `in`: 为空
  - `out`: 为空
  - `attrs.width`: int
  - `attrs.row`: int
  - `attrs.isSigned`: bool
- `kMemoryReadPort`
  - `in`: `[addr]`
  - `out`: 单个 result（读值）
  - `attrs.memSymbol`: string
- `kMemoryWritePort`
  - `in`: `[updateCond, addr, data, mask, event0, event1, ...]`
  - `out`: 为空
  - `attrs.memSymbol`: string
  - `attrs.eventEdge`: string[]

## XMR 操作
- `kXMRRead`
  - `in`: 为空
  - `out`: 单个 result
  - `attrs.xmrPath`: string，层次路径
- `kXMRWrite`
  - `in`：
    - 非存储目标：`[data]`
    - `kRegister`：`[updateCond, nextValue, mask, event0, event1, ...]`
    - `kLatch`：`[updateCond, nextValue, mask]`
    - `kMemory`：`[updateCond, addr, data, mask, event0, event1, ...]`
  - `out`: 为空
  - `attrs.xmrPath`: string，层次路径
  - `attrs.eventEdge`: string[]，仅 `kRegister/kMemory` 目标必需（长度需匹配事件 operand 数）

说明：`kXMRRead/kXMRWrite` 是中间表示，必须在 emit 之前被 resolve pass 展开，
JSON 中若仍保留该 op，表示流程未完成。

## 属性编码
- 属性负载统一为对象 `{ t: <type>, v?: <scalar>, vs?: <array> }`。
- 支持的 `t`：`bool`、`int`、`double`、`string`、`bool[]`、`int[]`、`double[]`、`string[]`。
- 标量使用 `v`，数组使用 `vs`，数组条目保持给定顺序；非有限浮点将被拒绝。

## 排序与布局约定
- Graph 列表按名称排序；`vals`、`ops` 保留创建顺序；端口数组顺序来自 Graph 端口列表（GraphView 冻结路径会按端口名排序）；`attrs` 保持插入顺序。
- `Compact` 模式无多余空格与换行；`PrettyCompact` 模式下 `vals`、`ports.in/out`、`ops` 数组的元素均单行展示；`Pretty` 模式每个字段独立换行。

## 兼容性
- Design 解析要求使用 `graphs/vals/ports/ops` 等新键名；属性对象支持 `t/k/kind` 与 `v/value`、`vs/values` 的兼容别名。

## 示例
```json
{
  "graphs": [
    {
      "name": "sample",
      "vals": [
        {"sym": "in", "w": 1, "sgn": false, "in": true, "out": false, "users": []},
        {"sym": "out", "w": 1, "sgn": false, "in": false, "out": true, "users": []}
      ],
      "ports": {
        "in": [
          {"name": "in", "val": "in"}
        ],
        "out": [
          {"name": "out", "val": "out"}
        ]
      },
      "ops": [
        {"sym": "drv", "kind": "kAssign", "in": ["in"], "out": ["out"]}
      ]
    }
  ],
  "tops": [
    "sample"
  ]
}
```
