add_library(gst-video-context)
add_library(gst-video::context ALIAS gst-video-context)

set(SRC_LIST)

find_package(PkgConfig)
find_package(CUDAToolkit)
if (CUDAToolkit_FOUND)
    message(STATUS "CUDA toolkit found")
    target_link_libraries(gst-video-context PUBLIC CUDA::toolkit)
    pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-cuda-1.0)
    list(APPEND SRC_LIST gst-video-context/gst-video-context.cpp)
else ()
    message(STATUS "CUDA toolkit not found, building without CUDA support")
    pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0)
    list(APPEND SRC_LIST gst-video-context/no-cuda-context.cpp)
endif ()

target_include_directories(gst-video-context PUBLIC gst-video-context)
target_sources(gst-video-context PUBLIC gst-video-context/gst-video-context.hpp PRIVATE ${SRC_LIST})

target_link_libraries(gst-video-context PUBLIC
        PkgConfig::GSTREAMER
        wolf::core)

target_compile_features(gst-video-context PUBLIC cxx_std_17)
set_target_properties(gst-video-context PROPERTIES LINKER_LANGUAGE CXX)