/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Lua 5.5 scripting engine module for Valkey.
 *
 * Per-user architecture: each authenticated user gets their own pair of
 * lua_State instances (one for EVAL, one for FUNCTION), created lazily on
 * first use. Scripts are compiled and stored as source text during
 * compile_code (which has no user identity) and loaded into the appropriate
 * per-user state during call_function.
 *
 * This eliminates the need for readonly-table protection (which requires
 * to patch lua implementation).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "valkeymodule.h"
#include <errno.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <string.h>

#include "engine_structs.h"
#include "function_lua55.h"
#include "script_lua55.h"

#define DEFAULT_ENGINE_NAME "LUA"
#define REGISTRY_FUNC_CACHE_NAME "__func_cache"
#define MAX_ENGINE_NAMES 4

static ValkeyModuleString *engine_names[MAX_ENGINE_NAMES] = {NULL};
static const char *engine_names_cstr[MAX_ENGINE_NAMES] = {NULL};
static int engine_names_count = 0;

#define REGISTRY_ERROR_HANDLER_NAME "__ERROR_HANDLER__"

/* Adds server.replicate_commands()
 *
 * DEPRECATED: Now do nothing and always return true.
 * Turn on single commands replication if the script never called
 * a write command so far, and returns true. Otherwise if the script
 * already started to write, returns false and stick to whole scripts
 * replication, which is our default. */
static int lua55ServerReplicateCommandsCommand(lua_State *lua) {
    lua_pushboolean(lua, 1);
    return 1;
}

static int lua55ServerBreakpointCommand(lua_State *lua) {
    lua_pushboolean(lua, 0);
    return 1;
}

static int lua55ServerDebugCommand(lua_State *lua) {
    (void)lua;
    return 0;
}

/* Add a helper function we use for pcall error reporting.
 * Note that when the error is in the C function we want to report the
 * information about the caller, that's what makes sense from the point
 * of view of the user debugging a script. */
static void lua55StateInstallErrorHandler(lua_State *lua) {
    lua_pushstring(lua, REGISTRY_ERROR_HANDLER_NAME);
    char *errh_func = "local dbg = debug\n"
                      "debug = nil\n"
                      "local error_handler = function (err)\n"
                      "  local i = dbg.getinfo(2,'nSl')\n"
                      "  if i and i.what == 'C' then\n"
                      "    i = dbg.getinfo(3,'nSl')\n"
                      "  end\n"
                      "  if type(err) ~= 'table' then\n"
                      "    err = {err='ERR ' .. tostring(err)}"
                      "  end"
                      "  if i then\n"
                      "    err['source'] = i.source\n"
                      "    err['line'] = i.currentline\n"
                      "  end"
                      "  return err\n"
                      "end\n"
                      "return error_handler";
    luaL_loadbuffer(lua, errh_func, strlen(errh_func), "@err_handler_def");
    lua_pcall(lua, 0, 1, 0);
    lua_settable(lua, LUA_REGISTRYINDEX);
}

static void initializeEvalExtras(lua_State *lua) {
    lua_getglobal(lua, "server");

    lua_pushstring(lua, "breakpoint");
    lua_pushcfunction(lua, lua55ServerBreakpointCommand);
    lua_settable(lua, -3);

    lua_pushstring(lua, "debug");
    lua_pushcfunction(lua, lua55ServerDebugCommand);
    lua_settable(lua, -3);

    lua_pushstring(lua, "replicate_commands");
    lua_pushcfunction(lua, lua55ServerReplicateCommandsCommand);
    lua_settable(lua, -3);

    lua_setglobal(lua, "server");

    lua_pushstring(lua, REGISTRY_ERROR_HANDLER_NAME);
    lua_gettable(lua, LUA_REGISTRYINDEX);
    lua_pushvalue(lua, -1);
    lua_setglobal(lua, "__server__err__handler");
    lua_setglobal(lua, "__redis__err__handler");
}

static uint32_t parse_semver(const char *version) {
    unsigned int major = 0, minor = 0, patch = 0;
    sscanf(version, "%u.%u.%u", &major, &minor, &patch);
    return ((major & 0xFF) << 16) | ((minor & 0xFF) << 8) | (patch & 0xFF);
}

