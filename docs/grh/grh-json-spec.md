# GRH JSON 规范（StoreJson）

仿真阶段也使用本格式：`grhsim`/`grhsim.cpu` 扩展操作、阶段属性、完整类型及
虚拟归属保存在同一个 GRH Graph。独立 GrhSIM JSON loader 已删除；旧模型需从
原始 SV/GRH 重新生成。SinkSchedule 与 emitter 私有计划不序列化，重载后的
重建方法见 [仿真流程](../grhsim.md#python-and-checkpoints)。

StoreJson 输出 GRH 设计的紧凑 JSON 表示，统一 CLI 与测试的入口。默认写入 `grh.json`（位于 `StoreOptions::outputDir`，缺省为当前工作目录），并通过 `StoreOptions::jsonMode` 控制排版：
- `Compact`: 单行最紧凑格式。
- `PrettyCompact`（默认）：保持顶层/Graph 缩进，但 `vals`/`ports`/`ops` 元素各占一行。
- `Pretty`: 全量缩进换行（每个字段独立一行）。

`PrettyCompact` 的文件输出与 `StoreJson::storeToString` 使用同一套排版规则，逐字节一致；
文件路径通过固定 64 KiB 输出缓冲区增量写入，不构建完整 JSON 字符串。
Graph metadata 只排序属性指针，不复制属性负载。图名/别名排序和单个常量格式化等
局部临时数据仍会分配内存。`storeToString` 按接口约定保留完整字符串；`Compact` 和
`Pretty` 继续使用 Slang JsonWriter 的完整字符串输出路径。

文件打开、写入、flush 和 close 均成功后才返回成功的 artifact。失败会记录诊断并返回
`success=false` 和空 artifact 列表；目标文件可能已包含部分内容，不能作为有效 checkpoint 使用。

## 顶层结构

数组构造以普通 op 名 `kArrayCreate` 持久化，operands 按低元素到高元素排列。
结果 value 必须记录 `shape=[N]+element.shape` 和 `w=N*element.w`；没有额外数组属性。
例如两个 8 位标量按 operands `[a,b]` 构造的结果为 `w=16, shape=[2]`，低字节来自 a。
载入后由 `grh.verify` 独立复核完整类型；容器布局不变。

- `graphs`: 按图名升序排列的数组，每个元素为单个 Graph 描述。
- `aliases`: 可选，别名对象。键为 alias，值为目标 Graph 名称（symbol）。
- `declaredSymbols`: 用户定义模块名数组（必需，缺失视为错误）。
- `tops`: 顶层图名数组，顺序遵循 `Store` 解析后的顶层列表（包括 `topOverrides`）。

## Graph 结构（按字段顺序输出）
- `symbol`: 图名称（必需，缺失视为错误）。
- `formatVersion`: 当前为 3，版本属于每个 Graph。版本 2 的字符串引用格式仍可加载，重新导出时写为版本 3。
- `dialects`: 方言名到整数版本的对象；基础 `grh:2` 必需，使用扩展 op 时声明对应版本，
  如 `grhsim:1`、`grhsim.cpu:1`。未知方言、kind 或不支持的版本拒绝读取。
- `metadata`: Graph 属性对象，负载编码与 op attrs 相同；保存转换阶段及逻辑域等语义。
  分区/调度缓存、裸实体 ID、CPU 布局及 chunk 不进入此对象。
- `declaredSymbols`: 模块内部声明的符号名数组（必需）。
- `generateGroups`: 可选，generate 块内声明的副本分组数组（仅非空时写出，缺失视为空集合）。每项 `{ scope, name, symbols }`，均为符号文本：
  - `scope`: 无轮次的作用域路径（嵌套时以 `$` 连接块名，如 `gen_loop`、`outer$inner`）；hier-flatten 后叠加实例路径前缀（如 `u_inst$gen_loop`）。
  - `name`: 裸声明名（flatten 不改动该字段）。
  - `symbols`: 按 elaboration 顺序排列的副本符号名（如 `gen_loop$0$sig`；flatten 后为 `u_inst$gen_loop$0$sig`）。
- `vals`: 值列表，保持创建顺序。每个值包含：
  - `id`: 本份 Graph JSON 内从 1 开始的局部值 ID；只用于结构关联，不是运行时 `ValueId`，也不保证跨 compact 或变换稳定。
  - `sym`: 符号名；仅语义对象写出，临时对象省略。版本 3 中缺失表示无语义 symbol。
  - `w`: 位宽（数组值为总位宽，含全部维度）。
  - `sgn`: 是否有符号。
  - `type`: kind 名称，`logic` / `real` / `string`；logic 可省略。
  - `domain`: `2state` / `4state`，版本 2 必需，与 kind/width/sgn/shape 组成完整类型。
  - `shape`: 可选，int64 数组，非空时写出（见 grh-ir.md §2.3 的数组类型模型；
    元素位宽 = `w / ∏shape`，必须整除）。
  - `in` / `out` / `inout`: 输入/输出/inout 标记（`inout` 不可与 `in/out` 同时为真）。
  - `def`: 可选，定义该值的 Operation 局部 ID。
  - `users`: 使用者数组 `{ op, idx }`，`op` 为 Operation 局部 ID，描述操作与操作数索引。
  - `loc`: 可选，源码位置与扩展调试信息 `{ file, line, col, endLine, endCol, origin, pass, note }`。
- `ports`: 端口对象。
  - `in`: 输入端口数组，每项 `{ name, val }`，其中 `val` 为值局部 ID。
  - `out`: 输出端口数组，每项 `{ name, val }`，其中 `val` 为值局部 ID。
  - `inout`: inout 端口数组，每项 `{ name, in, out, oe }`，三个引用均为值局部 ID。
- `ops`: 操作列表，保持创建顺序。每个操作包含：
  - `id`: 本份 Graph JSON 内从 1 开始的局部操作 ID；只用于结构关联。
  - `sym`: 符号名；仅语义对象写出，临时对象省略。
  - `kind`: 操作类型字符串（来自 `wolvrix::lib::grh::toString(OperationKind)`）。
  - `in` / `out`: 操作数与结果值局部 ID 数组，保持插入顺序。
  - `attrs`: 可选属性对象，键为属性名，值为属性负载。
  - `loc`: 可选，源码位置与扩展调试信息 `{ file, line, col, endLine, endCol, origin, pass, note }`。

FIRRTL node 信息也使用现有 metadata/attrs 负载：Graph 的
`firrtl.nodeSchema` 为整数 2、策略为 `soft-node-v2`；op 的 `firrtl.node.members`、`roots`、`origins`
为字符串数组，`firrtl.node.rootResults` 为与 roots 等长的整数数组，记录当前
op 的 result 序号。身份 key 和根映射不保存裸实体 ID。具体字段和 verifier
约束见 [node 属性](../ingest/node-attributes.md)，不增加独立 JSON 方言。
这些都是可选来源关联；允许部分保留、重叠及丢失根，不构成 simplify 屏障。
schema 1 / source-node-v1 checkpoint 必须从源重新 ingest。

## 存储单元 Port 操作
- 存储声明类操作使用 `sym` 作为声明名，`attrs` 仅承载类型信息。
- `kRegister`
  - `in`: 为空
  - `out`: 为空
  - `attrs.width`: int
  - `attrs.isSigned`: bool
  - `attrs.shape`: int[]，可选，数组寄存器的形状（非空数组）
- `kRegisterReadPort`
  - `in`: 为空
  - `out`: 单个 result（读值）
  - `attrs.regSymbol`: string
- `kRegisterReadArrayPort`
  - `in`: `[i0, i1, ...]`（1..rank 个索引；不足 rank 时读出高维子数组）
  - `out`: 单个 result（元素或子数组读值）
  - `attrs.regSymbol`: string
- `kRegisterWritePort`
  - `in`: `[updateCond, nextValue, event0, event1, ...]`
  - `out`: 为空
  - `attrs.regSymbol`: string
  - `attrs.eventEdge`: string[]
- `kRegisterWriteMaskPort`
  - `in`: `[updateCond, nextValue, mask, event0, event1, ...]`
  - `out`: 为空
  - `attrs.regSymbol`: string
  - `attrs.eventEdge`: string[]
- `kRegisterWriteArrayPort`
  - `in`: `[updateCond, i0..ik-1, data, event0, event1, ...]`（1 ≤ k ≤ rank；k<rank 时 data 为子数组）
  - `out`: 为空
  - `attrs.regSymbol`: string
  - `attrs.eventEdge`: string[]
- `kRegisterWriteArrayMaskPort`
  - `in`: `[updateCond, i0..ik-1, data, mask, event0, event1, ...]`
  - `out`: 为空
  - `attrs.regSymbol`: string
  - `attrs.eventEdge`: string[]
- `kRegisterFillPort`
  - `in`: `[updateCond, data, event0, event1, ...]`（data 为元素宽，广播写满全部元素）
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

## XMR 操作
- `kXMRRead`
  - `in`: 为空
  - `out`: 单个 result
  - `attrs.xmrPath`: string，层次路径
- `kXMRWrite`
  - `in`：
    - 非存储目标：`[data]`
    - `kRegister`（标量）：`[updateCond, nextValue, mask, event0, event1, ...]`
    - `kRegister`（数组）：`[updateCond, addr, data, mask, event0, event1, ...]`
    - `kLatch`：`[updateCond, nextValue, mask]`
  - `out`: 为空
  - `attrs.xmrPath`: string，层次路径
  - `attrs.eventEdge`: string[]，仅 `kRegister` 目标必需（长度需匹配事件 operand 数）

说明：`kXMRRead/kXMRWrite` 是中间表示，必须在 emit 之前被 resolve pass 展开，
JSON 中若仍保留该 op，表示流程未完成。

## 属性编码
- 属性负载统一为对象 `{ t: <type>, v?: <scalar>, vs?: <array> }`。
- 支持的 `t`：`bool`、`int`、`double`、`string`、`bool[]`、`int[]`、`double[]`、`string[]`。
- 标量使用 `v`，数组使用 `vs`，数组条目保持给定顺序；非有限浮点将被拒绝。

## 排序与布局约定
- Graph 列表按名称排序；`vals`、`ops` 保留创建顺序；端口数组顺序来自 Graph 端口列表（`compact()` / `freeze()` 会按端口名排序）；operation 的 `attrs` 按 key 文本排序，图级 metadata 保持插入顺序。
- **op 顺序是语义的一部分**：同一 kRegister 的全部写端口（kRegisterWritePort /
  kRegisterWriteMaskPort / kRegisterWriteArrayPort / kRegisterWriteArrayMaskPort /
  kRegisterFillPort）构成唯一写组，同时激活时按 op 声明顺序依次应用、后声明者覆盖
  （见 grh-ir.md §6.4）。`ops` 数组有序，因此该顺序在 JSON 中免费持久化；
  任何 pass 不得重排同 regSymbol 写端口的相对顺序。
- `Compact` 模式无多余空格与换行；`PrettyCompact` 模式下 `vals`、`ports.in/out`、`ops` 数组的元素均单行展示；`Pretty` 模式每个字段独立换行。

## 兼容性

版本 3 为值和操作使用独立的 Graph-local 整数命名空间：`vals[].id` 与 `ops[].id` 均为正整数且在各自数组内唯一；`ports`、操作的 `in/out`、值的 `def` 和 `users[].op` 只能引用对应命名空间中已定义的 ID。临时对象没有 `sym` 字段，round trip 不会将局部 ID 或旧版本的临时显示名提升为 SymbolTable symbol。局部 ID 由当前导出顺序决定，不能作为跨文件句柄。

`LoadJson::loadFile(path)` 和 Python `read_json_file` 使用 64 KiB 输入缓冲，
将 JSON 数组保留为可定位的文件区间，按记录读取 value/op 和属性数组。
因此不同时保存完整文件文本和完整 JSON DOM；对象字段顺序、端口顺序、
属性编码和旧格式兼容规则与文本入口相同。加载不冻结图，不恢复旧 ID。
属性数组直接积累为最终类型的向量；最终 Graph 和单条记录本身仍需内存。
非数组对象仍会物化，含大量标量属性的 metadata 对象和单个超长字符串
也会占用额外内存，不保证任意规模输入都能装入。文件须在整个同步读取过程中保持不变。
`LoadJson::load(text)` / `load_json_text` 保留完整文本 DOM 路径。
文件错误、语法或图构建错误都在 session 发布新设计之前返回；失败替换
保留已有设计。

- Design 解析要求使用 `graphs/vals/ports/ops` 等新键名；属性对象支持 `t/k/kind` 与 `v/value`、`vs/values` 的兼容别名。
- 无 formatVersion 的旧基础 GRH 默认为 FourState；不接受未知显式版本。
  已删除的 `kAssignAs` 明确拒绝，提示重新生成。扩展 op 必须声明所属方言版本，
  不能借无版本输入绕过检查。load 创建新设计实例，不恢复 revision 或 session 分析。
- GrhSIM 形态仍使用本文容器，op 名为 `grhsim.edge-detect` 等注册名称；阶段保存在
  metadata 的 `grhsim.stage`。`converted` 图无虚拟归属，后续归属为 op 的
  `grhsim.partition:string[]`。公开 verifier 拒绝部分转换、非法阶段或不完整归属。
  不使用旧 `wolvrix.grhsim.v3` 作为统一图的容器。
- 状态的完整类型使用声明 attrs 的 `width/isSigned/valueType/domain/shape`。
  typed InitSpec 保存为等长数组 `initSpecKinds/Values/Files/Formats/Starts/Counts/Seeds/HasSeeds/Random`，
  数组完整键名均带 `initSpec` 前缀。kind 是 const/fill/readmem/random；start/count=-1
  表示省略，hasSeeds 区分缺省 seed 与 seed=0。数组顺序就是初始化覆盖顺序。
  setStateInitSpec 写入这组规范属性并移除旧 initKind/initValue/initFile/initStart/initLen。

## 示例
```json
{
  "graphs": [
    {
      "name": "sample",
      "vals": [
        {"id": 1, "sym": "in", "w": 1, "sgn": false, "in": true, "out": false, "users": [{"op": 1, "idx": 0}]},
        {"id": 2, "sym": "out", "w": 1, "sgn": false, "in": false, "out": true, "users": []}
      ],
      "ports": {
        "in": [
          {"name": "in", "val": 1}
        ],
        "out": [
          {"name": "out", "val": 2}
        ]
      },
      "ops": [
        {"id": 1, "sym": "drv", "kind": "kAssign", "in": [1], "out": [2]}
      ]
    }
  ],
  "tops": [
    "sample"
  ]
}
```
