FIRRTL version 6.0.0
circuit top :
  public module top :
    input clk : Clock
    input reset : UInt<1>
    input initialValue : UInt<8>[4]
    input en0 : UInt<1>
    input en1 : UInt<1>
    input addr0 : UInt<3>
    input addr1 : UInt<3>
    input data0 : UInt<8>
    input data1 : UInt<8>
    input copy : UInt<1>
    output tableOut : UInt<8>[4]
    output readOut : UInt<8>
    regreset table : UInt<8>[4], clk, reset, initialValue
    node sampled = table[addr0]
    node payload = mux(copy, sampled, data1)
    when en0 :
      connect table[addr0], data0
    when en1 :
      connect table[addr1], payload
    connect tableOut, table
    connect readOut, table[addr1]
