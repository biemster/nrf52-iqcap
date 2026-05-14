find_package(PkgConfig)

PKG_CHECK_MODULES(PC_GR_NRF52 gnuradio-nrf52)

FIND_PATH(
    GR_NRF52_INCLUDE_DIRS
    NAMES gnuradio/nrf52/api.h
    HINTS $ENV{NRF52_DIR}/include
        ${PC_NRF52_INCLUDEDIR}
    PATHS ${CMAKE_INSTALL_PREFIX}/include
          /usr/local/include
          /usr/include
)

FIND_LIBRARY(
    GR_NRF52_LIBRARIES
    NAMES gnuradio-nrf52
    HINTS $ENV{NRF52_DIR}/lib
        ${PC_NRF52_LIBDIR}
    PATHS ${CMAKE_INSTALL_PREFIX}/lib
          ${CMAKE_INSTALL_PREFIX}/lib64
          /usr/local/lib
          /usr/local/lib64
          /usr/lib
          /usr/lib64
          )

include("${CMAKE_CURRENT_LIST_DIR}/gnuradio-nrf52Target.cmake")

INCLUDE(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(GR_NRF52 DEFAULT_MSG GR_NRF52_LIBRARIES GR_NRF52_INCLUDE_DIRS)
MARK_AS_ADVANCED(GR_NRF52_LIBRARIES GR_NRF52_INCLUDE_DIRS)
