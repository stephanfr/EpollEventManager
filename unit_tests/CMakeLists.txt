include(GoogleTest)

find_package(Catch2 REQUIRED)
add_executable(tests BasicTests.cpp)
target_include_directories( tests PRIVATE ../include )
target_link_libraries(tests PRIVATE Catch2::Catch2WithMain gtest )

include(CTest)
include(Catch)
catch_discover_tests(tests)
