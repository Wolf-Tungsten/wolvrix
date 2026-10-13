// stage-07.1 fixture: element reads on a struct-array input port must slice
// the port value, never spin up a phantom array-state register. Regression
// for the XiangShan CoreMark IFU assertion (io_fromFtq_req_bits.fetch[*]
// reads returned 0 from a write-less duplicate register).
typedef struct packed { logic [48:0] addr; } port_addr_t;
typedef struct packed {
    logic        valid;
    port_addr_t  startVAddr;
} port_fetch_t;

module ingest_port_array_elem_read (
    input  logic          clk,
    input  logic [1:0]    sel,
    input  logic [48:0]   other,
    input  port_fetch_t [1:0] req_bits,
    input  logic [3:0][7:0] bytes,
    output logic [48:0]   o,
    output logic [7:0]    const_byte
);
    assign o = req_bits[sel].startVAddr.addr + other;
    assign const_byte = bytes[2];
endmodule
