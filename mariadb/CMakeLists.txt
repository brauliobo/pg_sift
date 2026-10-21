# sift for MariaDB with CMake: the same library as the Makefile (libsift_udf.so), built against the installed headers.
#   cmake -S mariadb -B /tmp/sift-cmake -DCMAKE_BUILD_TYPE=Release && nice cmake --build /tmp/sift-cmake -j4
# Finds the MariaDB server headers with mariadb_config / mysql_config (or -DMDB_INC=<dir with mysql_version.h>) and needs
# nothing else: the portable core (src/portable.list) is compiled against the host in host/, which is plain C. The UDF file
# is compiled against the MariaDB headers only.
cmake_minimum_required(VERSION 3.16)
project(sift_mariadb C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
find_package(Threads REQUIRED)

# --- MariaDB headers
if(NOT MDB_INC)
  foreach(cfg mariadb_config mysql_config)
    find_program(MDB_CONFIG ${cfg})
    if(MDB_CONFIG)
      execute_process(COMMAND ${MDB_CONFIG} --variable=pkgincludedir OUTPUT_VARIABLE incdir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
      if(EXISTS "${incdir}/server/mysql_version.h")
        set(MDB_INC "${incdir}/server")
        break()
      endif()
    endif()
    unset(MDB_CONFIG CACHE)
  endforeach()
endif()
if(NOT MDB_INC OR NOT EXISTS "${MDB_INC}/mysql_version.h")
  message(FATAL_ERROR "MariaDB server headers not found: install the server development package or pass -DMDB_INC=<dir>")
endif()
file(STRINGS "${MDB_INC}/mysql_version.h" mdb_ver_line REGEX "^#define[ \t]+MYSQL_SERVER_VERSION")
string(REGEX REPLACE ".*\"(.*)\".*" "\\1" MDB_VER "${mdb_ver_line}")
message(STATUS "MariaDB headers: ${MDB_INC} (${MDB_VER})")
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION SIFT_VERSION LIMIT_COUNT 1)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(SRC ${ROOT}/src)
# the sources include each other by their path from src/ (src/agg and src/compat: see src/areas.mk)
set(SIFT_INCDIRS ${SRC})
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN})

# the portable core: the .c files of the list every host shares (bm25/bitmap.c is compiled from a copy, see below)
file(STRINGS ${SRC}/portable.list portable REGEX "^[^#].*\\.c$")
list(REMOVE_ITEM portable bm25/bitmap.c)
list(TRANSFORM portable PREPEND ${SRC}/)

# src/bm25/bitmap.c with its seek limit per thread (a global that a query sets while it seeks)
add_custom_command(OUTPUT ${GEN}/bitmap.c
  COMMAND sed "s/^static uint32_t seek_limit = /static _Thread_local uint32_t seek_limit = /" ${SRC}/bm25/bitmap.c > ${GEN}/bitmap.c
  COMMAND grep -q "_Thread_local uint32_t seek_limit" ${GEN}/bitmap.c
  DEPENDS ${SRC}/bm25/bitmap.c VERBATIM)

# the host: the C library vocabulary of sqlite/host (its host.c as it is), the matcher, the mapped store and the glue
set(HOST ${ROOT}/sqlite/host/host.c ${ROOT}/sqlite/host/container.c ${ROOT}/sqlite/host/mb.c ${ROOT}/sqlite/host/spool.c ${ROOT}/sqlite/host/host_stubs.c
  host/regex.c host/regex_parse.c host/regex_exec.c host/store.c host/stubs.c)
set(GLUE engine.c docs.c persist.c query.c feed.c merge.c)
set(UDF udf.c udf_host.c udf_query.c udf_drain.c)

add_library(sift_engine OBJECT ${portable} ${GEN}/bitmap.c ${HOST} ${GLUE})
target_include_directories(sift_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/host ${CMAKE_CURRENT_SOURCE_DIR} ${SIFT_INCDIRS})
target_compile_definitions(sift_engine PRIVATE _GNU_SOURCE SIFT_HOST_MARIADB
  SIFT_MDB_SERVER_VERSION="${MDB_VER}" SIFT_MDB_RELEASE="${SIFT_VERSION}"
  # the codec's names are those of two UDFs
  sift_count=codec_count sift_topk=codec_topk)
target_compile_options(sift_engine PRIVATE -O2 -ffp-contract=off -fwrapv -fno-strict-aliasing -Wall -Wno-declaration-after-statement
  -Wno-unused-function -Wno-misleading-indentation -march=x86-64 -mtune=generic -fvisibility=hidden)

# the UDFs include MariaDB headers and nothing of the engine
add_library(sift_udf SHARED ${UDF} $<TARGET_OBJECTS:sift_engine>)
set_target_properties(sift_udf PROPERTIES PREFIX "lib" OUTPUT_NAME sift_udf)
target_include_directories(sift_udf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${MDB_INC} ${MDB_INC}/private)
set_source_files_properties(${UDF} PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden;-Wall")
target_link_libraries(sift_udf PRIVATE m ${CMAKE_DL_LIBS} Threads::Threads -Wl,--exclude-libs,ALL -Wl,-Bsymbolic)

install(TARGETS sift_udf LIBRARY DESTINATION lib/mysql/plugin)
install(FILES sql/functions.sql sql/sift.sql DESTINATION share/sift)
