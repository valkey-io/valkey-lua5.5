/*
 * Copyright (c) 2021, Redis Ltd.
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * FUNCTION subsystem for the Lua 5.5 engine module.
 *
 * In the per-user architecture, compilation happens in a scratch lua_State:
 *   1. The library code is loaded and executed (calling register_function).
 *   2. Library source code is stored in lua55Library.
 *   3. Function metadata (name, flags) is returned as lua55Function structs.
 *
 * At call time (in engine_lua55.c), the library source is recompiled
 * in the per-user lua_State and functions are registered there.
 */

#include "function_lua55.h"
#include "script_lua55.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define REGISTRY_LOAD_CTX_NAME "__LIBRARY_CTX__"
#define LIBRARY_API_NAME "__LIBRARY_API__"

typedef uint64_t monotime;

static monotime getMonotonicUs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

static inline uint64_t elapsedUs(monotime start_time) {
    return (getMonotonicUs() - start_time);
}

static inline uint64_t elapsedMs(monotime start_time) {
    return (elapsedUs(start_time) / 1000);
}

typedef struct {
    void **items;
    size_t len;
    size_t cap;
} ptrArray;

static ptrArray ptrArrayCreate(void) {
    return (ptrArray){NULL, 0, 0};
}

static void ptrArrayAdd(ptrArray *a, void *item) {
    if (a->len >= a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->items = ValkeyModule_Realloc(a->items, a->cap * sizeof(void *));
    }
    a->items[a->len++] = item;
}

static void ptrArrayDestroy(ptrArray *a) {
    if (a->items)
        ValkeyModule_Free(a->items);
    a->items = NULL;
    a->len = a->cap = 0;
}

typedef struct loadCtx {
    ptrArray functions;
    monotime start_time;
    size_t timeout;
} loadCtx;

/* Hook for FUNCTION LOAD execution.
 * Cancels execution on timeout. */
static void lua55EngineLoadHook(lua_State *lua, lua_Debug *ar) {
    VALKEYMODULE_NOT_USED(ar);
    loadCtx *load_ctx = lua55GetFromRegistry(lua, REGISTRY_LOAD_CTX_NAME);
    ValkeyModule_Assert(load_ctx);
    uint64_t duration = elapsedMs(load_ctx->start_time);
    if (load_ctx->timeout > 0 && duration > load_ctx->timeout) {
        lua_sethook(lua, lua55EngineLoadHook, LUA_MASKLINE, 0);
        lua55PushError(lua, "FUNCTION LOAD timeout");
        lua55Error(lua);
    }
}

typedef struct flagStr {
    ValkeyModuleScriptingEngineScriptFlag flag;
    const char *str;
} flagStr;

static flagStr scripts_flags_def[] = {
    {.flag = VMSE_SCRIPT_FLAG_NO_WRITES, .str = "no-writes"},
    {.flag = VMSE_SCRIPT_FLAG_ALLOW_OOM, .str = "allow-oom"},
    {.flag = VMSE_SCRIPT_FLAG_ALLOW_STALE, .str = "allow-stale"},
    {.flag = VMSE_SCRIPT_FLAG_NO_CLUSTER, .str = "no-cluster"},
    {.flag = VMSE_SCRIPT_FLAG_ALLOW_CROSS_SLOT, .str = "allow-cross-slot-keys"},
    {.flag = 0, .str = NULL},
};

static int lua55RegisterFunctionReadFlags(lua_State *lua, uint64_t *flags) {
    int j = 1;
    int f_flags = 0;
    while (1) {
        lua_pushinteger(lua, j++);
        lua_gettable(lua, -2);
        int t = lua_type(lua, -1);
        if (t == LUA_TNIL) {
            lua_pop(lua, 1);
            break;
        }
        if (!lua_isstring(lua, -1)) {
            lua_pop(lua, 1);
            return C_ERR;
        }

        const char *flag_str = lua_tostring(lua, -1);
        int found = 0;
        for (flagStr *flag = scripts_flags_def; flag->str; ++flag) {
            if (!strcasecmp(flag->str, flag_str)) {
                f_flags |= flag->flag;
                found = 1;
                break;
            }
        }
        lua_pop(lua, 1);
        if (!found)
            return C_ERR;
    }

    *flags = f_flags;
    return C_OK;
}

