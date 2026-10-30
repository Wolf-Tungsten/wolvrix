# FIRRTL node attributes

`ingest/node_attributes.hpp` defines schema v1 for source node ownership and
result boundaries on ordinary GRH operations. It also provides inspection and
the sharing rules used by `simplify` and `grhsim-simplify`. FIRRTL parsing,
complete node expression CSE and node-based nonsink grouping are later stages;
these attributes alone do not enable them.

Graphs containing the attributes carry integer metadata `firrtl.nodeSchema=1`.
All fields use the existing GRH JSON attribute representation:

| Attribute | Type | Meaning |
| --- | --- | --- |
| `firrtl.node.members` | string[] | Source node leaf expressions owning this computation |
| `firrtl.node.roots` | string[] | Source node leaf results supplied by this operation |
| `firrtl.node.rootResults` | int[] | Corresponding zero-based operation result indices |
| `firrtl.node.origins` | string[] | Diagnostic history, without sharing permission |

A key is compact canonical JSON
`[sourceUnit,circuit,module,instancePath,nodeName,fieldPath]`, with both paths
encoded as string arrays. `key`/`parseKey` preserve escaped names and distinguish
path components containing separators. `qualify` prepends an actual instance
path to all identities on a cloned operation. Bundle leaves have distinct field
paths; a Vector remains one shaped value. Entity IDs are absent from persisted
keys and roots: result indices resolve against the current operation's results.

For `node n = and(eq(a,b),or(c,d))`, the inputs of `eq` are `[a,b]`, the inputs
of `or` are `[c,d]`, and `and` takes their two results. All three operations have
`members=[N]`, where N is n's key. Only `and` has `roots=[N], rootResults=[0]`.
A later `node y = mux(n,x,z)` has its own member key Y; referencing n does not
add Y to n's computation.

`write` sorts and deduplicates member/origin keys and `(root,result)` pairs.
`inspect` checks the schema, known attribute names, types, canonical keys,
paired root arrays, result bounds, duplicate roots and escaping members without
a root. `GrhVerifyPass` reports these errors as diagnostics. `Inspection::report`
prints each key, member count and actual root operation/result/value symbol.
Origins-only fragments appear separately in `isolatedFragments` and retain
individual sharing barriers before execution leaves are assigned.

Functional CSE signatures exclude node attributes. A separate node scope key
restricts primitive sharing to the same member set. Unannotated SV operations
share as before, within their existing execution owner. Immutable constants can
be pooled. Once General operations have a complete nonsink leaf
`[general,nonsink,supernode,node]` or sink leaf
`[general,small-sink|large-sink,group]`, primitive sharing is allowed inside that
leaf. `sameScope` also compares execution owners, so equal node scopes do not
permit sharing across leaves. `cseScope` must be combined with the caller's
existing execution owner scope.

Structural inlining, NOT/XOR fusion, mux chain recovery and masked-OR recovery
respect these boundaries. Rewrites retain node attributes; new internal
operations inherit members/history, without copying their parent's roots.
`copyMembers` is intended for new operations. `unite` requires an explicit
reason for merging different member sets. `mergeCseOperations` checks primitive
scope; `mergeEquivalentOperations` accepts a result permutation after the caller
has proven whole-node equivalence. Neither helper proves functional equivalence.

When a simplification replaces a root with a same-scope result or a constant,
`redirectRoots` moves its result mapping. An input or another node's result uses
a transparent `kAssign` carrier carrying the original identity and result type.
For example, slicing bits 15:8 from `concat(a8,b8)` inside N leaves
`assign(a8)` as N's result carrier, and removes the concat/slice. It does not
attach N to a8's unrelated producer. Source labels do not prevent constant
folding or dead code removal.

Clone/freeze/JSON preserve the existing representation. Hierarchical inlining,
phase cloning, bit packing, whole-node CSE and nonsink consumption still require
their planned integration and end-to-end validation; the qualification and root
mapping helpers provide their metadata boundary.

Validation uses the playground Makefile:

```sh
make test_firrtl_node_attributes
make test_firrtl_cse_scope
make test_wolvrix
```

The scope suite checks independent two-state observations across normal and
GrhSIM simplification, array shapes, root-to-input rewrites, isolated fragments,
masked-OR boundaries and sharing within/across execution leaves. The metadata
suite covers escaped keys, instance qualification, result permutations, corrupt
attributes and clone/freeze/JSON persistence. It forces actual operation/value
ID compaction, renames their symbols, and checks that source identities and
resolved root mappings survive. Multi-result tests match each node leaf's root
value to its actual output port after result permutation and JSON reload.
