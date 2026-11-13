FIRRTL version 6.0.0
circuit F28Hierarchy :
  module F28Leaf :
    input a : UInt<8>
    output result : UInt<8>
    node inverted = not(a)
    connect result, inverted
  module F28Child :
    input a : UInt<8>
    output result : UInt<8>
    node masked = and(a, UInt<8>(15))
    inst leaf of F28Leaf
    connect leaf.a, masked
    connect result, leaf.result
  module F28Hierarchy :
    input a : UInt<8>
    input b : UInt<8>
    output leftResult : UInt<8>
    output rightResult : UInt<8>
    output combined : UInt<8>
    inst left of F28Child
    inst right of F28Child
    connect left.a, a
    connect right.a, b
    connect leftResult, left.result
    connect rightResult, right.result
    connect combined, xor(left.result, right.result)
