> Historical independent-IR design, retired on 2026-10-26. APIs, pass names and
> implementation paths below describe that archived version. For the current
> unified GRH simulator, see the [simulation workflow](../../../grhsim.md).

# GrhSIM IR Flows

Flow 文档组织从模型输入、后端映射到生成物验证的完整路线。模型、方言和后端的字段定义
仍由各自规范维护；逐次实验结果不放在这里。

- [CPU 单线程活动度仿真](./cpu-st.md)：compute/commit、多事件域、活动度传播、C++ emit 和验收流程。
- [GrhSIM IR Overview](../overview.md)
