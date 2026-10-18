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

// A leading unit extent folds all leaf shapes into scalar storage. Each
// element assignment must still splice only 7 / 8 / 6 addressed bits.
module ingest_struct_array_partial_write (
    input logic clk,
    input logic reset,
    input logic write_valid,
    input logic [1:0] write_idx,
    input logic [20:0] write_bits,
    output wire [20:0] static_bits,
    output wire [20:0] dynamic_bits,
    output wire [20:0] member_bits,
    output wire [20:0] scalar_bits,
    output wire [20:0] member_scalar_bits
);
    typedef struct packed {
        logic [6:0] setIdx;
        logic [7:0] wayMask;
        struct packed { logic [2:0] value; } [1:0] data;
    } Entry;
    typedef struct packed { Entry entry; } Wrapped;
    Entry [0:0][3:0] staticEntries, dynamicEntries;
    Wrapped [0:0][3:0] wrappedEntries;

    always_ff @(posedge clk) begin
        if (reset) begin
            staticEntries <= '0;
            dynamicEntries <= '0;
            wrappedEntries <= '0;
        end else if (write_valid) begin
            staticEntries[0][3] <= '{7'h50, 8'h10, '{3'h1, 3'h2}};
            staticEntries[0][2] <= '{7'h2d, 8'h20, '{3'h3, 3'h4}};
            staticEntries[0][1] <= '{7'h6a, 8'h40, '{3'h5, 3'h6}};
            staticEntries[0][0] <= '{7'h03, 8'h80, '{3'h7, 3'h0}};
            dynamicEntries[0][write_idx] <= write_bits;
            wrappedEntries[0][write_idx].entry <= write_bits;
        end
    end
    assign static_bits = staticEntries[0][write_idx];
    assign dynamic_bits = dynamicEntries[0][write_idx];
    assign member_bits = wrappedEntries[0][write_idx].entry;
    assign scalar_bits = {
        dynamicEntries[0][write_idx].setIdx,
        dynamicEntries[0][write_idx].wayMask,
        dynamicEntries[0][write_idx].data[1].value,
        dynamicEntries[0][write_idx].data[0].value
    };
    assign member_scalar_bits = {
        wrappedEntries[0][write_idx].entry.setIdx,
        wrappedEntries[0][write_idx].entry.wayMask,
        wrappedEntries[0][write_idx].entry.data[1].value,
        wrappedEntries[0][write_idx].entry.data[0].value
    };
endmodule
