# GSIM whole-node CSE

`grhsim-node-cse` runs on one flattened, unpartitioned `converted` graph after
local simplification, bit packing and masked-OR recovery, before Event/Output
extraction. It uses source boundaries from [node attributes](node-attributes.md);
it does not retain the FIRRTL AST or depend on GSIM/CIRCT at build or run time.

## Equivalence and sharing

The pass reconstructs a temporary DAG for each node from its unique result
root and member operations. At another node result it uses that node's exact
canonical expression term. Other boundary values retain their own ValueId and
complete type. Iterative postorder traversal and memoized interned child terms
avoid expanding shared subexpressions. A hash selects a bucket; full signature
comparison establishes equivalence, including kind, full LogicalType, sorted
functional attrs, constant payload and ordered operand terms/types. Source and
execution provenance do not participate. Equal-type assigns are transparent.

For example, these are two different whole expressions:

```firrtl
node p = xor(and(a, b), c)
node q = or(and(a, b), d)
```

Ordinary primitive CSE can share their internal `and` before this pass.
For two identical complete expressions the equivalence fact is computed first;
it can prove downstream equivalence even if the upstream expressions do not
meet the physical-sharing threshold. There is no commutative operand reorder
or other algebraic equivalence proof.

The first implementation shares an equivalent group when at least one holds:

| Test | Threshold |
| --- | --- |
| Group size | at least 5 nodes |
| Scalar result width | greater than 256 bits |
| A group member's distinct successors | greater than 1 |

Successors are consuming source nodes, graph outputs or effect/state terminals.
Repeated operands in one consumer count once; pure unannotated glue is followed
to its consumer boundary. A stable source-key representative receives merged
roots/member identities and users/outputs are redirected. Matching internal
terms inherit source identities. DCE removes obsolete private computations;
declared observations remain typed assign views. Small low-fanout groups remain
physically independent only when ordinary CSE has not already shared them.

Candidates require a complete, single scalar Logic result, input dependence
and only pure CSE-eligible members. Arrays/non-Logic, multiple results, state or
effects, execution owners, multiple roots, escaping private interiors, missing
root ownership and constant-only expressions are skipped and counted. Array
shape is retained rather than flattened to make a candidate eligible.

## Configuration and checkpoints

Factory options are `-top=NAME`, `-firrtl-node-cse=off|gsim` and `-report=PATH`.
Python uses `session.run_pass("grhsim-node-cse", firrtl_node_cse="gsim")`.
`off` computes/report equivalence without whole-node sharing; ordinary primitive
sharing is independent of this option and has no source-node barriers. Pure SV
uses the same ordinary optimizer and partitioner without this extra analysis.

The pass stores:

| Metadata | Value |
| --- | --- |
| `firrtl.nodeScopePolicy` | `soft-node-v2`, established at source ingest |
| `firrtl.nodeOptimization` | `gsim-soft-v2` |
| `firrtl.nodeCseConfig` | `v2/off/5/256/1` or `v2/gsim/5/256/1` |
| `firrtl.nodeCseStats` | versioned counts, thresholds hit and skip reasons |
| `firrtl.nodeRetention` | source availability and attribute byte census |

Same-policy reruns validate rather than physically merge again. Mode/revision
changes fail. Shared-flow resume rejects missing source-policy evidence or a
post-converted checkpoint lacking CSE evidence; re-ingest is required. A
completed converted checkpoint skips repeating the completed pre-CSE stage;
this does not preserve source-expression boundaries. Stage metadata and final SinkSchedule are rebuilt in
their normal order. Generation identity includes schema, scope policy,
optimization revision and CSE configuration.

The playground exposes `--firrtl-node-cse` in XS/re-emission scripts,
`XS_GRHSIM_FIRRTL_NODE_CSE` in Make, and
`GRHSIM_REEMIT_FIRRTL_NODE_CSE` for an explicit re-emission assertion.

## Validation

Use Make targets `test_grhsim_node_cse`, `test_firrtl_node_pipeline`,
`test_firrtl_node_attributes`, `test_firrtl_cse_scope`, `test_grhsim_flow`,
`test_grhsim_readable_store` and `test_wolvrix`. The complete-pipeline fixture
covers ordinary rewrites, conversion, packing, masked-OR, CSE, JSON and all
split/partition stages. Independent multi-cycle observations and compiled
FIRRTL/SV scoreboards check behavior. Thresholds, injected hash collisions,
functional/type differences, distinct instance inputs and clone/rename/ID
compaction are explicit regressions.

`make inspect_firrtl_node_retention FIRRTL_NODE_MODULE=RenameTable
FIRRTL_NODE_CSE=off|gsim` measures one self-contained real FIRRTL hierarchy.
It reports key reference/byte repetition, JSON size with/without node metadata,
elapsed time and process RSS. These are compiler/representation measurements,
not an end-to-end XiangShan simulation speedup. Current op associations are
consumed separately by the common nonsink partitioner.

Schema 2 associations may overlap or lose roots during ordinary optimization.
Incomplete/multiple-root/overlapping-dependency candidates are skipped; they
cannot make a valid executable graph fail or require freezing earlier simplify.
F08 strong-isolation checkpoints are incompatible and must be re-ingested.
