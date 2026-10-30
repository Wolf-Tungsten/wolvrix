FIRRTL version 6.0.0
circuit T :
  ; Type aliases have their own namespace and can match module names.
  type T = {| Idle, Ready : UInt<8> |}
  layer A, bind :
    layer B, inline :
  public module T enablelayer A :
    input clock : Clock
    input a : UInt<8>
    input en : UInt<1>
    output y : UInt<8>
    output p : RWProbe<UInt<8>, A.B>
    wire e : T
    node tagged = T(Ready, UInt<8>(0h2))
    connect e, tagged
    match e :
      Idle : skip
      Ready(payload) :
        connect y, payload
    object obj of C
    propassign obj.x, integer_add(Integer(-8), Integer(9))
    propassign obj.words, List<String>("a", "b")
    propassign obj.flag, bool_and(Bool(true), Bool(false))
    propassign obj.real, Double(-1.25e-2)
    propassign obj.path, path("~T|T")
    propassert prop_eq(obj.x, Integer(1)), "one"
    node r = intrinsic(example_value<width = 8>: UInt<8>, a)
    define p = rwprobe(y)
    force_initial(p, a)
    release_initial(p)
    force(clock, en, p, a)
    release(clock, en, p)
    fprintf(clock, en, "file_%d", a, "a=%d", a)
    fflush(clock, en, "file_%d", a)
    assume(clock, en, en, "assume")
    cover(clock, en, en, "cover")
  class C :
    input x : Integer
    input words : List<String>
    input flag : Bool
    input real : Double
    input path : Path
    output z : Integer
    propassign z, x
  extclass Ext :
    output value : AnyRef
  intmodule Builtin knownlayer A, A.B :
    input a : UInt<8>
    output b : UInt<8>
    intrinsic = example_builtin
    parameter N = 17
  formal Test of T :
    depth = 16
    options = { name = "example", values = [1, -2, 'raw'] }