static void get_version_info(ValkeyModuleCtx *ctx, char **redis_version, uint32_t *redis_version_num, char **server_name, char **valkey_version, uint32_t *valkey_version_num) {
    ValkeyModuleServerInfoData *info = ValkeyModule_GetServerInfo(ctx, "server");
    ValkeyModule_Assert(info != NULL);

    const char *rv = ValkeyModule_ServerInfoGetFieldC(info, "redis_version");
    *redis_version = lua55_strcpy(rv);
    *redis_version_num = parse_semver(*redis_version);

    const char *sn = ValkeyModule_ServerInfoGetFieldC(info, "server_name");
    *server_name = lua55_strcpy(sn);

    const char *vv = ValkeyModule_ServerInfoGetFieldC(info, "valkey_version");
    *valkey_version = lua55_strcpy(vv);
    *valkey_version_num = parse_semver(*valkey_version);

    ValkeyModule_FreeServerInfo(ctx, info);
}

static lua_State *
createUserLuaState(lua55EngineCtx *engine_ctx,
                   ValkeyModuleScriptingEngineSubsystemType type) {
    lua_State *lua = luaL_newstate();

    lua55RegisterServerAPI(engine_ctx, lua);
    lua55StateInstallErrorHandler(lua);

    if (type == VMSE_EVAL) {
        initializeEvalExtras(lua);
    }

    lua_pushstring(lua, REGISTRY_FUNC_CACHE_NAME);
    lua_newtable(lua);
    lua_settable(lua, LUA_REGISTRYINDEX);

    if (type == VMSE_FUNCTION) {
        lua_newtable(lua);
        lua_setfield(lua, LUA_REGISTRYINDEX, "__library_functions");

        lua_newtable(lua);
        lua_setfield(lua, LUA_REGISTRYINDEX, "__library_compiled");

        lua_getglobal(lua, SERVER_API_NAME);
        lua_pushstring(lua, "register_function");
        lua_pushcfunction(lua, lua55UserStateRegisterFunction);
        lua_settable(lua, -3);
        lua_pop(lua, 1);
    }

    return lua;
}

static lua55PerUserState *createPerUserState(lua55EngineCtx *engine_ctx) {
    lua55PerUserState *us = ValkeyModule_Calloc(1, sizeof(*us));
    us->eval_lua = createUserLuaState(engine_ctx, VMSE_EVAL);
    us->function_lua = createUserLuaState(engine_ctx, VMSE_FUNCTION);
    return us;
}

static void destroyPerUserState(lua55PerUserState *us) {
    if (us->eval_lua) {
        lua_close(us->eval_lua);
    }
    if (us->function_lua) {
        lua_close(us->function_lua);
    }
    ValkeyModule_Free(us);
}

static lua55PerUserState *getOrCreateUserState(lua55EngineCtx *engine_ctx,
                                               const char *username) {
    lua55PerUserState *us = ValkeyModule_DictGetC(
        engine_ctx->user_states, (void *)username, strlen(username), NULL);
    if (us)
        return us;

    us = createPerUserState(engine_ctx);
    ValkeyModule_DictSetC(engine_ctx->user_states, (void *)username,
                          strlen(username), us);
    return us;
}

static lua_State *createCompileScratchState(lua55EngineCtx *engine_ctx) {
    lua_State *lua = luaL_newstate();
    luaL_openlibs(lua);
    (void)engine_ctx;
    return lua;
}

static lua55EngineCtx *createEngineContext(ValkeyModuleCtx *ctx) {
    lua55EngineCtx *engine_ctx = ValkeyModule_Calloc(1, sizeof(*engine_ctx));

    get_version_info(ctx, &engine_ctx->redis_version,
                     &engine_ctx->redis_version_num, &engine_ctx->server_name,
                     &engine_ctx->valkey_version,
                     &engine_ctx->valkey_version_num);

    engine_ctx->lua_enable_insecure_api = 0;
    engine_ctx->next_func_id = 0;
    engine_ctx->next_lib_id = 0;

    engine_ctx->compile_lua = createCompileScratchState(engine_ctx);
    engine_ctx->user_states = ValkeyModule_CreateDict(NULL);
    engine_ctx->libraries = ValkeyModule_CreateDict(NULL);

    return engine_ctx;
}

static void destroyEngineContext(lua55EngineCtx *engine_ctx) {
    ValkeyModuleDictIter *iter =
        ValkeyModule_DictIteratorStartC(engine_ctx->user_states, "^", NULL, 0);

    char *key;
    size_t keylen;
    lua55PerUserState *us;
    while ((key = ValkeyModule_DictNextC(iter, &keylen, (void **)&us)) != NULL) {
        destroyPerUserState(us);
    }
    ValkeyModule_DictIteratorStop(iter);
    ValkeyModule_FreeDict(NULL, engine_ctx->user_states);

    ValkeyModuleDictIter *lib_iter =
        ValkeyModule_DictIteratorStartC(engine_ctx->libraries, "^", NULL, 0);
    lua55Library *lib;
    while (ValkeyModule_DictNextC(lib_iter, &keylen, (void **)&lib) != NULL) {
        ValkeyModule_Free(lib->code);
        ValkeyModule_Free(lib->name);
        ValkeyModule_Free(lib);
    }
    ValkeyModule_DictIteratorStop(lib_iter);
    ValkeyModule_FreeDict(NULL, engine_ctx->libraries);

    if (engine_ctx->compile_lua)
        lua_close(engine_ctx->compile_lua);

    ValkeyModule_Free(engine_ctx->redis_version);
    ValkeyModule_Free(engine_ctx->server_name);
    ValkeyModule_Free(engine_ctx->valkey_version);
    ValkeyModule_Free(engine_ctx);
}

