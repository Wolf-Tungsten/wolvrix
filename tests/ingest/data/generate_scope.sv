module generate_scope_basic(
    input  logic [7:0] a,
    output logic [7:0] y
);
    genvar i;
    generate
        for (i = 0; i < 4; i++) begin : gen_loop
            logic [7:0] sig;
            wire  [7:0] tmp;
            assign sig = a;
            assign tmp = sig ^ 8'hff;
        end
    endgenerate
    assign y = a;
endmodule

module generate_scope_collision(
    input  logic [7:0] a,
    output logic [7:0] y
);
    logic [7:0] sig;
    assign sig = a + 8'h1;

    genvar i;
    generate
        for (i = 0; i < 4; i++) begin : gen_loop
            logic [7:0] sig;
            assign sig = a + 8'h2;
        end
    endgenerate

    assign y = sig;
endmodule

module generate_scope_init(
    input  logic       clk,
    input  logic [7:0] d,
    output logic [7:0] q
);
    genvar i;
    generate
        for (i = 0; i < 2; i++) begin : gen_init
            logic [7:0] acc = 8'h3c;
            always @(posedge clk) begin
                acc <= d;
            end
        end
    endgenerate
    assign q = d;
endmodule
