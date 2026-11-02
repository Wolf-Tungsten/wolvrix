# RTL source and simulation critical paths

This opt-in diagnostic operates on a completed **GrhSIM + XiangShan clean
32-thread build**. It does not alter the production package, RTL, default
Verilator path, or default clean-build settings. The first implementation
reports batch measurements and their associated RTL, rather than estimating
elapsed time for individual source lines.

## Run

From the outer playground repository:

```bash
make xs_grhsim                         # existing clean flow, if not built yet
make xs_grhsim_critical_path
```

The second command reads `build/xs/grhsim32/result.json`, follows its retained
stage, and writes `build/xs/grhsim32/critical-path/`. It needs the original
compiler, RTL sources, generated Difftest interface, CoreMark image and NEMU
library. Like the clean flow, it targets x86-64 Linux and selects 32 local CPUs.
Generation can use all available CPUs. It does not require a particular host
or CPU number. A large XiangShan report needs substantial RAM and disk space;
the operation mapping alone is several GB.

```bash
make xs_grhsim_critical_path XS_GRHSIM_CRITICAL_ARGS='--output build/xs/critical-next --runtime-cpus 96-127 --codepages thp'

# Separate stages; explicit reuse verifies plan/batch membership and input hashes.
python3 scripts/grhsim_critical_path.py static --output build/xs/critical-static
python3 scripts/grhsim_critical_path.py profile --output build/xs/critical-static
python3 scripts/grhsim_critical_path.py report --output build/xs/critical-static
```

`run` (the make default) performs all stages. `--native-dir DIR` explicitly
reuses an existing native source export. `--reuse-discovery` explicitly reuses
completed wrapper-only discovery traces, after checking the binary, workload,
CPU set, window, page mode and trace identities. Choose a new output directory
when changing the binary, emitter configuration or instrumentation. No old
measurement silently becomes a new experiment.

The default window is **1,000 cycles after reset for warmup, then 10,000 cycles,
two independent repeats**. All 32 lanes have phase timestamps. The three
partitions with the largest accumulated eval completion margin are selected
automatically for batch probes; each selected partition gets two additional
runs. Adjacent half-cycles are sampled every 64 cycles. Use
`--part-limit 32` to collect all partitions, including those with no stable
critical-step votes. With the default stride this performs 2 discovery runs
and 64 batch-probe runs. `--sample-stride 1` additionally samples every cycle,
at much greater cost. Batch probes instrument one selected lane per run.

`--codepages auto` attempts THP on the first discovery run, then fixes that
run's **effective** page mode for every remaining run. Mixed or partial page
coverage is rejected. `--codepages thp` requires complete code THP. The report
records actual CPUs, pages and sampling settings. These binaries are diagnostic:
their process time is not a normal walltime comparison.

## Files and interpretation

| File | Contents |
| --- | --- |
| `report.html` | 中文交互报告，首先列出每个分区、每个 clock 的动态第一名及 RTL/Chisel 定位；其后单独列出静态路径 |
| `dynamic_top1.json` / `.csv` / `.md` | 动态第一名、统计范围、样本数、重复排名、分轮次耗时、入口零掩码占比、RTL 模块/信号与 Chisel 候选 |
| `critical_paths.md` / `critical_paths.csv` | Direct path list for review without opening a browser; each row has partition, clock, modules, signal/expression chain and source locations |
| `rtl_paths.json` | Structured Chinese view data used by the report, with exact path nodes and batch-to-module/signal associations |
| `chisel` in `rtl_paths.json` | Chisel package/class candidates, declaration lines, signal-match scores and ambiguity status |
| `report.json` | Full rankings, static paths, source associations, phase/round costs and limitations |
| `critical_steps.jsonl` | Last completing partition, longest body, thread, global duration and exclusive completion margin for each step/phase/repeat |
| `mapping/source_map.json` | Stable source spans, batch membership, work inventory, input bit targets and emitted cross-partition copy sites |
| `mapping/operation_map.jsonl` | Graph-local operation/value IDs, symbols, group, active ID, phase, source span and recorded transformation annotations |
| `mapping/batch_map.csv` | Compute/commit batch to groups, active IDs and source-span sets |
| `mapping/mapping_summary.json` | Mapped/unknown operation and work coverage; multi-source batch count; ASC coverage |
| `mapping/operation_asc_map.jsonl` | Exact original-graph ASC memberships for queried diagnostic partitions |
| `discovery/`, `batch-probe/` | Separate diagnostic packages, binaries, trace files, compiler/alignment/archive audits, and functional logs |
| `static_inputs.json`, `asc/inputs.json`, `runs.json` | Input/tool identities, source-to-plan proof and run settings |

`source_span_id` hashes the normalized source path and line/column range. Paths
inside this build's RTL root use an `rtl/` prefix, so relocating the checkout
does not change these IDs. Graph-local operation/value IDs are qualified by
partition and frozen graph identity. IDs are not assumed stable across
regeneration.

