# Byte-identity gates. The tiny synthetic models are generated at test time by
# scripts/make-tiny-moe.py (requires python3 with the gguf package); if it is
# unavailable the tests are skipped rather than failing the build. One model per expert
# layout the streamer supports: qwen3moe (split gate/up/down) and gemma4 (fused gate_up).

add_executable(bmoe_moe_gates moe_gates.cpp)
target_link_libraries(bmoe_moe_gates PRIVATE bmoe_core)

# The cache governor is pure policy (no llama.cpp, no syscalls, no model), so unlike the gates it
# needs neither a fixture nor the native backend: it runs against a synthetic device.
add_executable(bmoe_cache_governor_test cache_governor_test.cpp)
target_link_libraries(bmoe_cache_governor_test PRIVATE bmoe_core)
add_test(NAME cache_governor COMMAND bmoe_cache_governor_test)

find_program(PYTHON3 NAMES python3 python)

if(PYTHON3)
    # arch → (fixture id, gguf filename). Each gets a generate fixture + a gate run.
    foreach(spec "qwen3moe" "gemma4")
        set(_model "${CMAKE_CURRENT_BINARY_DIR}/tiny-moe-${spec}.gguf")
        set(_fixture "TINY_MOE_MODEL_${spec}")

        add_test(
            NAME moe_make_tiny_model_${spec}
            COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/scripts/make-tiny-moe.py --arch ${spec} --out ${_model}
        )
        set_tests_properties(moe_make_tiny_model_${spec} PROPERTIES FIXTURES_SETUP ${_fixture})

        add_test(NAME moe_gates_${spec} COMMAND bmoe_moe_gates ${_model})
        set_tests_properties(moe_gates_${spec} PROPERTIES FIXTURES_REQUIRED ${_fixture})
    endforeach()
else()
    message(WARNING "python3 not found: MoE gates will not run (models cannot be generated)")
endif()
