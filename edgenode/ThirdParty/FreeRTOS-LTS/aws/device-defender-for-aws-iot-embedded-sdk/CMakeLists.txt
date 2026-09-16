cmake_minimum_required(VERSION 3.15)

project(DeviceDefender LANGUAGES C)

# ------------------------------------------------------------------------------
# Includes
# ------------------------------------------------------------------------------

include(${CMAKE_CURRENT_LIST_DIR}/defenderFilePaths.cmake)

# ------------------------------------------------------------------------------
# Library targets
# ------------------------------------------------------------------------------

add_library( aws_device_defender )


target_sources( aws_device_defender PUBLIC ${DEFENDER_SOURCES} )

target_include_directories( aws_device_defender PUBLIC ${DEFENDER_INCLUDE_PUBLIC_DIRS} )

if(STANDALONE_TEST_BUILD_UNIX)
    target_compile_definitions( aws_device_defender PUBLIC DEFENDER_DO_NOT_USE_CUSTOM_CONFIG=1 )
endif()
