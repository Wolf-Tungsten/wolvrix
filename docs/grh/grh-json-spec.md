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

- `formatVersion`: 必需且只能为 5，版本属于整个 Design；旧版本和无版本输入直接拒绝，并提示从源重新 ingest。
- `graphs`: 按图名升序排列的数组，每个元素为单个 Graph 描述。
- `aliases`: 可选，别名对象。键为 alias，值为目标 Graph 名称（symbol）。
- `declaredSymbols`: 用户定义模块名数组（必需，缺失视为错误）。
- `tops`: 顶层图名数组，顺序遵循 `Store` 解析后的顶层列表（包括 `topOverrides`）。

## Graph 结构（按字段顺序输出）
- `symbol`: 图名称（必需，缺失视为错误）。
- `dialects`: 方言名到整数版本的对象；基础 `grh:2` 必需，使用扩展 op 时声明对应版本，
  如 `grhsim:1`、`grhsim.cpu:1`。未知方言、kind 或不支持的版本拒绝读取。
- `semanticTree`: 可选，Graph 自包含的计算 Node 组织；没有此结构时省略，不写 null，也不引用其他 Graph。
  - `sourceUnit` / `circuit`: 来源单元和电路名。
  - `rootPath`: 根 Path 的显式 ID，固定为 1；根不是 Node。
  - `namesFlattened`: 必需的布尔值，区分原始叶名和已经编码的限定名，避免重复 flatten 时再次转义。
  - `paths`: 内部路径数组，每项 `{id,parent,name}`。根的 parent 为 0；其他 parent 必须指向实际 Path。名字非空，同级名字唯一，不能有环。
  - `nodes`: 唯一一种 Node 的数组，每项 `{id,parent,name,ops,anchors}`。Node 永远是叶子，parent 必须指向 Path；名字非空，同级名字唯一。没有 kind、index 或角色分类。
  - Path ID 与 Node ID 属于两个独立的正 uint32 域。ID 显式保存，允许空洞，不依赖数组次序；读入时按 parent 关系构建。普通存储和 clone/compact 不重编号 Node。
  - `ops`: 必需，本 Node 实际 GRH operation 成员的局部 ID 集合；不能重复或悬空。同一个 op 可以属于多个 Node。
  - `anchors`: 可选，缺省为空；为本 Node 保留边界的实际 value 局部 ID 集合。输入、别名等 Node 允许 ops 为空；anchors 不是新增 Node 类型，也不恢复旧来源投影。
  - 写出时 Path/Node 按自身 ID 升序，ops/anchors 按对应 Graph 局部 ID 升序，空数组也显式写出。
  - hier-flatten 后 paths 只保留根，所有 Node 直接挂根；限定名中的路径分隔符和原名中的分隔符按 SemanticTree 编码规则区分。
  加载时拒绝无效父级、重复 ID/名字、悬空或重复成员，以及旧 kind/index/semanticBindings。旧 checkpoint 必须重新 ingest，不能仅修改版本号。
- `metadata`: Graph 属性对象，负载编码与 op attrs 相同；保存转换阶段及逻辑域等语义。
  分区/调度缓存、裸实体 ID、CPU 布局及 chunk 不进入此对象。
- `declaredSymbols`: 模块内部声明的符号名数组（必需）。
- `generateGroups`: 可选，generate 块内声明的副本分组数组（仅非空时写出，缺失视为空集合）。每项 `{ scope, name, symbols }`，均为符号文本：
  - `scope`: 无轮次的作用域路径（嵌套时以 `$` 连接块名，如 `gen_loop`、`outer$inner`）；hier-flatten 后叠加实例路径前缀（如 `u_inst$gen_loop`）。
  - `name`: 裸声明名（flatten 不改动该字段）。
  - `symbols`: 按 elaboration 顺序排列的副本符号名（如 `gen_loop$0$sig`；flatten 后为 `u_inst$gen_loop$0$sig`）。
