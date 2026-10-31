FIRRTL version 6.0.0
circuit top :
  public module top :
    input clk : Clock
    input en : UInt<1>
    input gate : UInt<1>
    input data : UInt<8>
    output y : UInt<8>
    reg q : UInt<8>, clk
    connect q, data
    connect y, q
    printf(clk, en, "A:%d/%x/%b/%c/%%\n", q, q, UInt<3>(5), q) : first
    when gate :
      printf(clk, en, "B:%d\n", q) : second
      cover(clk, UInt<1>(1), en, "C%literal{{comment}}\n") : hit
    assert(clk, UInt<1>(1), en, "unreachable")
    assume(clk, UInt<1>(1), en, "unreachable")
    intrinsic(circt_chisel_ifelsefatal<format = "unreachable", label = "chisel3_builtin">, clk, UInt<1>(1), en)
