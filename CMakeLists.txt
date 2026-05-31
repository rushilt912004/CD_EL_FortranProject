cmake_minimum_required(VERSION 3.20)
project(CommonAnalyzer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─────────────────────────────────────────────────────────────
#  Option: build with full Flang backend (default) or
#          lightweight regex backend (no LLVM required)
# ─────────────────────────────────────────────────────────────
option(USE_REGEX_BACKEND
    "Build without Flang (regex-based parser)"
    OFF)

# ─────────────────────────────────────────────────────────────
#  Full Flang backend
# ─────────────────────────────────────────────────────────────
if(NOT USE_REGEX_BACKEND)
    # Require an LLVM installation that includes Flang
    find_package(LLVM REQUIRED CONFIG)
    find_package(Flang REQUIRED CONFIG)

    message(STATUS "LLVM version: ${LLVM_PACKAGE_VERSION}")
    message(STATUS "Flang found at: ${Flang_DIR}")

    include_directories(
        ${LLVM_INCLUDE_DIRS}
        ${FLANG_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/include
    )

    add_definitions(${LLVM_DEFINITIONS})

    add_executable(common-analyzer
        src/main.cpp
    )

    # Link against Flang's parser, semantics, and support libs
    target_link_libraries(common-analyzer
        PRIVATE
        FortranParser
        FortranSemantics
        FortranEvaluate
        FortranRuntime
        LLVMSupport
        LLVMCore
    )

else()
    # ── Lightweight regex backend ─────────────────────────────
    message(STATUS "Building with regex backend (no Flang required)")

    include_directories(${CMAKE_SOURCE_DIR}/include)

    add_executable(common-analyzer
        src/main.cpp
    )

    target_compile_definitions(common-analyzer
        PRIVATE USE_REGEX_BACKEND)
endif()

# ─────────────────────────────────────────────────────────────
#  Install
# ─────────────────────────────────────────────────────────────
install(TARGETS common-analyzer DESTINATION bin)

# ─────────────────────────────────────────────────────────────
#  Test target (runs the test suite via CTest)
# ─────────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)
