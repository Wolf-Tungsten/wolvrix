FIRRTL version 6.0.0
circuit top :
  module first :
    input shared_reference_name_that_exceeds_small_string_storage_and_is_reused_across_modules : UInt<8>
    output y : UInt<8>
    wire _val_0 : UInt<8>
    node forward_reference_name_that_exceeds_small_string_storage_and_is_reused_across_modules = shared_reference_name_that_exceeds_small_string_storage_and_is_reused_across_modules
    node _op_0 = xor(forward_reference_name_that_exceeds_small_string_storage_and_is_reused_across_modules, and(forward_reference_name_that_exceeds_small_string_storage_and_is_reused_across_modules, forward_reference_name_that_exceeds_small_string_storage_and_is_reused_across_modules))
    connect _val_0, not(_op_0)
    connect y, _val_0
  module second :
    input shared_reference_name_that_exceeds_small_string_storage_and_is_reused_across_modules : UInt<13>
    output y : UInt<13>
    wire _val_0 : UInt<13>
    node forward_reference_name_that_exceeds_small_string_storage_and_is_reused_across_modules = shared_reference_name_that_exceeds_small_string_storage_and_is_reused_across_modules
    node _op_0 = or(forward_reference_name_that_exceeds_small_string_storage_and_is_reused_across_modules, and(forward_reference_name_that_exceeds_small_string_storage_and_is_reused_across_modules, forward_reference_name_that_exceeds_small_string_storage_and_is_reused_across_modules))
    connect _val_0, _op_0
    connect y, _val_0
  public module top :
    input a : UInt<8>
    input b : UInt<13>
    output y : UInt<8>
    output z : UInt<13>
    inst one of first
    inst two of second
    connect one.shared_reference_name_that_exceeds_small_string_storage_and_is_reused_across_modules, a
    connect two.shared_reference_name_that_exceeds_small_string_storage_and_is_reused_across_modules, b
    connect y, one.y
    connect z, two.y
