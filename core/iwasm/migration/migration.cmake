set (MIGRATION_DIR ${CMAKE_CURRENT_LIST_DIR})

add_definitions (-DWASM_ENABLE_MIGRATION=1)

include_directories(${MIGRATION_DIR})

file (GLOB source_all ${MIGRATION_DIR}/*.c)

set (MIGRATION_SOURCE ${source_all})
