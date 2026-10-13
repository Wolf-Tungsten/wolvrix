typedef struct packed {
    logic [5:0] addr;
} read_bits_t;

typedef struct packed {
    logic       valid;
    read_bits_t bits;
} read_lane_t;

module read_addr_child (
    input  logic             clk,
    input  logic [3:0][5:0]  addr,
    output logic [5:0]       q
);
    always_ff @(posedge clk)
        q <= addr[0];
endmodule

module ingest_instance_struct_array_field_read (
    input  logic                    clk,
    input  read_lane_t [1:0][3:0]  lanes,
    output logic [5:0]             q
);
    read_addr_child u_read_addr (
        .clk(clk),
        .addr({{lanes[0][3].bits.addr}, {lanes[0][2].bits.addr},
               {lanes[0][1].bits.addr}, {lanes[0][0].bits.addr}}),
        .q(q)
    );
endmodule
