FIRRTL version 6.0.0
circuit Top :%[[
  {"class":"example.Keep", "data":[true, false, null, -1.5e2, "a ]] ; b"]}
]] @[design.scala 1:1]
  type Word = UInt<8>
  layer Verification, bind, "verification" :
    layer Assert, inline :
  public module Top : @[design.scala 2:1]
    input clock : Clock
    input reset : AsyncReset
    input a : Word
    input b : UInt<8>
    input en : UInt<1>
    output packet : { valid : UInt<1>, data : UInt<32>[4] }[8]
    output xp : Probe<UInt<8>, Verification>
    output y : UInt<8>
    inst late of Later
    wire `0` : UInt<8>
    wire pair : const { flip in : UInt<8>, out : UInt<8> }
    regreset q : UInt<8>, clock, reset, UInt<8>(0h00)
    node sum = add(a, b) @[design.scala 3:4]
    node nested = xor(and(a, b), not(sum))
    node concat = cat(a, b, sum)
    node indexed = bits(concat, 7, 0)
    node multiline = mux(en,
      pad(a, 8),
      b)
    when en : @[design.scala 4:2]
      node local = UInt<8>(0b10101010)
      connect q, local
    else when eq(a, b) :
      connect q, UInt<8>(0o52)
    else : connect q, SInt<8>(-0d42)
    connect `0`, sum
    connect y, indexed
    define xp = probe(a)
    node observed = read(xp)
    cmem table : UInt<8>[16]
    read mport r = table[a], clock
    smem sequential : UInt<8>[4] old
    write mport w = sequential[1], clock
    connect w, b
    mem m :
      data-type => UInt<8>
      depth => 16
      read-latency => 1
      write-latency => 1
      read-under-write => old
      reader => r
      writer => w
    connect m.r.addr, a
    invalidate pair.out
    printf(clock, en, "a=\"%x\"; bracket=]\\\n", a) : log
    assert(clock, en, en, "check") : check
    layerblock Verification :
      layerblock Assert :
        intrinsic(circt_chisel_ifelsefatal<format = "bad\n", label = "fixture">, clock, en, en)
    stop(clock, en, 0) : done
  extmodule Later :
    input in : UInt<8>
    output out : SInt<9>
    defname = SvLater
    parameter N = 340282366920938463463374607431768211455
    parameter NEG = -1
    parameter TEXT = "hello\"world"
    parameter RAW = '`MACRO'
