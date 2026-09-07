/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * FUNCTION subsystem support for the Lua 5.5 engine module.
 *
 * In the per-user architecture, compile_code uses a scratch lua_State to
 * run the library top-level code (which calls server.register_function),
 * then stores the library source code and function metadata.
 */

#ifndef _FUNCTION_LUA55_H_
#define _FUNCTION_LUA55_H_

#include "engine_structs.h"
#include "valkeymodule.h"

ValkeyModuleScriptingEngineCompiledFunction **
lua55FunctionLibraryCreate(lua_State *lua, const char *code, size_t code_len, size_t timeout, size_t *out_num_compiled_functions, ValkeyModuleString **err);

void lua55InitFunctionScratchState(lua55EngineCtx *ctx, lua_State *lua);

#endif