static ValkeyModuleScriptingEngineMemoryInfo
lua55EngineGetMemoryInfo(ValkeyModuleCtx *module_ctx,
                         ValkeyModuleScriptingEngineCtx *engine_ctx_opaque,
                         ValkeyModuleScriptingEngineSubsystemType type) {
    VALKEYMODULE_NOT_USED(module_ctx);
    lua55EngineCtx *ctx = (lua55EngineCtx *)engine_ctx_opaque;
    ValkeyModuleScriptingEngineMemoryInfo mem_info = {0};

    ValkeyModuleDictIter *iter =
        ValkeyModule_DictIteratorStartC(ctx->user_states, "^", NULL, 0);

    char *key;
    size_t keylen;
    lua55PerUserState *us;
    while ((key = ValkeyModule_DictNextC(iter, &keylen, (void **)&us)) != NULL) {
        if ((type == VMSE_EVAL || type == VMSE_ALL) && us->eval_lua) {
            mem_info.used_memory += lua55Memory(us->eval_lua);
        }
        if ((type == VMSE_FUNCTION || type == VMSE_ALL) && us->function_lua) {
            mem_info.used_memory += lua55Memory(us->function_lua);
        }
    }
    ValkeyModule_DictIteratorStop(iter);

    if (ctx->compile_lua) {
        mem_info.used_memory += lua55Memory(ctx->compile_lua);
    }

    if (type == VMSE_FUNCTION || type == VMSE_ALL) {
        ValkeyModuleDictIter *lib_iter =
            ValkeyModule_DictIteratorStartC(ctx->libraries, "^", NULL, 0);
        lua55Library *lib;
        while (ValkeyModule_DictNextC(lib_iter, &keylen, (void **)&lib) != NULL) {
            mem_info.used_memory += lib->code_len;
            mem_info.used_memory += ValkeyModule_MallocSize(lib->code);
            mem_info.used_memory += ValkeyModule_MallocSize(lib->name);
            mem_info.used_memory += ValkeyModule_MallocSize(lib);
        }
        ValkeyModule_DictIteratorStop(lib_iter);
    }

    mem_info.engine_memory_overhead = ValkeyModule_MallocSize(engine_ctx_opaque);

    return mem_info;
}

static ValkeyModuleScriptingEngineCompiledFunction **
lua55EngineCompileCode(ValkeyModuleCtx *module_ctx,
                       ValkeyModuleScriptingEngineCtx *engine_ctx_opaque,
                       ValkeyModuleScriptingEngineSubsystemType type,
                       const char *code,
                       size_t code_len,
                       size_t timeout,
                       size_t *out_num_compiled_functions,
                       ValkeyModuleString **err) {
    lua55EngineCtx *ctx = (lua55EngineCtx *)engine_ctx_opaque;
    ValkeyModuleScriptingEngineCompiledFunction **functions = NULL;

    if (type == VMSE_EVAL) {
        lua_State *lua = ctx->compile_lua;

        if (luaL_loadbuffer(lua, code, code_len, "@user_script")) {
            *err = ValkeyModule_CreateStringPrintf(
                module_ctx, "Error compiling script (new function): %s",
                lua_tostring(lua, -1));
            lua_pop(lua, 1);
            return NULL;
        }

        ValkeyModule_Assert(lua_isfunction(lua, -1));
        lua_pop(lua, 1);

        lua55Function *func_data = ValkeyModule_Calloc(1, sizeof(*func_data));
        func_data->is_from_eval = 1;
        func_data->source.text = ValkeyModule_Alloc(code_len);
        memcpy(func_data->source.text, code, code_len);
        func_data->source.text_len = code_len;

        ValkeyModuleScriptingEngineCompiledFunction *func =
            ValkeyModule_Calloc(1, sizeof(*func));
        func->name = NULL;
        func->function = func_data;
        func->desc = NULL;
        func->f_flags = 0;

        *out_num_compiled_functions = 1;
        functions = ValkeyModule_Calloc(1, sizeof(*functions));
        *functions = func;
    } else {
        lua_State *scratch = luaL_newstate();
        lua55InitFunctionScratchState(ctx, scratch);

        functions = lua55FunctionLibraryCreate(scratch, code, code_len, timeout,
                                               out_num_compiled_functions, err);

        if (functions) {
            lua55Library *lib = ValkeyModule_Calloc(1, sizeof(*lib));
            lib->code = ValkeyModule_Alloc(code_len);
            memcpy(lib->code, code, code_len);
            lib->code_len = code_len;
            lib->lib_id = ++ctx->next_lib_id;
            lib->name = lua55_asprintf("lib_%lu", lib->lib_id);
            lib->ref_count = *out_num_compiled_functions;

            for (size_t i = 0; i < *out_num_compiled_functions; i++) {
                lua55Function *func = (lua55Function *)functions[i]->function;
                func->function_ref.lib_id = lib->lib_id;
            }

            ValkeyModule_DictSetC(ctx->libraries, (char *)&lib->lib_id,
                                  sizeof(lib->lib_id), lib);
        }

        lua_close(scratch);
    }

    if (functions) {
        for (size_t i = 0; i < *out_num_compiled_functions; i++) {
            lua55Function *fd = (lua55Function *)functions[i]->function;
            fd->func_id = ++ctx->next_func_id;
        }
    }

    return functions;
}

