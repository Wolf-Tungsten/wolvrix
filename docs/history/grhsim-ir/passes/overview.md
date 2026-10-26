> Historical independent-IR design, retired on 2026-10-26. APIs, pass names and
> implementation paths below describe that archived version. For the current
> unified GRH simulator, see the [simulation workflow](../../../grhsim.md).

# GrhSIM IR Pass System

本文定义 GrhSIM IR 的 pass 模型和 Python 编排接口。Pass 可以由 C++ 或 Python 实现，但都通过
同一套 Python API 组合和执行。

CPU 流程与实现中的 pass 顺序见 [CPU ST flow](../flows/cpu-st.md)。可选 B7
[`cpu.st.recover-masked-or`](recover-masked-or.md) 在 semantic seal 之前恢复
`cpu.compute.maskedOr`，其 operands、匹配边界、报告及开关在该页定义。

## 1. 边界

Pass manager 构建或修改 [GrhSIM IR Overview](../overview.md) 中定义的 `GrhSimModel`，包括其
语义分量和后端映射：

```text
GrhSimModel
  I, O, S, F, G, Init                     // 语义分量，定义见 overview
  mappings: Map<BackendId, BackendMapping>   // 各仿真后端的映射方案
```

`PassManager.build(model)` 接受 `mappings` 为空的 `GrhSimModel`；`PassManager.run(model)` 要求
输入已携带有效后端映射。两者的返回值都必须是至少携带一个有效后端映射的 `GrhSimModel`。
这里的"有效"指完整且与当前模型一致（见第 6 节）。

Pass 之间只传递修改后的 `GrhSimModel`。Pass 内部的临时计算在该 pass 结束时消失，不能通过
全局变量或其他隐藏状态传给下一个 pass。

Python 负责：

- 选择 pass 及其参数；
- 决定 pass 的先后顺序；
- 使用普通 Python 条件、循环和函数组合 pipeline。

Pass 系统不另外定义控制流语言。

## 2. Pass

Pass 只有一种：

```text
Pass.run(model: GrhSimModel) -> PassResult<GrhSimModel>
```

Pass 返回完整的新模型：可以修改语义分量，也可以创建或推进任意后端映射。一个后端的映射
通常由多个 pass 分阶段构建，例如 CPU 后端可以按 `CpuBackendMapping` 的三个分量各用一个
pass。

Pass 不得原地修改输入模型，必须返回替代对象。因此 pass manager 总能通过对象同一性判断
"哪个分量变了"，这是失效规则的基础（见第 6 节）。

大多数 pass 实际只做两件事之一，可以按风格称呼（是惯例，不是类型边界）：

- **model 风格**：修改语义分量，不操心后端映射；
- **backend 风格**：只推进一个后端的映射，不动语义分量，也不动其他后端的映射。

### 2.1 `PassResult`

```text
PassResult<T>
  value: T
  changed: Boolean
```

`value` 是 pass 的完整替代输出；`changed = false` 表示输出与输入相同，此时可以直接返回原
对象。`PassResult` 只是 pass API 的返回值，不是 GrhSIM IR 的分量。

## 3. Python 接口

Python 包 `wolvrix.grhsim_ir` 提供以下接口：

`BackendMapping` 是各后端映射在 Python 中的公共基类；具体字段仍由对应后端定义。

```python
class Pass(Protocol):
    name: str
    def run(self, model: GrhSimModel) -> PassResult[GrhSimModel]: ...

class PassManager:
    def add(self, pass_: Pass) -> None: ...
    def build(self, model: GrhSimModel) -> GrhSimModel: ...
    def run(self, model: GrhSimModel) -> GrhSimModel: ...
```

Python 实现只要满足该 protocol 就能加入 `PassManager`。C++ pass 通过 Python binding 暴露为
满足同一 protocol 的对象，因此 pipeline 不区分 pass 的实现语言。

典型编排形式为：

```python
pm = PassManager()
pm.add(model_pass)
pm.add(cpu_data_layout_pass)
pm.add(cpu_partition_pass)
pm.add(cpu_schedule_pass)

sim = pm.build(model)
```

`model_pass` 和三个 `cpu_*` pass 都是已经构造好的 pass 对象。Python 可以在调用 `add` 前根据
目标机器、配置或模型内容选择对象，不需要把控制流编码进 pass manager。

## 4. Pass 注册

需要按名称创建的 pass 通过统一 registry 注册：

```python
class PassRegistry:
    def register(self, name: str, factory: Callable[..., Pass]) -> None: ...
    def create(self, name: str, **options) -> Pass: ...
    def names(self) -> list[str]: ...
```

Pass 名称必须全局唯一，并使用 `<owner>.<name>` 形式。方言定义的 pass 使用方言名作为 owner，
后端相关的 pass 使用后端名作为 owner。Native binding 和 Python 模块都向同一个 registry
注册 factory；factory 负责检查自己的 Python 参数。

Registry 只负责从名称和参数创建 pass，不保存 pass 的执行结果。

## 5. 执行规则

`PassManager.build` 和 `PassManager.run` 都按 `add` 的顺序执行，并遵守以下规则：

1. `build` 开始前验证输入 `GrhSimModel`（`mappings` 可以为空）；`run` 开始前同样验证输入，
   并要求其 `mappings` 非空、每个映射完整有效。
2. 每个 pass 返回后：先验证新的语义分量；再按第 6 节的失效规则移除失效映射；最后对新建或
   被重写的每个后端映射做结构验证。
3. 任一 pass 报错或验证失败时立即停止，失败输出不写入结果模型，后续 pass 不执行。
4. 全部 pass 完成后，只有 `mappings` 非空、其中每个映射都完整有效且与当前模型一致时才能
   返回。否则本次执行失败，不返回结果。

Pass manager 对每个 pass 执行同一套固定行为。Pass 的名称、注册顺序或 Python 对象类型不能
绕过语义分量和后端映射之间的失效规则。

## 6. 验证与失效

`GrhSimModel` 语义分量的验证规则由 overview 和当前已加载方言共同定义；`BackendMapping` 的
验证规则由 overview 和对应后端共同定义，后端必须区分"结构合法"与"完整"两档：结构合法
（只引用当前模型中存在的对象、op、value 和类型）是每次 pass 返回后的即时要求，完整只在
pass manager 返回前要求。验证是 pass manager 的固定步骤，不作为可选 pass 插入 pipeline。

模型修改和后端映射之间只有一条失效规则，由对象同一性判定：pass 返回后，若某个语义分量与
输入不再是同一对象，输出 `mappings` 中所有与输入相同的映射对象一律失效（视为不存在）；
pass 新建或重写的映射不受影响。

因此，只改语义分量的 pass 不需要关心 `mappings`：把输入的映射原样留在输出里即可，pass
manager 会自动移除它们。只推进后端映射的 pass 必须保持语义分量的对象同一性，否则它保留的
其他后端映射也会被一并移除。

pass manager 绝不返回 `mappings` 为空或包含失效、不完整映射的 `GrhSimModel`。

## 7. 文档

- [CPU 单线程活动度仿真 Flow](../flows/cpu-st.md)
- [GrhSIM IR Overview](../overview.md)
- [Core Dialect](../dialects/core.md)
- [CPU 后端](../backends/cpu.md)
