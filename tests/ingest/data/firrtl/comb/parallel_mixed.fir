FIRRTL version 6.0.0
circuit top :
  module before :
    input a : UInt<8>
    output y : UInt<8>
    node incremented = add(a, UInt<8>(3))
    connect y, incremented
  module implementation :
    input a : UInt<8>
    output y : UInt<8>
    node inverted = xor(a, UInt<8>(85))
    connect y, inverted
  module after :
    input a : UInt<8>
    output y : UInt<8>
    node incremented = add(a, UInt<8>(7))
    connect y, incremented
  extmodule External :
    input a : UInt<8>
    output y : UInt<8>
    defname = bridge
  public module top :
    input a : UInt<8>
    output y : UInt<8>
    inst first of before
    inst middle of External
    inst last of after
    connect first.a, a
    connect middle.a, first.y
    connect last.a, middle.y
    connect y, last.y
