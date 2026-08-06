add_executable(bmoe-cli main.cpp)
target_link_libraries(bmoe-cli PRIVATE bmoe_core)

add_executable(bmoe-server server_main.cpp)
target_link_libraries(bmoe-server PRIVATE bmoe_core)
