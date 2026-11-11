FIRRTL version 6.0.0
circuit top :
  public module top :
    input clock : Clock
    input en : UInt<1>
    input pred : UInt<1>
    printf(clock, en, "line-seven")
    assert(clock, pred, en, "line-eight")
    stop(clock, en, 0)
