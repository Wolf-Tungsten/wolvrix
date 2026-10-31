FIRRTL version 6.0.0
circuit top :
  public module top :
    input clk : Clock
    input arst : AsyncReset
    input srst : UInt<1>
    input resetData : UInt<4>
    input value : UInt<4>
    input enable : UInt<1>
    input whole : UInt<1>
    input row : UInt<1>
    input last : UInt<1>
    input overwrite : UInt<1>
    input i : UInt<2>
    input j : UInt<2>
    input source : { data : UInt<4>[2], flag : UInt<1> }[2]
    output asyncOut : UInt<4>
    output syncOut : UInt<4>
    output nextOut : UInt<4>
    output result : { data : UInt<4>[2], flag : UInt<1> }[2]
    regreset a : UInt<4>, clk, arst, UInt<4>(3)
    regreset s : UInt<4>, clk, srst, resetData
    reg r : { data : UInt<4>[2], flag : UInt<1> }[2], clk
    node next = add(a, UInt<4>(1))
    when enable :
      connect a, next
      connect s, add(s, value)
    when whole :
      connect r, source
    when row :
      connect r[i], source[0]
    when last :
      connect r[r[0].data[0]].data[j], value
      connect r[i].flag, UInt<1>(1)
    when overwrite :
      connect r, source
    connect asyncOut, a
    connect syncOut, s
    connect nextOut, bits(next, 3, 0)
    connect result, r
