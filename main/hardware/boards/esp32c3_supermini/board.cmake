# ──────────────────────────────────────────────────────────────
# Board fragment: ESP32-C3 SuperMini
#   ESP32-C3FH4/FN4 (RISC-V, 4 MB flash, no PSRAM) · USB-C on the chip's
#   native USB Serial/JTAG · blue LED on GPIO8, active LOW
#
# A board fragment may append to BOARD_SOURCES (extra .cpp files under this
# folder that need compiling). Component deps are NOT set here — see the note
# in main/CMakeLists.txt: managed deps go in main/idf_component.yml, IDF
# built-ins in COMPONENT_REQUIRES.
# ──────────────────────────────────────────────────────────────

list(APPEND BOARD_SOURCES "${CMAKE_CURRENT_LIST_DIR}/BoardContext.cpp")
