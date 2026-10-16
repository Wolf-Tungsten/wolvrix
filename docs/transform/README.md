# Wolvrix Transform Passes

This directory documents the transform passes that operate on GRH designs.

If you are using Wolvrix from Python, every transform pass is invoked through the session API:

```python
sess.run_pass("<pass-name>", design="design.main", ...)
```

The keyword layout is always the same:

- `design=...` selects the design that will be modified in place
- `out_*` names additional session values produced by the pass
- `in_*` names additional session values consumed by the pass
- all remaining keyword arguments are ordinary configuration options

## Typical Session Example

```python
with wolvrix.Session() as sess:
    sess.read_sv("top.sv", out_design="design.main")
    sess.run_pass("xmr-resolve", design="design.main")
    sess.run_pass("simplify", design="design.main")
    sess.run_pass("stats", design="design.main", out_stats="stats.main")
```

## Main Passes

| Pass | Purpose | Typical Use |
| --- | --- | --- |
| [`blackbox-guard`](blackbox-guard.md) | Create stub graphs for unresolved black boxes | importing incomplete designs |
| [`activity-schedule`](activity-schedule.md) | build GrhSIM activity supernode schedule data | GrhSIM emit and schedule analysis |
| [`comb-loop-elim`](comb-loop-elim.md) | detect and optionally repair combinational loops | loop cleanup and diagnostics |
| [`hier-flatten`](hier-flatten.md) | flatten hierarchy | later optimization or verification |
| [`instance-inline`](instance-inline.md) | inline a selected instance path | local expansion after partitioning |
| [`latch-transparent-read`](latch-transparent-read.md) | model latch transparency | latch-heavy RTL |
| [`slice-index-const`](slice-index-const.md) | convert constant dynamic slices to static slices | simplify slicing patterns |
| [`multidriven-guard`](multidriven-guard.md) | detect multiple-driver conflicts | structural checking before flattening |
| [`register-write-normalize`](register-write-normalize.md) | extract guarded writes and remove proven array hold paths | two-state array access recovery before scheduling |
| [`mux-tree-fold`](register-write-normalize.md#动态写恢复) | recover dynamic accesses from expanded array decoders | aggregate array read/write recovery |
| [`strip-debug`](strip-debug.md) | separate debug logic from main logic | DPI and system-task isolation |
| [`xmr-resolve`](xmr-resolve.md) | resolve cross-module references | hierarchical cleanup |
| [`memory-init-check`](memory-init-check.md) | validate memory initialization consistency | memory checking |
| [`repcut`](repcut.md) | partition one graph with RepCut | large-graph partitioning |
| [`simplify`](simplify.md) | run standard IR simplifications | general cleanup and optimization |
| [`stats`](stats.md) | collect design statistics | analysis and debugging |

## Internal Passes Used By `simplify`

| Pass | Purpose |
| --- | --- |
| [`const-fold`](const-fold.md) | constant folding |
| [`dead-code-elim`](dead-code-elim.md) | dead code elimination |
| [`redundant-elim`](redundant-elim.md) | redundant logic elimination |

## CLI Usage

```bash
wolvrix --pass=<pass-name> input.sv
wolvrix --pass=<pass-name>:-<option>=<value> input.sv
wolvrix --pass=<pass1> --pass=<pass2> input.sv
```

Example:

```bash
wolvrix \
  --pass=blackbox-guard \
  --pass=multidriven-guard \
  --pass=xmr-resolve \
  --pass=hier-flatten \
  --pass=simplify \
  input.sv
```

## Suggested Order

1. `blackbox-guard`
2. `memory-init-check`
3. `multidriven-guard`
4. `xmr-resolve`
5. `strip-debug`
6. `repcut`
7. `instance-inline`
8. `hier-flatten`
9. `latch-transparent-read`
10. `slice-index-const`
11. `simplify`
12. `comb-loop-elim`
13. `stats`

This is not a hard rule, but it is a reasonable starting point for a typical pipeline.

## Notes

- some passes rewrite the IR, so intermediate results can become stale
- `simplify` already includes several lower-level cleanup passes
- path-based passes depend on stable hierarchy
