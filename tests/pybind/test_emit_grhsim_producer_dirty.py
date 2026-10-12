#!/usr/bin/env python3
"""Execute emitted dirty-word models against an independent bit reference."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import wolvrix


class ProducerDirtyWordsTest(unittest.TestCase):
    def test_generated_state_changes(self):
        compiler = shutil.which(os.environ.get("CXX", "clang++"))
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        artifacts = Path(__file__).resolve().parents[2] / "build/artifacts"
        artifacts.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="producer_dirty_", dir=artifacts) as directory:
            root = Path(directory)
            scalar = [f"q{i}" for i in range(24)]
            boolean = [f"b{i}" for i in range(96)]
            concat = ",".join(scalar)
            fixture = """module top(input logic clk, latch_en, input logic [31:0] data_in,
                input logic [23:0] en, input logic [11:0] live,
                output logic [119:0] state_out, reverse_out,
                output logic [131:0] mixed_out, output logic [95:0] bool_out,
                output logic [99:0] cheap_out, output logic [247:0] wide_out,
                output logic [127:0] latch_out);
                logic [127:0] wide;
            """
            fixture += "logic [4:0] " + ",".join(scalar) + ";\n"
            fixture += "logic " + ",".join(boolean) + ";\n"
            fixture += "logic [3:0] " + ",".join(f"l{i}" for i in range(32)) + ";\n"
            fixture += "always_ff @(posedge clk) begin\n"
            for i, name in enumerate(scalar):
                fixture += f"if (en[{i}]) {name} <= (data_in >> {i % 17});\n"
            for i, name in enumerate(boolean):
                fixture += f"if (en[{i % 24}]) {name} <= data_in[{i % 32}];\n"
            fixture += "if(en[23]) q0[2:0] <= data_in[28:26];\n"
            fixture += "if(en[0]) wide <= {4{data_in}}; end\n"
            fixture += "always_latch if(latch_en) begin\n"
            for i in range(32):
                fixture += f"l{i} <= data_in >> {i % 20};\n"
            fixture += "end\nassign latch_out = {" + ",".join(f"l{i}" for i in range(32)) + "};\n"
            fixture += f"assign state_out = {{{concat}}};\n"
            fixture += "assign reverse_out = {" + ",".join(reversed(scalar)) + "};\n"
            fixture += f"assign mixed_out = {{live[11:9],{concat},live[8:0]}};\n"
            fixture += "assign bool_out = {" + ",".join(boolean) + "};\n"
            fixture += "assign cheap_out = {20{q0}};\n"
            fixture += f"assign wide_out = {{{concat},wide}}; endmodule\n"
            (root / "fixture.sv").write_text(fixture)
            harness = r'''
#include "grhsim_top.hpp"
#include <array>
#include <cstdint>
#include <thread>
#include <cstdio>
template<std::size_t N> void put(std::array<std::uint64_t,N>& a, unsigned pos,
                               unsigned width, std::uint64_t value) {
    for(unsigned i=0;i<width;++i)
        a[(pos+i)/64] |= ((value>>i)&1) << ((pos+i)%64);
}
struct Reference {
    std::array<unsigned,24> q{};
    std::array<unsigned,96> b{};
    std::array<unsigned,32> l{};
    std::uint32_t wide = 0;
    bool check(const GrhSIM_top& sim) const {
        std::array<std::uint64_t,2> state{},rev{},bits{},cheap{};
        std::array<std::uint64_t,2> latch{};
        std::array<std::uint64_t,3> mixed{};
        std::array<std::uint64_t,4> w{};
        for(unsigned i=0;i<24;++i){
            put(state,(23-i)*5,5,q[i]); put(rev,i*5,5,q[i]);
            put(mixed,9+(23-i)*5,5,q[i]); put(w,128+(23-i)*5,5,q[i]);
        }
        for(unsigned i=0;i<96;++i) put(bits,95-i,1,b[i]);
        for(unsigned i=0;i<32;++i) put(latch,(31-i)*4,4,l[i]);
        for(unsigned i=0;i<20;++i) put(cheap,i*5,5,q[0]);
        for(unsigned i=0;i<4;++i) put(w,i*32,32,wide);
        put(mixed,0,9,sim.live); put(mixed,129,3,sim.live>>9);
        return sim.state_out==state && sim.reverse_out==rev && sim.bool_out==bits
            && sim.mixed_out==mixed && sim.cheap_out==cheap && sim.wide_out==w
            && sim.latch_out==latch;
    }
    void commit(const GrhSIM_top& sim) {
        for(unsigned i=0;i<24;++i) if((sim.en>>i)&1) q[i]=(sim.data_in>>(i%17))&31;
        for(unsigned i=0;i<96;++i) if((sim.en>>(i%24))&1) b[i]=(sim.data_in>>(i%32))&1;
        if((sim.en>>23)&1) q[0]=(q[0]&24)|((sim.data_in>>26)&7);
        if(sim.en&1) wide=sim.data_in;
    }
};
int main(){
    std::array<GrhSIM_top,2> sims;
    std::array<Reference,2> refs;
    std::uint64_t rng=37;
    auto random=[&](){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;};
    for(unsigned init=0;init<3;++init){
        for(unsigned i=0;i<2;++i){sims[i].init();sims[i].clk=false;refs[i]={};}
        for(unsigned step=0;step<700;++step) for(unsigned i=0;i<2;++i){
            auto& sim=sims[i]; auto& ref=refs[i];
            sim.clk=false; sim.data_in=random();sim.live=random()&4095;
            sim.latch_en=random()&1;
            if(sim.latch_en) for(unsigned j=0;j<32;++j) ref.l[j]=(sim.data_in>>(j%20))&15;
            sim.en=step%7==0?0:step%7==1?0xffffff:1u<<(random()%24);
            sim.eval(); if(!ref.check(sim)) return 1;
            ref.commit(sim); sim.clk=true;
            if(step%257==0){std::thread worker([&]{sim.eval();});worker.join();}
            else sim.eval();
            if(!ref.check(sim)) {std::fprintf(stderr,"commit mismatch %u/%u\n",step,i);return 2;}
            sim.eval(); if(!ref.check(sim)) return 3;
            sim.clk=false;sim.eval(); if(!ref.check(sim)) return 4;
        }
    }
}
'''
            environment = {k: v for k, v in os.environ.items() if not k.startswith("WOLVRIX_GRHSIM_")}
            environment.update(WOLVRIX_GRHSIM_WORDWISE_WIDE_CONCAT="1",
                               WOLVRIX_GRHSIM_DIRECT_BOOL_STATE_VIEWS="1",
                               WOLVRIX_GRHSIM_WORDWISE_WIDE_CONCAT_BOOL_PACK="1")
            defaults = None
            for mode in ("default", "off", "on", "grouped", "eager", "precedence"):
                with self.subTest(mode=mode), patch.dict(os.environ, environment, clear=True):
                    destination = root / mode
                    if mode == "grouped":
                        os.environ["WOLVRIX_GRHSIM_COMMIT_SCALAR_SAME_READER_GROUPS"] = "1"
                    if mode == "eager":
                        os.environ["WOLVRIX_GRHSIM_COMMIT_SCALAR_EAGER_MAX_ACTIVATION_WRITES"] = "3"
                    if mode == "precedence":
                        os.environ["WOLVRIX_GRHSIM_PRODUCER_DIRTY_WORDS"] = "1"
                    enabled = mode in ("on", "grouped", "eager")
                    kwargs = {} if mode == "default" else {"producer_dirty_words": enabled}
                    with wolvrix.Session() as session:
                        session.read_sv(str(root / "fixture.sv"), slang_args=["--top", "top"])
                        session.run_pass("simplify", design="design.main", semantics="2state",
                                         keep_declared_symbols=False)
                        session.run_pass("activity-schedule", design="design.main", path="top")
                        session.emit_grhsim_cpp(design="design.main", output=str(destination), **kwargs)
                    sources = {p.name: p.read_text() for p in destination.glob("grhsim_top*")
                               if p.suffix in (".hpp", ".cpp")}
                    if mode == "default":
                        defaults = sources
                    elif not enabled:
                        self.assertEqual(defaults, sources)
                    self.assertEqual("producer_dirty_words_" in sources["grhsim_top.hpp"], enabled)
                    (destination / "harness.cpp").write_text(harness)
                    flags = os.environ.get("GRHSIM_TEST_CXXFLAGS", "-O3").split()
                    subprocess.run([compiler, "-std=c++20", "-pthread", *flags,
                                    "-I" + str(destination),
                                    *map(str, destination.glob("*.cpp")),
                                    "-o", str(destination / "harness")], check=True)
                    subprocess.run([str(destination / "harness")], check=True)


if __name__ == "__main__":
    unittest.main()
