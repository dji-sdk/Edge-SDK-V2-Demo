# FindMosquitto.cmake
# -------------------
# Locate the mosquitto C++ binding (libmosquittopp).
#
# This module defines:
#   Mosquitto_FOUND          - True if libmosquittopp was found
#   Mosquitto_INCLUDE_DIRS   - Include directories for mosquittopp.h
#   Mosquitto_LIBRARIES      - Libraries to link against
#
# It also provides an imported target:
#   Mosquitto::Mosquitto

find_path(Mosquitto_INCLUDE_DIRS
    NAMES mosquittopp.h
    HINTS /usr/include /usr/local/include
)

find_library(Mosquitto_LIBRARIES
    NAMES mosquittopp
    HINTS /usr/lib /usr/local/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Mosquitto
    REQUIRED_VARS Mosquitto_LIBRARIES Mosquitto_INCLUDE_DIRS
)

if(Mosquitto_FOUND AND NOT TARGET Mosquitto::Mosquitto)
    add_library(Mosquitto::Mosquitto UNKNOWN IMPORTED)
    set_target_properties(Mosquitto::Mosquitto PROPERTIES
        IMPORTED_LOCATION "${Mosquitto_LIBRARIES}"
        INTERFACE_INCLUDE_DIRECTORIES "${Mosquitto_INCLUDE_DIRS}"
    )
endif()

mark_as_advanced(Mosquitto_INCLUDE_DIRS Mosquitto_LIBRARIES)