- `vals`: 值列表，保持创建顺序。每个值包含：
  - `id`: 本份 Graph JSON 内从 1 开始的局部值 ID；只用于结构关联，不是运行时 `ValueId`，也不保证跨 compact 或变换稳定。
  - `sym`: 符号名；仅语义对象写出，临时对象省略。
  - `w`: 位宽（数组值为总位宽，含全部维度）。
  - `sgn`: 是否有符号。
  - `type`: kind 名称，`logic` / `real` / `string`。
  - `domain`: `2state` / `4state`，必需，与 kind/width/sgn/shape 组成完整类型。
  - `shape`: 可选，int64 数组，非空时写出（见 grh-ir.md §2.3 的数组类型模型；
    元素位宽 = `w / ∏shape`，必须整除）。
  - `in` / `out` / `inout`: 输入/输出/inout 标记（`inout` 不可与 `in/out` 同时为真）。
  - `def`: 可选，定义该值的 Operation 局部 ID；省略时由 operation 的 `out` 边恢复。
  - `users`: 可选，使用者数组 `{ op, idx }`，`op` 为 Operation 局部 ID；省略时由 operation 的 `in` 边恢复。
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
  - `attrs`: 可选属性对象，键为属性名，值为属性负载。FIRRTL effect
    operations (`kSystemTask`) use `firrtl.line` (int) for the 1-based source
    line of the originating FIRRTL statement; FIRRTL instance boundary
    operations (`kInstance`) use the same attribute for their `inst` statement.
    This is the parser line convention used by GSIM's serialized `lineno` and
    survives graph transforms and StoreJson round trips.
  - `loc`: 可选，源码位置与扩展调试信息 `{ file, line, col, endLine, endCol, origin, pass, note }`。

Node 的 ops/anchors 是持久化成员事实的唯一入口。value/op 不再写出或接受 semanticIds，
也不生成旧 semanticBindings、`firrtl.node.members/origins/roots/rootResults` 或 `originModules`。
加载完成后 Graph API 从这些成员同步建立反向查询；真实运行时 OperationId/ValueId 带图身份与代数，
不能直接写入 JSON。写出和读入均经由 Graph 局部 ID 表映射。
加载器先按实际 op/value 聚合全部 Node 归属，再为每个实体一次性建立双向成员；共享高扇出
实现不会在成员池中留下所有逐次增长的归属前缀。

FIRRTL 的正式端口关系保存在 Graph metadata 的 `firrtl.semanticPortNames:string[]` 与
`firrtl.semanticPortNodes:int[]`；instance 的实际端口关系保存在对应 op attrs 的
`semanticPortNames:string[]` 与 `semanticPortNodes:int[]`。两数组须成对出现、等长，名字非空唯一，
Node ID 必须为本 Graph 实际存在的正 uint32；instance 或 blackbox 端口 Node 同时必须属于该 op。
零宽等没有 runtime value 的端点仍引用正常 Node，不用零或负数充当占位。加载时严格检查这些关系，
不会将溢出整数截断为有效 Node ID。它们记录实际端口对应关系，不增加 Node 类型。

F28 首阶段支持正常 ingest-firrtl→xmr-resolve→hier-flatten，以及 grh.verify、stats、
memory-init-check 和 multidriven-guard 只读检查。带 SemanticTree 的 Graph 进入尚未迁移的
split-static-array、DCE、comb-loop-elim 或 GrhSIM 转换/优化/分区时，PassManager 在该 pass
修改图之前明确拒绝；不自动删除 SemanticTree。旧 source-endpoint NodeEvolution trace 也明确拒绝新树。
没有 SemanticTree 的 Graph 继续执行原有变换流程。

生产环境默认省略全部 `def/users` 和 `loc`。加载后以 operation 的 `in/out` 边重建拓扑；`def/users`
存在时仍校验与 operation 边一致。省略 `loc` 不影响电路或 SemanticTree，但被省略的源码位置在重新加载后不可恢复。
C++ `StoreOptions.emitDefUsers`、`emitSourceLocations` 和 Python `store_json` 的
`emit_def_users`、`emit_source_locations` 默认值均为 false；调试时显式设为 true 可导出完整字段。

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
  - `attrs.writerOrder`: int64，非负且同 regSymbol 写组内唯一
- `kRegisterWriteMaskPort`
  - `in`: `[updateCond, nextValue, mask, event0, event1, ...]`
  - `out`: 为空
  - `attrs.regSymbol`: string
  - `attrs.eventEdge`: string[]
  - `attrs.writerOrder`: int64，非负且同 regSymbol 写组内唯一
- `kRegisterWriteArrayPort`
  - `in`: `[updateCond, i0..ik-1, data, event0, event1, ...]`（1 ≤ k ≤ rank；k<rank 时 data 为子数组）
  - `out`: 为空
  - `attrs.regSymbol`: string
  - `attrs.eventEdge`: string[]
  - `attrs.writerOrder`: int64，非负且同 regSymbol 写组内唯一
- `kRegisterWriteArrayMaskPort`
  - `in`: `[updateCond, i0..ik-1, data, mask, event0, event1, ...]`
  - `out`: 为空
  - `attrs.regSymbol`: string
  - `attrs.eventEdge`: string[]
  - `attrs.writerOrder`: int64，非负且同 regSymbol 写组内唯一
