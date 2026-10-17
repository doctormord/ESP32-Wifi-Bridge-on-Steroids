# Project CMakeLists for the pure ESP-IDF build.
# Expected by PlatformIO with framework = espidf.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_wifi_bridge)

# Enable NBSTAT answers in lwIP's NetBIOS responder.
#
# Without this the bridge only answers queries for its exact known name - but
# a router cannot know that name before it has asked for it. NBSTAT ("node
# status") is exactly the query used to DISCOVER names; that is how many home
# routers learn the names of Windows machines.
#
# lwIP defaults LWIP_NETBIOS_RESPOND_NAME_QUERY to 0, and ESP-IDF offers no
# Kconfig option for it. So it is defined directly on the lwip component -
# the way ESP-IDF intends third-party components to be parameterized without
# patching them.
idf_component_get_property(lwip_lib lwip COMPONENT_LIB)
target_compile_definitions(${lwip_lib} PRIVATE LWIP_NETBIOS_RESPOND_NAME_QUERY=1)
