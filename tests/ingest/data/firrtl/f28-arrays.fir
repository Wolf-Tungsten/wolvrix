FIRRTL version 6.0.0
circuit F28Arrays :
  module F28Arrays :
    input data : UInt<8>[2]
    input index : UInt<1>
    output copied : UInt<8>[2]
    output result : UInt<8>
    output first : UInt<8>
    wire staging : UInt<8>[2]
    connect staging[0], data[0]
    connect staging[1], xor(data[1], UInt<8>(3))
    node alias = staging
    node staticRead = alias[0]
    node dynamicRead = alias[index]
    connect copied, alias
    connect result, dynamicRead
    connect first, staticRead
