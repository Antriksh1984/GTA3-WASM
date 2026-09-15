# Rendering diagnostic tool (Phase 4, docs/WEBGL_COMPATIBILITY.md). Links directly
# against librw + librw_skeleton (the same GL3 renderer re3 itself uses) -- see
# rendertest/main.cpp's header comment for why this exists and how it differs from
# building the full re3 target.
add_executable(rendertest
    main.cpp
)

target_link_libraries(rendertest
    PUBLIC
        librw
        librw_skeleton
)

if(EMSCRIPTEN)
    set(RENDERTEST_EMSCRIPTEN_FLAGS
        -sUSE_GLFW=3
        -sUSE_WEBGL2=1
        -sALLOW_MEMORY_GROWTH=1
        -sWASM=1
        -sEXPORTED_RUNTIME_METHODS=callMain
        # Same MODULARIZE/INVOKE_RUN=0 pattern as src/CMakeLists.txt's re3_wasm target
        # (see docs/BROWSER_RUNTIME.md) -- web/rendertest.html controls instantiation
        # and calls callMain() itself once the canvas is ready.
        -sMODULARIZE=1
        -sEXPORT_NAME=createRenderTestModule
        -sINVOKE_RUN=0
        -O2
    )
    target_compile_options(rendertest PUBLIC ${RENDERTEST_EMSCRIPTEN_FLAGS})
    target_link_options(rendertest PUBLIC ${RENDERTEST_EMSCRIPTEN_FLAGS})
    set_target_properties(rendertest PROPERTIES SUFFIX ".js")

    set(RENDERTEST_WEB_DIR "${PROJECT_SOURCE_DIR}/web/build")
    add_custom_command(TARGET rendertest POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RENDERTEST_WEB_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE_DIR:rendertest>/rendertest.js"
            "${RENDERTEST_WEB_DIR}/rendertest.js"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE_DIR:rendertest>/rendertest.wasm"
            "${RENDERTEST_WEB_DIR}/rendertest.wasm"
        COMMENT "Copying rendertest.js/.wasm to web/build/"
        VERBATIM
    )
endif()
