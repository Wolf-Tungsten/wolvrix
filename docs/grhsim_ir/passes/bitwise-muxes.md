# 位选择规范化

`grhsim.bitwise-muxes` 是无参数的 SemanticTransform，将全部 operand/result
同为 unsigned、two-state、1-bit logic 的 `core.compute.mux` 转为
`core.compute.bitSelect`。不带 parameters/object refs；不满足条件时保留原 op。

```text
y:u1 = mux(c:u1, a:u1, b:u1)
=>
y:u1 = bitSelect(c, a, b)
```

`bitSelect(mask, whenSet, whenClear)` 的三个 operands 与唯一 result 必须有
相同的完整 TypeId，目前支持 1–64 位 two-state logic。没有 parameters 或
object refs。逐位定义：result[i] 为 mask[i] ? whenSet[i] : whenClear[i]。
例如 `bitSelect(4'b1010, 4'b1100, 4'b0011) = 4'b1001`。signed 类型同样按
位处理，最终按 result 的 signedness 解释。verifier 检查 arity、类型与位宽。

普通 mux 的 condition 是“非零”谓词；bitSelect 的第一个参数是逐位 mask。
两者仅在上述 1-bit 情况下可直接互换。多位条件、signed bit、四态或类型转换
不参与该 pass。IR 原有 producers、operand/result IDs、共享及全部依赖保留，
包括两个分支 producer 的求值；state/commit/DPI 不会变为条件执行。

CPU emitter 发射纯按位表达式。无符号 bit 使用
`(c & a) | ((c ^ 1) & b)`，其余合法 scalar 使用 `(mask & a) | (~mask & b)`，
沿用既有结果归一化与变化通知。无需 runtime helper 或宽值副本。
这使已经求出的 bit 数据选择能以算术执行；具体原生指令和收益仍由编译与
测量确定，不能保证编译器永远不生成分支。

变换后重建 CPU mapping。XiangShan 默认在 pack-bit-registers 之后执行；
禁用寄存器打包时在 semantic pipeline 之后执行。JSON 按普通 semantic op
往返，没有新增 mapping 字段。其他 backend 不支持此 op 时应显式报错。
