FIRRTL version 6.0.0
circuit F28MemoryEffects :
  module F28MemoryEffects :
    input clock : Clock
    input enable : UInt<1>
    input address : UInt<2>
    input data : UInt<8>
    output result : UInt<8>
    output delayedResult : UInt<8>
    cmem storage : UInt<8>[4]
    smem delayed : UInt<8>[4]
    write mport writer = storage[address], clock
    write mport delayedWriter = delayed[address], clock
    when enable :
      connect writer, data
      connect delayedWriter, data
    read mport reader = storage[address], clock
    read mport delayedReader = delayed[address], clock
    connect result, reader
    connect delayedResult, delayedReader
    printf(clock, enable, "data=%d", data) : trace
    assert(clock, UInt<1>(1), enable, "enabled") : check
    stop(clock, UInt<1>(0), 0) : done
