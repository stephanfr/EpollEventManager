cmake_minimum_required(VERSION 3.14.0)
project(EpollEventManager VERSION 0.1.0)

# specify the C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
#set(CMAKE_CXX_CLANG_TIDY "clang-tidy;-checks=*,-fuchsia-default-arguments")

#add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

include(FetchContent)

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
  spdlog
  GIT_REPOSITORY "https://github.com/gabime/spdlog.git"
  GIT_TAG        v1.8.2)

FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(
  concurrentqueue
  GIT_REPOSITORY "https://github.com/cameron314/concurrentqueue.git"
  GIT_TAG        v1.0.3 )

FetchContent_MakeAvailable(concurrentqueue)

FetchContent_Declare(
  readerwriterqueue
  GIT_REPOSITORY "https://github.com/cameron314/readerwriterqueue.git"
  GIT_TAG        v1.0.3 )

FetchContent_MakeAvailable(readerwriterqueue)

FetchContent_Declare(
  cppresult
  GIT_REPOSITORY "https://github.com/stephanfr/CppResult.git" )

FetchContent_MakeAvailable(cppresult)


include_directories(
  ${formatlib_SOURCE_DIR}/include
  ${formatlib_BIN_DIR}
  ${spdlog_SOURCE_DIR}/include
  ${spdlog_BIN_DIR}
  ${cppresult_SOURCE_DIR}/include
  ${concurrentqueue_SOURCE_DIR}
  ${readerwriterqueue_SOURCE_DIR}
  )

enable_testing()

include_directories(include)

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)

add_subdirectory("unit_tests")

