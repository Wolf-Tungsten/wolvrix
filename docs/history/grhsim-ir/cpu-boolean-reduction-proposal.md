> Historical independent-IR design, retired on 2026-10-26. APIs, pass names and
> implementation paths below describe that archived version. For the current
> unified GRH simulator, see the [simulation workflow](../../grhsim.md).

# CPU mask/array 规约：设计与实现范围

2026-10-16：已实现 `cpu.compute.maskedOr` 的连续数组/静态视图编码、B7 恢复 pass、CPU
映射与 C++ 发射。默认关闭，使用开关进行整机 A/B。本文记录选择这一落点的原因；
精确接口见 [CPU dialect](dialects/cpu.md#2-cpucomputemaskedor)，匹配规则和运行方法见
[recover-masked-or](passes/recover-masked-or.md)。

## 问题与原始电路

香山当前 aggregate checkpoint 的生成 C++ 有 2,108 条长度至少 16 的布尔链：

| 阶段 | 链数 | 其中 OR-of-AND | 最大长度 |
| --- | ---: | ---: | ---: |
| Event | 21 | 21 | 47 |
| General | 2,083 | 788 | 128 |
| Output | 4 | 0 | 67 |

长度按二元 OR/AND 节点数计，48 项 OR 是 47 个 OR 节点。这是生成 C++ 的静态形状
统计，不是 IR 匹配数或动态热点数。部分长链有重复项，编译器也能重结合 OR；单纯减少
括号深度并不能证明收益。

讨论起点是 TLBStorage 的 `Mux1H(hitVecReg zip perm)`。RTL 降级为：

```text
permission = OR_{i=0..47}(hitVecReg[i] & entries[i].perm.r)
```

IR 保留了 packed hit mask 和 rank-one permission 状态数组，但逐个 slice/read 后
展开的 AND/OR 使 CPU 后端看不到整体选择结构。生成布局中 mask 为 `uint64_t`，
permission 为 `std::array<bool,48>`；旧代码逐项读取并累计。这个逻辑也可以处在 Event
锥中：phase 决定何时求值，不能消除求值内部的长链。

Chisel Mux1H 对多命中不提供选择结果保证；当前 OR 电路的多命中行为已经确定。
`maskedOr` 明确定义为所有选中项的按位 OR，零命中为零，不引入 one-hot 假设。
W=1 是布尔 maskedAny，W>1 则表示多位数据的相同结构。

## 为什么恢复 mask/array

如果只把 N 个已求出的布尔值塞进一个 reduction op，原先 N 次读取、frame spill 和
producer 计算仍可能全部发生。恢复原始 mask 与数组版本以后，代码生成才可以只访问
选中的行：

```cpp
auto remaining = hits & UINT64_C(0x0000ffffffffffff);
bool permission = false;
while (remaining != 0) {
    const auto i = std::countr_zero(remaining);
    if (perm_r[i]) { permission = true; break; }
    remaining &= remaining - 1;
}
```

W>1 使用整数 accumulator，扫描所有选中项。空 mask 不读取行；布尔结果为 true
即可结束。数组使用原有 row-backed 布局，避免每次重建 bool 临时数组或 permission
位图。输入本来就是两个 packed mask 时，优先使用已有 `and` + `reduceOr`。

`array` 仍是一个有 producer 的语义 value，不把裸指针藏进 parameter。状态读取的
版本、phase 和提交顺序必须保持：General 看到提交前值，Output 看到该阶段值；有
boundary/frame 快照时直接读取快照。只有稳定状态数组 view 才能跳过 whole-array
副本。Runtime helper 若有需要仍遵循 caller buffer ABI；首版无需新 helper。

## 已实现的边界

- 单个 CPU op，N>=2、W>=1，全部 two-state；支持 signedness/shape 视图以及
  超过 64 位的 mask/result。连续数组用 dense 编码，离散来源用静态 views 描述。
- 同一 phase 中恢复 OR 树；支持 bit AND、zero-else mux、replicate/sign-mask AND，
  接受部分覆盖、旋转/置换/重复索引、多个 mask、独立状态和计算结果。
- 规范化 singleton/assign/rep=1、concat/多维数组静态位视图；支持 mux 数据截断、
  嵌套 AND 中的广播 mask，以及最大区域失败后的内部子规约恢复。
- 保留共享/声明观察边界，保留副作用；只清理被替换区域中无用的纯 producer 和读。
- CPU dialect fingerprint 升为 v2；语义值仍为 core.logic。
- B7 位于 phase simplify 后、semantic seal 前；之后 C1–C8 只映射一次。
- C1/C2 按 2N-1 计费，C6/C8 估计循环大小；纯计算分类与 used-bits 认识新 op。
- C++ 使用 `countr_zero` 稀疏扫描，支持已有 row-backed 数组与 packed 值 fallback。
- 专项测试覆盖 multi-hit、小规模穷举、48/64/65/352 边界、Event、General 旧值采样、
  Output、跨 helper/TU、共享/观察前缀、JSON 往返、非法视图拒绝和无匹配幂等性。

实际香山 checkpoint 首次识别 220 个区域（Event 20、General 200），全部 N=48、
W=1；移除 16,648 个旧 op，其中 2,688 个为无用的 element read。共享读会保留，
所以恢复项数不等于实际省下的读取数。

扩展后的同 checkpoint 恢复 4,217 个区域（Event 25、General 4,192），覆盖首轮
审计固定的 3,440 个结构候选；生成 C++ 的 >=16 节点布尔链为 636 条。
这里的结构候选覆盖不是 Chisel 源码调用点覆盖，静态下降也不代表整机提速。

## 性能验证与下一步

`make bench_grhsim_masked_or` 比较编译优化后的完整展开表达式与当前稀疏代码，使用
运行时生成的 mask/array，覆盖零命中、单命中、四命中、全命中、布尔全零和不同 W。
首轮 Clang 22 `-O3` 的 48 项布尔结果中，零/单/四命中分别约 8.3/5.6/4.3 倍；
全选、数据全零时约为原速度的 0.39 倍。多位结果的密集扫描也可能较慢。
这是热数组微基准，不能据此推算整机收益。

整机比较需保持输入程序、seed、周期、编译选项和 trace 设置一致，测仿真总时间、
phase 时间和激活次数。融合会改变 General 的边界过滤和活动粒度，局部加速不保证
总时间下降。现阶段使用 `XS_WOLF_GRHSIM_IR_MASKED_OR=1` 显式开启。

后续按实测热点独立评估：

1. 多个 permission 字段共用 hit mask 时，复用一次置位遍历；或把相关字段组合成
   一次多位 maskedOr，避免重复解码选择位。
2. 给密集 mask 增加固定扫描/分组实现，由静态信息或 profiling 选策略；不把性能
   策略变成 op 语义。
3. 对数组写入维护 packed permission 布局，摊销打包成本。需完整处理 Init、
   部分写、多写优先级、提交和 trace 一致性。
4. 根据剩余热点考虑任意标量 `boolReduce` / `anyAnd` 或纯 producer 锥的惰性执行。
   这两种 op 和通用惰性执行均未在首版实现；后者需要额外副作用/采样证明。

所有 build/test/benchmark 通过 Makefile target 执行，日志与临时产物放在仓库 `ptmp/`。