static void lua55EngineFunctionCall(
    ValkeyModuleCtx *module_ctx,
    ValkeyModuleScriptingEngineCtx *engine_ctx_opaque,
    ValkeyModuleScriptingEngineServerRuntimeCtx *server_ctx,
    ValkeyModuleScriptingEngineCompiledFunction *compiled_function,
    ValkeyModuleScriptingEngineSubsystemType type,
    ValkeyModuleString **keys,
    size_t nkeys,
    ValkeyModuleString **args,
    size_t nargs) {
    lua55EngineCtx *ctx = (lua55EngineCtx *)engine_ctx_opaque;

    ValkeyModuleString *username_str =
        ValkeyModule_GetCurrentUserName(module_ctx);
    const char *username = username_str ? ValkeyModule_StringPtrLen(username_str, NULL) : "default";

    lua55PerUserState *us = getOrCreateUserState(ctx, username);
    if (username_str) ValkeyModule_FreeString(NULL, username_str);

    lua_State *lua = (type == VMSE_EVAL) ? us->eval_lua : us->function_lua;
    lua55Function *func = (lua55Function *)compiled_function->function;

    lua_pushstring(lua, REGISTRY_FUNC_CACHE_NAME);
    lua_gettable(lua, LUA_REGISTRYINDEX);

    lua_pushlightuserdata(lua, (void *)(uintptr_t)func->func_id);
    lua_rawget(lua, -2);

    if (lua_isfunction(lua, -1)) {
        lua_remove(lua, -2);
    } else {
        lua_pop(lua, 1);

        const char *chunk_name =
            (type == VMSE_EVAL) ? "@user_script" : "@user_function";
        if (func->is_from_eval) {
            if (luaL_loadbuffer(lua, func->source.text, func->source.text_len,
                                chunk_name) != 0) {
                const char *errmsg = lua_tostring(lua, -1);
                ValkeyModule_ReplyWithErrorFormat(module_ctx,
                                                  "ERR Error loading script: %s",
                                                  errmsg ? errmsg : "unknown error");
                lua_pop(lua, 2);
                return;
            }
        } else {
            int nokey;
            lua55Library *lib = ValkeyModule_DictGetC(
                ctx->libraries, (char *)&func->function_ref.lib_id,
                sizeof(func->function_ref.lib_id), &nokey);

            if (lib == NULL || nokey) {
                ValkeyModule_ReplyWithErrorFormat(
                    module_ctx, "ERR Library for function %s not found",
                    func->function_ref.name);
                lua_pop(lua, 2);
                return;
            }

            lua_getfield(lua, LUA_REGISTRYINDEX, "__library_compiled");
            lua_pushinteger(lua, (lua_Integer)func->function_ref.lib_id);
            lua_gettable(lua, -2);
            int is_compiled = !lua_isnil(lua, -1);
            lua_pop(lua, 1);
            lua_pop(lua, 1);

            if (!is_compiled) {
                const char *lib_name;
                char lib_name_buf[64];
                if (strlen(lib->name) < sizeof(lib_name_buf)) {
                    strcpy(lib_name_buf, lib->name);
                    lib_name = lib_name_buf;
                } else {
                    lib_name = lib->name;
                }

                if (lua55CompileLibraryInUserState(lua, lib->code, lib->code_len,
                                                   lib_name) != 0) {
                    const char *errmsg = lua_tostring(lua, -1);
                    ValkeyModule_ReplyWithErrorFormat(
                        module_ctx, "ERR Error compiling library %s: %s", lib_name,
                        errmsg ? errmsg : "unknown error");
                    lua_pop(lua, 2);
                    return;
                }

                lua_getfield(lua, LUA_REGISTRYINDEX, "__library_compiled");
                lua_pushinteger(lua, (lua_Integer)func->function_ref.lib_id);
                lua_pushboolean(lua, 1);
                lua_settable(lua, -3);
                lua_pop(lua, 1);
            }

            lua_getfield(lua, LUA_REGISTRYINDEX, "__library_functions");
            lua_getfield(lua, -1, func->function_ref.name);

            if (lua_isnil(lua, -1) || !lua_isfunction(lua, -1)) {
                lua_pop(lua, 3);
                ValkeyModule_ReplyWithErrorFormat(
                    module_ctx, "ERR Function %s not found in library",
                    func->function_ref.name);
                return;
            }

            lua_remove(lua, -2);
        }

        lua_pushlightuserdata(lua, (void *)(uintptr_t)func->func_id);
        lua_pushvalue(lua, -2);
        lua_rawset(lua, -4);

        lua_remove(lua, -2);
    }

    lua_pushstring(lua, REGISTRY_ERROR_HANDLER_NAME);
    lua_gettable(lua, LUA_REGISTRYINDEX);

    lua_insert(lua, -2);

    lua55CallFunction(module_ctx, server_ctx, type, lua, keys, nkeys, args, nargs,
                      ctx->lua_enable_insecure_api);

    lua_pop(lua, 1);
}

