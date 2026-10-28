// LLVM 21.1.5/23.1.2 x86 native SLP regression: the scalar store at slot 77
// must not precede a vector load of the old values used by the reduction.
// Keep the non-power-of-two group and the adjacent independent group.
#include <array>
#include <cstdint>
#include <cstdio>
struct ReductionModel {
 std::array<bool,80> values{};
 std::uint16_t index{};
 std::array<std::uint8_t,2> active{};
 void evaluate();
};
#ifndef GRHSIM_REDUCTION_MAIN
void ReductionModel::evaluate() {
 bool changed_group_0=false, changed_group_1=false, changed_group_2=false, shared_change=false;
#define UPDATE(slot, constant, changed) { \
 const bool next = index == constant; \
 changed = static_cast<bool>(changed | (values[slot] != next)); \
 values[slot] = next; \
}
 UPDATE(33, 16, changed_group_0);
 UPDATE(34, 17, changed_group_0);
 UPDATE(35, 18, changed_group_0);
 UPDATE(36, 19, changed_group_0);
 UPDATE(37, 20, changed_group_0);
 UPDATE(38, 21, changed_group_0);
 UPDATE(39, 22, changed_group_0);
 UPDATE(40, 23, changed_group_0);
 UPDATE(41, 24, changed_group_0);
 UPDATE(42, 25, changed_group_0);
 UPDATE(43, 26, changed_group_0);
 UPDATE(44, 27, changed_group_0);
 UPDATE(45, 28, changed_group_0);
 UPDATE(46, 29, changed_group_0);
 UPDATE(47, 30, changed_group_0);
 UPDATE(48, 31, changed_group_0);
 UPDATE(49, 112, changed_group_0);
 UPDATE(50, 113, changed_group_0);
 UPDATE(51, 114, changed_group_0);
 UPDATE(52, 115, changed_group_0);
 UPDATE(53, 116, changed_group_0);
 UPDATE(54, 117, changed_group_0);
 UPDATE(55, 118, changed_group_0);
 UPDATE(56, 119, changed_group_0);
 UPDATE(57, 120, changed_group_0);
 UPDATE(58, 121, changed_group_0);
 UPDATE(59, 122, changed_group_0);
 UPDATE(60, 123, changed_group_0);
 UPDATE(61, 124, changed_group_0);
 UPDATE(62, 125, changed_group_0);
 UPDATE(63, 126, changed_group_0);
 UPDATE(64, 127, changed_group_0);
 UPDATE(65, 160, changed_group_0);
 UPDATE(66, 161, changed_group_0);
 UPDATE(67, 162, changed_group_0);
 UPDATE(68, 163, changed_group_0);
 UPDATE(69, 164, changed_group_0);
 UPDATE(70, 165, changed_group_0);
 UPDATE(71, 166, changed_group_0);
 UPDATE(72, 167, changed_group_0);
 UPDATE(73, 168, changed_group_0);
 UPDATE(74, 169, changed_group_0);
 UPDATE(75, 170, changed_group_0);
 UPDATE(76, 171, changed_group_0);
 UPDATE(77, 172, changed_group_0);
 UPDATE(78, 173, changed_group_0);
 UPDATE(79, 174, changed_group_0);
 const bool next = index == 191;
 const bool changed = values[0] != next;
 changed_group_1 |= changed;
 active[0] |= static_cast<std::uint8_t>(-static_cast<std::uint8_t>(changed)) & 4;
 values[0] = next;
 UPDATE(1, 224, shared_change);
 UPDATE(2, 225, shared_change);
 UPDATE(3, 226, shared_change);
 UPDATE(4, 227, shared_change);
 UPDATE(5, 228, shared_change);
 UPDATE(6, 229, shared_change);
 UPDATE(7, 230, shared_change);
 UPDATE(8, 231, shared_change);
 UPDATE(9, 232, shared_change);
 UPDATE(10, 233, shared_change);
 UPDATE(11, 234, shared_change);
 UPDATE(12, 235, shared_change);
 UPDATE(13, 236, shared_change);
 UPDATE(14, 237, shared_change);
 UPDATE(15, 238, shared_change);
 UPDATE(16, 239, shared_change);
#undef UPDATE
 changed_group_1 |= shared_change; changed_group_2 |= shared_change;
 active[0] |= static_cast<std::uint8_t>(-static_cast<std::uint8_t>(changed_group_0)) & 16;
 active[1] |= static_cast<std::uint8_t>(-static_cast<std::uint8_t>(changed_group_1)) & 1;
 active[0] |= static_cast<std::uint8_t>(-static_cast<std::uint8_t>(changed_group_2)) & 8;
}
#else
int main() {
 ReductionModel m;
 for(unsigned from=0;from<512;++from) for(unsigned to=0;to<512;++to) {
  m.values.fill(false);m.active.fill(0);m.index=from;m.evaluate();
  std::array<bool,47> before;for(unsigned i=0;i<47;++i)before[i]=m.values[33+i];
  m.active.fill(0);m.index=to;m.evaluate();
  bool changed=false;for(unsigned i=0;i<47;++i)changed|=before[i]!=m.values[33+i];
  if(bool(m.active[0]&16)!=changed) {
   std::printf("FAIL from=%u to=%u changed=%u flags=%u\n",from,to,changed,m.active[0]);return 1;
  }
 }
 std::puts("PASS 262144 change-reduction transitions");
}

#endif
