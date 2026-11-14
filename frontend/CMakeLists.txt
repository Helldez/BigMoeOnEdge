# What every front-end does the same way: where a person's data goes (user_dirs.h), so a model's
# measurements kept by one are read by the others, and what the compute backends are told before they
# load (backend_env.h), so every front-end starts them alike. bmoe-cli, bmoe-server and the apps link it.
add_library(bmoe_frontend STATIC user_dirs.cpp backend_env.cpp)
target_include_directories(bmoe_frontend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(bmoe_frontend PUBLIC cxx_std_17)
if(WIN32)
    target_link_libraries(bmoe_frontend PUBLIC shell32 ole32)
endif()