- `kRegisterFillPort`
  - `in`: `[updateCond, data, event0, event1, ...]`（data 为元素宽，广播写满全部元素）
  - `out`: 为空
  - `attrs.regSymbol`: string
  - `attrs.eventEdge`: string[]
  - `attrs.writerOrder`: int64，非负且同 regSymbol 写组内唯一
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
  - `attrs.writerOrder`: int64，非负且同 latchSymbol 写组内唯一

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
- `ops` 数组仅保留创建顺序，**不承载写口优先级语义**。同一 regSymbol 的全部写端口
  （kRegisterWritePort / kRegisterWriteMaskPort / kRegisterWriteArrayPort /
  kRegisterWriteArrayMaskPort / kRegisterFillPort）必须带唯一非负 `writerOrder`，
  按数值升序应用，同地址重叠位由较大值覆盖（见 grh-ir.md §6.4）。latch 写组使用 latchSymbol 的同一规则。
  pass 不得依赖数组位置改写优先级；旧图的 declaration-order fallback 仅属迁移兼容行为。
- `Compact` 模式无多余空格与换行；`PrettyCompact` 模式下 `vals`、`ports.in/out`、`ops` 数组的元素均单行展示；`Pretty` 模式每个字段独立换行。

## 兼容性

版本 4 为值和操作使用独立的 Graph-local 整数命名空间：`vals[].id` 与 `ops[].id` 均为正整数且在各自数组内唯一；`ports`、操作的 `in/out`、值的 `def` 和 `users[].op` 只能引用对应命名空间中已定义的 ID。临时对象没有 `sym` 或 `tmp` 字段，round trip 不会将局部 ID 提升为 SymbolTable symbol。局部 ID 和 SemanticId 由当前 Graph 及其导出顺序决定，不能作为跨文件句柄。

`LoadJson::loadFile(path)` 和 Python `read_json_file` 使用 64 KiB 输入缓冲，
将 JSON 数组保留为可定位的文件区间，按记录读取 value/op 和属性数组。
因此不同时保存完整文件文本和完整 JSON DOM；对象字段顺序、端口顺序和
属性编码与文本入口相同。加载不冻结图，不恢复运行时实体 ID。
属性数组直接积累为最终类型的向量；最终 Graph 和单条记录本身仍需内存。
非数组对象仍会物化，含大量标量属性的 metadata 对象和单个超长字符串
也会占用额外内存，不保证任意规模输入都能装入。文件须在整个同步读取过程中保持不变。
`LoadJson::load(text)` / `load_json_text` 保留完整文本 DOM 路径。
Python `Session.semantic_stats` 直接遍历已加载 Graph，返回 `trees/root_path_count`、排除根的
`path_count`、`node_count`、`name_bytes`、`total_op_count`、`op_membership_count`、
`anchor_membership_count`、`covered_unique_op_count`、`uncovered_op_count`、`shared_op_count` 和
`empty_op_node_count`。共享 op 的成员关系可以多条，但唯一覆盖只计一次；空 Node 单独统计。
`Session.semantic_validate` 复用原生结构/双向成员校验，返回 `valid/error_count/checked_graphs/examples`。
文件错误、语法或图构建错误都在 session 发布新设计之前返回；失败替换
保留已有设计。

- Design 只接受根 `formatVersion: 5`；v4 及更早版本和无版本输入必须重新从源生成，不提供字段迁移。
- 属性对象只接受 `{t,v}` 或 `{t,vs}`，不读取旧 k/kind/value/values 别名。
- 字符串实体引用和 tmp 已删除。扩展 op 必须声明所属方言版本。
  load 创建新设计实例，不恢复 revision 或 session 分析。
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
  "formatVersion": 5,
  "graphs": [{
    "symbol": "sample",
    "dialects": {"grh": 2},
    "semanticTree": {
      "sourceUnit": "unit",
      "circuit": "sample",
      "rootPath": 1,
      "namesFlattened": false,
      "paths": [{"id": 1, "parent": 0, "name": "sample"}],
      "nodes": [
        {"id": 1, "parent": 1, "name": "in", "ops": [], "anchors": [1]},
        {"id": 2, "parent": 1, "name": "out", "ops": [1], "anchors": [2]}
      ]
    },
    "metadata": {},
    "declaredSymbols": [],
    "vals": [
      {"id": 1, "sym": "in", "w": 1, "sgn": false, "type": "logic", "domain": "4state",
       "in": true, "out": false, "inout": false},
      {"id": 2, "sym": "out", "w": 1, "sgn": false, "type": "logic", "domain": "4state",
       "in": false, "out": true, "inout": false}
    ],
    "ports": {
      "in": [{"name": "in", "val": 1}],
      "out": [{"name": "out", "val": 2}],
      "inout": []
    },
    "ops": [
      {"id": 1, "sym": "drv", "kind": "kAssign", "in": [1], "out": [2]}
    ]
  }],
  "aliases": {},
  "declaredSymbols": [],
  "tops": ["sample"]
}
```
