// stage-07.1 fixture: struct-array replicate/concat must lower field-wise.
// Whole-array flat forms would be 160x72=11520 / 512x72=36864 / 72x72=5184
// bits; per-leaf forms cap at elements x maxFieldWidth = 512x64=32768 and
// the whole-array widths must never materialize.
module ingest_struct_array_fieldwise (
    input  logic        clk,
    input  logic [8:0]  rd_idx,
    input  logic [2:0]  bank_idx,
    input  logic [1:0]  way_idx,
    input  logic [2335:0] bank_input,
    output logic [63:0] out_data,
    output logic [63:0] out_bank_data,
    output logic [6:0]  out_tag
);
    typedef struct packed {
        logic [63:0] data;
        logic [6:0]  tag;
        logic        valid;
    } Entry;  // 72 bits

    Entry [351:0] robEntries;
    Entry [63:0]  uop;
    Entry         single;
    wire Entry [159:0] rep   = {160{robEntries[9'h0]}};
    wire Entry [511:0] pad   = {rep, robEntries};
    wire Entry [71:0]  mix   = {{8{uop[6'h0]}}, uop};
    wire Entry [3:0]   banks = {{robEntries[9'h3]}, {robEntries[9'h2]},
                                {robEntries[9'h1]}, {robEntries[9'h0]}};
    // Degenerate {1{struct}}: the per-leaf replicate must keep the leaf width
    // (stage-07 bug4: a bare symbol leaf carried no widthHint and the
    // replicate collapsed to a 1-bit result).
    wire Entry         one   = {1{single}};
    // The flat bank leaf needs an 11-bit offset for bank_idx * 256.
    wire struct packed {
        logic [7:0] ecc;
        logic [63:0] raw_data;
        logic error_delayed;
    } [0:0][7:0][3:0] bankResult = bank_input;

    always_ff @(posedge clk) begin
        robEntries[9'h0].data  <= 64'h1234;
        robEntries[9'h0].tag   <= 7'h55;
        robEntries[9'h0].valid <= 1'b1;
        uop[6'h0].data         <= 64'h77;
        uop[6'h0].tag          <= 7'h2a;
        uop[6'h0].valid        <= 1'b0;
        single.data            <= 64'h55;
        single.tag             <= 7'h11;
        single.valid           <= 1'b1;
    end

    assign out_data = pad[rd_idx].data ^ mix[rd_idx[5:0]].data ^ banks[rd_idx[1:0]].data ^ one.data;
    assign out_bank_data = bankResult[0][bank_idx][way_idx].raw_data;
    assign out_tag  = pad[rd_idx].tag  ^ mix[rd_idx[5:0]].tag  ^ banks[rd_idx[1:0]].tag ^ one.tag;
endmodule
