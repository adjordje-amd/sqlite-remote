cmake_minimum_required(VERSION 3.15...3.28)
project(sqlite-remote
    VERSION 0.1.0
    DESCRIPTION "SQLite virtual table for remote query execution via SSH"
    LANGUAGES C)

find_package(SQLite3 REQUIRED)

add_library(remote MODULE src/remote_vtable.c)
target_link_libraries(remote PRIVATE SQLite::SQLite3)
set_target_properties(remote PROPERTIES PREFIX "")

add_executable(example example/main.c)
target_link_libraries(example PRIVATE SQLite::SQLite3)
