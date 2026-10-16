# register-write-normalize

在二态 GRH 中，将寄存器或锁存器写口的纯保持分支提取为写使能，避免为了生成 next-value 而读取每个旧值。pass 遍历 design 中的全部 graph，覆盖标量/整数组整体写、单常量地址数组写，以及它们的 mask 变体和锁存器写口。在模块级运行，位于 `xmr-resolve` 之后、调度之前。先用原有 `mux-tree-fold` 折叠简单链，再处理剩余的多写口形式，可避免把原本已经可折叠的单链改成更复杂的使能。

```python
sess.run_pass("mux-tree-fold", design="design.main")
sess.run_pass("register-write-normalize", design="design.main", semantics="2state")
sess.run_pass("mux-tree-fold", design="design.main", guarded_writes="2state")
sess.run_pass("grh.verify", design="design.main")
```

CLI 参数分别为 `-semantics=2state` 和 `-guarded-writes=2state`。normalizer 的工厂入口要求显式指定二态语义；C++ 默认构造不执行改写。未知态条件下，SV 条件表达式的逐位合并与条件写入不同，因此不能将此变换用于四态模拟。

## 操作数和示例

写口的 `operands[0]` 总是 1 位更新使能，事件信号排在数据/掩码之后；`regSymbol` 或 `latchSymbol` 指向目标状态。整体写的有效载荷位于 `operands[1]`，所以 `kRegisterWritePort` 使用 `[E, D, events...]`，`kRegisterWriteMaskPort` 使用 `[E, D, M, events...]`，`kLatchWritePort` 使用 `[E, D, M]`。单维 `kRegisterWriteArrayPort` 使用 `[E, i, D, events...]`，其 mask 变体使用 `[E, i, D, M, events...]`。`kRegisterReadPort`/`kLatchReadPort` 无索引，`kRegisterReadArrayPort` 的操作数 `[i]` 读取该寄存器事件发生前的元素（或一维子数组）。

```text
write(E, e, mux(H, D, read(e)))
    => write(E && H, e, D)

write(E, mux(H, D, read(q)), M)
    => write(E && H, D, M)

write(E, e, mux(A, DA, mux(B, DB, read(e))))
    => write(E && (A || B), e, mux(A, DA, DB))
```

第二例的 payload 在写使能为假时无关紧要，因此可以剪掉保持叶子。掩码原样保留，保持分支被证明时直接关闭整个写口即可。normalizer 不增加写口数；即使后续无法恢复动态写，也不会将一条更新展开成多个写口。只用于保持的旧读和 mux 由随后的 `mux-tree-fold` 死锥清理删除；其他功能读保留。

## 写入优先级

按 `regSymbol`/`latchSymbol` 收集完整有序写组。仅当每个被删除的保持路径与所有可能覆盖该地址的较早写口都能证明互斥时，才允许删除。证明分析布尔别名、取反、AND/OR 和复合条件，使用预算限制，无法证明时保留原写口。动态地址、掩码、整数组写和同状态的其他写口都作为潜在重叠写参与检查；整体写候选会检查完整的此前写组。

以下代码必须保留第二条的保持写，否则 `B=0` 时会错误地暴露第一条写入：

```text
write(1, e, X)
write(1, e, mux(B, Y, read(e)))
```

任何改写都重建整个寄存器的有序写组，同时保留未改写的写口及事件信息，避免新创建的操作被排到原有高优先级写口之后。

非保持 payload 仍依赖同一寄存器的读改写路径暂时保留；它们需要独立的汇总/算术规范化，不能当作简单覆盖写。当前数组保持识别针对同寄存器、同一常量地址、同宽度的直接数组读及透明别名；整体写识别同状态的直接整体读，不识别整数组读后的切片，也不处理动态/多维地址的别名证明。其他功能路径的旧读仍保留。

## 动态写恢复

`mux-tree-fold` 的 `guarded_writes="2state"` 扩展分析 guarded payload 的各个 mux 分支，计划成功后才创建写口。例如，对每行 `e`：

```text
write(V && P == e, e, D) => write(V, P, D)
```

支持同一行多个写口、OR 合并的零数据写入、外层 if/else 与内层 mux 的不同分组，以及 `~(|P)` 等价的零地址比较。匹配时将其他写源的 `Q == e` 替换为 `Q == P`，保留同址竞争的屏蔽条件。

每个源必须覆盖其地址域中的全部合法行；有常量高位的 concat 可缩小地址域。事件、地址、数据和归一化使能必须匹配；各行写入顺序必须能形成同一无环优先级顺序。整数组、动态地址和掩码写形成有序屏障，两侧独立匹配。覆盖不全、数据不统一、事件不同、优先级冲突、超过分析预算或不能减少写口数时保留原组。

该变换不建立逐行读者订阅，也不改变 GrhSIM 的激活策略。静态读写口减少应与实际运行时间分别验证。

## 验证

`make test_grh_register_write` 运行独立二态状态转移比较与结构检查，覆盖整体/数组/掩码/锁存器写口、保持、同址竞争、零数据、越界地址、非均匀整数组写、事件差异、地址覆盖和优先级反例。

在已有 aggregate RTL 上运行 `make probe_rob_register_write PYTHON=<python>`；目标先安装当前 Python 包，可用 `SKIP_PY_INSTALL=1` 复用已安装版本。快照和统计默认写入 `ptmp/rob-register-write/`。探针在原有 `mux-tree-fold` 后保存基线，再按实际新增管线运行规范化/折叠，要求 `debug_exuData` 恰好保留 8 个动态读和 24 个动态写，且 ROB 各数组的元素写口数均不增加。可用 `ROB_REGISTER_WRITE_ARGS='--baseline ptmp/rob-register-write/before.json'` 在已折叠快照上继续排查。
