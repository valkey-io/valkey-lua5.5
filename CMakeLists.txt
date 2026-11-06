cmake_minimum_required(VERSION 3.13)
project(valkey-lua55 C)

if(APPLE)
    set(IS_MACOS TRUE)
else()
    set(IS_MACOS FALSE)
endif()

set(LUA_SRC_DIR ${CMAKE_SOURCE_DIR}/deps/lua)

set(LUA_CORE_SRCS
    ${LUA_SRC_DIR}/lapi.c
    ${LUA_SRC_DIR}/lcode.c
    ${LUA_SRC_DIR}/lctype.c
    ${LUA_SRC_DIR}/ldebug.c
    ${LUA_SRC_DIR}/ldo.c
    ${LUA_SRC_DIR}/ldump.c
    ${LUA_SRC_DIR}/lfunc.c
    ${LUA_SRC_DIR}/lgc.c
    ${LUA_SRC_DIR}/llex.c
    ${LUA_SRC_DIR}/lmem.c
    ${LUA_SRC_DIR}/lobject.c
    ${LUA_SRC_DIR}/lopcodes.c
    ${LUA_SRC_DIR}/lparser.c
    ${LUA_SRC_DIR}/lstate.c
    ${LUA_SRC_DIR}/lstring.c
    ${LUA_SRC_DIR}/ltable.c
    ${LUA_SRC_DIR}/ltm.c
    ${LUA_SRC_DIR}/lundump.c
    ${LUA_SRC_DIR}/lvm.c
    ${LUA_SRC_DIR}/lzio.c
)

set(LUA_AUX_SRCS
    ${LUA_SRC_DIR}/lauxlib.c
)

set(LUA_LIB_SRCS
    ${LUA_SRC_DIR}/lbaselib.c
    ${LUA_SRC_DIR}/lcorolib.c
    ${LUA_SRC_DIR}/ldblib.c
    ${LUA_SRC_DIR}/liolib.c
    ${LUA_SRC_DIR}/lmathlib.c
    ${LUA_SRC_DIR}/loadlib.c
    ${LUA_SRC_DIR}/loslib.c
    ${LUA_SRC_DIR}/lstrlib.c
    ${LUA_SRC_DIR}/ltablib.c
    ${LUA_SRC_DIR}/lutf8lib.c
    ${LUA_SRC_DIR}/linit.c
)

add_library(lua_static STATIC
    ${LUA_CORE_SRCS}
    ${LUA_AUX_SRCS}
    ${LUA_LIB_SRCS}
)

target_include_directories(lua_static PUBLIC ${LUA_SRC_DIR})

target_compile_options(lua_static PRIVATE
    -O2
    -fPIC
    -fvisibility=hidden
    -Wall
    -Wno-empty-body
    -Wno-deprecated-declarations
)

if(APPLE)
    target_compile_definitions(lua_static PRIVATE LUA_USE_MACOSX)
elseif(UNIX)
    target_compile_definitions(lua_static PRIVATE LUA_USE_LINUX)
endif()

set(MODULE_SRCS
    src/engine_lua55.c
    src/script_lua55.c
    src/function_lua55.c
    src/sha1.c
    src/rand.c
    src/lua_cjson.c
    src/lua_struct.c
    src/lua_cmsgpack.c
    src/lua_bit.c
    src/lua_coroutine.c
    src/strbuf.c
    src/fpconv.c
    src/fpconv_dtoa.c
)

add_library(valkeylua55 SHARED ${MODULE_SRCS})

target_compile_options(valkeylua55 PRIVATE
    -std=gnu11
    -W -Wall
    -fno-common
    -fPIC
    -fvisibility=hidden
    -O2
    -D_GNU_SOURCE
)

target_include_directories(valkeylua55 PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${LUA_SRC_DIR}
)

target_link_libraries(valkeylua55 PRIVATE lua_static)

if(IS_MACOS)
    set_target_properties(valkeylua55 PROPERTIES
        SUFFIX ".so"
        PREFIX "lib"
        LINK_FLAGS "-undefined dynamic_lookup"
    )
else()
    set_target_properties(valkeylua55 PROPERTIES
        SUFFIX ".so"
        PREFIX "lib"
    )
    target_link_libraries(valkeylua55 PRIVATE m dl)
endif()

install(TARGETS valkeylua55
    LIBRARY DESTINATION lib
)
