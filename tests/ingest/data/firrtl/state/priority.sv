module top(
    input logic clk, arst, srst, enable, whole, row, last, overwrite,
    input logic [3:0] resetData, value,
    input logic [1:0] i, j,
    input logic [3:0] source__DOT__data [0:1][0:1],
    input logic source__DOT__flag [0:1],
    output logic [3:0] asyncOut, syncOut, nextOut,
    output logic [15:0] result__DOT__data,
    output logic [1:0] result__DOT__flag
);
    logic [3:0] a, s;
    logic [3:0] dataState [0:1][0:1];
    logic flagState [0:1];
    always_ff @(posedge clk or posedge arst)
        if (arst) a <= 4'd3;
        else if (enable) a <= a + 4'd1;
    always_ff @(posedge clk) begin
        if (srst) s <= resetData;
        else if (enable) s <= s + value;
        if (whole) begin
            dataState <= source__DOT__data;
            flagState <= source__DOT__flag;
        end
        if (row && i < 2) begin
            dataState[i] <= source__DOT__data[0];
            flagState[i] <= source__DOT__flag[0];
        end
        if (last) begin
            if (dataState[0][0] < 2 && j < 2) dataState[dataState[0][0]][j] <= value;
            if (i < 2) flagState[i] <= 1'b1;
        end
        if (overwrite) begin
            dataState <= source__DOT__data;
            flagState <= source__DOT__flag;
        end
    end
    assign asyncOut = a;
    assign syncOut = s;
    assign nextOut = a + 4'd1;
    assign result__DOT__flag = {flagState[1], flagState[0]};
    assign result__DOT__data = {dataState[1][1], dataState[1][0], dataState[0][1], dataState[0][0]};
endmodule