static ValkeyModuleString *lua55GetStringObject(lua_State *lua, int index) {
    if (!lua_isstring(lua, index))
        return NULL;
    size_t len;
    const char *str = lua_tolstring(lua, index, &len);
    return ValkeyModule_CreateString(NULL, str, len);
}

typedef struct pendingFunc {
    ValkeyModuleString *name;
    ValkeyModuleString *desc;
    int has_callback;
    uint64_t flags;
} pendingFunc;

static int lua55RegisterFunctionReadNamedArgs(lua_State *lua, pendingFunc *pf) {
    char *err = NULL;

    if (!lua_istable(lua, 1)) {
        err = "calling server.register_function with a single argument is only "
              "applicable to Lua table "
              "(representing named arguments).";
        goto error;
    }

    lua_pushnil(lua);
    while (lua_next(lua, -2)) {
        if (!lua_isstring(lua, -2)) {
            err = "named argument key given to server.register_function is not a "
                  "string";
            goto error;
        }

        const char *key = lua_tostring(lua, -2);
        if (!strcasecmp(key, "function_name")) {
            if (!(pf->name = lua55GetStringObject(lua, -1))) {
                err = "function_name argument given to server.register_function must "
                      "be a string";
                goto error;
            }
        } else if (!strcasecmp(key, "description")) {
            if (!(pf->desc = lua55GetStringObject(lua, -1))) {
                err = "description argument given to server.register_function must be "
                      "a string";
                goto error;
            }
        } else if (!strcasecmp(key, "callback")) {
            if (!lua_isfunction(lua, -1)) {
                err = "callback argument given to server.register_function must be a "
                      "function";
                goto error;
            }
            lua_pop(lua, 1);
            pf->has_callback = 1;
            continue;
        } else if (!strcasecmp(key, "flags")) {
            if (!lua_istable(lua, -1)) {
                err = "flags argument to server.register_function must be a table "
                      "representing function flags";
                goto error;
            }
            if (lua55RegisterFunctionReadFlags(lua, &pf->flags) != C_OK) {
                err = "unknown flag given";
                goto error;
            }
        } else {
            err = "unknown argument given to server.register_function";
            goto error;
        }
        lua_pop(lua, 1);
    }

    if (!pf->name) {
        err = "server.register_function must get a function name argument";
        goto error;
    }
    if (!pf->has_callback) {
        err = "server.register_function must get a callback argument";
        goto error;
    }

    return C_OK;

error:
    if (pf->name) {
        ValkeyModule_FreeString(NULL, pf->name);
        pf->name = NULL;
    }
    if (pf->desc) {
        ValkeyModule_FreeString(NULL, pf->desc);
        pf->desc = NULL;
    }
    lua55PushError(lua, err);
    return C_ERR;
}

static int lua55RegisterFunctionReadPositionalArgs(lua_State *lua,
                                                   pendingFunc *pf) {
    char *err = NULL;

    if (!(pf->name = lua55GetStringObject(lua, 1))) {
        err = "first argument to server.register_function must be a string";
        goto error;
    }

    if (!lua_isfunction(lua, 2)) {
        err = "second argument to server.register_function must be a function";
        goto error;
    }

    lua_pop(lua, 1);
    pf->has_callback = 1;
    return C_OK;

error:
    if (pf->name) {
        ValkeyModule_FreeString(NULL, pf->name);
        pf->name = NULL;
    }
    lua55PushError(lua, err);
    return C_ERR;
}

static int lua55RegisterFunctionReadArgs(lua_State *lua, pendingFunc *pf) {
    int argc = lua_gettop(lua);
    if (argc < 1 || argc > 2) {
        lua55PushError(lua,
                       "wrong number of arguments to server.register_function");
        return C_ERR;
    }
    if (argc == 1) {
        return lua55RegisterFunctionReadNamedArgs(lua, pf);
    } else {
        return lua55RegisterFunctionReadPositionalArgs(lua, pf);
    }
}

static int lua55FunctionRegisterFunction(lua_State *lua) {
    loadCtx *load_ctx = lua55GetFromRegistry(lua, REGISTRY_LOAD_CTX_NAME);
    if (!load_ctx) {
        lua55PushError(
            lua,
            "server.register_function can only be called on FUNCTION LOAD command");
        return lua55Error(lua);
    }

    pendingFunc *pf = ValkeyModule_Calloc(1, sizeof(*pf));
    pf->has_callback = 0;

    if (lua55RegisterFunctionReadArgs(lua, pf) != C_OK) {
        ValkeyModule_Free(pf);
        return lua55Error(lua);
    }

    ptrArrayAdd(&load_ctx->functions, pf);
    return 0;
}