static void lua55EngineFreeFunction(
    ValkeyModuleCtx *module_ctx,
    ValkeyModuleScriptingEngineCtx *engine_ctx_opaque,
    ValkeyModuleScriptingEngineSubsystemType type,
    ValkeyModuleScriptingEngineCompiledFunction *compiled_function) {
    VALKEYMODULE_NOT_USED(module_ctx);
    VALKEYMODULE_NOT_USED(type);
    lua55EngineCtx *ctx = (lua55EngineCtx *)engine_ctx_opaque;
    lua55Function *func = (lua55Function *)compiled_function->function;

    if (func->is_from_eval) {
        ValkeyModule_Free(func->source.text);
    } else {
        const char *func_name = func->function_ref.name;

        ValkeyModuleDictIter *iter =
            ValkeyModule_DictIteratorStartC(ctx->user_states, "^", NULL, 0);

        char *key;
        size_t keylen;
        lua55PerUserState *us;
        while ((key = ValkeyModule_DictNextC(iter, &keylen, (void **)&us)) !=
               NULL) {
            if (us->function_lua) {
                lua55RemoveFunctionFromUserState(us->function_lua, func_name);
            }
        }
        ValkeyModule_DictIteratorStop(iter);

        int nokey;
        lua55Library *lib = ValkeyModule_DictGetC(
            ctx->libraries, (char *)&func->function_ref.lib_id,
            sizeof(func->function_ref.lib_id), &nokey);

        if (lib && !nokey) {
            lib->ref_count--;
            if (lib->ref_count == 0) {
                ValkeyModule_Free(lib->code);
                ValkeyModule_Free(lib->name);
                ValkeyModule_Free(lib);
                ValkeyModule_DictDelC(ctx->libraries,
                                      (char *)&func->function_ref.lib_id,
                                      sizeof(func->function_ref.lib_id), NULL);
            }
        }

        ValkeyModule_Free(func->function_ref.name);
    }
    ValkeyModule_Free(func);

    if (compiled_function->name)
        ValkeyModule_FreeString(NULL, compiled_function->name);
    if (compiled_function->desc)
        ValkeyModule_FreeString(NULL, compiled_function->desc);
    ValkeyModule_Free(compiled_function);
}

static size_t lua55EngineFunctionMemoryOverhead(
    ValkeyModuleCtx *module_ctx,
    ValkeyModuleScriptingEngineCompiledFunction *compiled_function) {
    VALKEYMODULE_NOT_USED(module_ctx);
    lua55Function *func = (lua55Function *)compiled_function->function;
    size_t data_len = func->is_from_eval ? func->source.text_len
                                         : (strlen(func->function_ref.name) + 1);
    return data_len + ValkeyModule_MallocSize(func) +
           (compiled_function->name
                ? ValkeyModule_MallocSize(compiled_function->name)
                : 0) +
           (compiled_function->desc
                ? ValkeyModule_MallocSize(compiled_function->desc)
                : 0) +
           ValkeyModule_MallocSize(compiled_function);
}

typedef struct resetCtx {
    lua55PerUserState **states;
    size_t count;
} resetCtx;

