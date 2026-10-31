module top(
    input logic clk, reset,
    input logic [7:0] initialValue [0:3],
    input logic en0, en1,
    input logic [2:0] addr0, addr1,
    input logic [7:0] data0, data1,
    input logic copy,
    output logic [31:0] tableOut,
    output logic [7:0] readOut
);
    logic [7:0] tableState [0:3];
    always_ff @(posedge clk) begin
        if (reset) tableState <= initialValue;
        else begin
            if (en0 && addr0 < 4) tableState[addr0] <= data0;
            if (en1 && addr1 < 4) tableState[addr1] <= copy ? (addr0 < 4 ? tableState[addr0] : 8'b0) : data1;
        end
    end
    assign tableOut = {tableState[3], tableState[2], tableState[1], tableState[0]};
    assign readOut = addr1 < 4 ? tableState[addr1] : 8'b0;
endmodule
