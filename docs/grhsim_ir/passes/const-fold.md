# `grhsim.const-fold`

完整的常量运算折叠（M5d-2）。把所有 operand 都是常量的两态 logic `core.compute.*` op
**就地**改写为 `core.compute.constant`：op 的 result value、ID 与全部依赖边保持不变，
因此声明来源关联（`declProvenance`）无需任何维护。被折叠后失去消费者的常量 op 由后续
`grhsim.used-bits` 的死锥清扫回收；本 pass 自身从不删除实体、不调用 `compact()`。

## 折叠规则

- 只处理两态 `core.logic` 结果与 operand；四态值（X/Z 语义）一律跳过。
- 支持的 op：`assign`、`add/sub/mul/div/mod`、全部比较（`eq/ne/caseEq/caseNe/
  wildcardEq/wildcardNe/lt/le/gt/ge`，两态下 case/wildcard 变体退化为按位相等）、
  `and/or/xor/xnor/not`、`logicAnd/logicOr/logicNot`、六个归约、`shl/lshr/ashr`、
  `mux/bitSelect`（条件常量选臂）、`prioritySelect`（全常量时取第一个非零条件的臂）、
  `concat/replicate/sliceStatic/sliceDynamic/sliceArray`。
- 常量经 `constValue`/`value` 参数以 SystemVerilog 字面量解析（slang `SVInt`），按各
  自 value 类型的宽度与符号扩展后再求值；结果按 result 类型宽度截断/符号扩展后重新
  序列化为 `<width>'h<hex>` 字面量。符号位驱动有符号比较、除法、取模与算术右移。
- `div/mod` 除数为常量零时**不折叠**：未折叠的运行时行为（C++ 除法，UB）不得被折叠值
  替换。
- 任意宽度都折叠（不限 64 位）；超过 64 位的值走 slang 多字运算。

## 实现

一次 worklist 传播即达局部不动点：折叠一个 op 后只把其 result 的消费者重新入队；
每个 op 至多折叠一次，总复杂度 O(ops)。作为 `grhsim.simplify` 子步骤调用时按
`SimplifyScope` 限定可改写的 op 集合（常量本身在任何 scope 下都可作为折叠输入被读取）。
独立运行时计数 `const_fold_ops`。
