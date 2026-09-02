# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Derive version from git tag (v0.1.0 -> 0.1.0), fallback to 0.0.0-dev
execute_process(
    COMMAND git describe --tags --abbrev=0
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_TAG_RESULT
)
if(GIT_TAG_RESULT EQUAL 0 AND GIT_TAG MATCHES "^v(.+)$")
    set(PROJECT_VER "${CMAKE_MATCH_1}")
else()
    set(PROJECT_VER "0.0.0-dev")
endif()

# ── Per-board sdkconfig ───────────────────────────────────────
# Common defaults plus an optional per-board overlay living in the board
# folder (main/hardware/boards/<board>/sdkconfig.defaults), letting a board
# change flash size / PSRAM / partitions without touching the shared
# defaults. sdkconfig fragments cannot include one another, so this list is
# the composition point: the board file overlays the common one and wins on
# conflicts. BOARD is the same cache var consumed by main/CMakeLists.txt.
if(NOT DEFINED BOARD)
    set(BOARD "esp32_devkit")
endif()
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_SOURCE_DIR}/main/hardware/boards/${BOARD}/sdkconfig.defaults")
    list(APPEND SDKCONFIG_DEFAULTS "main/hardware/boards/${BOARD}/sdkconfig.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(Strux)

# ── sdkconfig drift guard ────────────────────────────
# A generated sdkconfig is loaded AFTER the defaults and wins every conflict, and an
# option sitting at its default is still recorded there — as "# CONFIG_FOO is not set"
# — so adding CONFIG_FOO=y to sdkconfig.defaults changes NOTHING in a tree that already
# has a sdkconfig. It fails silently: the build succeeds and runs the old configuration,
# which is how a config change can look landed and do nothing. The CONFIG_IDF_TARGET
# case in main/CMakeLists.txt is this same mechanism, and the reason a wrong-toolchain
# build is possible at all.
#
# So every assertion in the composed defaults is checked against what was actually
# generated, and a mismatch is a build error naming the options that did not take. The
# fix it prints is the only fix: delete the generated file — it is gitignored and
# reproducible — and let set-target write a new one.
#
# For a deliberate local override of something the defaults assert:
#   idf.py -DSTRUX_ALLOW_SDKCONFIG_DRIFT=ON build
#
# Only lines of the form CONFIG_X=... are checked, which is every line either defaults
# file uses. A default whose *value* contained a semicolon would be misread, because
# file(STRINGS) hands back a CMake list; none do, and if one ever does it is this loop
# that needs fixing, not the value.
if(NOT STRUX_ALLOW_SDKCONFIG_DRIFT)
    idf_build_get_property(_strux_sdkconfig SDKCONFIG)
    file(READ "${_strux_sdkconfig}" _strux_actual)
    string(REPLACE "\r" "" _strux_actual "${_strux_actual}")  # generated file is CRLF
    set(_strux_actual "\n${_strux_actual}\n")              # so every line has both

    set(_strux_drift "")
    foreach(_strux_file ${SDKCONFIG_DEFAULTS})
        # project.cmake has already made these absolute; BASE_DIR keeps this working
        # either way, since the list is composed with relative paths above.
        get_filename_component(_strux_path "${_strux_file}" ABSOLUTE
                               BASE_DIR "${CMAKE_SOURCE_DIR}")
        file(RELATIVE_PATH _strux_shown "${CMAKE_SOURCE_DIR}" "${_strux_path}")
        file(STRINGS "${_strux_path}" _strux_lines)
        foreach(_strux_line ${_strux_lines})
            if(NOT _strux_line MATCHES "^(CONFIG_[A-Za-z0-9_]+)=")
                continue()
            endif()
            set(_strux_key "${CMAKE_MATCH_1}")
            string(FIND "${_strux_actual}" "\n${_strux_line}\n" _strux_pos)
            if(NOT _strux_pos EQUAL -1)
                continue()
            endif()
            # Say what it is instead, so the message is diagnosable without a diff.
            if("${_strux_actual}" MATCHES "\n${_strux_key}=([^\n]*)\n")
                set(_strux_is "it is ${_strux_key}=${CMAKE_MATCH_1}")
            elseif("${_strux_actual}" MATCHES "\n# ${_strux_key} is not set\n")
                set(_strux_is "it is not set")
            else()
                set(_strux_is "it is absent entirely — its dependencies are unmet")
            endif()
            list(APPEND _strux_drift
                 "  ${_strux_shown} asks for ${_strux_line}, but ${_strux_is}")
        endforeach()
    endforeach()

    if(_strux_drift)
        string(REPLACE ";" "\n" _strux_drift "${_strux_drift}")
        message(FATAL_ERROR
            "${_strux_sdkconfig} disagrees with the defaults it was generated from:\n"
            "${_strux_drift}\n\n"
            "A generated sdkconfig wins over sdkconfig.defaults, so those lines are not "
            "in effect and this build would silently use the old configuration. Delete "
            "the generated file and let set-target rewrite it:\n"
            "  rm ${_strux_sdkconfig}\n"
            "  idf.py -DBOARD=${BOARD} set-target <chip>\n\n"
            "If a difference is a deliberate local override, build with "
            "-DSTRUX_ALLOW_SDKCONFIG_DRIFT=ON.")
    endif()
endif()

# ── Frontend build ────────────────────────────────────────────
# Automatically build the React frontend before generating the FAT image.
# If pnpm is not installed, creates a placeholder so the build still succeeds.

find_program(PNPM pnpm)
if(PNPM)
    # --no-frozen-lockfile lets package.json edits auto-update the lockfile
    # (CMake-driven builds shouldn't fail just because a dep was added).
    # --config.confirm-modules-purge=false skips the interactive prompt that
    # appears when node_modules is out of sync.
    add_custom_target(frontend_build
        COMMAND ${PNPM} install --no-frozen-lockfile --config.confirm-modules-purge=false
        COMMAND ${PNPM} build
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/frontend
        COMMENT "Building frontend (pnpm install && pnpm build)..."
    )
endif()

# Ensure www/ exists (empty dir is fine — FAT generation just needs a valid directory)
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/www)
    file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/www)
endif()
file(GLOB WWW_CONTENTS ${CMAKE_SOURCE_DIR}/www/*)
if(NOT PNPM AND NOT WWW_CONTENTS)
    file(WRITE ${CMAKE_SOURCE_DIR}/www/index.html
        "<!DOCTYPE html><html><body><h1>Frontend not built</h1>"
        "<p>Install <a href=\"https://pnpm.io\">pnpm</a> and rebuild, "
        "or run: <code>cd frontend && pnpm install && pnpm build</code></p>"
        "</body></html>\n")
    message(STATUS "pnpm not found — frontend will not be built automatically")
endif()

fatfs_create_spiflash_image(www www FLASH_IN_PROJECT)

if(PNPM)
    add_dependencies(fatfs_www_bin frontend_build)
endif()
