#include "grhsim_cpu_fp_cone.hpp"

#include <iostream>
#include <stdexcept>

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

int main()
{
    GrhSIM_cpu_fp_cone sim;
    sim.init();
    sim.clk = false; sim.a = true; sim.b = true; sim.data = false;
    sim.eval();
    require(!sim.q && !sim.t, "settle changed state");
    sim.data = true; sim.clk = true;
    sim.eval();
    require(sim.q && sim.t, "first posedge did not write the registers");
    sim.clk = false;
    sim.eval();
    require(sim.q && sim.t, "falling-edge eval changed state");
    // Posedge eval immediately after the elided negedge: the data change rides
    // along with the clock. The stale cone intermediate must have been staged
    // by the fast path or this posedge is lost.
    sim.data = false; sim.clk = true;
    sim.eval();
    require(!sim.q, "posedge after a falling-edge eval was missed (q)");
    require(!sim.t, "posedge after a falling-edge eval was missed (t)");
    sim.clk = false;
    sim.eval();
    require(!sim.q && !sim.t, "second falling-edge eval changed state");
    // Pure-clock posedge immediately after an elided negedge: no data input
    // changes. The toggle register must still observe the edge.
    sim.clk = true;
    sim.eval();
    require(!sim.q, "pure-clock posedge rewrote the data register");
    require(sim.t, "pure-clock posedge after a falling-edge eval was missed (t)");
    std::cout << "fp deep cone PASS\n";
    return 0;
}
