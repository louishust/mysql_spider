# Copyright (C) 2011 DeNA Co.,Ltd.. All rights reserved.
# See COPYRIGHT.txt for details.

INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/plugin/handlersocket/libhsclient
  ${CMAKE_SOURCE_DIR}/plugin/handlersocket/handlersocket)

SET(HS_SOURCES
  libhsclient/config.cpp libhsclient/escape.cpp libhsclient/fatal.cpp
  libhsclient/hstcpcli.cpp libhsclient/socket.cpp libhsclient/string_util.cpp
  handlersocket/database.cpp handlersocket/handlersocket.cpp
  handlersocket/hstcpsvr_worker.cpp handlersocket/hstcpsvr.cpp)

MYSQL_ADD_PLUGIN(handlersocket ${HS_SOURCES}
  MODULE_ONLY MODULE_OUTPUT_NAME "handlersocket")
