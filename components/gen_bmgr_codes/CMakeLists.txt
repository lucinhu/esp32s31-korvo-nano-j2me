# Board information output
message(STATUS "Selected Board: esp32_s31_korvo1")
message(STATUS "Board Path: ${CMAKE_CURRENT_LIST_DIR}/../esp32_s31_korvo1")

idf_component_register(
    SRC_DIRS "." "../esp32_s31_korvo1"
    INCLUDE_DIRS "." "../esp32_s31_korvo1"
    REQUIRES esp_board_manager
)

# This is equivalent to adding WHOLE_ARCHIVE option to the idf_component_register call above:
idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
