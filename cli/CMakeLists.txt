add_executable(bmoe-cli main.cpp)
target_link_libraries(bmoe-cli PRIVATE bmoe_core)
set_target_properties(bmoe-cli PROPERTIES OUTPUT_NAME "bmoe-cli")
