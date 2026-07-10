# Byte-identity gates. The tiny synthetic model is generated at test time by
# scripts/make-tiny-moe.py (requires python3 with the gguf package); if it is
# unavailable the test is skipped rather than failing the build.

add_executable(bmoe_moe_gates moe_gates.cpp)
target_link_libraries(bmoe_moe_gates PRIVATE bmoe_core)

set(TINY_MODEL "${CMAKE_CURRENT_BINARY_DIR}/tiny-moe.gguf")

find_program(PYTHON3 NAMES python3 python)

if(PYTHON3)
    add_test(
        NAME moe_make_tiny_model
        COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/scripts/make-tiny-moe.py --out ${TINY_MODEL}
    )
    set_tests_properties(moe_make_tiny_model PROPERTIES FIXTURES_SETUP TINY_MOE_MODEL)

    add_test(NAME moe_gates COMMAND bmoe_moe_gates ${TINY_MODEL})
    set_tests_properties(moe_gates PROPERTIES FIXTURES_REQUIRED TINY_MOE_MODEL)
else()
    message(WARNING "python3 not found: MoE gates will not run (model cannot be generated)")
endif()
