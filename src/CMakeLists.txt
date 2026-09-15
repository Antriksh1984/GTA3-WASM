if(NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
endif()
set(THREADS_PREFER_PTHREAD_FLAG ON)

file(GLOB_RECURSE ${PROJECT}_SOURCES "*.cpp" "*.h" "*.rc")

function(header_directories RETURN_LIST)
    file(GLOB_RECURSE ALL_SRCS *.h *.cpp *.c)
    set(RELDIRS)
    foreach(SRC ${ALL_SRCS})
        file(RELATIVE_PATH RELSRC "${CMAKE_CURRENT_SOURCE_DIR}" "${SRC}")
        get_filename_component(RELDIR "${RELSRC}" DIRECTORY)
        list(APPEND RELDIRS ${RELDIR})
    endforeach()
    list(REMOVE_DUPLICATES RELDIRS)
    set(${RETURN_LIST} ${RELDIRS} PARENT_SCOPE)
endfunction()

header_directories(${PROJECT}_INCLUDES)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/extras/GitSHA1.cpp.in" "${CMAKE_CURRENT_BINARY_DIR}/extras/GitSHA1.cpp" @ONLY)
list(APPEND ${PROJECT}_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/extras/GitSHA1.cpp")

if(EMSCRIPTEN)
    add_executable(${EXECUTABLE}
        ${${PROJECT}_SOURCES}
    )
else()
    add_executable(${EXECUTABLE} WIN32
        ${${PROJECT}_SOURCES}
    )
endif()

target_link_libraries(${EXECUTABLE} PRIVATE
    librw::librw
)
if(NOT EMSCRIPTEN)
    target_link_libraries(${EXECUTABLE} PRIVATE Threads::Threads)
endif()

target_include_directories(${EXECUTABLE}
    PRIVATE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${${PROJECT}_INCLUDES}>
)

target_compile_definitions(${EXECUTABLE}
    PRIVATE
        $<IF:$<CONFIG:DEBUG>,DEBUG,NDEBUG>
        LIBRW
        ${PROJECT}_NO_AUTOLINK
)

if(LIBRW_PLATFORM_D3D9)
    target_compile_definitions(${EXECUTABLE}
        PUBLIC
            USE_D3D9
    )
endif()

target_compile_definitions(${EXECUTABLE} PRIVATE CMAKE_BUILD)

if(${PROJECT}_AUDIO STREQUAL "OAL")
    if(NOT EMSCRIPTEN)
        find_package(OpenAL REQUIRED)
        target_include_directories(${EXECUTABLE} PRIVATE ${OPENAL_INCLUDE_DIR})
        target_link_libraries(${EXECUTABLE} PRIVATE ${OPENAL_LIBRARY})
        target_compile_definitions(${EXECUTABLE} PRIVATE ${OPENAL_DEFINITIONS})
    endif()
    target_compile_definitions(${EXECUTABLE} PRIVATE AUDIO_OAL)
elseif(${PROJECT}_AUDIO STREQUAL "MSS")
    find_package(MilesSDK REQUIRED)
    target_compile_definitions(${EXECUTABLE} PRIVATE AUDIO_MSS)
    target_link_libraries(${EXECUTABLE} PRIVATE MilesSDK::MilesSDK)
endif()

if(NOT EMSCRIPTEN)
    find_package(mpg123 REQUIRED)
    target_link_libraries(${EXECUTABLE} PRIVATE
        MPG123::libmpg123
    )
endif()
if(${PROJECT}_WITH_OPUS)
    find_package(opusfile REQUIRED)
    target_link_libraries(${EXECUTABLE} PRIVATE
        opusfile::opusfile
    )
    target_compile_definitions(${EXECUTABLE} PRIVATE AUDIO_OPUS)
endif()
if(${PROJECT}_WITH_LIBSNDFILE)
    find_package(SndFile REQUIRED)
    target_link_libraries(${EXECUTABLE} PRIVATE
        SndFile::SndFile
    )
    target_compile_definitions(${EXECUTABLE} PRIVATE AUDIO_OAL_USE_SNDFILE)
endif()

target_compile_definitions(${EXECUTABLE} PRIVATE )

option(${PROJECT}_WITH_SANITIZERS "Use UB sanitizers (better crash log)" OFF)
option(${PROJECT}_WITH_ASAN "Use Address sanitizer (better crash log)" OFF)

if(${PROJECT}_WITH_SANITIZERS)
    target_compile_options(${EXECUTABLE} PUBLIC
            -fsanitize=undefined,float-divide-by-zero,integer,implicit-conversion,implicit-integer-truncation,implicit-integer-arithmetic-value-change,local-bounds,nullability
            -g3 -fno-omit-frame-pointer)
    target_link_options(${EXECUTABLE} PUBLIC -fsanitize=undefined,float-divide-by-zero,integer,implicit-conversion,implicit-integer-truncation,implicit-integer-arithmetic-value-change,local-bounds,nullability)
endif()

if(${PROJECT}_WITH_ASAN)
    target_compile_options(${EXECUTABLE} PUBLIC -fsanitize=address -g3 -fno-omit-frame-pointer)
    target_link_options(${EXECUTABLE} PUBLIC -fsanitize=address)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    target_compile_options(${EXECUTABLE}
        PRIVATE
            "-Wall"
    )
    if (NOT LIBRW_PLATFORM_PS2)
        target_compile_options(${EXECUTABLE}
            PRIVATE
                -Wextra
                -Wdouble-promotion
                -Wpedantic
        )
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(${EXECUTABLE}
        PUBLIC
            /Zc:sizedDealloc-
    )
endif()

set_target_properties(${EXECUTABLE}
    PROPERTIES
        C_STANDARD 11
        C_EXTENSIONS OFF
        C_STANDARD_REQUIRED ON
        CXX_STANDARD 11
        CXX_EXTENSIONS OFF
        CXX_STANDARD_REQUIRED ON
)

if(${PROJECT}_INSTALL)
    install(
        TARGETS ${EXECUTABLE}
        EXPORT ${EXECUTABLE}-targets
        RUNTIME DESTINATION "."
    )
    if(MSVC)
        install(FILES $<TARGET_PDB_FILE:${EXECUTABLE}> DESTINATION "." OPTIONAL)
    endif()
endif()

if(EMSCRIPTEN)
    # Emscripten's OpenAL implementation doesn't ship AL/efx.h (see oal_utils.h/.cpp).
    # This vendored replacement lives outside src/audio/oal/AL so it can never shadow
    # a real OpenAL SDK's efx.h on native (non-Emscripten) builds.
    target_include_directories(${EXECUTABLE} PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/audio/oal/emscripten_al>
    )
    set(EMSCRIPTEN_FLAGS
        -sUSE_GLFW=3
        -sUSE_WEBGL2=1
        -sUSE_MPG123=1
        -sALLOW_MEMORY_GROWTH=1
        # No -sUSE_PTHREADS: Phase 8's threading audit (docs/THREADING_WASM.md) found
        # the only background thread anywhere in re3 or its dependencies is the
        # CD-streaming worker (src/core/CdStreamPosix.cpp), which now runs
        # synchronously on the main thread under __EMSCRIPTEN__ instead -- there is
        # nothing left in this build that needs real threads. This also means no
        # SharedArrayBuffer / cross-origin-isolation requirement for hosting.
        -sWASM=1
        -sFORCE_FILESYSTEM=1
        -sEXPORTED_RUNTIME_METHODS=cwrap,ccall,FS,callMain
        # IDBFS lets web/vfs.js persist the mounted asset root across page loads
        # (task 6C) -- it's a JS library, not linked in by default.
        -lidbfs.js
        # MODULARIZE + EXPORT_NAME: the generated .js defines a factory function
        # instead of auto-running against a global `Module`, so web/launcher.js can
        # control instantiation, wire up progress/error callbacks, and decide exactly
        # when to hand off to main() -- see docs/BROWSER_RUNTIME.md.
        -sMODULARIZE=1
        -sEXPORT_NAME=createRe3Module
        # Don't auto-invoke main(): the launcher calls Module.callMain() itself once
        # the canvas and diagnostics UI are ready, so engine startup failures surface
        # as a rejected promise instead of running before anything can observe them.
        -sINVOKE_RUN=0
        -O2
    )
    target_compile_options(${EXECUTABLE} PUBLIC ${EMSCRIPTEN_FLAGS})
    target_link_options(${EXECUTABLE} PUBLIC ${EMSCRIPTEN_FLAGS})
    # --emit-symbol-map: writes src/re3_wasm.js.symbols (wasm-function[N] ->
    # real C++ name), used to symbolicate the gameplay crash investigated in
    # docs/WASM_GAMEPLAY_STABILITY.md. Kept permanently -- zero runtime cost
    # (not one of the -s flags above; doesn't change the emitted .js/.wasm at
    # all, just adds this one extra file next to them) and it's the fastest
    # path to reading the next WASM crash trace. NOT copied into web/build/ by
    # the POST_BUILD step below, so it never ships.
    #
    # -sASSERTIONS=2/-sSAFE_HEAP=1/-sSTACK_OVERFLOW_CHECK=2 were also tried
    # while symbolicating that crash and were NOT kept: together or
    # individually, they made boot itself abort with "segmentation fault"
    # deep inside librw's WebGL device setup (rw::gl3::deviceSystemGLFW) --
    # well before gameplay, and before any of that investigation's own code
    # even ran. That's a real, separate, pre-existing memory-safety finding
    # (see the doc's "separate, non-blocking, unresolved observation"-style
    # notes) worth its own follow-up with those flags re-enabled, but they are
    # NOT safe to leave on by default given that.
    target_link_options(${EXECUTABLE} PUBLIC --emit-symbol-map)
    # Emit re3_wasm.js (+ re3_wasm.wasm) directly -- web/index.html is our own shell,
    # so we don't need Emscripten's default generated HTML page.
    set_target_properties(${EXECUTABLE} PROPERTIES SUFFIX ".js")

    # Keep web/ a self-contained, directly servable directory: copy the build output
    # there after every build instead of making the web shell reach into build-wasm/.
    set(RE3_WEB_DIR "${PROJECT_SOURCE_DIR}/web/build")
    add_custom_command(TARGET ${EXECUTABLE} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RE3_WEB_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE_DIR:${EXECUTABLE}>/${EXECUTABLE}.js"
            "${RE3_WEB_DIR}/${EXECUTABLE}.js"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE_DIR:${EXECUTABLE}>/${EXECUTABLE}.wasm"
            "${RE3_WEB_DIR}/${EXECUTABLE}.wasm"
        COMMENT "Copying ${EXECUTABLE}.js/.wasm to web/build/"
        VERBATIM
    )
endif()
