/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Shared scripting functionality for the Lua 5.5 engine module.
 * Provides:
 *   - Lua state initialization with server API registration
 *   - server.call()/server.pcall() implementation
 *   - Reply conversion (Lua <-> RESP)
 *   - Execution hooks (timeout/kill detection)
 *   - Library compilation helpers
 */

#ifndef __SCRIPT_LUA55_H_
#define __SCRIPT_LUA55_H_

#include "engine_structs.h"
#include "valkeymodule.h"

#define C_OK 0
#define C_ERR -1

typedef struct lua_State lua_State;

#define REGISTRY_RUN_CTX_NAME "__RUN_CTX__"
#define REGISTRY_MODULE_CTX_NAME "__MODULE_CTX__"
#define REDIS_API_NAME "redis"
#define SERVER_API_NAME "server"

#define LUA_HOOK_CHECK_INTERVAL 100000
#define LUA_GC_CYCLE_PERIOD 50
#define LUA_FULL_GC_CYCLE 500

typedef struct errorInfo {
    char *msg;
    char *source;
    char *line;
    int ignore_err_stats_update;
} errorInfo;

void lua55RegisterServerAPI(lua55EngineCtx *ctx, lua_State *lua);
void lua55RegisterLogFunction(lua_State *lua);
void lua55RegisterVersion(lua55EngineCtx *ctx, lua_State *lua);

void lua55PushError(lua_State *lua, const char *error);
int lua55Error(lua_State *lua);

void lua55SaveOnRegistry(lua_State *lua, const char *name, void *ptr);
void *lua55GetFromRegistry(lua_State *lua, const char *name);

void lua55CallFunction(ValkeyModuleCtx *ctx,
                       ValkeyModuleScriptingEngineServerRuntimeCtx *r_ctx,
                       ValkeyModuleScriptingEngineSubsystemType type,
                       lua_State *lua,
                       ValkeyModuleString **keys,
                       size_t nkeys,
                       ValkeyModuleString **args,
                       size_t nargs,
                       int lua_enable_insecure_api);

void lua55ExtractErrorInformation(lua_State *lua, errorInfo *err_info);
void lua55ErrorInformationDiscard(errorInfo *err_info);

unsigned long lua55Memory(lua_State *lua);

int lua55CompileLibraryInUserState(lua_State *lua, const char *code, size_t code_len, const char *library_name);

void lua55RemoveFunctionFromUserState(lua_State *lua,
                                      const char *function_name);

int lua55UserStateRegisterFunction(lua_State *lua);

char *lua55_strcpy(const char *str);
char *lua55_asprintf(char const *fmt, ...);
#endif
