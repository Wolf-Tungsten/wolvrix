module stmt_lowerer_case(
    input logic a,
    input logic b,
    input logic c,
    input logic d,
    output logic y,
    output logic z,
    output wire w
);
    always_comb begin
        if (a)
            y = b;
        else
            y = c;
        if (b && c)
            z = d;
    end

    assign w = a & b;
endmodule

module stmt_lowerer_if_chain(
    input logic a,
    input logic b,
    input logic c,
    input logic d,
    output logic y,
    output logic z
);
    always_comb begin
        if (a)
            y = b;
        else if (c)
            z = d;
    end
endmodule

module stmt_lowerer_case_stmt(
    input logic [1:0] sel,
    input logic a,
    input logic b,
    input logic c,
    output logic y
);
    always_comb begin
        case (sel)
            2'b00, 2'b01: y = a;
            2'b10: y = b;
            default: y = c;
        endcase
    end
endmodule

module stmt_lowerer_case_incomplete_stmt(
    input logic [1:0] sel,
    input logic a,
    input logic b,
    output logic y
);
    always_comb begin
        case (sel)
            2'b00: y = a;
            2'b01: y = b;
        endcase
    end
endmodule

module stmt_lowerer_casez_stmt(
    input logic [1:0] sel,
    input logic a,
    input logic b,
    input logic c,
    output logic y
);
    always_comb begin
        casez (sel)
            2'b0?: y = a;
            2'b1?: y = b;
            default: y = c;
        endcase
    end
endmodule

module stmt_lowerer_casex_stmt(
    input logic [1:0] sel,
    input logic a,
    input logic b,
    input logic c,
    output logic y
);
    always_comb begin
        casex (sel)
            2'b0x: y = a;
            2'b1x: y = b;
            default: y = c;
        endcase
    end
endmodule

module stmt_lowerer_casez_2state_stmt(
    input bit [1:0] sel,
    input logic a,
    input logic b,
    input logic c,
    output logic y
);
    always_comb begin
        casez (sel)
            2'b0?: y = a;
            2'b1?: y = b;
            default: y = c;
        endcase
    end
endmodule

module stmt_lowerer_casex_2state_stmt(
    input bit [1:0] sel,
    input logic a,
    input logic b,
    input logic c,
    output logic y
);
    always_comb begin
        casex (sel)
            2'b0x: y = a;
            2'b1x: y = b;
            default: y = c;
        endcase
    end
endmodule

module stmt_lowerer_repeat_stmt(
    input logic a,
    output logic y
);
    always_comb begin
        repeat (3) begin
            y = a;
        end
    end
endmodule

module stmt_lowerer_for_stmt(
    input logic a,
    output logic y
);
    always_comb begin
        for (int i = 0; i < 2; i = i + 1) begin
            y = a;
        end
    end
endmodule

module stmt_lowerer_foreach_stmt(
    input logic a,
    output logic y
);
    logic [7:0] arr [0:1];
    always_comb begin
        foreach (arr[i]) begin
            y = a;
        end
    end
endmodule