static void resetLuaContextAsync(void *context) {
    resetCtx *rctx = context;
    for (size_t i = 0; i < rctx->count; i++) {
        destroyPerUserState(rctx->states[i]);
    }
    ValkeyModule_Free(rctx->states);
    ValkeyModule_Free(rctx);
}

static int isLuaInsecureAPIEnabled(ValkeyModuleCtx *module_ctx) {
    int result = 0;
    ValkeyModuleCallReply *reply = ValkeyModule_Call(
        module_ctx, "CONFIG", "ccE", "GET", "lua-enable-insecure-api");
    if (ValkeyModule_CallReplyType(reply) == VALKEYMODULE_REPLY_ERROR) {
        ValkeyModule_FreeCallReply(reply);
        return 0;
    }
    ValkeyModule_Assert(ValkeyModule_CallReplyType(reply) ==
                            VALKEYMODULE_REPLY_ARRAY &&
                        ValkeyModule_CallReplyLength(reply) == 2);
    ValkeyModuleCallReply *val = ValkeyModule_CallReplyArrayElement(reply, 1);
    ValkeyModule_Assert(ValkeyModule_CallReplyType(val) ==
                        VALKEYMODULE_REPLY_STRING);
    const char *val_str = ValkeyModule_CallReplyStringPtr(val, NULL);
    result = strncmp(val_str, "yes", 3) == 0;
    ValkeyModule_FreeCallReply(reply);
    return result;
}

static ValkeyModuleScriptingEngineCallableLazyEnvReset *
lua55EngineResetEnv(ValkeyModuleCtx *module_ctx,
                    ValkeyModuleScriptingEngineCtx *engine_ctx_opaque,
                    ValkeyModuleScriptingEngineSubsystemType type,
                    int async) {
    VALKEYMODULE_NOT_USED(module_ctx);
    lua55EngineCtx *ctx = (lua55EngineCtx *)engine_ctx_opaque;
    ValkeyModuleScriptingEngineCallableLazyEnvReset *callback = NULL;

    if (type == VMSE_FUNCTION || type == VMSE_ALL) {
        ValkeyModuleDictIter *lib_iter =
            ValkeyModule_DictIteratorStartC(ctx->libraries, "^", NULL, 0);
        char *key;
        size_t keylen;
        lua55Library *lib;
        while ((key = ValkeyModule_DictNextC(lib_iter, &keylen, (void **)&lib)) !=
               NULL) {
            ValkeyModule_Free(lib->code);
            ValkeyModule_Free(lib->name);
            ValkeyModule_Free(lib);
        }
        ValkeyModule_DictIteratorStop(lib_iter);
        ValkeyModule_FreeDict(NULL, ctx->libraries);
        ctx->libraries = ValkeyModule_CreateDict(NULL);

        ctx->next_lib_id = 0;
    }

    size_t n = 0;
    lua55PerUserState **old_states = NULL;

    ValkeyModuleDictIter *iter =
        ValkeyModule_DictIteratorStartC(ctx->user_states, "^", NULL, 0);

    char *key;
    size_t keylen;
    lua55PerUserState *us;

    while ((key = ValkeyModule_DictNextC(iter, &keylen, (void **)&us)) != NULL) {
        n++;
    }
    ValkeyModule_DictIteratorStop(iter);

    if (n > 0) {
        old_states = ValkeyModule_Calloc(n, sizeof(*old_states));
        size_t idx = 0;
        iter = ValkeyModule_DictIteratorStartC(ctx->user_states, "^", NULL, 0);
        while ((key = ValkeyModule_DictNextC(iter, &keylen, (void **)&us)) !=
               NULL) {
            if (type == VMSE_ALL) {
                old_states[idx++] = us;
            } else {
                lua55PerUserState *partial = ValkeyModule_Calloc(1, sizeof(*partial));
                if (type == VMSE_EVAL) {
                    partial->eval_lua = us->eval_lua;
                    us->eval_lua = createUserLuaState(ctx, VMSE_EVAL);
                } else {
                    partial->function_lua = us->function_lua;
                    us->function_lua = createUserLuaState(ctx, VMSE_FUNCTION);
                }
                old_states[idx++] = partial;
            }
        }
        ValkeyModule_DictIteratorStop(iter);

        if (type == VMSE_ALL) {
            ValkeyModule_FreeDict(NULL, ctx->user_states);
            ctx->user_states = ValkeyModule_CreateDict(NULL);
        }
    }

    if (async && n > 0) {
        resetCtx *rctx = ValkeyModule_Alloc(sizeof(*rctx));
        rctx->states = old_states;
        rctx->count = n;
        callback = ValkeyModule_Calloc(1, sizeof(*callback));
        *callback = (ValkeyModuleScriptingEngineCallableLazyEnvReset){
            .context = rctx,
            .engineLazyEnvResetCallback = resetLuaContextAsync,
        };
    } else if (n > 0) {
        for (size_t i = 0; i < n; i++) {
            destroyPerUserState(old_states[i]);
        }
        ValkeyModule_Free(old_states);
    }

    if (ctx->compile_lua) {
        lua_close(ctx->compile_lua);
        ctx->compile_lua = createCompileScratchState(ctx);
    }

    ctx->lua_enable_insecure_api = isLuaInsecureAPIEnabled(module_ctx);

    return callback;
}

