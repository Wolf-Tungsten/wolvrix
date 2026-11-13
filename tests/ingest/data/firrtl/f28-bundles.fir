FIRRTL version 6.0.0
circuit F28Bundles :
  module F28Bundles :
    input left : {bits : {lo : UInt<8>, hi : UInt<8>}, lanes : UInt<4>[2]}
    input right : {bits : {lo : UInt<8>, hi : UInt<8>}, lanes : UInt<4>[2]}
    input select : UInt<1>
    output copied : {bits : {lo : UInt<8>, hi : UInt<8>}, lanes : UInt<4>[2]}
    output result : UInt<8>
    node selected = mux(select, left, right)
    node alias = selected
    node nestedAlias = alias.bits
    connect copied, alias
    connect result, xor(nestedAlias.lo, nestedAlias.hi)
