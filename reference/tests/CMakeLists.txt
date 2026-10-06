cmake_minimum_required(VERSION 3.21)
project(CleanFeedRegressionTests LANGUAGES CXX)

enable_testing()
add_executable(overlay_settings_tests
    overlay_settings_tests.cpp
    ../src/settings.cpp
    ../src/keybinds.cpp
)
target_compile_features(overlay_settings_tests PRIVATE cxx_std_23)
target_include_directories(overlay_settings_tests PRIVATE stubs ../src)
add_test(NAME overlay_settings COMMAND overlay_settings_tests)

add_executable(ui_visibility_tests ui_visibility_tests.cpp)
target_compile_features(ui_visibility_tests PRIVATE cxx_std_23)
target_include_directories(ui_visibility_tests PRIVATE ../src)
add_test(NAME ui_visibility COMMAND ui_visibility_tests)

add_executable(trajectory_visibility_tests trajectory_visibility_tests.cpp)
target_compile_features(trajectory_visibility_tests PRIVATE cxx_std_23)
target_include_directories(trajectory_visibility_tests PRIVATE ../src)
add_test(NAME trajectory_visibility COMMAND trajectory_visibility_tests)