### 动态第一名

报告按 `(partition, clock)` 选择实测 batch 累计时间最大的项，计算与提交批次
共同参与排名。第一名不使用静态工作量、源码规模或单次最大值决定，也不将时间
按操作数分配给 RTL 信号。提供两个可切换范围：

- **全局稳定拖尾采样**：只计两次独立 discovery 中该分区均最后完成的步骤。
  未采到这类步骤时显示“没有稳定拖尾采样”，不借用静态第一名。
- **分区全部采样**：包含该分区非拖尾步骤，用于查看本分区成本。
  它不能证明这个 batch 决定了全局完成时间。

两次 batch probe 分别排名，再给出合计第一名、每次第一名、并列项和一致次数。
采样步数是各次运行之和，同一输入步骤会在重复运行中再次采样，不代表独立输入数。
占比的分母为同分区、同 clock、同统计范围的所有 batch 采样时间。
报告同时显示调用数、平均/最大单次时间、首轮/提交后/后续传播时间以及入口零掩码时间。
入口为零的检查成本与批次正文不能简单等同；多活动字节 batch 用 `entry_word=UINT32_MAX`
和各字节掩码的按位或记录“全零/非零”，具体字节和 dispatch mask 保存在 build manifest。

完整 32 分区示例：

```bash
make xs_grhsim_critical_path XS_GRHSIM_CRITICAL_ARGS='--output build/xs/critical-all32 --part-limit 32'
```

### Chisel 映射

中文路径视图还会扫描 canonical XiangShan Scala 源码，排除 `out/`、`build/`、
`target/` 和 `mill-out/`。生成 RTL module 先按名称匹配 Scala 的 `class`、`object`
或 `trait`，再去掉数字实例后缀（例如 `MSHR_64` 归一为 `MSHR`），最后用 RTL
源码行里的稳定信号前缀给同名候选打分。

结果会标成唯一匹配、信号最佳匹配、名称唯一但缺少信号证据、歧义匹配或未解析；
歧义时保留所有候选，不从扁平 RTL 名称猜 Chisel 实例层次。报告展示 package、
Scala 文件和声明行。这是模块/源码证据映射，不等同于唯一的 Chisel Data 或表达式
来源；`Vec` 展开、wrapper、重命名和 elaboration 可能破坏一对一关系。

Existing GRH provenance is retained, including `origin`, `pass`, `note`, clone
locations and serialized locations. Many current ingest operations carry only
a **point** (start line/column). They remain points; no end range is invented.
Recorded pass annotations are not a complete transformation history. Generated
operations without a reliable operation/result source remain unknown. A batch
can contain many source spans, so its time must not be apportioned to lines by
operation count or summed once per associated span.

The ASC join rebuilds the original ASC membership from `memory.json`, using the
same builder as the clean assignment flow. It matches operation symbol, kind,
result symbols and assigned partition exactly. It queries only the automatically
selected diagnostic partitions. Renamed, rewritten or unqueried operations
remain unknown. `operation_asc_map.jsonl` is an enrichment sidecar; it does not
replace the operation map's originally unknown ASC fields.

Static paths use the existing GrhSIM structural work inventory and activity
masks for the explicitly alternating clock. They traverse combinational
operand dependencies only. Register, latch, memory, DPI and system-task
boundaries cut paths; implicit cross-cycle state edges are not introduced.
These paths are **not chip timing paths**, probabilities, instruction counts
or elapsed-time predictions. Memory bridge accesses, fanout and shared-source
associations are reported as structure. Cross-partition edges come from actual
wrapper copy sites. Only input activation targets actually exported by the
planner are attached to batches; whole-input/unknown targets remain explicit.

Dynamic discovery uses absolute monotonic timestamps: a late-starting short
thread can finish after a longer body. Both last completion and longest body
are reported. Exclusive margin is the last completion minus the second-last
completion. Host completion after that is separate. Update means inter-lane
publication; commit batches execute **inside eval**.

Batch critical-step labels are frozen from steps where independent discovery
runs agree on the last partition. Timing a batch perturbs execution, so batch
probe runs do not select their own critical-step labels. Batch intervals retain
thread, clock, round, entry activity word/mask, and whether the previous commit
activated another round. An entry mask does not prove which groups execute
later within that batch. Zero-mask dispatcher costs are shown separately.
The report classifies static-and-dynamic, static-only, dynamic-only and unknown
source associations. “Dynamic” means measured in a stably tailing partition's
sampled step; it is not proof that an individual RTL expression causes the tail.

The HTML includes the 100 leading batch associations and every dynamic winner,
up to 80 source entries per batch, and all partition/clock static rank-one paths.
Long static paths are folded after 36 displayed nodes; JSON retains their full
order. Counts and truncation are visible. Source excerpts are displayed as text,
without executing RTL filenames or source text as HTML.