static ValkeyModuleScriptingEngineDebuggerEnableRet lua55EngineDebuggerEnable(
    ValkeyModuleCtx *module_ctx,
    ValkeyModuleScriptingEngineCtx *engine_ctx_opaque,
    ValkeyModuleScriptingEngineSubsystemType type,
    const ValkeyModuleScriptingEngineDebuggerCommand **commands,
    size_t *commands_len) {
    VALKEYMODULE_NOT_USED(module_ctx);
    VALKEYMODULE_NOT_USED(engine_ctx_opaque);
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(commands);
    VALKEYMODULE_NOT_USED(commands_len);
    return VMSE_DEBUG_NOT_SUPPORTED;
}

static void
lua55EngineDebuggerDisable(ValkeyModuleCtx *module_ctx,
                           ValkeyModuleScriptingEngineCtx *engine_ctx_opaque,
                           ValkeyModuleScriptingEngineSubsystemType type) {
    VALKEYMODULE_NOT_USED(module_ctx);
    VALKEYMODULE_NOT_USED(engine_ctx_opaque);
    VALKEYMODULE_NOT_USED(type);
}

static void
lua55EngineDebuggerStart(ValkeyModuleCtx *module_ctx,
                         ValkeyModuleScriptingEngineCtx *engine_ctx_opaque,
                         ValkeyModuleScriptingEngineSubsystemType type,
                         ValkeyModuleString *source) {
    VALKEYMODULE_NOT_USED(module_ctx);
    VALKEYMODULE_NOT_USED(engine_ctx_opaque);
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(source);
}

static void
lua55EngineDebuggerEnd(ValkeyModuleCtx *module_ctx,
                       ValkeyModuleScriptingEngineCtx *engine_ctx_opaque,
                       ValkeyModuleScriptingEngineSubsystemType type) {
    VALKEYMODULE_NOT_USED(module_ctx);
    VALKEYMODULE_NOT_USED(engine_ctx_opaque);
    VALKEYMODULE_NOT_USED(type);
}

static ValkeyModuleString *engineNameGet(const char *name, void *privdata) {
    VALKEYMODULE_NOT_USED(name);
    VALKEYMODULE_NOT_USED(privdata);
    return engine_names[0];
}

static int engineNameSet(const char *name, ValkeyModuleString *val, void *privdata, ValkeyModuleString **err) {
    VALKEYMODULE_NOT_USED(name);
    VALKEYMODULE_NOT_USED(privdata);
    VALKEYMODULE_NOT_USED(err);

    size_t len;
    const char *orig = ValkeyModule_StringPtrLen(val, &len);

    char *copy = ValkeyModule_Alloc(len + 1);
    memcpy(copy, orig, len);
    copy[len] = '\0';

    for (int i = 0; i < engine_names_count; i++) {
        ValkeyModule_FreeString(NULL, engine_names[i]);
        engine_names[i] = NULL;
        engine_names_cstr[i] = NULL;
    }
    engine_names_count = 0;

    char *saveptr = NULL;
    char *token = strtok_r(copy, ",", &saveptr);
    while (token != NULL && engine_names_count < MAX_ENGINE_NAMES) {
        while (*token == ' ' || *token == '\t')
            token++;
        char *end = token + strlen(token) - 1;
        while (end > token && (*end == ' ' || *end == '\t'))
            end--;
        end[1] = '\0';

        if (strlen(token) > 0) {
            ValkeyModuleString *name_str =
                ValkeyModule_CreateString(NULL, token, strlen(token));
            engine_names[engine_names_count] = name_str;
            engine_names_cstr[engine_names_count] =
                ValkeyModule_StringPtrLen(name_str, NULL);
            engine_names_count++;
        }
        token = strtok_r(NULL, ",", &saveptr);
    }

    ValkeyModule_Free(copy);
    return VALKEYMODULE_OK;
}

static lua55EngineCtx *engine_ctx = NULL;

#define VKM_PUBLIC __attribute__((visibility("default")))

