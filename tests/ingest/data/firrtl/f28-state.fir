FIRRTL version 6.0.0
circuit F28State :
  module F28State :
    input clock : Clock
    input reset : UInt<1>
    input enable : UInt<1>
    input data : UInt<8>
    output result : UInt<8>
    output heldResult : UInt<8>
    output arrayResult : UInt<8>[2]
    regreset state : UInt<8>, clock, reset, UInt<8>(7)
    reg held : UInt<8>, clock
    reg array : UInt<8>[2], clock
    connect array[0], data
    when enable :
      connect state, data
      connect array[1], state
    connect result, state
    connect heldResult, held
    connect arrayResult, array
