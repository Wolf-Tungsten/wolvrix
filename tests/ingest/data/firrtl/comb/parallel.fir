FIRRTL version 6.0.0
circuit top :
  public module identity :
    input a : UInt<1>
    output y : UInt<1>
    connect y, a
  public module invert :
    input a : UInt<1>
    output y : UInt<1>
    node inverted = not(a)
    connect y, inverted
  public module mask :
    input a : UInt<1>
    output y : UInt<1>
    connect y, and(a, UInt<1>(1))
  public module chain :
    input a : UInt<1>
    output y : UInt<1>
    node n0 = not(a)
    node n1 = not(n0)
    node n2 = not(n1)
    node n3 = not(n2)
    node n4 = not(n3)
    connect y, n4
  public module branch :
    input a : UInt<1>
    output y : UInt<1>
    inst first of invert
    inst second of mask
    connect first.a, a
    connect second.a, first.y
    connect y, second.y
  public module top :
    input a : UInt<1>
    output y : UInt<1>
    inst first of branch
    inst second of chain
    inst third of identity
    connect first.a, a
    connect second.a, first.y
    connect third.a, second.y
    connect y, third.y
