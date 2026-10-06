set(SPOUT_SOURCES
    Spout.cpp
    SpoutCopy.cpp
    SpoutDirectX.cpp
    SpoutFrameCount.cpp
    SpoutGL.cpp
    SpoutGLextensions.cpp
    SpoutReceiver.cpp
    SpoutSender.cpp
    SpoutSenderNames.cpp
    SpoutSharedMemory.cpp
    SpoutUtils.cpp
)

add_library(Spout2Static STATIC ${SPOUT_SOURCES})
target_include_directories(Spout2Static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(Spout2Static PRIVATE
    SPOUT_BUILD_STATIC
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)
target_link_libraries(Spout2Static PUBLIC
    opengl32 kernel32 user32 gdi32 winspool comdlg32 comctl32 advapi32
    shell32 ole32 oleaut32 uuid odbc32 odbccp32 d3d9 d3d11 dxgi version winmm
)

if(MSVC)
    target_compile_options(Spout2Static PRIVATE /W0 /utf-8)
endif()

# SpoutCopy uses _mm_shuffle_epi8 for its RGB conversion fast path. MSVC
# accepts that intrinsic on x64, while Clang requires the SSSE3 feature to be
# enabled explicitly.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(Spout2Static PRIVATE -mssse3)
endif()
