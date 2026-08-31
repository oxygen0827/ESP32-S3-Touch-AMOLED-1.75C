cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS components/ExternLib)
# Keep board credentials out of the project defaults. A developer may place
# a private sdkconfig.local beside this file; it is loaded only on that host.
# The generated config lives under the build directory so the checked-in
# sdkconfig remains credential-free and the small local overlay is not
# rewritten by Kconfig.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local")
    set(CLARE_PRIVATE_CONFIG TRUE)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig.private")
    # S3 port note: the stale C6-target `sdkconfig` is deliberately NOT part of
    # the defaults chain — feeding an esp32c6 generated config into an esp32s3
    # build silently poisons target-dependent symbols.
    set(SDKCONFIG_DEFAULTS
        "sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local")
else()
    set(CLARE_PRIVATE_CONFIG FALSE)
    set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Clare_S3)

if(CLARE_PRIVATE_CONFIG AND EXISTS "${SDKCONFIG}")
    file(CHMOD "${SDKCONFIG}" FILE_PERMISSIONS OWNER_READ OWNER_WRITE)
endif()
