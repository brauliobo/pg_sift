# sift for MariaDB with CMake: the same library as the Makefile (libsift_udf.so), built against the installed headers.
#   cmake -S mariadb -B /tmp/sift-cmake -DCMAKE_BUILD_TYPE=Release && nice cmake --build /tmp/sift-cmake -j4
# Finds the MariaDB server headers with mariadb_config / mysql_config (or -DMDB_INC=<dir with mysql_version.h>) and the
# PostgreSQL server headers and libpgcommon / libpgport archives with pg_config (or -DPG_CONFIG=<path>). The UDF file is
# compiled against the MariaDB headers only; the engine (src/) against the PostgreSQL headers only.
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

# --- PostgreSQL headers and archives
if(NOT PG_CONFIG)
  find_program(PG_CONFIG pg_config REQUIRED)
endif()
execute_process(COMMAND ${PG_CONFIG} --includedir-server OUTPUT_VARIABLE PG_INC OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${PG_CONFIG} --libdir OUTPUT_VARIABLE PG_LIB OUTPUT_STRIP_TRAILING_WHITESPACE)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(SRC ${ROOT}/src)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN})

# the engine sources of src/, compiled unmodified
set(PGSRC sift_bitmap_scalar sift_bitmap_avx2 sift_bitmap_avx512 sift_kernels sift_kernels_avx2 sift_kernels_avx512
          sift_analyze sift_analyze_stem sift_query sift_dict sift_posting sift_segment sift_prefix sift_eval)
list(TRANSFORM PGSRC PREPEND ${SRC}/ OUTPUT_VARIABLE pg_files)
list(TRANSFORM pg_files APPEND .c)

# the part of src/sift_build.c before the heap build, and src/sift_bitmap.c with its seek limit per thread
add_custom_command(OUTPUT ${GEN}/sift_build_core.c
  COMMAND sed "/^\\/\\* ---------- ambuild ---------- \\*\\//,\$d" ${SRC}/sift_build.c > ${GEN}/sift_build_core.c
  DEPENDS ${SRC}/sift_build.c VERBATIM)
add_custom_command(OUTPUT ${GEN}/sift_bitmap_tl.c
  COMMAND sed "s/^static uint32_t seek_limit/static __thread uint32_t seek_limit/" ${SRC}/sift_bitmap.c > ${GEN}/sift_bitmap_tl.c
  DEPENDS ${SRC}/sift_bitmap.c VERBATIM)

set(SHIM shim.c store.c regex.c engine.c query.c feed.c merge.c)
add_library(sift_engine OBJECT ${pg_files} ${GEN}/sift_build_core.c ${GEN}/sift_bitmap_tl.c ${SHIM})
target_include_directories(sift_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SRC} ${PG_INC})
target_compile_definitions(sift_engine PRIVATE _GNU_SOURCE
  SIFT_MDB_SERVER_VERSION="${MDB_VER}"
  # the current and top memory context of the engine are per thread; the codec's names are those of two UDFs
  CurrentMemoryContext=\(*sift_cur_cx\(\)\) TopMemoryContext=\(*sift_top_cx\(\)\) sift_count=codec_count sift_topk=codec_topk)
target_compile_options(sift_engine PRIVATE -O2 -ffp-contract=off -fwrapv -fno-strict-aliasing -Wall -Wno-declaration-after-statement
  -Wno-unused-function -march=x86-64 -mtune=generic -fvisibility=hidden)

# the UDFs include MariaDB headers and nothing of PostgreSQL
add_library(sift_udf SHARED udf.c $<TARGET_OBJECTS:sift_engine>)
set_target_properties(sift_udf PROPERTIES PREFIX "lib" OUTPUT_NAME sift_udf)
target_include_directories(sift_udf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${MDB_INC} ${MDB_INC}/private)
set_source_files_properties(udf.c PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden;-Wall")
target_link_libraries(sift_udf PRIVATE ${PG_LIB}/libpgcommon_shlib.a ${PG_LIB}/libpgport_shlib.a m ${CMAKE_DL_LIBS} Threads::Threads
  -Wl,--exclude-libs,ALL -Wl,-Bsymbolic)

install(TARGETS sift_udf LIBRARY DESTINATION lib/mysql/plugin)
install(FILES sql/functions.sql sql/sift.sql DESTINATION share/sift)
