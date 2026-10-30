FIRRTL version 6.0.0
circuit top :
  public module top :
    input a : UInt<5>
    input b : UInt<3>
    input sel : UInt<1>
    output y : UInt<7>
    output local : UInt<6>
    output aliasOut : UInt<5>
    output constantOut : UInt<8>
    node n0 = add(and(a, pad(b, 5)), UInt<5>(1)) @[comb.scala 17:3]
    node n1 = add(and(a, pad(b, 5)), UInt<5>(2))
    node repeated = add(and(a, pad(b, 5)), and(a, pad(b, 5)))
    node alias = a
    node constant = UInt<8>(42)
    wire selected : UInt
    connect selected, n0
    when sel :
      connect selected, n1
    node result = add(selected, UInt<2>(1))
    connect y, result
    connect local, repeated
    connect aliasOut, alias
    connect constantOut, constant
