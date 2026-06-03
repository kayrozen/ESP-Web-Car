cmake_minimum_required(VERSION 3.16)

# ESP-ADF must be set: export ADF_PATH=/path/to/esp-adf
# ESP-IDF must come from ADF: export IDF_PATH=$ADF_PATH/esp-idf
include($ENV{ADF_PATH}/CMakeLists.txt)

project(adf_prototype)
