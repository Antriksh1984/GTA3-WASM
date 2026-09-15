# Audio diagnostic tool (Phase 6, docs/AUDIO_WASM.md). Links against nothing
# from src/ or vendor/librw -- only Emscripten's own OpenAL implementation, the
# same backend src/audio/sampman_oal.cpp targets via AUDIO_OAL. See
# audiotest/main.cpp's header comment for why this exists.
#
# Unlike rendertest (which links librw and is genuinely useful natively too),
# this tool exists purely to validate the WASM/browser OpenAL path -- native
# builds already exercise real OpenAL through the actual game. It is therefore
# gated entirely behind EMSCRIPTEN: main.cpp has no native OpenAL include/link
# setup (unlike src/CMakeLists.txt's find_package(OpenAL REQUIRED) for the main
# target), so building it natively would fail to configure/link. Skipping it
# entirely here keeps native builds exactly as they were before this phase.
if(EMSCRIPTEN)
    add_executable(audiotest
        main.cpp
    )

    set(AUDIOTEST_EMSCRIPTEN_FLAGS
        -sALLOW_MEMORY_GROWTH=1
        -sWASM=1
        -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,callMain
        # No USE_GLFW/USE_WEBGL2 -- this tool never touches a canvas or GL context.
        # MODULARIZE/INVOKE_RUN=0 mirrors src/CMakeLists.txt and rendertest's own
        # pattern (see docs/BROWSER_RUNTIME.md), even though here main() itself is
        # a no-op and INVOKE_RUN=0 is mostly cosmetic -- kept for consistency and so
        # web/audiotest-launcher.js can control exactly when the module loads.
        -sMODULARIZE=1
        -sEXPORT_NAME=createAudioTestModule
        -sINVOKE_RUN=0
        -sEXIT_RUNTIME=0
        -O2
    )
    target_compile_options(audiotest PUBLIC ${AUDIOTEST_EMSCRIPTEN_FLAGS})
    target_link_options(audiotest PUBLIC ${AUDIOTEST_EMSCRIPTEN_FLAGS})
    set_target_properties(audiotest PROPERTIES SUFFIX ".js")

    set(AUDIOTEST_WEB_DIR "${PROJECT_SOURCE_DIR}/web/build")
    add_custom_command(TARGET audiotest POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${AUDIOTEST_WEB_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE_DIR:audiotest>/audiotest.js"
            "${AUDIOTEST_WEB_DIR}/audiotest.js"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE_DIR:audiotest>/audiotest.wasm"
            "${AUDIOTEST_WEB_DIR}/audiotest.wasm"
        COMMENT "Copying audiotest.js/.wasm to web/build/"
        VERBATIM
    )
endif()
