# FIRRTL node attributes

`ingest/node_attributes.hpp` defines optional source associations on ordinary
GRH ops. Schema 2 and `firrtl.nodeScopePolicy=soft-node-v2` replace F02/F08
source-node isolation. Associations are partition hints, never optimization
scopes, execution owners, mandatory roots or DontTouch constraints.

| Attribute | Type | Meaning |
| --- | --- | --- |
| `firrtl.node.members` | string[] | Direct source-node associations for this op |
| `firrtl.node.roots` | string[] | Optional source-result associations still mapped accurately |
| `firrtl.node.rootResults` | int[] | Corresponding zero-based result indices |
| `firrtl.node.origins` | string[] | Diagnostic history, excluded from partition affinity |

Graphs carry integer `firrtl.nodeSchema=2`. A key is canonical JSON
`[sourceUnit,circuit,module,instancePath,nodeName,fieldPath]`; paths are arrays.
Flattening qualifies keys and inventories with actual instance path components.
Bundle leaves have distinct fields; Vectors retain LogicalType shape. No raw
entity IDs are persisted in the associations.

For `node n = and(eq(a,b),or(c,d))`, eq takes `[a,b]`, or takes `[c,d]`, and and
takes their results. All three start with member N; only and has root `(N,0)`.
Referencing n does not recursively label its upstream ops. After ordinary CSE
of `xor(and(a,b),c)` and `or(and(a,b),d)`, the shared and can have `{N1,N2}`;
xor/or each retain their own member. These overlapping sets are not must-link
components and are never transitively unioned by the partitioner.

## Optimization transparency

Functional CSE signatures exclude node attrs and `grhsim.nodeClone`. Ordinary
CSE shares across source nodes and SV, independently of the whole-node option.
Inlining/reassociation, constant/alias propagation, mux recovery, packing,
write normalization and DCE use ordinary functional constraints. Actual complete
execution owners, types, state/effects and declarations retain their constraints.

Equivalent ops union direct associations. New/replaced ops inherit bounded
associations from ops involved in the rewrite. Roots move only with an exact
result mapping; otherwise they are dropped. `redirectRoots` never creates an
op/value. Folding a node to an input does not require an assign carrier; a real
declaration/port view can still require one under unannotated rules.

`write` sorts/deduplicates sets and caps each op at 64 members, 64 roots and
64 origins. Exceeding any cap clears optional attrs and increments
`firrtl.nodeHintDegraded`, without rejecting the functional rewrite. No upstream
provenance closure is built. `inspect` checks schema, known names/types,
canonical/sorted/unique keys, paired root arrays and result bounds. Missing
roots, duplicate source roots, partial/nonconvex regions and phase copies are
valid. `grhsim.nodeClone=true` is diagnostic only. Representation corruption is
a verifier error; incomplete source regions are not.

`scanAttributeErrors` performs the same validation and returns the complete
ordered diagnostic list without building `inspect`'s node/member/root census.
`redundant-elim` uses this validation entry point. `copyQualified` borrows source
associations, builds final instance-qualified keys with a shared
`QualificationContext`, and moves the normalized vectors into the clone.
Result positions must match the source; no borrowed data survives target mutation.

## Policy and checkpoints

The lowerer establishes policy and `firrtl.nodeInventory` at source creation.
Flattening imports qualified inventories. `firrtl.nodeEliminated` records
source roots deleted by DCE. `firrtl.nodeRetention` v2 reports roots,
dead/constant/alias/shared/fragment identities, missing roots and key bytes.
Missing identities are diagnostic and never keep computation alive or fail a
legal optimization. Categories overlap.

Schema 1 / `source-node-v1` checkpoints require re-ingest; clearing attrs cannot
separate v2 identity. [Nonsink partitioning](../transform/grhsim-transforms.md#firrtl-soft-hints)
uses current members after simplify.

Playground validation targets: `test_firrtl_node_attributes`,
`test_wolvrix`. Transparency tests optimize annotated and stripped copies of
one graph and compare normalized functional structure/sharing plus independent
behavior. `inspect_firrtl_node_retention` / `inspect_firrtl_partitions` measure a
real hierarchy, metadata/JSON bytes and wall/RSS. Their stripped copy is only a
size census, never a resumable checkpoint.
