# `expr-opt`

`expr-opt` is the standalone GRH counterpart of GSIM's `ExprOpt`.
It runs on the unpartitioned ordinary GRH graph before `grh-to-grhsim`, after
the caller's explicit `dead-code-elim` pass, matching GSIM's
`RemoveDeadNodes → ExprOpt` boundary.
The pass does not perform DCE; it performs expression cleanup only.

The current GRH representation has no separate WHEN, ASUINT, or ASASYNCRESET
operation kinds. Their lowered identity views are represented by `kAssign`,
and the tree rewrite that remains observable in GrhSIM is a `kMux` whose two
data arms are the same value. The pass replaces that mux with the arm while
preserving an output/declaration view when the value is externally visible.

The pass uses an empty declaration anchor set by default. Input/output ports,
state accesses, and effectful operations remain observable through the GRH
root rules. The rewrite keeps an explicit alias row and transfers
operation/value SemanticTree IDs to the surviving value; DCE of any newly
dead cone remains a separate caller action.
For separately-created but structurally equivalent MUX arms, that merge is
recursive, so both source subtrees retain their operation/value identities
before the duplicate arm is reclaimed.

The pass is registered as `expr-opt` and accepts only `-top=<graph>`.
