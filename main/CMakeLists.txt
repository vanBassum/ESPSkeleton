# ── The substrate every layer stands on: RTOS wrappers, streams, JSON, the
# request/reply seam, date and time. NOT part of the framework — the board layer uses
# InitState and the application uses Timer, and both would be reaching inside strux/ to
# get them if this lived there. Nothing in here knows about a layer; that is the test
# for whether a thing belongs.
set(LIB_SOURCES
    "lib/system/DateTime.cpp"
    "lib/system/TimeSpan.cpp"
)

# ── The framework. Pulled from the Strux template; a fork should not need to touch
# this list, which is why it is kept apart from the application's.
set(STRUX_SOURCES
    "strux/NetworkManager/NetworkManager.cpp"
    "strux/NetworkManager/NetworkInterface.cpp"
    "strux/NetworkManager/WiFiInterface.cpp"
    "strux/NetworkManager/DnsConfiguration.cpp"
    "strux/WebServerManager/WebServerManager.cpp"
    "strux/WebServerManager/Authenticator.cpp"
    "strux/WebServerManager/StaticFileHandler.cpp"
    "strux/WebServerManager/WebSocketHandler.cpp"
    "strux/WebServerManager/ConnectionRegistry.cpp"
    "strux/WebServerManager/SessionTable.cpp"
    "strux/RelayManager/RelayManager.cpp"
    "strux/RelayManager/RelaySocket.cpp"
    "strux/TelemetryManager/TelemetryManager.cpp"
    "strux/CommandManager/CommandManager.cpp"
    "strux/UpdateManager/UpdateManager.cpp"
    "strux/UpdateManager/PartitionWriter.cpp"
    "strux/SettingsManager/SettingsManager.cpp"
    "strux/SettingsManager/Setting.cpp"
    "strux/SystemManager/SystemManager.cpp"
    "strux/ConsoleManager/ConsoleManager.cpp"
    "strux/TimeManager/TimeManager.cpp"
)

# ── The application. This is the list a fork owns.
set(APP_SOURCES
    "app/LedManager/LedManager.cpp"
)

set(SOURCE_FILES_LIST
    "main.cpp"
    ${LIB_SOURCES}
    ${STRUX_SOURCES}
    ${APP_SOURCES}
)

# Every folder is on the include path, so headers are included by name alone
# (`#include "Stream.h"`) and moving one between layers does not touch its callers.
set(INCLUDE_DIRS_LIST
    .
    "lib"
    "lib/common"
    "lib/rtos"
    "lib/json"
    "lib/protocol"
    "lib/system"
    "strux"
    "strux/NetworkManager"
    "strux/WebServerManager"
    "strux/RelayManager"
    "strux/TelemetryManager"
    "strux/CommandManager"
    "strux/UpdateManager"
    "strux/SettingsManager"
    "strux/SystemManager"
    "strux/ConsoleManager"
    "strux/TimeManager"
    "hardware"
    "app"
    "app/LedManager"
)

set(COMPONENT_REQUIRES
    esp_timer
    esp_netif
    esp_event
    esp_wifi
    nvs_flash
    esp_http_server
    esp_http_client
    esp_app_format
    app_update
    esp_partition
    spi_flash
    fatfs
    wear_levelling
    esp_driver_gpio
    mdns
    # The relay drives a WebSocket at the transport layer instead of through
    # esp_websocket_client — see RelaySocket.h for why. esp-tls comes with it for
    # the certificate bundle a wss:// pipe verifies against.
    tcp_transport
    esp-tls
)

# ── Board selection ────────────────────────────────────────────
# Pick the target board with -DBOARD=<name>; defaults to the generic ESP32
# DevKit. Each board lives in main/hardware/boards/<name>/ with its own
# BoardConfig.h and a board.cmake fragment that may append extra files to
# BOARD_SOURCES. Shared, board-independent drivers live in hardware/drivers/.
set(BOARD "esp32_devkit" CACHE STRING "Target board (folder under main/hardware/boards/)")

set(BOARD_DIR "${CMAKE_CURRENT_LIST_DIR}/hardware/boards/${BOARD}")
if(NOT EXISTS "${BOARD_DIR}/board.cmake")
    file(GLOB _available_boards RELATIVE "${CMAKE_CURRENT_LIST_DIR}/hardware/boards"
         "${CMAKE_CURRENT_LIST_DIR}/hardware/boards/*")
    message(FATAL_ERROR "Unknown BOARD '${BOARD}'. Available boards: ${_available_boards}")
endif()

set(BOARD_SOURCES "")
include("${BOARD_DIR}/board.cmake")

# NOTE on component REQUIRES: ESP-IDF resolves a component's REQUIRES in a
# separate "expansion" pass that runs this CMakeLists in script mode WITHOUT
# the -DBOARD cache var, so board.cmake can't switch REQUIRES per board (it
# would always see the default board). Component deps therefore go through the
# two channels that DO work regardless of board:
#   • managed deps -> main/idf_component.yml
#   • IDF built-ins -> COMPONENT_REQUIRES above
# board.cmake is limited to BOARD_SOURCES, which is evaluated in the normal
# configure pass where BOARD is visible.

# The selected board folder is the ONLY one on the include path, so headers
# included by name (e.g. BoardConfig.h) resolve to it.
list(APPEND INCLUDE_DIRS_LIST "hardware/boards/${BOARD}")
list(APPEND SOURCE_FILES_LIST ${BOARD_SOURCES})
message(STATUS "Building for BOARD=${BOARD}")

# Register component with ESP-IDF
idf_component_register(
    SRCS ${SOURCE_FILES_LIST}
    INCLUDE_DIRS ${INCLUDE_DIRS_LIST}
    REQUIRES ${COMPONENT_REQUIRES}
)

# Board identity define for the (rare) code-level conditionals.
string(TOUPPER "${BOARD}" BOARD_UPPER)
target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_${BOARD_UPPER})

# Switches over enums (e.g. SettingType) deliberately have no default case
# so that adding an enum value flags every converter that wasn't updated.
# That flag is only a warning by default — make it the error it's meant to be.
target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=switch)
