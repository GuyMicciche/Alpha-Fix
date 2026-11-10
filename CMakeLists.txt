#==============================================================================
# AlphaFix — CMake Build Configuration
#
# Builds the AlphaFix After Effects SmartFX plugin with FFmpeg integration.
#
# Prerequisites:
#   - After Effects SDK (set AE_SDK_PATH)
#   - FFmpeg dev libraries (set FFMPEG_PATH)
#   - Visual Studio 2022 (Windows)
#
# Build (from project root):
#   cmake -B build -S . -G "Visual Studio 17 2022" -A x64 ^
#       -DAE_SDK_PATH="C:/path/to/AfterEffectsSDK" ^
#       -DFFMPEG_PATH="C:/path/to/ffmpeg-shared"
#   cmake --build build --config Release
#
#==============================================================================

cmake_minimum_required(VERSION 3.20)
set(ALPHAFIX_VERSION "1.0.0" CACHE STRING "Release version (without v)")
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
endif()
project(AlphaFix VERSION ${ALPHAFIX_VERSION} LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── User-configurable paths ──────────────────────────────────────────────────

set(AE_SDK_PATH "" CACHE PATH "Path to After Effects SDK root")
set(FFMPEG_PATH "" CACHE PATH "Path to FFmpeg dev installation (include/ and lib/)")

if(NOT AE_SDK_PATH)
    message(FATAL_ERROR "AE_SDK_PATH not set.")
endif()

if(NOT FFMPEG_PATH)
    message(FATAL_ERROR "FFMPEG_PATH not set.")
endif()

# ── Validate SDK path ────────────────────────────────────────────────────────

if(NOT EXISTS "${AE_SDK_PATH}/Headers/AE_Effect.h")
    message(FATAL_ERROR "AE_Effect.h not found at ${AE_SDK_PATH}/Headers/")
endif()

if(NOT EXISTS "${FFMPEG_PATH}/include/libavformat/avformat.h")
    message(FATAL_ERROR "FFmpeg headers not found at ${FFMPEG_PATH}/include/")
endif()

# ── AE SDK include directories ───────────────────────────────────────────────
# Try both standard SDK layouts (flat and Examples-based)

set(AE_SDK_INCLUDES
    "${AE_SDK_PATH}/Headers"
    "${AE_SDK_PATH}/Headers/SP"
    "${AE_SDK_PATH}/Headers/adobesdk"
    "${AE_SDK_PATH}/Util"
    "${AE_SDK_PATH}/Resources"
)

# Some SDKs put things under Examples/
if(EXISTS "${AE_SDK_PATH}/Examples/Headers")
    list(APPEND AE_SDK_INCLUDES
        "${AE_SDK_PATH}/Examples/Headers"
        "${AE_SDK_PATH}/Examples/Headers/SP"
        "${AE_SDK_PATH}/Examples/Resources"
        "${AE_SDK_PATH}/Examples/Util"
    )
endif()

# ── Source files ──────────────────────────────────────────────────────────────

set(SOURCES
    src/AlphaFix.cpp
    src/AlphaFixDecoder.cpp
)

set(HEADERS
    src/AlphaFix.h
    src/AlphaFixDecoder.h
)

# AE SDK utility sources
set(AE_UTIL_SOURCES "")
foreach(UTIL_FILE "AEGP_SuiteHandler.cpp" "MissingSuiteError.cpp")
    if(EXISTS "${AE_SDK_PATH}/Util/${UTIL_FILE}")
        list(APPEND AE_UTIL_SOURCES "${AE_SDK_PATH}/Util/${UTIL_FILE}")
    elseif(EXISTS "${AE_SDK_PATH}/Examples/Util/${UTIL_FILE}")
        list(APPEND AE_UTIL_SOURCES "${AE_SDK_PATH}/Examples/Util/${UTIL_FILE}")
    endif()
endforeach()

# ══════════════════════════════════════════════════════════════════════════════
# PiPL Resource Pipeline (Windows)
#
# The AE SDK uses a custom resource format called PiPL.
# Pipeline: .r → (cl /EP) → .rr → (PiPLtool) → .rc → (rc) → .res
#
# NOTE: PiPLtool may omit commas in the generated .rc for some properties.
# After generation, the build will proceed — if AE still has PiPL issues,
# manually fix the commas in build/AlphaFix_PiPL.rc (add comma after "4L"
# on lines containing eVER, eINF, and aeFL properties).
# ══════════════════════════════════════════════════════════════════════════════

set(PIPL_RC_FILE "")

if(WIN32)
    # Find PiPLtool.exe
    find_file(PIPL_TOOL_EXE
        NAMES "PiPLtool.exe" "PiPLTool.exe"
        PATHS
            "${AE_SDK_PATH}/Resources"
            "${AE_SDK_PATH}/Examples/Resources"
            "${AE_SDK_PATH}/Util"
        NO_DEFAULT_PATH
    )
    if(NOT PIPL_TOOL_EXE)
        file(GLOB_RECURSE PIPL_SEARCH "${AE_SDK_PATH}/*PiPLtool*")
        if(PIPL_SEARCH)
            list(GET PIPL_SEARCH 0 PIPL_TOOL_EXE)
        endif()
    endif()

    if(NOT PIPL_TOOL_EXE)
        message(FATAL_ERROR "PiPLtool.exe not found in AE SDK at ${AE_SDK_PATH}")
    endif()
    message(STATUS "PiPLtool: ${PIPL_TOOL_EXE}")

    # Build include paths for cl /EP
    set(PIPL_INCLUDE_ARGS "")
    foreach(INC_DIR ${AE_SDK_INCLUDES})
        if(EXISTS "${INC_DIR}")
            list(APPEND PIPL_INCLUDE_ARGS "/I" "${INC_DIR}")
        endif()
    endforeach()

    set(PIPL_R_INPUT   "${CMAKE_SOURCE_DIR}/src/AlphaFix_PiPL.r")
    set(PIPL_RR_FILE   "${CMAKE_BINARY_DIR}/AlphaFix_PiPL.rr")
    set(PIPL_RC_OUTPUT "${CMAKE_BINARY_DIR}/AlphaFix_PiPL.rc")

    # Step 1: Preprocess .r → .rr
    add_custom_command(
        OUTPUT "${PIPL_RR_FILE}"
        COMMAND cl /EP /DWIN32 /D_WIN64 /DAE_OS_WIN /DMSWINDOWS
            ${PIPL_INCLUDE_ARGS}
            "${PIPL_R_INPUT}"
            > "${PIPL_RR_FILE}"
        DEPENDS "${PIPL_R_INPUT}"
        COMMENT "[PiPL] Preprocessing .r -> .rr"
        VERBATIM
    )

    # Step 2: Convert .rr → .rc
    add_custom_command(
        OUTPUT "${PIPL_RC_OUTPUT}"
        COMMAND "${PIPL_TOOL_EXE}" "${PIPL_RR_FILE}" "${PIPL_RC_OUTPUT}"
        DEPENDS "${PIPL_RR_FILE}"
        COMMENT "[PiPL] Generating .rc from .rr"
        VERBATIM
    )

    set(PIPL_RC_FILE "${PIPL_RC_OUTPUT}")
endif()

# ══════════════════════════════════════════════════════════════════════════════
# Regenerate MSVC-format import libs from FFmpeg DLLs
#
# Many pre-built "shared" FFmpeg distributions ship MinGW/dlltool-format .lib
# files. They link for normal imports but cannot be /DELAYLOAD'd by link.exe
# (LNK4199 "no imports found"). We work around that here by generating proper
# MSVC import libs once at configure time.
# ══════════════════════════════════════════════════════════════════════════════

if(WIN32)
    set(FFMPEG_DLLS_TO_FIX
        "avformat-62"
        "avcodec-62"
        "avutil-60"
        "swscale-9"
        "swresample-6"
    )

    set(FFMPEG_MSVC_LIB_DIR "${CMAKE_BINARY_DIR}/ffmpeg_msvc_libs")
    file(MAKE_DIRECTORY "${FFMPEG_MSVC_LIB_DIR}")

    # Find lib.exe and dumpbin.exe (ship with MSVC, on PATH inside the
    # Developer Command Prompt; CMake's find_program handles both cases)
    find_program(MSVC_LIB_TOOL     lib     REQUIRED)
    find_program(MSVC_DUMPBIN_TOOL dumpbin REQUIRED)

    set(GENERATED_FFMPEG_LIBS "")

    foreach(DLL_BASE ${FFMPEG_DLLS_TO_FIX})
        set(SRC_DLL "${FFMPEG_PATH}/bin/${DLL_BASE}.dll")
        set(OUT_DEF "${FFMPEG_MSVC_LIB_DIR}/${DLL_BASE}.def")
        set(OUT_LIB "${FFMPEG_MSVC_LIB_DIR}/${DLL_BASE}.lib")
        set(OUT_EXP "${FFMPEG_MSVC_LIB_DIR}/${DLL_BASE}.exp")

        if(NOT EXISTS "${SRC_DLL}")
            message(FATAL_ERROR
                "FFmpeg DLL not found: ${SRC_DLL}\n"
                "Make sure you've extracted the FFmpeg shared build to "
                "${FFMPEG_PATH}/bin/")
        endif()

        # Only regenerate if the .lib is missing OR older than its DLL
        if(NOT EXISTS "${OUT_LIB}" OR "${SRC_DLL}" IS_NEWER_THAN "${OUT_LIB}")
            message(STATUS "Generating MSVC import lib for ${DLL_BASE}.dll")

            # Step 1: dumpbin /exports to a temp file
            execute_process(
                COMMAND "${MSVC_DUMPBIN_TOOL}" /exports "${SRC_DLL}"
                OUTPUT_VARIABLE DUMP_OUTPUT
                RESULT_VARIABLE DUMP_RESULT
            )
            if(NOT DUMP_RESULT EQUAL 0)
                message(FATAL_ERROR "dumpbin failed on ${SRC_DLL}")
            endif()

            # Step 2: parse export names out of dumpbin output.
            # Lines look like:  "    1    0  00001234  av_function_name"
            # We want just the symbol in the last column.
            set(DEF_CONTENT "LIBRARY \"${DLL_BASE}.dll\"\nEXPORTS\n")
            string(REPLACE "\n" ";" DUMP_LINES "${DUMP_OUTPUT}")
            foreach(LINE ${DUMP_LINES})
                if(LINE MATCHES "^[ \t]+[0-9]+[ \t]+[0-9A-Fa-f]+[ \t]+[0-9A-Fa-f]+[ \t]+([A-Za-z_][A-Za-z0-9_@?$]*)")
                    string(APPEND DEF_CONTENT "    ${CMAKE_MATCH_1}\n")
                endif()
            endforeach()

            file(WRITE "${OUT_DEF}" "${DEF_CONTENT}")

            # Step 3: run lib /def: to produce the real .lib
            execute_process(
                COMMAND "${MSVC_LIB_TOOL}"
                        "/def:${OUT_DEF}"
                        "/out:${OUT_LIB}"
                        "/machine:x64"
                        "/nologo"
                RESULT_VARIABLE LIB_RESULT
                OUTPUT_QUIET
            )
            if(NOT LIB_RESULT EQUAL 0)
                message(FATAL_ERROR "lib.exe failed for ${DLL_BASE}")
            endif()
        endif()

        list(APPEND GENERATED_FFMPEG_LIBS "${OUT_LIB}")
    endforeach()

    message(STATUS "Generated MSVC FFmpeg import libs in: ${FFMPEG_MSVC_LIB_DIR}")
endif()


# ══════════════════════════════════════════════════════════════════════════════
# Build Target
# ══════════════════════════════════════════════════════════════════════════════

if(WIN32)
    add_library(AlphaFix SHARED
        ${SOURCES}
        ${HEADERS}
        ${AE_UTIL_SOURCES}
        ${PIPL_RC_FILE}
        src/AlphaFix.def
    )

    set_target_properties(AlphaFix PROPERTIES
        SUFFIX ".aex"
        PREFIX ""
    )

    # Linker options: .def file + delay-load FFmpeg DLLs
    target_link_options(AlphaFix PRIVATE
        "/DEF:${CMAKE_SOURCE_DIR}/src/AlphaFix.def"
        "/DELAYLOAD:avformat-62.dll"
        "/DELAYLOAD:avcodec-62.dll"
        "/DELAYLOAD:avutil-60.dll"
        "/DELAYLOAD:swresample-6.dll"
        "/DELAYLOAD:swscale-9.dll"
    )

    target_compile_definitions(AlphaFix PRIVATE
        _WINDOWS
        _WIN64
        WIN32
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
        MSWindows
    )

    target_link_libraries(AlphaFix PRIVATE
        ${GENERATED_FFMPEG_LIBS}
        delayimp.lib
    )

    if(MSVC)
        target_compile_options(AlphaFix PRIVATE
            /W3 /MP /Zi /EHsc
        )
    endif()

elseif(APPLE)
    add_library(AlphaFix MODULE
        ${SOURCES}
        ${HEADERS}
        ${AE_UTIL_SOURCES}
    )

    configure_file(cmake/Info.plist.in "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)
    set_target_properties(AlphaFix PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "plugin"
        PREFIX ""
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
        BUILD_WITH_INSTALL_RPATH YES
        INSTALL_RPATH "@loader_path/../Frameworks"
    )
    foreach(component avformat avcodec avutil swscale swresample)
        find_library(FFMPEG_${component} NAMES ${component}
            PATHS "${FFMPEG_PATH}/lib" NO_DEFAULT_PATH REQUIRED)
        target_link_libraries(AlphaFix PRIVATE "${FFMPEG_${component}}")
    endforeach()
    target_link_libraries(AlphaFix PRIVATE "-framework CoreFoundation")
    target_link_options(AlphaFix PRIVATE "-Wl,-headerpad_max_install_names")

    execute_process(COMMAND xcrun --find Rez OUTPUT_VARIABLE REZ_EXECUTABLE
        OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE REZ_RESULT)
    if(NOT REZ_RESULT EQUAL 0)
        message(FATAL_ERROR "Rez is required: install full Xcode and select it with xcode-select.")
    endif()
    set(REZ_INCLUDE_ARGS "")
    foreach(inc IN LISTS AE_SDK_INCLUDES)
        list(APPEND REZ_INCLUDE_ARGS -i "${inc}")
    endforeach()
    set(PIPL_RSRC "${CMAKE_CURRENT_BINARY_DIR}/AlphaFix.rsrc")
    add_custom_command(OUTPUT "${PIPL_RSRC}"
        COMMAND "${REZ_EXECUTABLE}" -useDF -d AE_OS_MAC=1 -d __LP64__=1
            -d __x86_64__=1 ${REZ_INCLUDE_ARGS}
            "${CMAKE_CURRENT_SOURCE_DIR}/src/AlphaFix_PiPL.r" -o "${PIPL_RSRC}"
        DEPENDS src/AlphaFix_PiPL.r VERBATIM)
    # Both Mac entry records are in the resource; the Rez processor define
    # only lets AEConfig.h preprocess successfully.
    add_custom_target(AlphaFixPiPL DEPENDS "${PIPL_RSRC}")
    add_dependencies(AlphaFix AlphaFixPiPL)
    target_sources(AlphaFix PRIVATE "${PIPL_RSRC}")
    set_source_files_properties("${PIPL_RSRC}" PROPERTIES
        GENERATED TRUE MACOSX_PACKAGE_LOCATION Resources)

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(ALPHAFIX_CODESIGN_IDENTITY "-" CACHE STRING "Mac signing identity (- means ad hoc)")
    add_custom_command(TARGET AlphaFix POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/bundle-mac.py"
            --bundle "$<TARGET_BUNDLE_DIR:AlphaFix>" --ffmpeg "${FFMPEG_PATH}"
            --identity "${ALPHAFIX_CODESIGN_IDENTITY}"
        VERBATIM)
else()
    message(FATAL_ERROR "AlphaFix supports Windows and macOS only.")
endif()

# ── Include directories ───────────────────────────────────────────────────────

target_include_directories(AlphaFix PRIVATE
    ${AE_SDK_INCLUDES}
    "${FFMPEG_PATH}/include"
    "${CMAKE_SOURCE_DIR}/src"
)


# ── User-configurable AE app path ─────────────────────────────────────────────

set(AE_BASE_DIR "" CACHE PATH "Override detected After Effects application directory")
if(NOT AE_BASE_DIR)
    if(WIN32)
        file(GLOB AE_CANDIDATES "C:/Program Files/Adobe/Adobe After Effects *")
        set(AE_PLUGIN_SUBDIR "Support Files/Plug-ins")
    else()
        file(GLOB AE_CANDIDATES "/Applications/Adobe After Effects *")
        set(AE_PLUGIN_SUBDIR "Plug-ins")
    endif()
    list(SORT AE_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
    foreach(candidate IN LISTS AE_CANDIDATES)
        if(NOT candidate MATCHES "[Bb]eta" AND IS_DIRECTORY "${candidate}/${AE_PLUGIN_SUBDIR}")
            set(AE_BASE_DIR "${candidate}")
            break()
        endif()
    endforeach()
endif()
if(WIN32)
    set(AE_PLUGIN_SUBDIR "Support Files/Plug-ins")
else()
    set(AE_PLUGIN_SUBDIR "Plug-ins")
endif()
# CI stages packages without an AE installation. Fail only if install is requested.
set(AE_PLUGIN_INSTALL_DIR "${AE_BASE_DIR}/${AE_PLUGIN_SUBDIR}/AlphaFix")
install(CODE "if(NOT IS_DIRECTORY \"${AE_BASE_DIR}/${AE_PLUGIN_SUBDIR}\")
    message(FATAL_ERROR \"After Effects not found. Set AE_BASE_DIR to its application folder.\")
endif()")

# ══════════════════════════════════════════════════════════════════════════════
# Runtime layout / install destinations
# ══════════════════════════════════════════════════════════════════════════════

if(WIN32)
    set(REQUIRED_FFMPEG_DLLS
        "${FFMPEG_PATH}/bin/avutil-60.dll"
        "${FFMPEG_PATH}/bin/avcodec-62.dll"
        "${FFMPEG_PATH}/bin/avformat-62.dll"
        "${FFMPEG_PATH}/bin/swresample-6.dll"
        "${FFMPEG_PATH}/bin/swscale-9.dll"
    )

    foreach(REQUIRED_DLL ${REQUIRED_FFMPEG_DLLS})
        if(NOT EXISTS "${REQUIRED_DLL}")
            message(FATAL_ERROR "Required FFmpeg DLL not found: ${REQUIRED_DLL}")
        endif()
    endforeach()

    set(STAGING_RELEASE_DIR
        "${CMAKE_SOURCE_DIR}/Release"
        CACHE PATH "Root-level staging folder for packaged plugin files"
    )

    set(STAGING_SUPPORT_FILES_DIR
        "${STAGING_RELEASE_DIR}/Support Files"
    )

    set(STAGING_PLUGIN_DIR
        "${STAGING_SUPPORT_FILES_DIR}/Plug-ins/AlphaFix"
    )

    set(STAGING_FFMPEG_DIR
        "${STAGING_PLUGIN_DIR}/alphafix.ffmpeg"
    )

    add_custom_command(TARGET AlphaFix POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${STAGING_SUPPORT_FILES_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${STAGING_PLUGIN_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${STAGING_FFMPEG_DIR}"

        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:AlphaFix>"
            "${STAGING_PLUGIN_DIR}/"

        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${REQUIRED_FFMPEG_DLLS}
            "${STAGING_FFMPEG_DIR}/"

        COMMENT "Staging AlphaFix.aex + FFmpeg DLLs into alphafix.ffmpeg subfolder"
        COMMAND_EXPAND_LISTS
    )

    add_custom_command(TARGET AlphaFix POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E tar "cf" "${CMAKE_SOURCE_DIR}/AlphaFix_v${PROJECT_VERSION}.zip" --format=zip "."
        WORKING_DIRECTORY "${STAGING_RELEASE_DIR}"
        COMMENT "Creating Release_v${PROJECT_VERSION}.zip"
    )


endif()

# ══════════════════════════════════════════════════════════════════════════════
# Install
# ══════════════════════════════════════════════════════════════════════════════

install(TARGETS AlphaFix
    LIBRARY DESTINATION "${AE_PLUGIN_INSTALL_DIR}"
    RUNTIME DESTINATION "${AE_PLUGIN_INSTALL_DIR}"
    BUNDLE  DESTINATION "${AE_PLUGIN_INSTALL_DIR}"
)

if(WIN32)
    set(AE_FFMPEG_INSTALL_DIR "${AE_PLUGIN_INSTALL_DIR}/alphafix.ffmpeg")
    install(FILES ${REQUIRED_FFMPEG_DLLS} DESTINATION "${AE_FFMPEG_INSTALL_DIR}")
endif()


# ══════════════════════════════════════════════════════════════════════════════
# Status
# ══════════════════════════════════════════════════════════════════════════════

message(STATUS "")
message(STATUS "=== AlphaFix Plugin Config ===")
message(STATUS "  AE SDK:     ${AE_SDK_PATH}")
message(STATUS "  FFmpeg:     ${FFMPEG_PATH}")
message(STATUS "  Install:    ${AE_PLUGIN_INSTALL_DIR}")
message(STATUS "  Platform:   ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Generator:  ${CMAKE_GENERATOR}")
if(PIPL_TOOL_EXE)
    message(STATUS "  PiPLtool:   ${PIPL_TOOL_EXE}")
endif()
message(STATUS "==============================")
message(STATUS "")