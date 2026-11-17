# Darwin cannot run the fixed-memory i386 harnesses. Reuse the native
# runners so CTest and CI exercise the same guest compiler/host ABI boundary.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    message(FATAL_ERROR "The macOS game target requires Apple Silicon")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_target(pc_game
    COMMAND "${Python3_EXECUTABLE}" tools/pc/build.py --target macos
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    USES_TERMINAL)
add_custom_target(pc_audit
    COMMAND "${Python3_EXECUTABLE}" tools/pc/audit.py
        --output "${CMAKE_BINARY_DIR}/port-audit.json"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
if(BUILD_TESTING)
    foreach(script test_llvm_guest test_ps1_layouts test_guest_storage_layout test_guest_store_canary)
        add_test(NAME pc_${script}
            COMMAND "${Python3_EXECUTABLE}" tools/pc/${script}.py)
        set_tests_properties(pc_${script} PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            TIMEOUT 180 RUN_SERIAL TRUE)
    endforeach()
    set(sanitizer_args "")
    if(MEMORIES_SANITIZERS)
        set(sanitizer_args --sanitize)
    endif()
    foreach(script test_native test_guest_contracts test_arm64_mod_loader test_arm64_mod_hooks
            test_direct_overlay_bridge test_libgs_sprite_guest test_macos_fonts)
        add_test(NAME pc_${script}
            COMMAND "${Python3_EXECUTABLE}" tools/pc/${script}.py ${sanitizer_args})
        set_tests_properties(pc_${script} PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            TIMEOUT 180 RUN_SERIAL TRUE)
    endforeach()
    add_test(NAME pc_host_renderer_boundaries
        COMMAND "${Python3_EXECUTABLE}" tools/pc/test_host_renderer_boundaries.py --differential ${sanitizer_args})
    set_tests_properties(pc_host_renderer_boundaries PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        TIMEOUT 180 RUN_SERIAL TRUE)
endif()
