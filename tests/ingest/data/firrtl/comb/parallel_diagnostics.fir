FIRRTL version 6.0.0
circuit top :
  module a_loop :
    input a : UInt<1>
    output y : UInt<1>[2]
    wire pair : UInt<1>[2]
    connect pair[0], a
    connect pair[1], not(pair[0])
    connect y, pair
  module z_loop :
    input a : UInt<1>
    output y : UInt<1>[2]
    wire pair : UInt<1>[2]
    connect pair[0], a
    connect pair[1], not(pair[0])
    connect y, pair
  public module top :
    input a : UInt<1>
    output y : UInt<1>[2]
    inst first of z_loop
    inst second of a_loop
    connect first.a, a
    connect second.a, first.y[1]
    connect y, second.y
