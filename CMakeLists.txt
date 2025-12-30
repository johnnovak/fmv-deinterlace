cmake_minimum_required(VERSION 3.22)

project(fmv-deinterlace LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)

add_executable(deinterlace
  src/deinterlace.cpp
)

