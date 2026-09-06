set(NPCAP_ROOT "" CACHE PATH "Path to the unpacked Npcap SDK")

find_path(NPCAP_INCLUDE_DIR
    NAMES pcap.h
    HINTS "${NPCAP_ROOT}"
    PATH_SUFFIXES Include include)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_npcap_library_suffixes Lib/x64 lib/x64 Lib lib)
else()
    set(_npcap_library_suffixes Lib lib)
endif()

find_library(NPCAP_WPCAP_LIBRARY
    NAMES wpcap
    HINTS "${NPCAP_ROOT}"
    PATH_SUFFIXES ${_npcap_library_suffixes})
find_library(NPCAP_PACKET_LIBRARY
    NAMES Packet
    HINTS "${NPCAP_ROOT}"
    PATH_SUFFIXES ${_npcap_library_suffixes})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Npcap
    REQUIRED_VARS NPCAP_INCLUDE_DIR NPCAP_WPCAP_LIBRARY NPCAP_PACKET_LIBRARY)

if(Npcap_FOUND AND NOT TARGET Npcap::Npcap)
    add_library(Npcap::Npcap INTERFACE IMPORTED)
    set_target_properties(Npcap::Npcap PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${NPCAP_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${NPCAP_WPCAP_LIBRARY};${NPCAP_PACKET_LIBRARY}")
endif()

mark_as_advanced(NPCAP_INCLUDE_DIR NPCAP_WPCAP_LIBRARY NPCAP_PACKET_LIBRARY)