## Conservative AST rewrite interface

The initial allowlisted rule is `unsigned-full-width-static-slice`:

```systemverilog
// a is an unsigned simple packed vector [7:0], with the same four-state type.
assign y = a[7:0];
// Candidate preserves this assignment process:
assign y = a;
```

Slang parses and elaborates the complete supplied top/filelist/options. A site
must be safe in **all elaborated parameter instances**. Signed values, partial
or dynamic selects, memory indexing, macros, delayed assignments and event
control cones are rejected. The tool does not rewrite clock/reset events,
state write priority, memory ports, DPI/system tasks or short-circuit logic.
The other proposed rule families (CSE, guard sharing and general alias removal)
are not enabled in this first allowlist.

```bash
cmake --build build/grhsim32-tools --target grhsim32_rtl_rewrite -j
python3 scripts/grhsim_rtl_rewrite.py propose \
  --report build/xs/grhsim32/critical-path/report.json \
  --source-map build/xs/grhsim32/critical-path/mapping/source_map.json \
  --filelist build/xs/grhsim32/input.f --top SimTop \
  --read-args /path/to/exact-slang-read-args.txt \
  --output build/xs/rewrite-candidate
```

Use the same include directories, defines and parameter options as the original
frontend invocation. `propose` requires overlap with a measured source span.
If no supported AST site overlaps a hotspot, it writes `status=no-candidate`.
Otherwise it writes original and rewritten RTL, two filelists, a reviewable
`change.patch`, and a manifest with hashes, source ranges, rule, structural
change and verification status. The original source tree is never modified.
The candidate applies only to the elaborated configuration, not every possible
parameterization.

## Finite cycle differential validation

```bash
python3 scripts/grhsim_rtl_rewrite.py verify \
  --manifest build/xs/rewrite-candidate/manifest.json \
  --commands /path/to/commands.json \
  --stimulus /path/to/window-a.jsonl --stimulus /path/to/window-b.jsonl \
  --cycles 100000 --output build/xs/rewrite-verification
```

`commands.json` contains identical `baseline` and `candidate` argv templates.
The runner substitutes `{rtl}` / `{filelist}`, `{stimulus}`, `{trace}`, `{cycles}`,
`{window}`, `{variant}` and `{include_directory}`; no shell expansion occurs.
Use `{include_directory}` to preserve relative includes after copying a source.
The adapter must consume the fixed inputs and record actual simulation output.
The checked-in `testcase/grhsim-critical-path/verilator_trace.py` demonstrates
this contract on a small Verilator fixture; it is **not** a generic XiangShan
observation adapter. NEMU checks are used for XiangShan diagnostics, while a
future XiangShan RTL candidate must supply its chosen per-cycle observations.

Stimulus JSONL contains one `{ "cycle": N, "inputs": {...} }` object per cycle.
Trace JSONL begins with:

```json
{"schema_version":1,"stimulus_sha256":"...","observables":["outputs.y","state.q"]}
```

It then contains exactly the requested number of records:

```json
{"cycle":0,"inputs":{"a":1,"reset":1},"outputs":{"y":1},"state":{"q":0},"commits":[]}
```

Cycles must be consecutive from zero. Inputs must match the immutable stimulus;
all outputs, declared observations, state and ordered commit lists must match.
Missing, extra or reordered cycles, mismatched headers, changed inputs, absent
observables, process failures and mismatches reject the candidate. Two distinct
input files are required. Validation commands, trace hashes, source identities
and results are saved; candidate inputs are checked before and after execution.

This is finite stimulus evidence, **not a formal equivalence proof**, and
Verilator does not supply exhaustive four-state validation. The local AST rule
is deliberately narrower than the differential test's ability to detect bugs.
No candidate is promoted to a default or advertised as faster by this tool.
A XiangShan candidate still requires complete GrhSIM regeneration, the specified
100,000-cycle/two-window observations, and at least eight fresh AB/BA pairs
before any walltime benefit can be reported. Behavioral equivalence and formal
backend hooks remain future work; there is no option that silently weakens
strict-cycle checks.

## Tests

```bash
cmake --build build/grhsim32-tools --target \
  grhsim32_source_map_fixture grhsim32_rtl_rewrite grhsim32_frontend -j
PYTHONPATH=scripts python3 -m unittest -v \
  scripts/test_grhsim_critical_path.py \
  scripts/test_grhsim_dynamic_top.py \
  scripts/test_grhsim_critical_path_integration.py \
  scripts/test_grhsim32.py
```

The native tests cover clone/JSON provenance, state boundaries and actual RTL
ingestion. Python tests cover source IDs, membership joins, trace corruption,
missing cycles, phases, source attribution and page audit. Slang tests cover the
allowlist's rejection conditions. The integration test builds both RTL versions
with real Verilator and compares two 100,000-cycle trajectories, including
resets and sequential state. Artifacts stay under `wolvrix/build/artifacts/`.
