FIRRTL version 6.0.0
circuit top :%[[
  {"class":"firrtl.transforms.DedupGroupAnnotation","target":"~|top","group":"top"},
  {"class":"firrtl.transforms.DontTouchAnnotation","target":"~top|top>observed"},
  {"class":"firrtl.transforms.DontTouchAnnotation","target":"~|top>packet.bytes[1]"},
  {"class":"firrtl.passes.InlineAnnotation","target":"~|top"},
  {"class":"chisel3.util.experimental.decode.DecodeTableAnnotation","target":"~|top>y","truthTable":"0->0\n1->1","minimizedTable":"1->1\n 0"},
  {"class":"firrtl.transforms.BlackBoxInlineAnno","target":"~|External","name":"External.sv","text":"// \u4e2d\u6587 \ud83d\ude00\nmodule External; endmodule\n"}
]]
  extmodule External :
    defname = External
  public module top :
    input a : UInt<8>
    output y : UInt<8>
    wire observed : UInt<8>
    wire packet : {bytes : UInt<8>[2]}
    connect observed, a
    connect packet.bytes[0], a
    connect packet.bytes[1], a
    connect y, a
