FIRRTL version 6.0.0
circuit top :
  public module top :
    input clk : Clock
    input wen : UInt<1>
    input addr : UInt<2>
    input data : {a : UInt<4>[2], b : SInt<5>}
    input mask : {a : UInt<1>[2], b : UInt<1>}
    output y : {a : UInt<4>[2], b : SInt<5>}
    mem m :
      data-type => {a : UInt<4>[2], b : SInt<5>}
      depth => 3
      reader => r
      writer => w
      read-latency => 0
      write-latency => 1
      read-under-write => undefined
    connect m.r.clk, clk
    connect m.r.en, UInt<1>(1)
    connect m.r.addr, addr
    connect m.w.clk, clk
    connect m.w.en, wen
    connect m.w.addr, addr
    connect m.w.data, data
    connect m.w.mask, mask
    connect y, m.r.data
