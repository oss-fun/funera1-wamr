set (MIGRATION_DIR ${CMAKE_CURRENT_LIST_DIR})

add_definitions (-DWASM_ENABLE_MIGRATION=1)

include_directories(${MIGRATION_DIR})

include(FetchContent)
FetchContent_Declare(
    wasmig
    GIT_REPOSITORY https://github.com/funera1/wasmig.git
    GIT_TAG main
    GIT_SHALLOW TRUE
    # GIT_TAG cce6121b09b5def323102b2b36142cec677c1638
)
FetchContent_GetProperties(wasmig)
if (NOT wasmig_POPULATED)
    message ("-- Fetching wasmig ..")
    FetchContent_Populate(wasmig)
endif()

include_directories("${wasmig_SOURCE_DIR}/include")
add_subdirectory(${wasmig_SOURCE_DIR} ${wasmig_BINARY_DIR})
file (GLOB_RECURSE c_source_wasmig ${wasmig_SOURCE_DIR}/src/*.c)

set (WASMIG_SUBBUILD_DIR "${FETCHCONTENT_BASE_DIR}/wasmig-subbuild")
set (WASMIG_UPDATES_DISCONNECTED OFF)

if (DEFINED FETCHCONTENT_UPDATES_DISCONNECTED AND FETCHCONTENT_UPDATES_DISCONNECTED)
    set (WASMIG_UPDATES_DISCONNECTED ON)
endif ()

if (DEFINED FETCHCONTENT_UPDATES_DISCONNECTED_WASMIG AND FETCHCONTENT_UPDATES_DISCONNECTED_WASMIG)
    set (WASMIG_UPDATES_DISCONNECTED ON)
endif ()

if (EXISTS "${WASMIG_SUBBUILD_DIR}/CMakeLists.txt")
    if (NOT WASMIG_UPDATES_DISCONNECTED)
        message ("-- Updating wasmig ..")
        execute_process(
            COMMAND ${CMAKE_COMMAND} --build "${WASMIG_SUBBUILD_DIR}" --target wasmig-populate
            RESULT_VARIABLE WASMIG_SYNC_RESULT
            OUTPUT_VARIABLE WASMIG_SYNC_OUTPUT
            ERROR_VARIABLE WASMIG_SYNC_OUTPUT
        )

        if (NOT WASMIG_SYNC_RESULT EQUAL 0)
            message (FATAL_ERROR "Failed to update wasmig:\n${WASMIG_SYNC_OUTPUT}")
        endif ()
    endif ()

    add_custom_target(
        wasmig_sync
        COMMAND ${CMAKE_COMMAND} --build "${WASMIG_SUBBUILD_DIR}" --target wasmig-populate
        COMMENT "Updating wasmig from origin/main"
        VERBATIM
    )

    if (TARGET libwasmig)
        add_dependencies(libwasmig wasmig_sync)
    endif ()
endif ()

file (GLOB source_all ${MIGRATION_DIR}/*.c)

set (MIGRATION_SOURCE ${source_all})
