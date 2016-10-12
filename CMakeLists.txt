cmake_minimum_required(VERSION 3.6)

project(midi-parser C)

include_directories(include)
add_library(midi-parser src/midi-parser.c)
set_property(TARGET midi-parser PROPERTY C_STANDARD 99)

add_executable(midi-dump src/midi-dump.c)
set_property(TARGET midi-dump PROPERTY C_STANDARD 99)
target_link_libraries(midi-dump midi-parser)
