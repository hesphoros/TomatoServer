cmake_minimum_required(VERSION 3.21)
project(TomatoServer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(Drogon CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)

add_executable(tomato_server
    src/main.cpp
    src/bootstrap/AppConfig.cpp
    src/storage/sqlite/Migrator.cpp
    src/http/ApiServer.cpp
)

target_include_directories(tomato_server
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(tomato_server
    PRIVATE
        Drogon::Drogon
        unofficial::sqlite3::sqlite3
        unofficial-sodium::sodium
)

if(MSVC)
    target_compile_definitions(tomato_server PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
