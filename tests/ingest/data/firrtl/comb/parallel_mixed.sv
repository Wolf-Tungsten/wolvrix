module bridge(input logic [7:0] a, output logic [7:0] y);
    implementation child(.a(a), .y(y));
endmodule
