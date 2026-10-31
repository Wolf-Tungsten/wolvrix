FIRRTL version 6.0.0
circuit top :
  public module top :
    input c0 : Clock
    input c1 : Clock
    input a0 : UInt<2>
    input a1 : UInt<2>
    input d0 : UInt<8>
    input d1 : UInt<8>
    input e0 : UInt<1>
    input e1 : UInt<1>
    input readAddr : UInt<2>
    output y : UInt<8>
    cmem m : UInt<8>[3]
    read mport r = m[readAddr], c0
    when e0 :
      write mport p0 = m[a0], c0
      connect p0, d0
    when e1 :
      write mport p1 = m[a1], c1
      connect p1, d1
    connect y, r
