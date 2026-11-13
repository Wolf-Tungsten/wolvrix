FIRRTL version 6.0.0
circuit F28AbiPorts :
  extmodule BoxA :
    input r : {data : SInt<8>, flip reply : SInt<8>}[1]
    input ignored : UInt<0>
    output empty : UInt<8>[0]
    defname = F28ScalarImplementation
    parameter MASK = 1
  extmodule BoxB :
    input r : {data : SInt<8>, flip reply : SInt<8>}[1]
    input ignored : UInt<0>
    output empty : UInt<8>[0]
    defname = F28ScalarImplementation
    parameter MASK = 2
  module F28AbiPorts :
    input data : SInt<8>
    output left : SInt<8>
    output right : SInt<8>
    output alternate : SInt<8>
    inst first of BoxA
    inst second of BoxA
    inst third of BoxB
    connect first.r[0].data, data
    connect second.r[0].data, data
    connect third.r[0].data, data
    connect left, first.r[0].reply
    connect right, second.r[0].reply
    connect alternate, third.r[0].reply
