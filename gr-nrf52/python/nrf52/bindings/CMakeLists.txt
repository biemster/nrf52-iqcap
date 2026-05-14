# Copyright 2020 Free Software Foundation, Inc.
#
# This file is part of GNU Radio
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

########################################################################
# Check if there is C++ code at all
########################################################################
if(NOT nrf52_sources)
    message(STATUS "No C++ sources... skipping python bindings")
    return()
endif(NOT nrf52_sources)

########################################################################
# Check for pygccxml
########################################################################
gr_python_check_module_raw("pygccxml" "import pygccxml" PYGCCXML_FOUND)

include(GrPybind)

########################################################################
# Python Bindings
########################################################################

list(APPEND nrf52_python_files
    iq_stream_source_python.cc python_bindings.cc)

gr_pybind_make_oot(nrf52 ../../.. gr::nrf52 "${nrf52_python_files}")

# copy bindings extension for use in QA test module
add_custom_command(
    TARGET nrf52_python
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:nrf52_python>
            ${PROJECT_BINARY_DIR}/test_modules/gnuradio/nrf52/)

install(
    TARGETS nrf52_python
    DESTINATION ${GR_PYTHON_DIR}/gnuradio/nrf52
    COMPONENT pythonapi)