module stmt_lowerer_while_static(
    input logic a,
    output logic y
);
    always_comb begin
        y = 1'b0;
        while (1'b1) begin
            y = a;
            break;
        end
    end
endmodule

module stmt_lowerer_do_while_static(
    input logic a,
    output logic y
);
    always_comb begin
        do begin
            y = a;
        end while (1'b0);
    end
endmodule

module stmt_lowerer_forever_static(
    input logic a,
    output logic y
);
    always_comb begin
        forever begin
            y = a;
            break;
        end
    end
endmodule

module stmt_lowerer_repeat_large_stmt(
    input logic a,
    output logic y
);
    always_comb begin
        repeat (5000) begin
            y = a;
        end
    end
endmodule

module stmt_lowerer_for_large_stmt(
    input logic a,
    output logic y
);
    always_comb begin
        for (int i = 0; i < 5000; i = i + 1) begin
            y = a;
        end
    end
endmodule

module stmt_lowerer_foreach_large_stmt(
    input logic a,
    output logic y
);
    logic [7:0] arr [0:4999];
    always_comb begin
        foreach (arr[i]) begin
            y = a;
        end
    end
endmodule

module stmt_lowerer_case_inside_stmt(
    input logic [7:0] sel,
    input logic a,
    input logic b,
    input logic c,
    input logic d,
    input logic e,
    output logic y
);
    always_comb begin
        case (sel) inside
            8'h00: y = a;
            [8'h10:8'h1f]: y = b;
            [8'h20 +/- 8'h03]: y = c;
            [8'h40 +%- 8'h10]: y = d;
            default: y = e;
        endcase
    end
endmodule

module stmt_lowerer_lhs_select(
    input logic [7:0] data,
    input logic [2:0] idx,
    output logic [7:0] y
);
    always_comb begin
        y[3] = data[0];
        y[7:4] = data[7:4];
        y[idx] = data[1];
        y[idx +: 2] = data[3:2];
        y[idx -: 2] = data[5:4];
    end
endmodule

typedef struct packed {
    logic [3:0] hi;
    logic [3:0] lo;
} stmt_pair_t;

module stmt_lowerer_lhs_concat(
    input logic [7:0] data,
    output logic [3:0] y,
    output logic [3:0] z
);
    always_comb begin
        {y, z} = data;
    end
endmodule

module stmt_lowerer_lhs_stream(
    input logic [7:0] data,
    output logic [3:0] y,
    output logic [3:0] z
);
    always_comb begin
        {>>{y, z}} = data;
    end
endmodule

module stmt_lowerer_lhs_member(
    input logic [3:0] a,
    input logic [3:0] b,
    output stmt_pair_t y
);
    always_comb begin
        y.hi = a;
        y.lo = b;
    end
endmodule

module stmt_lowerer_timed_delay(
    input logic a,
    output logic y
);
    always begin
        #1 y = a;
    end
endmodule

module stmt_lowerer_timed_event(
    input logic a,
    input logic clk,
    output logic y
);
    always begin
        @(posedge clk) y = a;
    end
endmodule

module stmt_lowerer_timed_wait(
    input logic a,
    output logic y
);
    always begin
        wait (a) y = a;
    end
endmodule

module stmt_lowerer_timed_wait_fork(
    input logic a,
    output logic y
);
    always begin
        wait fork;
        y = a;
    end
endmodule

module stmt_lowerer_timed_disable_fork(
    input logic a,
    output logic y
);
    always begin
        disable fork;
        y = a;
    end
endmodule

module stmt_lowerer_timed_event_trigger(
    input logic a,
    output logic y
);
    event ev;
    always begin
        -> ev;
        y = a;
    end
endmodule

module stmt_lowerer_timed_event_trigger_delay(
    input logic a,
    output logic y
);
    event ev;
    always begin
        ->> #1 ev;
        y = a;
    end
endmodule

module stmt_lowerer_timed_wait_order(
    input logic a,
    input logic b,
    output logic y
);
    event ev1;
    event ev2;
    always begin
        wait_order (ev1, ev2) begin
            y = a;
        end else begin
            y = b;
        end
    end
endmodule

module stmt_lowerer_for_break(
    input logic a,
    output logic y
);
    always_comb begin
        for (int i = 0; i < 4; i = i + 1) begin
            if (i == 2)
                break;
            y = a;
        end
    end
endmodule

module stmt_lowerer_for_continue(
    input logic a,
    output logic y
);
    always_comb begin
        for (int i = 0; i < 4; i = i + 1) begin
            if (i == 1)
                continue;
            y = a;
        end
    end
endmodule

module stmt_lowerer_foreach_break(
    input logic a,
    output logic y
);
    logic [7:0] arr [0:3];
    always_comb begin
        foreach (arr[i]) begin
            if (i == 2)
                break;
            y = a;
        end
    end
endmodule

module stmt_lowerer_foreach_continue(
    input logic a,
    output logic y
);
    logic [7:0] arr [0:3];
    always_comb begin
        foreach (arr[i]) begin
            if (i == 1)
                continue;
            y = a;
        end
    end
endmodule

module stmt_lowerer_for_break_dynamic(
    input logic a,
    input logic stop,
    output logic y
);
    always_comb begin
        for (int i = 0; i < 3; i = i + 1) begin
            if (stop)
                break;
            y = a;
        end
    end
endmodule

module stmt_lowerer_for_continue_dynamic(
    input logic a,
    input logic skip,
    output logic y
);
    always_comb begin
        for (int i = 0; i < 3; i = i + 1) begin
            if (skip)
                continue;
            y = a;
        end
    end
endmodule

module stmt_lowerer_for_break_case_dynamic(
    input logic [1:0] sel,
    input logic a,
    output logic y
);
    always_comb begin
        for (int i = 0; i < 3; i = i + 1) begin
            case (sel)
                2'b00: break;
                default: y = a;
            endcase
        end
    end
endmodule

import "DPI-C" function void dpi_capture(input logic [3:0] in_val,
                                         output logic [3:0] out_val);
import "DPI-C" function int dpi_sum(input int a, input int b);

module stmt_lowerer_display(
    input logic clk,
    input logic a,
    input logic b,
    output logic y
);
    always_ff @(posedge clk) begin
        $display("a=%0d", a);
        y <= b;
    end
endmodule

module stmt_lowerer_display_time(
    input logic clk
);
    always_ff @(posedge clk) begin
        $display("t=%t/%0t", $time, $time);
    end
endmodule

module stmt_lowerer_display_comb(
    input logic a
);
    always_comb begin
        $display("comb=%0d", a);
    end
endmodule

module stmt_lowerer_dpi(
    input logic clk,
    input logic [3:0] a,
    output logic [3:0] y
);
    always_ff @(posedge clk) begin
        dpi_capture(a, y);
    end
endmodule

module stmt_lowerer_dpi_return(
    input logic clk,
    input int a,
    input int b
);
    always_ff @(posedge clk) begin
        dpi_sum(a, b);
    end
endmodule

module stmt_lowerer_procedural_local(
    input logic clk,
    input logic [2:0] a,
    input logic [2:0] b,
    output logic [2:0] y
);
    always_ff @(posedge clk) begin
        automatic logic [2:0] tmp;
        automatic logic [2:0] next;
        tmp = a ^ b;
        next = tmp[1] ? {1'b0, tmp[1:0]} : {tmp[2], 1'b1, b[0]};
        y <= next;
    end
endmodule

module stmt_lowerer_mul_widths (
  input logic [52:0] a, b,
  output logic [106:0] packed_product, self_determined,
  output logic [127:0] contextual
);
  always_comb begin
    packed_product = {1'b0, {53'b0, a} * {53'b0, b}};
    self_determined = {54'b0, a * b};
    contextual = a * b;
  end
endmodule

module stmt_lowerer_boolean_operand_widths (
  input logic [5:0] index,
  input logic [63:0] onehot,
  output logic [11:0] predicates,
  output wire declaration_bad
);
  wire bad = (64'h1 << index) != onehot;
  assign declaration_bad = bad;
  always_comb begin
    predicates[0] = (64'h1 << index) == onehot;
    predicates[1] = (64'h1 << index) != onehot;
    predicates[2] = (64'h1 << index) === onehot;
    predicates[3] = (64'h1 << index) !== onehot;
    predicates[4] = (64'h1 << index) ==? onehot;
    predicates[5] = (64'h1 << index) !=? onehot;
    predicates[6] = (64'h1 << index) < onehot;
    predicates[7] = (64'h1 << index) <= onehot;
    predicates[8] = (64'h1 << index) > onehot;
    predicates[9] = (64'h1 << index) >= onehot;
    predicates[10] = (64'h1 << index) && onehot;
    predicates[11] = (64'h1 << index) || onehot;
  end
endmodule
