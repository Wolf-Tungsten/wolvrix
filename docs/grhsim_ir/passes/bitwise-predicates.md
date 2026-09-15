# Bitwise predicates

`grhsim.bitwise-predicates` converts two-state, unsigned one-bit logical AND/OR
into bitwise AND/OR. Both operands and the result must have precisely that type;
the operation must have no parameters or object references and exactly two inputs
and one output. The pass has no options. It belongs after compute canonicalization
and shared-compute cloning, before CPU mapping.

For already evaluated SSA values `a` and `b` in {0,1}:

```text
%p = core.compute.logicAnd(%a, %b) : logic<1, false, 2-state>
%q = core.compute.logicOr(%a, %b)  : logic<1, false, 2-state>

=> %p = core.compute.and(%a, %b)
   %q = core.compute.or(%a, %b)
```

The first and second operands are the left and right Boolean input. There are no
hidden enable, clock, history, or output operands. AND returns one exactly when
both are one; OR returns one when either is one. For example, a=0,b=1 produces
p=0,q=1; a=1,b=1 produces p=1,q=1.

The CPU emitter uses direct scalar bit operations for these normalized values.
This expresses eager data dependence without introducing C++ short-circuit
branches between already computed values. It does not move the producer of
either operand or speculatively execute effects. It preserves operation/value
IDs, dependencies, object state, initialization, commit snapshots and event
semantics. Pools are compacted after replacement and the SemanticTransform
invalidates existing backend mappings. Ordinary CPU mapping reconstructs the
layout and notifications; this pass does not merge equivalent expressions or
undo localized clones.

Wider operands must remain logical: for 8-bit a=2,b=4, logical AND is one but
bitwise AND is zero. Signed one-bit, four-state, parameterized operations and
mixed-width operands are also left unchanged. Logical NOT is not transformed.
The generated-value optimization is selected from types and op semantics only.
