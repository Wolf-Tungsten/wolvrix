FIRRTL version 6.0.0
circuit top :
  public module a_error :
    output a_missing : UInt<1>
  public module z_error :
    output z_missing : UInt<1>
  public module valid :
    output y : UInt<1>
    connect y, UInt<1>(1)
  public module top :
    output y : UInt<1>
    inst first of z_error
    inst second of a_error
    inst third of valid
    connect y, xor(first.z_missing, xor(second.a_missing, third.y))
