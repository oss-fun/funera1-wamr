set (MIGRATION_DIR ${CMAKE_CURRENT_LIST_DIR})

add_definitions (-DWASM_ENABLE_MIGRATION=1)

include_directories(${MIGRATION_DIR})

include(FetchContent)
FetchContent_Declare(
    wasmig
    GIT_REPOSITORY https://github.com/funera1/wasmig.git
    GIT_TAG HEAD
    # GIT_TAG cce6121b09b5def323102b2b36142cec677c1638
)
FetchContent_GetProperties(wasmig)
if (NOT wasmig_POPULATED)
    message ("-- Fetching wasmig ..")
    FetchContent_Populate(wasmig)
    include_directories("${wasmig_SOURCE_DIR}/include")
    add_subdirectory(${wasmig_SOURCE_DIR})
    file (GLOB_RECURSE c_source_wasmig ${wasmig_SOURCE_DIR}/src/*.c)
endif()

file (GLOB source_all ${MIGRATION_DIR}/*.c)

set (MIGRATION_SOURCE ${source_all})