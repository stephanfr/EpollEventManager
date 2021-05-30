
# The following is needed when running tests to allow the heap manager to decode function names
SET(CMAKE_EXE_LINKER_FLAGS  "${CMAKE_EXE_LINKER_FLAGS} -rdynamic")

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/609281088cfefc76f9d0ce82e1ff6c30cc3591e5.zip
)

FetchContent_MakeAvailable(googletest)

FetchContent_Declare(
    formatlib
    GIT_REPOSITORY "https://github.com/fmtlib/fmt.git"
    GIT_TAG "7.1.3")

FetchContent_MakeAvailable(formatlib)

FetchContent_Declare(
  cppresult
  GIT_REPOSITORY "https://github.com/stephanfr/CppResult.git" )

FetchContent_MakeAvailable(cppresult)

FetchContent_Declare(
  heapwatcher
  GIT_REPOSITORY "https://github.com/stephanfr/HeapWatcher.git" )

FetchContent_MakeAvailable(heapwatcher)

include_directories(
    ${formatlib_SOURCE_DIR}/include
    ${formatlib_BIN_DIR}
    ${cppresult_SOURCE_DIR}/include
    ${heapwatcher_SOURCE_DIR}/include
    ${heapwatcher_BIN_DIR}
  )


  include(GoogleTest)


find_package(Catch2 REQUIRED)
add_executable(tests BasicTests.cpp)
target_include_directories( tests PRIVATE ../include )
target_link_libraries(tests PRIVATE Catch2::Catch2WithMain gtest HeapWatcher )

include(CTest)
include(Catch)
catch_discover_tests(tests)
