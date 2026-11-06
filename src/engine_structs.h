/*
 * Shared data structures for the Lua 5.5 scripting engine module.
 *
 * Architecture: per-user Lua state isolation.
 *
 * Instead of a single shared lua_State (with complex readonly-table
 * protection that requires patching Lua), each authenticated user
 * gets their own lua_State, created lazily on first use.
 *
 * Scripts are compiled and stored as source text during compile_code
 * (which has no user identity) and then recompiled in the appropriate
 * per-user state during call_function.
 */

#ifndef _ENGINE_STRUCTS_H_
#define _ENGINE_STRUCTS_H_

#include <lua.h>
#include <stddef.h>
#include <stdint.h>

typedef struct lua55Library {
    char *code;
    size_t code_len;
    uint64_t lib_id;
    char *name;
    int ref_count;
} lua55Library;

typedef struct lua55Function {
    int is_from_eval;
    union {
        struct {
            char *text;
            size_t text_len;
        } source;
        struct {
            char *name;
            uint64_t lib_id;
            uint64_t func_id;
        } function_ref;
    };
    uint64_t func_id;
} lua55Function;

typedef struct lua55PerUserState {
    lua_State *eval_lua;
    lua_State *function_lua;
} lua55PerUserState;

struct ValkeyModuleDict;
struct ValkeyModuleString;

#define LUA55_ARGV_CACHE_SIZE 32
#define LUA55_ARGV_CACHE_MAX_LEN 64

typedef struct lua55EngineCtx {
    lua_State *compile_lua;

    struct ValkeyModuleDict *user_states;
    struct ValkeyModuleDict *libraries;

    char *redis_version;
    uint32_t redis_version_num;
    char *server_name;
    char *valkey_version;
    uint32_t valkey_version_num;

    uint64_t next_func_id;
    uint64_t next_lib_id;
    uint64_t eval_reset_func_id;

    int lua_enable_insecure_api;

    struct ValkeyModuleDict *core_commands;
    struct ValkeyModuleString *argv_cache[LUA55_ARGV_CACHE_SIZE];
    struct ValkeyModuleString *argv_buf[LUA55_ARGV_CACHE_SIZE];
    int argv_cache_busy;
} lua55EngineCtx;

#endif
