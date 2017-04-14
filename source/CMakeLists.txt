#
# EMULAB-LGPL
# Copyright The Deter-Project (c) 2016. All rights reserved
#

#
# build file for control component for cumulus switches
#

# setup ........................................................................

cmake_minimum_required(VERSION 3.0)

project(cumulus_controller)

include_directories( /usr/include/libxml2 )

set(CMAKE_MODULE_PATH
  ${CMAKE_MODULE_PATH}
  "${cumulus_controller_SOURCE_DIR}/cmake"
  "/usr/local/lib/cmake")

set( CMAKE_C_FLAGS
  "${CMAKE_C_FLAGS} \
  -std=c11 \
  -Wall -Wextra -pthread" )

set( CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3")

set( CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} \
  -g -O0 -fno-optimize-sibling-calls -fno-omit-frame-pointer" )

set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} \
  -g -std=c++14 \
  -Wall -Wextra \
  -fdiagnostics-color=always -pthread" )

set( CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} \
  -O3" )

set( CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} \
  -g -O0 -fno-optimize-sibling-calls -fno-omit-frame-pointer" )

include_directories( "${cumulus_controller_SOURCE_DIR}" )

# build ........................................................................

add_library( deter-cumulus dcc.cxx augeas.cxx util.cxx netlink.cxx )
target_link_libraries( deter-cumulus augeas fmt )

add_executable( dcc deter_cumulus_controller.cxx )
target_link_libraries( dcc deter-cumulus mhttpdxx microhttpd glog gflags )
  
#add_executable( dcc_test dcc_test.cxx catchme.cxx )
#target_link_libraries( dcc_test deter-cumulus )

install(TARGETS dcc RUNTIME DESTINATION BIN)

