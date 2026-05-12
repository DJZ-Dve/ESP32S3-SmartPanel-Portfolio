cmake_minimum_required(VERSION 3.16.0)

set(PROJECT_VER "1.3.0")
set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/src")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s3_myfirstproject_idf)