VKM_PUBLIC int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx,
                                   ValkeyModuleString **argv,
                                   int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, "lua55", 1, VALKEYMODULE_APIVER_1) ==
        VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    ValkeyModule_SetModuleOptions(
        ctx, VALKEYMODULE_OPTIONS_HANDLE_REPL_ASYNC_LOAD |
                 VALKEYMODULE_OPTIONS_HANDLE_ATOMIC_SLOT_MIGRATION |
                 VALKEYMODULE_OPTIONS_HANDLE_FORKLESS);

    if (ValkeyModule_RegisterStringConfig(ctx, "engine-name", DEFAULT_ENGINE_NAME,
                                          VALKEYMODULE_CONFIG_IMMUTABLE,
                                          engineNameGet, engineNameSet, NULL,
                                          NULL) == VALKEYMODULE_ERR) {
        ValkeyModule_Log(ctx, "warning", "Failed to register engine-name config");
        return VALKEYMODULE_ERR;
    }

    engine_ctx = createEngineContext(ctx);

    if (engine_names_count == 0) {
        engine_names[0] = ValkeyModule_CreateString(NULL, DEFAULT_ENGINE_NAME,
                                                    strlen(DEFAULT_ENGINE_NAME));
        engine_names_cstr[0] = ValkeyModule_StringPtrLen(engine_names[0], NULL);
        engine_names_count = 1;
    }

    if (ValkeyModule_LoadConfigs(ctx) == VALKEYMODULE_ERR) {
        ValkeyModule_Log(ctx, "warning", "Failed to load lua55 module configs");
        destroyEngineContext(engine_ctx);
        engine_ctx = NULL;
        for (int i = 0; i < engine_names_count; i++) {
            ValkeyModule_FreeString(NULL, engine_names[i]);
            engine_names[i] = NULL;
            engine_names_cstr[i] = NULL;
        }
        engine_names_count = 0;
        return VALKEYMODULE_ERR;
    }

    ValkeyModuleScriptingEngineMethods methods = {
        .version = VALKEYMODULE_SCRIPTING_ENGINE_ABI_VERSION,
        .compile_code = lua55EngineCompileCode,
        .free_function = lua55EngineFreeFunction,
        .call_function = lua55EngineFunctionCall,
        .get_function_memory_overhead = lua55EngineFunctionMemoryOverhead,
        .reset_env = lua55EngineResetEnv,
        .get_memory_info = lua55EngineGetMemoryInfo,
        .debugger_enable = lua55EngineDebuggerEnable,
        .debugger_disable = lua55EngineDebuggerDisable,
        .debugger_start = lua55EngineDebuggerStart,
        .debugger_end = lua55EngineDebuggerEnd,
    };

    for (int i = 0; i < engine_names_count; i++) {
        int result = ValkeyModule_RegisterScriptingEngine(ctx, engine_names_cstr[i],
                                                          engine_ctx, &methods);

        if (result == VALKEYMODULE_ERR) {
            ValkeyModule_Log(ctx, "warning",
                             "Failed to register '%s' scripting engine",
                             engine_names_cstr[i]);
            for (int j = 0; j < i; j++) {
                ValkeyModule_UnregisterScriptingEngine(ctx, engine_names_cstr[j]);
            }
            destroyEngineContext(engine_ctx);
            engine_ctx = NULL;
            return VALKEYMODULE_ERR;
        }
    }

    engine_ctx->lua_enable_insecure_api = isLuaInsecureAPIEnabled(ctx);

    for (int i = 0; i < engine_names_count; i++) {
        ValkeyModule_Log(
            ctx, "notice",
            "Lua 5.5 scripting engine registered as '%s' (per-user isolation)",
            engine_names_cstr[i]);
    }

    return VALKEYMODULE_OK;
}

VKM_PUBLIC int ValkeyModule_OnUnload(ValkeyModuleCtx *ctx) {
    for (int i = 0; i < engine_names_count; i++) {
        if (ValkeyModule_UnregisterScriptingEngine(ctx, engine_names_cstr[i]) !=
            VALKEYMODULE_OK) {
            ValkeyModule_Log(ctx, "error", "Failed to unregister Lua 5.5 engine '%s'",
                             engine_names_cstr[i]);
        }
    }

    destroyEngineContext(engine_ctx);
    engine_ctx = NULL;

    for (int i = 0; i < engine_names_count; i++) {
        ValkeyModule_FreeString(NULL, engine_names[i]);
        engine_names[i] = NULL;
        engine_names_cstr[i] = NULL;
    }
    engine_names_count = 0;

    ValkeyModule_Log(ctx, "notice", "Lua 5.5 scripting engine unloaded");

    return VALKEYMODULE_OK;
}
