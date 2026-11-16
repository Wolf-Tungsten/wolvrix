# Producer-change caching inside conditional compute regions

The experimental attribute `conditional_mux_change_cache` (environment variable
`WOLVRIX_GRHSIM_CONDITIONAL_MUX_CHANGE_CACHE`) defaults to **off**. An explicit
attribute takes precedence over the environment. It requires
[conditional mux cones](grhsim-conditional-mux-cones.md); disabling that parent
disables this extension.

Merged regions can run because of an unrelated changing input even while a
private expression has stable inputs. The existing
[exact-input cache](grhsim-conditional-mux-input-cache.md) compares 1–4 keys.
This extension applies to the same proven cones with 5–32 scalar boundary
sources. It uses source-change notifications and one sticky dirty bit per cone,
so a consumer visit does not compare every source. Both cache options may be
used together; their source-count ranges are disjoint.

For a `kMux`, operands are `[condition, true_value, false_value]`. Its result
selects the true value when condition is nonzero. The complete source set covers
the selector and **both** arms, including an unselected arm:

```text
init: dirty = true
producer update: dirty |= source_changed
consumer region:
    if dirty:
        result = condition ? private_true_chain : private_false_chain
        perform the existing result change check and downstream notification
        dirty = false
    later consumers read the existing persistent result
```

## Safety and invalidation

The parent cone still requires scalar pure operations, private intermediates,
16–256 operations in each absorbed arm, valid schedule order and no storage
alias hazards. The root must own an exclusive canonical persistent scalar slot.
The rule neither allocates persistent slots for local results nor changes region
membership. Dependency expansion is bounded by depth 32 and 256 visited values;
proof failure retains the original lowering.

Each boundary source must have a proven notification path:

- A model input is checked against the previous input before that value advances.
- A live ordinary scalar register view is invalidated at the actual state write.
  A register with no writers is immutable until `init()`.
- A materialized value, including a materialized register read, is invalidated
  when **that stored value** changes. It requires an exclusive canonical scalar
  slot and existing tracked-change lowering. It must not be mistaken for a live
  state view: commit can precede the update of the stored read snapshot.

The dirty bit accumulates changes until the corresponding cone is evaluated.
A notification may set an already-dirty bit, and multiple changed sources may
coalesce into a single miss. The bit is cleared only after computing the root;
reinitialization sets every bit again. Register-to-memory storage, latches,
side effects, behavioral groups and unproven aliases fall back. Fullpass modes,
local reordering and the parent's conflicting lowering modes also fall back.
All normal result changes and region activation remain governed by the original
scheduler. A cache hit suppresses only a computation with provably valid output.

## Cost and validation

The execution plan reports `conditional_mux_change_cache_roots` and source
counts, and marks dependent body execution as unmodeled. It is not an estimate
of notifier instruction cost. Each eligible source adds a dirty update, which
can outweigh saved arithmetic. Measure both notifier-site executions and actual
changed-source notifications; consumer hit rate alone is insufficient.

`emit-grhsim-mux-change-cache` checks default/off identity, attribute precedence,
parent/fullpass fallback, read-only and writable state, untracked-persistent-source fallback,
repeated initialization, clock-only updates, held-clock data changes and
unrelated region activation. Generated models run against an independent oracle
at O0 and O3 with ASan/UBSan. Existing conditional-cone and exact-input-cache
regressions must also pass.

The proof uses graph semantics rather than CV32E40P names or profile thresholds.
XiangShan control cones may qualify, but fanout and source-update frequency can
make notification cost different. Cross-partition performance and XiangShan
benefit require separate evidence; this feature is not enabled by default.

An optional [producer-region notification aggregation](grhsim-grouped-cache-notifications.md)
can group these updates without changing cache eligibility. It also defaults to off.
