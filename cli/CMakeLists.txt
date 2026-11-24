add_executable(bmoe-cli main.cpp)
target_link_libraries(bmoe-cli PRIVATE bmoe_core bmoe_frontend)
bmoe_utf8_process(bmoe-cli)