static void freePendingFunc(pendingFunc *pf) {
    if (pf->name)
        ValkeyModule_FreeString(NULL, pf->name);
    if (pf->desc)
        ValkeyModule_FreeString(NULL, pf->desc);
    ValkeyModule_Free(pf);
}

ValkeyModuleScriptingEngineCompiledFunction **
lua55FunctionLibraryCreate(lua_State *lua, const char *code, size_t code_len, size_t timeout, size_t *out_num_compiled_functions, ValkeyModuleString **err) {
    ValkeyModuleScriptingEngineCompiledFunction **compiled_functions = NULL;

    if (luaL_loadbuffer(lua, code, code_len, "@user_function")) {
        *err = ValkeyModule_CreateStringPrintf(NULL, "Error compiling function: %s",
                                               lua_tostring(lua, -1));
        lua_pop(lua, 1);
        return NULL;
    }
    ValkeyModule_Assert(lua_isfunction(lua, -1));

    loadCtx load_ctx = {
        .functions = ptrArrayCreate(),
        .start_time = getMonotonicUs(),
        .timeout = timeout,
    };
    lua55SaveOnRegistry(lua, REGISTRY_LOAD_CTX_NAME, &load_ctx);

    lua_sethook(lua, lua55EngineLoadHook, LUA_MASKCOUNT, LUA_HOOK_CHECK_INTERVAL);

    if (lua_pcall(lua, 0, 0, 0)) {
        errorInfo err_info = {0};
        lua55ExtractErrorInformation(lua, &err_info);
        *err = ValkeyModule_CreateStringPrintf(
            NULL, "Error registering functions: %s", err_info.msg);
        lua_pop(lua, 1);
        lua55ErrorInformationDiscard(&err_info);

        for (size_t i = 0; i < load_ctx.functions.len; i++) {
            freePendingFunc(load_ctx.functions.items[i]);
        }
        ptrArrayDestroy(&load_ctx.functions);
        goto done;
    }

    *out_num_compiled_functions = load_ctx.functions.len;
    compiled_functions = ValkeyModule_Calloc(
        load_ctx.functions.len,
        sizeof(ValkeyModuleScriptingEngineCompiledFunction *));

    for (size_t i = 0; i < load_ctx.functions.len; i++) {
        pendingFunc *pf = load_ctx.functions.items[i];

        lua55Function *func_data = ValkeyModule_Calloc(1, sizeof(*func_data));
        func_data->is_from_eval = 0;
        func_data->function_ref.name =
            lua55_strcpy(ValkeyModule_StringPtrLen(pf->name, NULL));
        func_data->function_ref.lib_id = 0;
        func_data->function_ref.func_id = 0;

        ValkeyModuleScriptingEngineCompiledFunction *cf =
            ValkeyModule_Calloc(1, sizeof(*cf));
        cf->name = pf->name;
        cf->desc = pf->desc;
        cf->function = func_data;
        cf->f_flags = pf->flags;
        compiled_functions[i] = cf;

        pf->name = NULL;
        pf->desc = NULL;
        ValkeyModule_Free(pf);
    }
    ptrArrayDestroy(&load_ctx.functions);

done:
    lua_sethook(lua, NULL, 0, 0);
    lua55SaveOnRegistry(lua, REGISTRY_LOAD_CTX_NAME, NULL);
    return compiled_functions;
}

void lua55InitFunctionScratchState(lua55EngineCtx *ctx, lua_State *lua) {
    luaL_requiref(lua, "_G", luaopen_base, 1);
    lua_pop(lua, 1);

    luaL_requiref(lua, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(lua, 1);

    luaL_requiref(lua, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(lua, 1);

    lua_newtable(lua);

    lua_pushstring(lua, "register_function");
    lua_pushcfunction(lua, lua55FunctionRegisterFunction);
    lua_settable(lua, -3);

    lua55RegisterLogFunction(lua);
    lua55RegisterVersion(ctx, lua);

    lua_setglobal(lua, SERVER_API_NAME);

    lua_getglobal(lua, SERVER_API_NAME);
    lua_setglobal(lua, REDIS_API_NAME);
}
