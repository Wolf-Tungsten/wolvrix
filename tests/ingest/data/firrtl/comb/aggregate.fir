FIRRTL version 6.0.0
circuit top :
  public module top :
    input packet : {valid : UInt<1>, data : UInt<4>[2], meta : {flag : UInt<1>}}[2]
    input other : {valid : UInt<1>, data : UInt<2>[2], meta : {flag : UInt<1>}}[2]
    input one : UInt<1>[1][1]
    input sel : UInt<1>
    input sel2 : UInt<1>
    input i : UInt<2>
    input j : UInt<2>
    input data : UInt<4>
    output result : {valid : UInt<1>, data : UInt<5>[2], meta : {flag : UInt<1>}}[2]
    output tableOut : UInt<4>[2][2]
    output readOut : UInt<4>
    output sub : UInt<4>[2]
    output packed : UInt<16>
    output equal : UInt<1>
    output single : UInt<1>[1][1]
    output lastOut : UInt<4>[2]
    node selected = mux(sel, packet, other)
    node aliasPacket = packet
    wire table : UInt<4>[2][2]
    wire last : UInt<4>[2]
    connect result, selected
    connect table[0], aliasPacket[0].data
    connect table[1], aliasPacket[1].data
    when sel :
      connect table[i][j], data
    when sel2 :
      connect table[j][i], UInt<4>(15)
    connect tableOut, table
    connect readOut, table[i][j]
    connect sub, table[i]
    connect packed, cat(table[1][1], table[1][0], table[0][1], table[0][0])
    connect equal, and(and(eq(tableOut[0][0],table[0][0]),eq(tableOut[0][1],table[0][1])),and(eq(tableOut[1][0],table[1][0]),eq(tableOut[1][1],table[1][1])))
    connect single, one
    connect last[0], data
    connect last, packet[0].data
    connect lastOut, last
