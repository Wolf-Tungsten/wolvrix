module F28ScalarImplementation #(parameter int MASK = 0) (
    input logic [7:0] r_0_data,
    output logic [7:0] r_0_reply
);
    assign r_0_reply = r_0_data ^ MASK;
endmodule
