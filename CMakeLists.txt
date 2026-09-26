cmake_minimum_required(VERSION 3.26)
project(wolvrix VERSION 0.1.0 LANGUAGES C CXX)

find_package(Git QUIET)
set(WOLVRIX_GIT_SHA "unknown")
set(WOLVRIX_GIT_DATE "unknown")
if (GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE WOLVRIX_GIT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" show -s --format=%cs HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE WOLVRIX_GIT_DATE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

# Toolchain / language
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# Generate compile_commands.json for IDEs by default.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Ensure static dependencies can link into shared libs.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Output directories (avoid per-target duplication)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Ensure important runtime/artifact directories exist at configure time
file(MAKE_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

add_subdirectory(external/slang)

option(WOLVRIX_MTKAHYPAR_BUILD_SHARED "Build vendored mt-kahypar as a shared library" OFF)
option(WOLVRIX_ENABLE_MT_KAHYPAR "Enable vendored mt-kahypar integration" ON)
set(WOLVRIX_HAVE_MT_KAHYPAR 0)
if (WOLVRIX_ENABLE_MT_KAHYPAR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/mt-kahypar/CMakeLists.txt")
    block(SCOPE_FOR VARIABLES)
        set(BUILD_SHARED_LIBS ${WOLVRIX_MTKAHYPAR_BUILD_SHARED})
        set(KAHYPAR_PYTHON OFF)
        set(KAHYPAR_ENABLE_TESTING OFF)
        set(KAHYPAR_INSTALL_CLI OFF)
        set(KAHYPAR_BUILD_DEBIAN_PACKAGE OFF)
        if (NOT DEFINED KAHYPAR_DOWNLOAD_TBB)
            set(KAHYPAR_DOWNLOAD_TBB ON)
        endif()
        if (NOT DEFINED KAHYPAR_DISABLE_HWLOC)
            set(KAHYPAR_DISABLE_HWLOC OFF)
        endif()
        add_subdirectory(external/mt-kahypar EXCLUDE_FROM_ALL)
    endblock()
endif()

if (TARGET MtKaHyPar::mtkahypar)
    set(WOLVRIX_HAVE_MT_KAHYPAR 1)
    message(STATUS "MtKaHyPar enabled from vendored source")
elseif (NOT WOLVRIX_ENABLE_MT_KAHYPAR)
    message(STATUS "MtKaHyPar disabled by WOLVRIX_ENABLE_MT_KAHYPAR=OFF")
else()
    message(WARNING "Vendored MtKaHyPar not available; repcut mt-kahypar backend will be unavailable")
endif()

option(WOLVRIX_ENABLE_LIBFST "Enable vendored libfst integration" ON)
set(WOLVRIX_HAVE_LIBFST 0)
if (WOLVRIX_ENABLE_LIBFST AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/libfst/src/fstapi.c")
    find_package(ZLIB REQUIRED)
    add_library(wolvrix-libfst STATIC
        external/libfst/src/fastlz.c
        external/libfst/src/fstapi.c
        external/libfst/src/lz4.c
    )
    target_include_directories(wolvrix-libfst
        PUBLIC
            external/libfst/src
    )
    target_compile_definitions(wolvrix-libfst
        PRIVATE
            _GNU_SOURCE
    )
    target_link_libraries(wolvrix-libfst
        PUBLIC
            ZLIB::ZLIB
    )
    set(WOLVRIX_HAVE_LIBFST 1)
    message(STATUS "libfst enabled from vendored source")
elseif (NOT WOLVRIX_ENABLE_LIBFST)
    message(STATUS "libfst disabled by WOLVRIX_ENABLE_LIBFST=OFF")
else()
    message(WARNING "Vendored libfst not available; grhsim waveform emission will be unavailable")
endif()

# Libraries
set(WOLVRIX_LIB_SOURCES
    lib/core/diagnostics.cpp
    lib/core/grh.cpp
    lib/core/load.cpp
    lib/core/emit.cpp
    lib/core/toposort.cpp
    lib/emit/grhsim_cpp.cpp
    lib/emit/grhsim_runtime.cpp
    lib/emit/system_verilog.cpp
    lib/emit/verilator_repcut_package.cpp
    lib/core/store.cpp
    lib/core/ingest.cpp
    lib/core/transform.cpp
    lib/grhsim/ir/model.cpp
    lib/grhsim/ir/verifier.cpp
    lib/grhsim/dialect/registry.cpp
    lib/grhsim/dialect/core.cpp
    lib/grhsim/dialect/cpu.cpp
    lib/grhsim/pass/pass.cpp
    lib/grhsim/pass/reg_to_mem.cpp
    lib/grhsim/pass/canonicalize_compute.cpp
    lib/grhsim/pass/clone_shared_compute.cpp
    lib/grhsim/pass/bitwise_predicates.cpp
    lib/grhsim/pass/bitwise_muxes.cpp
    lib/grhsim/pass/mux_chain_fold.cpp
    lib/grhsim/pass/fuse_expr_chains.cpp
    lib/grhsim/pass/migrate_boundary_ops.cpp
    lib/grhsim/pass/demonitor_redundant.cpp
    lib/grhsim/pass/demonitor_edge_completion.cpp
    lib/grhsim/pass/migrate_boundary_ops_ec.cpp
    lib/grhsim/pass/fold_residue.cpp
    lib/grhsim/pass/used_bits.cpp
    lib/grhsim/pass/pack_bit_registers.cpp
    lib/grhsim/backend/cpu.cpp
    lib/grhsim/backend/cpu_partition.cpp
    lib/grhsim/backend/cpu_layout.cpp
    lib/grhsim/backend/cpu_schedule.cpp
    lib/grhsim/backend/cpu_emit.cpp
    lib/grhsim/backend/cpu_block_share.cpp
    lib/grhsim/backend/cpu_shape_share.cpp
    lib/grhsim/convert/grh_to_grhsim.cpp
    lib/grhsim/io/json.cpp
    lib/transform/activity_schedule.cpp
    lib/transform/demo_stats.cpp
    lib/transform/const_fold.cpp
    lib/transform/redundant_elim.cpp
    lib/transform/dead_code_elim.cpp
    lib/transform/simplify.cpp
    lib/transform/memory_init_check.cpp
    lib/transform/memory_read_retime.cpp
    lib/transform/mem_to_reg.cpp
    lib/transform/reg_to_mem.cpp
    lib/transform/multidriven_guard.cpp
    lib/transform/blackbox_guard.cpp
    lib/transform/comb_lane_pack.cpp
    lib/transform/comb_loop_elim.cpp
    lib/transform/hier_flatten.cpp
    lib/transform/instance_inline.cpp
    lib/transform/xmr_resolve.cpp
    lib/transform/latch_transparent_read.cpp
    lib/transform/slice_index_const.cpp
    lib/transform/strip_debug.cpp
    lib/transform/repcut.cpp
)

add_library(wolvrix-lib SHARED ${WOLVRIX_LIB_SOURCES})

target_include_directories(wolvrix-lib
    PUBLIC
        include
)

target_link_libraries(wolvrix-lib
    PUBLIC
        slang::slang
)

if (WOLVRIX_HAVE_MT_KAHYPAR)
    target_link_libraries(wolvrix-lib
        PRIVATE
            MtKaHyPar::mtkahypar
    )
endif()

if (WOLVRIX_HAVE_LIBFST)
    target_link_libraries(wolvrix-lib
        PRIVATE
            wolvrix-libfst
    )
endif()

target_compile_definitions(wolvrix-lib
    PUBLIC
        WOLVRIX_HAVE_MT_KAHYPAR=${WOLVRIX_HAVE_MT_KAHYPAR}
        WOLVRIX_HAVE_LIBFST=${WOLVRIX_HAVE_LIBFST}
    PRIVATE
        WOLVRIX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
set_target_properties(wolvrix-lib PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

add_library(wolvrix::lib ALIAS wolvrix-lib)

if (DEFINED WOLVRIX_BUILD_PYTHON AND WOLVRIX_BUILD_PYTHON)
    message(WARNING
        "WOLVRIX_BUILD_PYTHON is ignored. "
        "Install wolvrix into Python with `python -m pip install -e wolvrix --no-build-isolation`, "
        "which builds the bindings via scikit-build-core."
    )
endif()

set(WOLVRIX_BUILD_PYTHON_PACKAGE OFF)
if (DEFINED SKBUILD_PLATLIB_DIR OR DEFINED SKBUILD_PURELIB_DIR)
    set(WOLVRIX_BUILD_PYTHON_PACKAGE ON)
endif()

if (WOLVRIX_BUILD_PYTHON_PACKAGE)
    find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)

    set(WOLVRIX_PYTHON_SOURCE_DIR "${CMAKE_SOURCE_DIR}/app/pybind")
    set(WOLVRIX_PYTHON_BUILD_DIR "${CMAKE_BINARY_DIR}/python")
    set(WOLVRIX_PYTHON_PURELIB_DIR ".")
    set(WOLVRIX_PYTHON_PLATLIB_DIR ".")
    if (DEFINED SKBUILD_PURELIB_DIR)
        set(WOLVRIX_PYTHON_PURELIB_DIR "${SKBUILD_PURELIB_DIR}")
    endif()
    if (DEFINED SKBUILD_PLATLIB_DIR)
        set(WOLVRIX_PYTHON_PLATLIB_DIR "${SKBUILD_PLATLIB_DIR}")
    endif()

    add_custom_target(wolvrix-python-sources
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${WOLVRIX_PYTHON_SOURCE_DIR}"
            "${WOLVRIX_PYTHON_BUILD_DIR}"
    )

    file(GLOB_RECURSE WOLVRIX_PYBIND_SOURCES CONFIGURE_DEPENDS
        app/pybind/*.cpp
    )

    Python_add_library(wolvrix_python MODULE ${WOLVRIX_PYBIND_SOURCES})
    target_include_directories(wolvrix_python
        PRIVATE
            "${CMAKE_SOURCE_DIR}/app/pybind"
    )
    target_link_libraries(wolvrix_python
        PRIVATE
            wolvrix-lib
            Python::Module
    )
    set_target_properties(wolvrix_python PROPERTIES
        OUTPUT_NAME "_wolvrix"
        LIBRARY_OUTPUT_DIRECTORY "${WOLVRIX_PYTHON_BUILD_DIR}/wolvrix"
        RUNTIME_OUTPUT_DIRECTORY "${WOLVRIX_PYTHON_BUILD_DIR}/wolvrix"
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
    add_dependencies(wolvrix_python wolvrix-python-sources)

    install(DIRECTORY "${WOLVRIX_PYTHON_SOURCE_DIR}/wolvrix"
        DESTINATION "${WOLVRIX_PYTHON_PURELIB_DIR}"
    )
    install(TARGETS wolvrix_python
        LIBRARY DESTINATION "${WOLVRIX_PYTHON_PLATLIB_DIR}/wolvrix"
        RUNTIME DESTINATION "${WOLVRIX_PYTHON_PLATLIB_DIR}/wolvrix"
    )
    install(TARGETS wolvrix-lib
        LIBRARY DESTINATION "${WOLVRIX_PYTHON_PLATLIB_DIR}/wolvrix"
        RUNTIME DESTINATION "${WOLVRIX_PYTHON_PLATLIB_DIR}/wolvrix"
    )
    if (WOLVRIX_HAVE_MT_KAHYPAR AND WOLVRIX_MTKAHYPAR_BUILD_SHARED)
        install(TARGETS mtkahypar
            LIBRARY DESTINATION "${WOLVRIX_PYTHON_PLATLIB_DIR}/wolvrix"
            RUNTIME DESTINATION "${WOLVRIX_PYTHON_PLATLIB_DIR}/wolvrix"
        )
    endif()
    if (WOLVRIX_HAVE_MT_KAHYPAR AND TARGET tbb AND TARGET tbbmalloc)
        install(TARGETS tbb tbbmalloc
            LIBRARY DESTINATION "${WOLVRIX_PYTHON_PLATLIB_DIR}/wolvrix"
            RUNTIME DESTINATION "${WOLVRIX_PYTHON_PLATLIB_DIR}/wolvrix"
        )
    endif()
endif()

enable_testing()

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------
# Common test paths
set(WOLF_SV_TEST_INPUT_DIR "${CMAKE_SOURCE_DIR}/tests/data")
set(WOLF_SV_INGEST_TEST_DATA_DIR "${CMAKE_SOURCE_DIR}/tests/ingest/data")
set(WOLF_SV_TEST_ARTIFACT_DIR "${CMAKE_BINARY_DIR}/artifacts")

set(EMIT_ARTIFACT_DIR "${WOLF_SV_TEST_ARTIFACT_DIR}/emit")

# Ensure artifact directories exist
file(MAKE_DIRECTORY "${WOLF_SV_TEST_ARTIFACT_DIR}")
file(MAKE_DIRECTORY "${EMIT_ARTIFACT_DIR}")

# Helper: register an executable as a ctest by its target file (no hardcoded paths)
function(register_test_exe target_name)
    add_test(NAME "${target_name}" COMMAND $<TARGET_FILE:${target_name}>)
endfunction()

# grh tests
add_executable(grh-tests
    tests/grh/test_grh.cpp
)

target_link_libraries(grh-tests
    PRIVATE
        wolvrix-lib
)

target_compile_definitions(grh-tests
    PRIVATE
        GRH_STAGE1_JSON_PATH="${CMAKE_BINARY_DIR}/artifacts/grh_stage1_demo.json"
)

register_test_exe(grh-tests)

# grh roundtrip tests
add_executable(grh-roundtrip-tests
    tests/grh/test_grh_roundtrip.cpp
)

target_link_libraries(grh-roundtrip-tests
    PRIVATE
        wolvrix-lib
)

register_test_exe(grh-roundtrip-tests)

# grh clone tests
add_executable(grh-clone-tests
    tests/grh/test_grh_clone.cpp
)

target_link_libraries(grh-clone-tests
    PRIVATE
        wolvrix-lib
)

register_test_exe(grh-clone-tests)

# GrhSIM IR foundation
add_executable(grhsim-ir-tests
    tests/grhsim/test_grhsim_ir.cpp
)

target_link_libraries(grhsim-ir-tests
    PRIVATE
        wolvrix-lib
)

target_compile_definitions(grhsim-ir-tests
    PRIVATE
        WOLVRIX_GRHSIM_TEST_ARTIFACT_DIR="${WOLF_SV_TEST_ARTIFACT_DIR}/grhsim"
)

register_test_exe(grhsim-ir-tests)

add_executable(grhsim-cpu-mapping-tests tests/grhsim/test_cpu_mapping.cpp)
add_executable(grhsim-reg-to-mem-tests tests/grhsim/test_reg_to_mem.cpp tests/grhsim/test_reg_to_mem_semantics.cpp)
target_link_libraries(grhsim-reg-to-mem-tests PRIVATE wolvrix-lib)
target_compile_definitions(grhsim-reg-to-mem-tests PRIVATE
    WOLVRIX_GRHSIM_TEST_ARTIFACT_DIR="${WOLF_SV_TEST_ARTIFACT_DIR}/grhsim")
register_test_exe(grhsim-reg-to-mem-tests)
target_link_libraries(grhsim-cpu-mapping-tests PRIVATE wolvrix-lib)
register_test_exe(grhsim-cpu-mapping-tests)
add_executable(grhsim-cpu-schedule-tests tests/grhsim/test_cpu_schedule.cpp tests/grhsim/test_cpu_schedule_trace.cpp)
target_link_libraries(grhsim-cpu-schedule-tests PRIVATE wolvrix-lib)
register_test_exe(grhsim-cpu-schedule-tests)
find_program(WOLVRIX_TEST_VERILATOR verilator)
add_executable(grhsim-cpu-emit-tests tests/grhsim/test_cpu_emit.cpp)
target_link_libraries(grhsim-cpu-emit-tests PRIVATE wolvrix-lib)
target_compile_definitions(grhsim-cpu-emit-tests PRIVATE
    WOLVRIX_GRHSIM_TEST_ARTIFACT_DIR="${WOLF_SV_TEST_ARTIFACT_DIR}/grhsim"
    WOLVRIX_GRHSIM_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/grhsim/data"
    WOLVRIX_TEST_CXX="${CMAKE_CXX_COMPILER}"
    WOLVRIX_TEST_VERILATOR="${WOLVRIX_TEST_VERILATOR}")
register_test_exe(grhsim-cpu-emit-tests)
set_tests_properties(grhsim-cpu-emit-tests PROPERTIES SKIP_RETURN_CODE 77)

# core: toposort
add_executable(core-toposort
    tests/core/test_toposort.cpp
)

target_link_libraries(core-toposort
    PRIVATE
        wolvrix-lib
)

register_test_exe(core-toposort)

# emit base
add_executable(emit-base
    tests/emit/test_emit_base.cpp
)

target_link_libraries(emit-base
    PRIVATE
        wolvrix-lib
)

target_compile_definitions(emit-base
    PRIVATE
        WOLF_SV_EMIT_ARTIFACT_DIR="${EMIT_ARTIFACT_DIR}"
)

register_test_exe(emit-base)

add_executable(store-json
    tests/store/test_store_json.cpp
)

target_link_libraries(store-json
    PRIVATE
        wolvrix-lib
)

target_compile_definitions(store-json
    PRIVATE
        WOLF_SV_EMIT_ARTIFACT_DIR="${EMIT_ARTIFACT_DIR}"
)

register_test_exe(store-json)

add_executable(store-json-ir
    tests/store/test_store_json_ir.cpp
)

target_link_libraries(store-json-ir
    PRIVATE
        wolvrix-lib
)

target_compile_definitions(store-json-ir
    PRIVATE
        WOLF_SV_EMIT_ARTIFACT_DIR="${EMIT_ARTIFACT_DIR}"
)

register_test_exe(store-json-ir)

add_executable(emit-sv-readmem
    tests/emit/test_emit_sv_readmem.cpp
)

target_link_libraries(emit-sv-readmem
    PRIVATE
        wolvrix-lib
)

target_compile_definitions(emit-sv-readmem
    PRIVATE
        WOLF_SV_EMIT_ARTIFACT_DIR="${EMIT_ARTIFACT_DIR}"
)

register_test_exe(emit-sv-readmem)

add_executable(emit-sv-storage-ports
    tests/emit/test_emit_sv_storage_ports.cpp
)

target_link_libraries(emit-sv-storage-ports
    PRIVATE
        wolvrix-lib
)

target_compile_definitions(emit-sv-storage-ports
    PRIVATE
        WOLF_SV_EMIT_ARTIFACT_DIR="${EMIT_ARTIFACT_DIR}"
)

register_test_exe(emit-sv-storage-ports)

add_executable(emit-sv-top-reachable
    tests/emit/test_emit_sv_top_reachable.cpp
)

target_link_libraries(emit-sv-top-reachable
    PRIVATE
        wolvrix-lib
)

target_compile_definitions(emit-sv-top-reachable
    PRIVATE
        WOLF_SV_EMIT_ARTIFACT_DIR="${EMIT_ARTIFACT_DIR}"
)

register_test_exe(emit-sv-top-reachable)

add_executable(emit-grhsim-cpp
    tests/emit/test_emit_grhsim_cpp.cpp
)

target_link_libraries(emit-grhsim-cpp
    PRIVATE
        wolvrix-lib
)

target_compile_definitions(emit-grhsim-cpp
    PRIVATE
        WOLF_SV_EMIT_ARTIFACT_DIR="${EMIT_ARTIFACT_DIR}"
)

register_test_exe(emit-grhsim-cpp)

add_executable(emit-grhsim-cpp-memory-fill
    tests/emit/test_emit_grhsim_cpp_memory_fill.cpp
)

target_link_libraries(emit-grhsim-cpp-memory-fill
    PRIVATE
        wolvrix-lib
)

target_compile_definitions(emit-grhsim-cpp-memory-fill
    PRIVATE
        WOLF_SV_EMIT_ARTIFACT_DIR="${EMIT_ARTIFACT_DIR}"
)

register_test_exe(emit-grhsim-cpp-memory-fill)

add_executable(emit-verilator-repcut-package
    tests/emit/test_emit_verilator_repcut_package.cpp
)

target_link_libraries(emit-verilator-repcut-package
    PRIVATE
        wolvrix-lib
)

target_compile_definitions(emit-verilator-repcut-package
    PRIVATE
        WOLF_SV_EMIT_ARTIFACT_DIR="${EMIT_ARTIFACT_DIR}"
)

register_test_exe(emit-verilator-repcut-package)

# transform tests
add_executable(transform-pass-manager
    tests/transform/test_transform_pass_manager.cpp
)

target_link_libraries(transform-pass-manager
    PRIVATE
        wolvrix-lib
)

register_test_exe(transform-pass-manager)

add_executable(transform-dead-code-elim
    tests/transform/test_dead_code_elim_pass.cpp
)

target_link_libraries(transform-dead-code-elim
    PRIVATE
        wolvrix-lib
)

register_test_exe(transform-dead-code-elim)

add_executable(transform-redundant-elim
    tests/transform/test_redundant_elim_pass.cpp
)

target_link_libraries(transform-redundant-elim
    PRIVATE
        wolvrix-lib
)

register_test_exe(transform-redundant-elim)

add_executable(transform-simplify
    tests/transform/test_simplify_pass.cpp
)

target_link_libraries(transform-simplify
    PRIVATE
        wolvrix-lib
)

register_test_exe(transform-simplify)

add_executable(transform-hier-flatten
    tests/transform/test_hier_flatten_pass.cpp
)

target_link_libraries(transform-hier-flatten
    PRIVATE
        wolvrix-lib
)

register_test_exe(transform-hier-flatten)

add_executable(transform-instance-inline
    tests/transform/test_instance_inline_pass.cpp
)

target_link_libraries(transform-instance-inline
    PRIVATE
        wolvrix-lib
)

register_test_exe(transform-instance-inline)

add_executable(transform-comb-loop-elim
    tests/transform/test_comb_loop_elim_pass.cpp
)

target_link_libraries(transform-comb-loop-elim
    PRIVATE
        wolvrix-lib
)

register_test_exe(transform-comb-loop-elim)

add_executable(transform-comb-lane-pack
    tests/transform/test_comb_lane_pack_pass.cpp
)

target_link_libraries(transform-comb-lane-pack
    PRIVATE
        wolvrix-lib
)

register_test_exe(transform-comb-lane-pack)

add_executable(transform-activity-schedule
    tests/transform/test_activity_schedule_pass.cpp
)

target_link_libraries(transform-activity-schedule
    PRIVATE
        wolvrix-lib
)

target_compile_definitions(transform-activity-schedule
    PRIVATE
        WOLF_SV_TEST_ARTIFACT_DIR="${WOLF_SV_TEST_ARTIFACT_DIR}"
)

register_test_exe(transform-activity-schedule)

add_executable(transform-xmr-resolve-inout
    tests/transform/test_xmr_resolve_inout.cpp
)

target_link_libraries(transform-xmr-resolve-inout
    PRIVATE
        wolvrix-lib
)

register_test_exe(transform-xmr-resolve-inout)

add_executable(transform-xmr-resolve-storage
    tests/transform/test_xmr_resolve_storage.cpp
)

target_link_libraries(transform-xmr-resolve-storage
    PRIVATE
        wolvrix-lib
)

register_test_exe(transform-xmr-resolve-storage)

# transform: latch-transparent-read
add_executable(transform-latch-transparent-read
    tests/transform/test_latch_transparent_read.cpp
)

target_link_libraries(transform-latch-transparent-read
    PRIVATE
        wolvrix-lib
)

register_test_exe(transform-latch-transparent-read)

# transform: memory-read-retime
add_executable(transform-memory-read-retime
    tests/transform/test_memory_read_retime.cpp
)

target_link_libraries(transform-memory-read-retime
    PRIVATE
        wolvrix-lib
)

register_test_exe(transform-memory-read-retime)

# transform: reg-to-mem
add_executable(transform-reg-to-mem
    tests/transform/test_reg_to_mem_pass.cpp
)

target_link_libraries(transform-reg-to-mem
    PRIVATE
        wolvrix-lib
)

register_test_exe(transform-reg-to-mem)

# transform: strip-debug
add_executable(transform-strip-debug
    tests/transform/test_strip_debug_pass.cpp
)

target_link_libraries(transform-strip-debug
    PRIVATE
        wolvrix-lib
)

register_test_exe(transform-strip-debug)

# transform: repcut
add_executable(transform-repcut
    tests/transform/test_repcut_pass.cpp
)

target_link_libraries(transform-repcut
    PRIVATE
        wolvrix-lib
)

target_compile_definitions(transform-repcut
    PRIVATE
        WOLF_SV_TEST_ARTIFACT_DIR="${WOLF_SV_TEST_ARTIFACT_DIR}"
)

register_test_exe(transform-repcut)

add_executable(transform-repcut-partition-set
    tests/transform/test_repcut_partition_set.cpp
)

target_link_libraries(transform-repcut-partition-set
    PRIVATE
        wolvrix-lib
)

register_test_exe(transform-repcut-partition-set)

add_executable(transform-repcut-boundary-bundle
    tests/transform/test_repcut_boundary_bundle.cpp
)

target_link_libraries(transform-repcut-boundary-bundle
    PRIVATE
        wolvrix-lib
)

register_test_exe(transform-repcut-boundary-bundle)

add_executable(transform-repcut-partition-count
    tests/transform/test_repcut_partition_count.cpp
)

target_link_libraries(transform-repcut-partition-count
    PRIVATE
        wolvrix-lib
)

target_compile_definitions(transform-repcut-partition-count
    PRIVATE
        WOLF_SV_TEST_ARTIFACT_DIR="${WOLF_SV_TEST_ARTIFACT_DIR}"
)

register_test_exe(transform-repcut-partition-count)

# ingest: symbol collector
add_executable(ingest-symbol-collector
    tests/ingest/test_ingest_symbol_collector.cpp
)

target_link_libraries(ingest-symbol-collector
    PRIVATE
        wolvrix-lib
)

target_compile_definitions(ingest-symbol-collector
    PRIVATE
        WOLF_SV_INGEST_HDLBITS_DUT_DIR="${WOLF_SV_TEST_INPUT_DIR}/hdlbits/dut"
)

register_test_exe(ingest-symbol-collector)

# ingest: type resolver
add_executable(ingest-type-resolver
    tests/ingest/test_ingest_type_resolver.cpp
)

target_link_libraries(ingest-type-resolver
    PRIVATE
        wolvrix-lib
)

target_compile_definitions(ingest-type-resolver
    PRIVATE
        WOLF_SV_INGEST_HDLBITS_DUT_DIR="${WOLF_SV_TEST_INPUT_DIR}/hdlbits/dut"
)

register_test_exe(ingest-type-resolver)

# ingest: stmt lowerer
add_executable(ingest-stmt-lowerer
    tests/ingest/test_ingest_stmt_lowerer.cpp
)
target_link_libraries(ingest-stmt-lowerer
    PRIVATE
        wolvrix-lib
)
target_compile_definitions(ingest-stmt-lowerer
    PRIVATE
        WOLF_SV_INGEST_STMT_DATA_PATH="${WOLF_SV_INGEST_TEST_DATA_DIR}/stmt_lowerer.sv"
)
register_test_exe(ingest-stmt-lowerer)

# ingest: stmt lowerer error paths
add_executable(ingest-stmt-lowerer-errors
    tests/ingest/test_ingest_stmt_lowerer_errors.cpp
)
target_link_libraries(ingest-stmt-lowerer-errors
    PRIVATE
        wolvrix-lib
)
target_compile_definitions(ingest-stmt-lowerer-errors
    PRIVATE
        WOLF_SV_INGEST_STMT_ERROR_DATA_PATH="${WOLF_SV_INGEST_TEST_DATA_DIR}/stmt_lowerer_errors.sv"
)
register_test_exe(ingest-stmt-lowerer-errors)

# ingest: write-back
add_executable(ingest-write-back
    tests/ingest/test_ingest_write_back.cpp
)
target_link_libraries(ingest-write-back
    PRIVATE
        wolvrix-lib
)
target_compile_definitions(ingest-write-back
    PRIVATE
        WOLF_SV_INGEST_WRITE_BACK_DATA_PATH="${WOLF_SV_INGEST_TEST_DATA_DIR}/write_back.sv"
)
register_test_exe(ingest-write-back)

# ingest: write-back slice
add_executable(ingest-write-back-slice
    tests/ingest/test_ingest_write_back_slice.cpp
)
target_link_libraries(ingest-write-back-slice
    PRIVATE
        wolvrix-lib
)
target_compile_definitions(ingest-write-back-slice
    PRIVATE
        WOLF_SV_INGEST_WRITE_BACK_SLICE_DATA_PATH="${WOLF_SV_INGEST_TEST_DATA_DIR}/write_back_slice.sv"
        WOLF_SV_XS_BUGCASE_CASE009_DATA_PATH="${CMAKE_SOURCE_DIR}/../testcase/xs-bugcase/CASE_009/rtl/OldestArbiterResetCase009.sv"
        WOLF_SV_XS_BUGCASE_CASE011_DATA_PATH="${CMAKE_SOURCE_DIR}/../testcase/xs-bugcase/CASE_011/rtl/PackedAggregateBitSelectCase011.sv"
)
register_test_exe(ingest-write-back-slice)

# ingest: memory port lowerer
add_executable(ingest-memory-port-lowerer
    tests/ingest/test_ingest_memory_port_lowerer.cpp
)
target_link_libraries(ingest-memory-port-lowerer
    PRIVATE
        wolvrix-lib
)
target_compile_definitions(ingest-memory-port-lowerer
    PRIVATE
        WOLF_SV_INGEST_MEMORY_PORT_DATA_PATH="${WOLF_SV_INGEST_TEST_DATA_DIR}/memory_ports.sv"
)
register_test_exe(ingest-memory-port-lowerer)

# ingest: graph assembly basic
add_executable(ingest-graph-assembly-basic
    tests/ingest/test_ingest_graph_assembly_basic.cpp
)
target_link_libraries(ingest-graph-assembly-basic
    PRIVATE
        wolvrix-lib
)
target_compile_definitions(ingest-graph-assembly-basic
    PRIVATE
        WOLF_SV_INGEST_GRAPH_ASSEMBLY_DATA_PATH="${WOLF_SV_INGEST_TEST_DATA_DIR}/graph_assembly_basic.sv"
)
register_test_exe(ingest-graph-assembly-basic)

# ingest: graph assembly register multi-write
add_executable(ingest-graph-assembly-register-multi
    tests/ingest/test_ingest_graph_assembly_register_multi.cpp
)
target_link_libraries(ingest-graph-assembly-register-multi
    PRIVATE
        wolvrix-lib
)
target_compile_definitions(ingest-graph-assembly-register-multi
    PRIVATE
        WOLF_SV_INGEST_GRAPH_ASSEMBLY_REGISTER_MULTI_DATA_PATH="${WOLF_SV_INGEST_TEST_DATA_DIR}/graph_assembly_register_multi.sv"
)
register_test_exe(ingest-graph-assembly-register-multi)

# ingest: graph assembly register declaration initializer
add_executable(ingest-graph-assembly-register-init
    tests/ingest/test_ingest_graph_assembly_register_init.cpp
)
target_link_libraries(ingest-graph-assembly-register-init
    PRIVATE
        wolvrix-lib
)
target_compile_definitions(ingest-graph-assembly-register-init
    PRIVATE
        WOLF_SV_INGEST_GRAPH_ASSEMBLY_REGISTER_INIT_DATA_PATH="${WOLF_SV_INGEST_TEST_DATA_DIR}/graph_assembly_register_init.sv"
)
register_test_exe(ingest-graph-assembly-register-init)

# ingest: graph assembly memory
add_executable(ingest-graph-assembly-memory
    tests/ingest/test_ingest_graph_assembly_memory.cpp
)
target_link_libraries(ingest-graph-assembly-memory
    PRIVATE
        wolvrix-lib
)
target_compile_definitions(ingest-graph-assembly-memory
    PRIVATE
        WOLF_SV_INGEST_GRAPH_ASSEMBLY_MEMORY_DATA_PATH="${WOLF_SV_INGEST_TEST_DATA_DIR}/graph_assembly_memory.sv"
)
register_test_exe(ingest-graph-assembly-memory)

# ingest: graph assembly slice
add_executable(ingest-graph-assembly-slice
    tests/ingest/test_ingest_graph_assembly_slice.cpp
)
target_link_libraries(ingest-graph-assembly-slice
    PRIVATE
        wolvrix-lib
)
target_compile_definitions(ingest-graph-assembly-slice
    PRIVATE
        WOLF_SV_INGEST_GRAPH_ASSEMBLY_SLICE_DATA_PATH="${WOLF_SV_INGEST_TEST_DATA_DIR}/graph_assembly_slice.sv"
)
register_test_exe(ingest-graph-assembly-slice)

# ingest: graph assembly aggregate port slice
add_executable(ingest-graph-assembly-aggregate-port-slice
    tests/ingest/test_ingest_graph_assembly_aggregate_port_slice.cpp
)
target_link_libraries(ingest-graph-assembly-aggregate-port-slice
    PRIVATE
        wolvrix-lib
)
target_compile_definitions(ingest-graph-assembly-aggregate-port-slice
    PRIVATE
        WOLF_SV_INGEST_GRAPH_ASSEMBLY_AGGREGATE_PORT_SLICE_DATA_PATH="${WOLF_SV_INGEST_TEST_DATA_DIR}/graph_assembly_aggregate_port_slice.sv"
)
register_test_exe(ingest-graph-assembly-aggregate-port-slice)

# ingest: graph assembly dpi/display
add_executable(ingest-graph-assembly-dpi-display
    tests/ingest/test_ingest_graph_assembly_dpi_display.cpp
)
target_link_libraries(ingest-graph-assembly-dpi-display
    PRIVATE
        wolvrix-lib
)
target_compile_definitions(ingest-graph-assembly-dpi-display
    PRIVATE
        WOLF_SV_INGEST_GRAPH_ASSEMBLY_DPI_DISPLAY_DATA_PATH="${WOLF_SV_INGEST_TEST_DATA_DIR}/graph_assembly_dpi_display.sv"
)
register_test_exe(ingest-graph-assembly-dpi-display)

# ingest: graph assembly instance
add_executable(ingest-graph-assembly-instance
    tests/ingest/test_ingest_graph_assembly_instance.cpp
)
target_link_libraries(ingest-graph-assembly-instance
    PRIVATE
        wolvrix-lib
)
target_compile_definitions(ingest-graph-assembly-instance
    PRIVATE
        WOLF_SV_INGEST_GRAPH_ASSEMBLY_INSTANCE_DATA_PATH="${WOLF_SV_INGEST_TEST_DATA_DIR}/graph_assembly_instance.sv"
)
register_test_exe(ingest-graph-assembly-instance)

# ingest: graph assembly instance dedup
add_executable(ingest-graph-assembly-instance-dedup
    tests/ingest/test_ingest_graph_assembly_instance_dedup.cpp
)
target_link_libraries(ingest-graph-assembly-instance-dedup
    PRIVATE
        wolvrix-lib
)
target_compile_definitions(ingest-graph-assembly-instance-dedup
    PRIVATE
        WOLF_SV_INGEST_GRAPH_ASSEMBLY_INSTANCE_DEDUP_DATA_PATH="${WOLF_SV_INGEST_TEST_DATA_DIR}/graph_assembly_instance_dedup.sv"
)
register_test_exe(ingest-graph-assembly-instance-dedup)

# Installation rules
install(TARGETS wolvrix-lib LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
