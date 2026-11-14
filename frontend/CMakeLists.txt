# What every front-end asks the operating system the same way (user_dirs.h): bmoe-cli, bmoe-server and
# the desktop app link it, so a model's measurements kept by one are read by the others.
add_library(bmoe_user_dirs STATIC user_dirs.cpp)
target_include_directories(bmoe_user_dirs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(bmoe_user_dirs PUBLIC cxx_std_17)
if(WIN32)
    target_link_libraries(bmoe_user_dirs PUBLIC shell32 ole32)
endif()
