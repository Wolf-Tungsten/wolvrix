FIRRTL version 6.0.0
circuit top :
  public module top :
    input a : SInt<5>
    input b : SInt<3>
    input sh : UInt<2>
    output sum : SInt<6>
    output product : SInt<8>
    output quotient : SInt<6>
    output remainder : SInt<3>
    output less : UInt<1>
    output bitxor : UInt<5>
    output left : SInt<8>
    output right : SInt<5>
    output negative : SInt<6>
    node nsum = add(a,b)
    node nproduct = mul(a,b)
    node nquotient = div(a,b)
    node nremainder = rem(a,b)
    connect sum, nsum
    connect product, nproduct
    connect quotient, nquotient
    connect remainder, nremainder
    connect less, lt(a,b)
    connect bitxor, xor(a,b)
    connect left, dshl(a,sh)
    connect right, dshr(a,sh)
    connect negative, neg(a)
