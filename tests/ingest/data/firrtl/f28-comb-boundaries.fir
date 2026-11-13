FIRRTL version 6.0.0
circuit F28CombBoundaries :
  module F28CombBoundaries :
    input a : UInt<8>
    input b : UInt<8>
    input select : UInt<1>
    output result : UInt<9>
    output repeated : UInt<9>
    output aliasResult : UInt<8>
    node sum = add(a, b)
    node masked = and(a, b)
    node alias = masked
    node duplicate = add(and(a, b), and(a, b))
    wire selected : UInt<9>
    connect selected, mux(select, sum, pad(alias, 9))
    connect result, selected
    connect repeated, duplicate
    connect aliasResult, alias
