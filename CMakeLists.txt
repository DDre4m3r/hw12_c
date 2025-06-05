cmake_minimum_required(VERSION 3.10)
project(hw12 C)

set(CMAKE_C_STANDARD 11)

include_directories(include)

add_executable(http_server
        src/main.c
        src/parse_args.c
        src/server.c
)
