typedef struct packed {
    logic [21:0] target;
} target_entry_t;

`define SELECT_TARGET ((choose[0] ? a : 22'h0) | (choose[1] ? b : 22'h0))

module expression_width (
    input wire [1:0] choose,
    input wire [21:0] a, b, expected,
    input wire [7:0] short_target,
    input target_entry_t [1:0] entries,
    output wire [33:0] observed
);
    // Net initializers carry the destination width into expression lowering.
    wire ne = `SELECT_TARGET != expected;
    wire eq = `SELECT_TARGET == expected;
    wire case_eq = `SELECT_TARGET === expected;
    wire case_ne = `SELECT_TARGET !== expected;
    wire wildcard_eq = `SELECT_TARGET ==? expected;
    wire wildcard_ne = `SELECT_TARGET !=? expected;
    wire lt = `SELECT_TARGET < expected;
    wire le = `SELECT_TARGET <= expected;
    wire gt = `SELECT_TARGET > expected;
    wire ge = `SELECT_TARGET >= expected;
    wire logic_and = `SELECT_TARGET && expected;
    wire logic_or = `SELECT_TARGET || expected;
    wire logic_not = !`SELECT_TARGET;
    wire reduce_and = &`SELECT_TARGET;
    wire reduce_or = |`SELECT_TARGET;
    wire reduce_xor = ^`SELECT_TARGET;
    wire reduce_nand = ~&`SELECT_TARGET;
    wire reduce_nor = ~|`SELECT_TARGET;
    wire reduce_xnor = ~^`SELECT_TARGET;
    wire condition = `SELECT_TARGET ? 1'b1 : 1'b0;
    wire packed_ne = ((choose[0] ? entries[0].target : 22'h0)
                    | (choose[1] ? entries[1].target : 22'h0)) != expected;
    wire [7:0] wide_ne = `SELECT_TARGET != expected;
    // Comparison sizing also applies before arithmetic, including overflow.
    wire product_ne = (a * b) != expected;
    wire product_truth = (a * b) ? 1'b1 : 1'b0;
    wire product_not = !(a * b);
    wire product_and = (a * b) && expected;
    wire mixed_ne = ((choose[0] ? short_target : 8'h0)
                   | (choose[1] ? b : 22'h0)) != expected;

    assign observed = {mixed_ne, product_and, product_not, product_truth,
                       product_ne, wide_ne, packed_ne, condition, reduce_xnor, reduce_nor,
                       reduce_nand, reduce_xor, reduce_or, reduce_and, logic_not,
                       logic_or, logic_and, ge, gt, le, lt, wildcard_ne,
                       wildcard_eq, case_ne, case_eq, eq, ne};
endmodule

`undef SELECT_TARGET
