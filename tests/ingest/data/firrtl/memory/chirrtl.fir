FIRRTL version 6.0.0
circuit top :
  public module top :
    input clk : Clock
    input addr : UInt<2>
    input lane : UInt<2>
    input data : UInt<8>
    input part : UInt<4>
    input whole : UInt<1>
    input partial : UInt<1>
    output y : UInt<8>
    cmem m : UInt<4>[2][3]
    read mport r = m[addr], clk
    infer mport w = m[addr], clk
    when whole :
      connect w[0], bits(data, 3, 0)
      connect w[1], bits(data, 7, 4)
    when partial :
      connect w[lane], part
    connect y, cat(r[1], r[0])
