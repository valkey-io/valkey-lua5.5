/*
 * Copyright (c) 2009-2021, Redis Ltd.
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Core shared scripting logic for the Lua 5.5 engine module.
 * This is a close adaptation of the built-in Lua engine's script_lua.c
 * but using Lua 5.5 instead.
 */

#include "script_lua55.h"
#include "engine_structs.h"
#include "fpconv_dtoa.h"
#include "rand.h"
#include "sha1.h"
#include "valkeymodule.h"

#include <errno.h>
#include <ctype.h>
#include <lauxlib.h>
#include <limits.h>
#include <lua.h>
#include <lualib.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define PROPAGATE_NONE 0
#define PROPAGATE_AOF 1
#define PROPAGATE_REPL 2

#define LL_DEBUG 0
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3

typedef struct lua55FuncCallCtx {
    lua55EngineCtx *engine_ctx;
    ValkeyModuleCtx *module_ctx;
    ValkeyModuleScriptingEngineServerRuntimeCtx *run_ctx;
    ValkeyModuleScriptingEngineSubsystemType type;
    int replication_flags;
    int resp;
    int lua_enable_insecure_api;
} lua55FuncCallCtx;

typedef uint64_t monotime;

monotime getMonotonicUs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

uint64_t elapsedUs(monotime start_time) {
    return getMonotonicUs() - start_time;
}

uint64_t elapsedMs(monotime start_time) {
    return elapsedUs(start_time) / 1000;
}

static int server_math_random(lua_State *L);
static int server_math_randomseed(lua_State *L);

/* Cap on Lua reply nesting depth. Beyond this, we emit a single error
 * frame instead of recursing further, so we never write a postponed array
 * header without matching contents and the RESP stream stays well-formed. */
#define LUA55_MAX_REPLY_DEPTH 256

static void lua55ReplyToServerReplyDepth(ValkeyModuleCtx *ctx, int resp_version, lua_State *lua, int depth);

static inline void lua55ReplyToServerReply(ValkeyModuleCtx *ctx,
                                           int resp_version,
                                           lua_State *lua) {
    lua55ReplyToServerReplyDepth(ctx, resp_version, lua, 0);
}

/*
 * Save the give pointer on Lua registry, used to save the Lua context and
 * function context so we can retrieve them from lua_State.
 */
void lua55SaveOnRegistry(lua_State *lua, const char *name, void *ptr) {
    lua_pushstring(lua, name);
    if (ptr) {
        lua_pushlightuserdata(lua, ptr);
    } else {
        lua_pushnil(lua);
    }
    lua_settable(lua, LUA_REGISTRYINDEX);
}

/*
 * Get a saved pointer from registry
 */
void *lua55GetFromRegistry(lua_State *lua, const char *name) {
    lua_pushstring(lua, name);
    lua_gettable(lua, LUA_REGISTRYINDEX);

    if (lua_isnil(lua, -1)) {
        lua_pop(lua, 1);
        return NULL;
    }
    ValkeyModule_Assert(lua_islightuserdata(lua, -1));

    void *ptr = (void *)lua_topointer(lua, -1);
    ValkeyModule_Assert(ptr);

    lua_pop(lua, 1);

    return ptr;
}

char *lua55_asprintf(char const *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    size_t str_len = vsnprintf(NULL, 0, fmt, args) + 1;
    va_end(args);

    char *str = ValkeyModule_Alloc(str_len);

    va_start(args, fmt);
    vsnprintf(str, str_len, fmt, args);
    va_end(args);

    return str;
}

char *lua55_strcpy(const char *str) {
    size_t len = strlen(str);
    char *res = ValkeyModule_Alloc(len + 1);
    memcpy(res, str, len + 1);
    return res;
}

char *lua55_strncpy(const char *str, size_t len) {
    char *res = ValkeyModule_Alloc(len + 1);
    memcpy(res, str, len);
    res[len] = '\0';
    return res;
}

static char *lua55_strtrim(char *s, const char *cset) {
    char *end, *sp, *ep;
    size_t len;

    sp = s;
    ep = end = s + strlen(s) - 1;
    while (sp <= end && strchr(cset, *sp))
        sp++;
    while (ep > sp && strchr(cset, *ep))
        ep--;
    len = (ep - sp) + 1;
    if (s != sp)
        memmove(s, sp, len);
    s[len] = '\0';
    return s;
}

/* This function is used in order to push an error on the Lua stack in the
 * format used by luaPcall to return errors, which is a lua table
 * with an "err" field set to the error string including the error code.
 * Note that this table is never a valid reply by proper commands,
 * since the returned tables are otherwise always indexed by integers, never by
 * strings.
 *
 * The function takes ownership on the given err_buffer. */
static void lua55PushErrorBuff(lua_State *lua, const char *err_buffer) {
    char *msg;
    char *final_msg = NULL;

    if (err_buffer[0] == '-') {
        char *err_msg = strstr(err_buffer, " ");
        if (!err_msg) {
            msg = lua55_strcpy(err_buffer + 1);
            final_msg = lua55_asprintf("ERR %s", msg);
        } else {
            char *err_copy = lua55_strcpy(err_buffer);
            char *space = strstr(err_copy, " ");
            *space = '\0';
            msg = lua55_strcpy(space + 1);
            msg = lua55_strtrim(msg, "\r\n");
            final_msg = lua55_asprintf("%s %s", err_copy + 1, msg);
            ValkeyModule_Free(err_copy);
        }
    } else {
        msg = lua55_strcpy(err_buffer);
        msg = lua55_strtrim(msg, "\r\n");
        final_msg = lua55_asprintf("ERR %s", msg);
    }

    lua_newtable(lua);
    lua_pushstring(lua, "err");
    lua_pushstring(lua, final_msg);
    lua_settable(lua, -3);

    ValkeyModule_Free(msg);
    ValkeyModule_Free(final_msg);
}

void lua55PushError(lua_State *lua, const char *error) {
    lua55PushErrorBuff(lua, error);
}

int lua55Error(lua_State *lua) {
    return lua_error(lua);
}

/* ---------------------------------------------------------------------------
 * Server reply to Lua type conversion functions.
 * ------------------------------------------------------------------------- */

#define LUA55_REPLY_OK 0
#define LUA55_REPLY_TOO_DEEP 1
#define LUA55_REPLY_TOO_BIG 2
#define LUA55_REPLY_BAD_PROTO 3

#define LUA55_MAX_ARRAY_SIZE ((size_t)INT_MAX)
#define LUA55_MAX_HASH_SIZE ((size_t)1 << 30)

static void lua55ProcessReplyError(const char *err, int err_no, lua_State *lua);

static int lua55ReplyFailed(lua_State *lua, int base_top, int failed) {
    lua_settop(lua, base_top);
    switch (failed) {
    case LUA55_REPLY_BAD_PROTO:
        lua_pushnil(lua);
        return 0;
    case LUA55_REPLY_TOO_BIG:
        lua55PushError(lua, "reply too large to convert to a Lua value");
        return 1;
    default:
        lua55PushError(lua, "reached lua stack limit");
        return 1;
    }
}

static int callReplyIsStatus(ValkeyModuleCallReply *reply) {
    size_t len = 0, attr_len = 0;
    const char *proto = ValkeyModule_CallReplyProto(reply, &len);
    ValkeyModuleCallReply *attr = ValkeyModule_CallReplyAttribute(reply);
    if (attr) ValkeyModule_CallReplyProto(attr, &attr_len);
    return proto && attr_len < len && proto[attr_len] == '+';
}

static int callReplyToLuaType(lua_State *lua, ValkeyModuleCallReply *reply, int resp, int depth) {
    if (!lua_checkstack(lua, 6)) return LUA55_REPLY_TOO_DEEP;

    int type = ValkeyModule_CallReplyType(reply);
    if (type == VALKEYMODULE_REPLY_STRING && callReplyIsStatus(reply))
        type = VALKEYMODULE_REPLY_SIMPLE_STRING;
    if ((type == VALKEYMODULE_REPLY_ARRAY || type == VALKEYMODULE_REPLY_MAP ||
         type == VALKEYMODULE_REPLY_SET) &&
        depth >= LUA55_MAX_REPLY_DEPTH)
        return LUA55_REPLY_TOO_DEEP;

    switch (type) {
    case VALKEYMODULE_REPLY_STRING: {
        size_t len = 0;
        const char *str = ValkeyModule_CallReplyStringPtr(reply, &len);
        lua_pushlstring(lua, str, len);
        break;
    }
    case VALKEYMODULE_REPLY_SIMPLE_STRING: {
        size_t len = 0;
        const char *str = ValkeyModule_CallReplyStringPtr(reply, &len);
        lua_newtable(lua);
        lua_pushstring(lua, "ok");
        lua_pushlstring(lua, str, len);
        lua_rawset(lua, -3);
        break;
    }
    case VALKEYMODULE_REPLY_INTEGER:
        lua_pushinteger(lua, (lua_Integer)ValkeyModule_CallReplyInteger(reply));
        break;
    case VALKEYMODULE_REPLY_ARRAY: {
        size_t items = ValkeyModule_CallReplyLength(reply);
        if (items > LUA55_MAX_ARRAY_SIZE) return LUA55_REPLY_TOO_BIG;
        lua_createtable(lua, (int)items, 0);
        for (size_t i = 0; i < items; i++) {
            ValkeyModuleCallReply *val = ValkeyModule_CallReplyArrayElement(reply, i);
            int rc = callReplyToLuaType(lua, val, resp, depth + 1);
            if (rc != LUA55_REPLY_OK) return rc;
            lua_rawseti(lua, -2, (int)(i + 1));
        }
        break;
    }
    case VALKEYMODULE_REPLY_NULL:
    case VALKEYMODULE_REPLY_ARRAY_NULL:
        if (resp == 2) {
            lua_pushboolean(lua, 0);
        } else {
            lua_pushnil(lua);
        }
        break;
    case VALKEYMODULE_REPLY_MAP: {
        size_t items = ValkeyModule_CallReplyLength(reply);
        if (items > LUA55_MAX_HASH_SIZE) return LUA55_REPLY_TOO_BIG;
        lua_newtable(lua);
        lua_pushstring(lua, "map");
        lua_createtable(lua, 0, (int)items);
        for (size_t i = 0; i < items; i++) {
            ValkeyModuleCallReply *key = NULL;
            ValkeyModuleCallReply *val = NULL;
            ValkeyModule_CallReplyMapElement(reply, i, &key, &val);
            int rc = callReplyToLuaType(lua, key, resp, depth + 1);
            if (rc == LUA55_REPLY_OK) rc = callReplyToLuaType(lua, val, resp, depth + 1);
            if (rc != LUA55_REPLY_OK) return rc;
            if (lua_isnil(lua, -2)) {
                lua_pop(lua, 2);
            } else {
                lua_rawset(lua, -3);
            }
        }
        lua_rawset(lua, -3);
        break;
    }
    case VALKEYMODULE_REPLY_SET: {
        size_t items = ValkeyModule_CallReplyLength(reply);
        if (items > LUA55_MAX_HASH_SIZE) return LUA55_REPLY_TOO_BIG;
        lua_newtable(lua);
        lua_pushstring(lua, "set");
        lua_createtable(lua, 0, (int)items);
        for (size_t i = 0; i < items; i++) {
            ValkeyModuleCallReply *val = ValkeyModule_CallReplySetElement(reply, i);
            int rc = callReplyToLuaType(lua, val, resp, depth + 1);
            if (rc != LUA55_REPLY_OK) return rc;
            if (lua_isnil(lua, -1)) {
                lua_pop(lua, 1);
                continue;
            }
            lua_pushboolean(lua, 1);
            lua_rawset(lua, -3);
        }
        lua_rawset(lua, -3);
        break;
    }
    case VALKEYMODULE_REPLY_BOOL:
        lua_pushboolean(lua, ValkeyModule_CallReplyBool(reply));
        break;
    case VALKEYMODULE_REPLY_DOUBLE:
        lua_newtable(lua);
        lua_pushstring(lua, "double");
        lua_pushnumber(lua, ValkeyModule_CallReplyDouble(reply));
        lua_rawset(lua, -3);
        break;
    case VALKEYMODULE_REPLY_BIG_NUMBER: {
        size_t len = 0;
        const char *str = ValkeyModule_CallReplyBigNumber(reply, &len);
        lua_newtable(lua);
        lua_pushstring(lua, "big_number");
        lua_pushlstring(lua, str, len);
        lua_rawset(lua, -3);
        break;
    }
    case VALKEYMODULE_REPLY_VERBATIM_STRING: {
        size_t len = 0;
        const char *format = NULL;
        const char *str = ValkeyModule_CallReplyVerbatim(reply, &len, &format);
        lua_newtable(lua);
        lua_pushstring(lua, "verbatim_string");
        lua_newtable(lua);
        lua_pushstring(lua, "string");
        lua_pushlstring(lua, str, len);
        lua_rawset(lua, -3);
        lua_pushstring(lua, "format");
        lua_pushlstring(lua, format, 3);
        lua_rawset(lua, -3);
        lua_rawset(lua, -3);
        break;
    }
    case VALKEYMODULE_REPLY_ERROR: {
        size_t len = 0;
        const char *str = ValkeyModule_CallReplyStringPtr(reply, &len);
        char *err = lua55_strncpy(str, len);
        lua55ProcessReplyError(err, 0, lua);
        ValkeyModule_Free(err);
        break;
    }
    default:
        lua_pushnil(lua);
        break;
    }
    return LUA55_REPLY_OK;
}

/* ---------------------------------------------------------------------------
 * Lua reply to server reply conversion functions.
 * ------------------------------------------------------------------------- */

static char *strmapchars(char *s, const char *from, const char *to, size_t setlen) {
    size_t j, i, l = strlen(s);

    for (j = 0; j < l; j++) {
        for (i = 0; i < setlen; i++) {
            if (s[j] == from[i]) {
                s[j] = to[i];
                break;
            }
        }
    }
    return s;
}

static char *copy_string_from_lua_stack(lua_State *lua) {
    size_t len;
    const char *str = lua_tolstring(lua, -1, &len);

    if (!str || len == 0) {
        char *res = ValkeyModule_Alloc(1);
        res[0] = '\0';
        return res;
    }

    char *res = ValkeyModule_Alloc(len + 1);
    memcpy(res, str, len);
    res[len] = 0;
    return res;
}

/* Reply to client converting the top element in the Lua stack to a
 * server reply. As a side effect the element is consumed from the stack. */
static void lua55ReplyToServerReplyDepth(ValkeyModuleCtx *ctx, int resp_version, lua_State *lua, int depth) {
    int t = lua_type(lua, -1);

    if (depth >= LUA55_MAX_REPLY_DEPTH || !lua_checkstack(lua, 4)) {
        ValkeyModule_ReplyWithError(ctx, "ERR reached lua stack limit");
        lua_pop(lua, 1);
        return;
    }

    switch (t) {
    case LUA_TSTRING: {
        size_t slen = 0;
        const char *sptr = lua_tolstring(lua, -1, &slen);
        ValkeyModule_ReplyWithStringBuffer(ctx, sptr, slen);
        break;
    }
    case LUA_TBOOLEAN:
        if (resp_version == 2) {
            int b = lua_toboolean(lua, -1);
            if (b) {
                ValkeyModule_ReplyWithLongLong(ctx, 1);
            } else {
                ValkeyModule_ReplyWithNull(ctx);
            }
        } else {
            ValkeyModule_ReplyWithBool(ctx, lua_toboolean(lua, -1));
        }
        break;
    case LUA_TNUMBER:
        /* In Lua 5.3+ numbers split into integer and float subtypes; both
         * report as LUA_TNUMBER. We coerce to long long either way to mirror
         * historical Redis Lua behaviour where the public reply is integer. */
        ValkeyModule_ReplyWithLongLong(ctx, (long long)lua_tonumber(lua, -1));
        break;
    case LUA_TTABLE:
        /* Error reply? */
        lua_pushstring(lua, "err");
        lua_rawget(lua, -2);
        t = lua_type(lua, -1);
        if (t == LUA_TSTRING) {
            lua_pop(lua, 1);
            errorInfo err_info = {0};
            lua55ExtractErrorInformation(lua, &err_info);
            ValkeyModule_ReplyWithCustomErrorFormat(
                ctx, !err_info.ignore_err_stats_update, "%s", err_info.msg);
            lua55ErrorInformationDiscard(&err_info);
            lua_pop(lua, 1);
            return;
        }
        lua_pop(lua, 1);

        /* Status reply? */
        lua_pushstring(lua, "ok");
        lua_rawget(lua, -2);
        t = lua_type(lua, -1);
        if (t == LUA_TSTRING) {
            char *ok = copy_string_from_lua_stack(lua);
            strmapchars(ok, "\r\n", "  ", 2);
            ValkeyModule_ReplyWithSimpleString(ctx, ok);
            ValkeyModule_Free(ok);
            lua_pop(lua, 2);
            return;
        }
        lua_pop(lua, 1);

        /* Double reply? */
        lua_pushstring(lua, "double");
        lua_rawget(lua, -2);
        t = lua_type(lua, -1);
        if (t == LUA_TNUMBER) {
            ValkeyModule_ReplyWithDouble(ctx, lua_tonumber(lua, -1));
            lua_pop(lua, 2);
            return;
        }
        lua_pop(lua, 1);

        /* Big number reply? */
        lua_pushstring(lua, "big_number");
        lua_rawget(lua, -2);
        t = lua_type(lua, -1);
        if (t == LUA_TSTRING) {
            char *big_num = copy_string_from_lua_stack(lua);
            strmapchars(big_num, "\r\n", "  ", 2);
            ValkeyModule_ReplyWithBigNumber(ctx, big_num, strlen(big_num));
            ValkeyModule_Free(big_num);
            lua_pop(lua, 2);
            return;
        }
        lua_pop(lua, 1);

        /* Verbatim reply? */
        lua_pushstring(lua, "verbatim_string");
        lua_rawget(lua, -2);
        t = lua_type(lua, -1);
        if (t == LUA_TTABLE) {
            lua_pushstring(lua, "format");
            lua_rawget(lua, -2);
            t = lua_type(lua, -1);
            if (t == LUA_TSTRING) {
                char *format = (char *)lua_tostring(lua, -1);
                lua_pushstring(lua, "string");
                lua_rawget(lua, -3);
                t = lua_type(lua, -1);
                if (t == LUA_TSTRING) {
                    size_t len;
                    char *str = (char *)lua_tolstring(lua, -1, &len);
                    ValkeyModule_ReplyWithVerbatimStringType(ctx, str, len, format);
                    lua_pop(lua, 4);
                    return;
                }
                lua_pop(lua, 1);
            }
            lua_pop(lua, 1);
        }
        lua_pop(lua, 1);

        /* Map reply? */
        lua_pushstring(lua, "map");
        lua_rawget(lua, -2);
        t = lua_type(lua, -1);
        if (t == LUA_TTABLE) {
            int maplen = 0;
            ValkeyModule_ReplyWithMap(ctx, VALKEYMODULE_POSTPONED_LEN);
            lua_pushnil(lua);
            while (lua_next(lua, -2)) {
                lua_pushvalue(lua, -2);
                lua55ReplyToServerReplyDepth(ctx, resp_version, lua, depth + 1);
                lua55ReplyToServerReplyDepth(ctx, resp_version, lua, depth + 1);
                maplen++;
            }
            ValkeyModule_ReplySetMapLength(ctx, maplen);
            lua_pop(lua, 2);
            return;
        }
        lua_pop(lua, 1);

        /* Set reply? */
        lua_pushstring(lua, "set");
        lua_rawget(lua, -2);
        t = lua_type(lua, -1);
        if (t == LUA_TTABLE) {
            int setlen = 0;
            ValkeyModule_ReplyWithSet(ctx, VALKEYMODULE_POSTPONED_LEN);
            lua_pushnil(lua);
            while (lua_next(lua, -2)) {
                lua_pop(lua, 1);
                lua_pushvalue(lua, -1);
                lua55ReplyToServerReplyDepth(ctx, resp_version, lua, depth + 1);
                setlen++;
            }
            ValkeyModule_ReplySetSetLength(ctx, setlen);
            lua_pop(lua, 2);
            return;
        }
        lua_pop(lua, 1);

        /* Array reply. */
        ValkeyModule_ReplyWithArray(ctx, VALKEYMODULE_POSTPONED_LEN);
        int j = 1, mbulklen = 0;
        while (1) {
            lua_pushnumber(lua, j++);
            lua_rawget(lua, -2);
            t = lua_type(lua, -1);
            if (t == LUA_TNIL) {
                lua_pop(lua, 1);
                break;
            }
            lua55ReplyToServerReplyDepth(ctx, resp_version, lua, depth + 1);
            mbulklen++;
        }
        ValkeyModule_ReplySetArrayLength(ctx, mbulklen);
        break;
    default:
        ValkeyModule_ReplyWithNull(ctx);
    }
    lua_pop(lua, 1);
}

/* ---------------------------------------------------------------------------
 * Lua server.* functions implementations.
 * ------------------------------------------------------------------------- */

static uint32_t digits10(uint64_t v) {
    if (v < 10)
        return 1;
    if (v < 100)
        return 2;
    if (v < 1000)
        return 3;
    if (v < 1000000000000UL) {
        if (v < 100000000UL) {
            if (v < 1000000) {
                if (v < 10000)
                    return 4;
                return 5 + (v >= 100000);
            }
            return 7 + (v >= 10000000UL);
        }
        if (v < 10000000000UL) {
            return 9 + (v >= 1000000000UL);
        }
        return 11 + (v >= 100000000000UL);
    }
    return 12 + digits10(v / 1000000000000UL);
}

static int ull2string(char *dst, size_t dstlen, unsigned long long value) {
    static const char digits[201] = "0001020304050607080910111213141516171819"
                                    "2021222324252627282930313233343536373839"
                                    "4041424344454647484950515253545556575859"
                                    "6061626364656667686970717273747576777879"
                                    "8081828384858687888990919293949596979899";

    uint32_t length = digits10(value);
    if (length >= dstlen)
        goto err;

    uint32_t next = length - 1;
    dst[next + 1] = '\0';
    while (value >= 100) {
        int const i = (value % 100) * 2;
        value /= 100;
        dst[next] = digits[i + 1];
        dst[next - 1] = digits[i];
        next -= 2;
    }

    if (value < 10) {
        dst[next] = '0' + (uint32_t)value;
    } else {
        int i = (uint32_t)value * 2;
        dst[next] = digits[i + 1];
        dst[next - 1] = digits[i];
    }
    return length;
err:
    if (dstlen > 0)
        dst[0] = '\0';
    return 0;
}

static int ll2string(char *dst, size_t dstlen, long long svalue) {
    unsigned long long value;
    int negative = 0;

    if (svalue < 0) {
        if (svalue != LLONG_MIN) {
            value = -svalue;
        } else {
            value = ((unsigned long long)LLONG_MAX) + 1;
        }
        if (dstlen < 2)
            goto err;
        negative = 1;
        dst[0] = '-';
        dst++;
        dstlen--;
    } else {
        value = svalue;
    }

    int length = ull2string(dst, dstlen, value);
    if (length == 0)
        return 0;
    return length + negative;

err:
    if (dstlen > 0)
        dst[0] = '\0';
    return 0;
}

static int double2ll(double d, long long *out) {
#if (__DBL_MANT_DIG__ >= 52) && (__DBL_MANT_DIG__ <= 63) && \
    (LLONG_MAX == 0x7fffffffffffffffLL)
    if (d < (double)(-LLONG_MAX / 2) || d > (double)(LLONG_MAX / 2))
        return 0;
    long long ll = d;
    if (ll == d) {
        *out = ll;
        return 1;
    }
#else
    VALKEYMODULE_NOT_USED(d);
    VALKEYMODULE_NOT_USED(out);
#endif
    return 0;
}

static lua55FuncCallCtx *lua55GetRunCtx(lua_State *lua, const char *api) {
    lua55FuncCallCtx *rctx = lua55GetFromRegistry(lua, REGISTRY_RUN_CTX_NAME);
    if (!rctx) {
        char *msg = lua55_asprintf("%s is not available while loading a library", api);
        lua55PushError(lua, msg);
        ValkeyModule_Free(msg);
    }
    return rctx;
}

static ValkeyModuleString *lua55ArgvCacheGet(lua55EngineCtx *ectx,
                                             int j,
                                             const char *s,
                                             size_t len) {
    ValkeyModuleString *cached = ectx->argv_cache[j];
    if (cached) {
        size_t cached_len = 0;
        const char *cached_s = ValkeyModule_StringPtrLen(cached, &cached_len);
        if (cached_len != len || memcmp(cached_s, s, len) != 0) {
            ValkeyModule_FreeString(NULL, cached);
            cached = NULL;
        }
    }
    if (!cached) {
        cached = ValkeyModule_CreateString(NULL, s, len);
        ectx->argv_cache[j] = cached;
    }
    ValkeyModule_RetainString(NULL, cached);
    return cached;
}

static int lua55ArgvCacheAllowed(lua55EngineCtx *ectx, lua_State *lua) {
    if (!ectx->core_commands || lua_type(lua, 1) != LUA_TSTRING) return 0;
    size_t len = 0;
    const char *name = lua_tolstring(lua, 1, &len);
    if (len == 0 || len > LUA55_ARGV_CACHE_MAX_LEN) return 0;

    if (ectx->argv_cache[0]) {
        size_t cached_len = 0;
        const char *cached = ValkeyModule_StringPtrLen(ectx->argv_cache[0], &cached_len);
        if (cached_len == len && memcmp(cached, name, len) == 0) return 1;
    }

    char lower[LUA55_ARGV_CACHE_MAX_LEN];
    for (size_t i = 0; i < len; i++) lower[i] = (char)tolower((unsigned char)name[i]);
    int nokey = 1;
    ValkeyModule_DictGetC(ectx->core_commands, lower, len, &nokey);
    return !nokey;
}

static int lua55CommandListApply(ValkeyModuleDict *names, ValkeyModuleCallReply *reply, int add) {
    if (!reply || ValkeyModule_CallReplyType(reply) != VALKEYMODULE_REPLY_ARRAY) return 0;
    size_t n = ValkeyModule_CallReplyLength(reply);
    for (size_t i = 0; i < n; i++) {
        size_t len = 0;
        const char *name =
            ValkeyModule_CallReplyStringPtr(ValkeyModule_CallReplyArrayElement(reply, i), &len);
        if (!name || len == 0 || len > LUA55_ARGV_CACHE_MAX_LEN || memchr(name, '|', len)) continue;
        char lower[LUA55_ARGV_CACHE_MAX_LEN];
        for (size_t k = 0; k < len; k++) lower[k] = (char)tolower((unsigned char)name[k]);
        if (add) {
            ValkeyModule_DictSetC(names, lower, len, (void *)1);
        } else {
            ValkeyModule_DictDelC(names, lower, len, NULL);
        }
    }
    return 1;
}

ValkeyModuleDict *lua55CoreCommandsCreate(ValkeyModuleCtx *ctx) {
    ValkeyModuleDict *core = ValkeyModule_CreateDict(NULL);
    ValkeyModuleCallReply *all = ValkeyModule_Call(ctx, "COMMAND", "c", "LIST");
    int ok = lua55CommandListApply(core, all, 1);
    if (all) ValkeyModule_FreeCallReply(all);

    ValkeyModuleCallReply *mods = ok ? ValkeyModule_Call(ctx, "MODULE", "c", "LIST") : NULL;
    ok = mods && ValkeyModule_CallReplyType(mods) == VALKEYMODULE_REPLY_ARRAY;
    size_t nmods = ok ? ValkeyModule_CallReplyLength(mods) : 0;
    for (size_t i = 0; ok && i < nmods; i++) {
        ValkeyModuleCallReply *info = ValkeyModule_CallReplyArrayElement(mods, i);
        size_t nfields = ValkeyModule_CallReplyLength(info);
        const char *name = NULL;
        size_t name_len = 0;
        for (size_t j = 0; j + 1 < nfields && !name; j += 2) {
            size_t key_len = 0;
            const char *key =
                ValkeyModule_CallReplyStringPtr(ValkeyModule_CallReplyArrayElement(info, j), &key_len);
            if (key && key_len == 4 && memcmp(key, "name", 4) == 0)
                name = ValkeyModule_CallReplyStringPtr(ValkeyModule_CallReplyArrayElement(info, j + 1),
                                                       &name_len);
        }
        if (!name) {
            ok = 0;
            break;
        }
        ValkeyModuleCallReply *cmds =
            ValkeyModule_Call(ctx, "COMMAND", "cccb", "LIST", "FILTERBY", "MODULE", name, name_len);
        ok = lua55CommandListApply(core, cmds, 0);
        if (cmds) ValkeyModule_FreeCallReply(cmds);
    }
    if (mods) ValkeyModule_FreeCallReply(mods);

    if (!ok) {
        ValkeyModule_FreeDict(NULL, core);
        return NULL;
    }
    return core;
}

void lua55ArgvCacheFree(lua55EngineCtx *ectx) {
    for (int j = 0; j < LUA55_ARGV_CACHE_SIZE; j++) {
        if (ectx->argv_cache[j]) ValkeyModule_FreeString(NULL, ectx->argv_cache[j]);
        ectx->argv_cache[j] = NULL;
    }
    if (ectx->core_commands) ValkeyModule_FreeDict(NULL, ectx->core_commands);
    ectx->core_commands = NULL;
}

static void freeLua55ServerArgv(lua55EngineCtx *ectx,
                                ValkeyModuleCtx *ctx,
                                ValkeyModuleString **argv,
                                int argc) {
    for (int j = 0; j < argc; j++) ValkeyModule_FreeString(ctx, argv[j]);
    if (argv == ectx->argv_buf) {
        ectx->argv_cache_busy = 0;
    } else {
        ValkeyModule_Free(argv);
    }
}

static ValkeyModuleString **lua55ArgsToServerArgv(lua55EngineCtx *ectx,
                                                  ValkeyModuleCtx *ctx,
                                                  lua_State *lua,
                                                  int *argc) {
    int j;
    *argc = lua_gettop(lua);
    if (*argc == 0) {
        lua55PushError(lua, "Please specify at least one argument for this call");
        return NULL;
    }

    int use_cache = !ectx->argv_cache_busy && *argc <= LUA55_ARGV_CACHE_SIZE &&
                    lua55ArgvCacheAllowed(ectx, lua);
    ValkeyModuleString **lua_argv;
    if (use_cache) {
        lua_argv = ectx->argv_buf;
        ectx->argv_cache_busy = 1;
    } else {
        lua_argv = ValkeyModule_Alloc(sizeof(ValkeyModuleString *) * *argc);
    }

    for (j = 0; j < *argc; j++) {
        char *obj_s;
        size_t obj_len;
        char dbuf[64];

        if (lua_type(lua, j + 1) == LUA_TNUMBER) {
            /* In Lua 5.3+ we should prefer integer subtype detection to
             * avoid float->str round-trips for already-integer values. */
            if (lua_isinteger(lua, j + 1)) {
                long long lvalue = (long long)lua_tointeger(lua, j + 1);
                obj_len = ll2string(dbuf, sizeof(dbuf), lvalue);
            } else {
                lua_Number num = lua_tonumber(lua, j + 1);
                long long lvalue;
                if (double2ll((double)num, &lvalue)) {
                    obj_len = ll2string(dbuf, sizeof(dbuf), lvalue);
                } else {
                    obj_len = fpconv_dtoa((double)num, dbuf);
                    dbuf[obj_len] = '\0';
                }
            }
            obj_s = dbuf;
        } else {
            obj_s = (char *)lua_tolstring(lua, j + 1, &obj_len);
            if (obj_s == NULL) break;
        }

        if (use_cache && obj_len <= LUA55_ARGV_CACHE_MAX_LEN) {
            lua_argv[j] = lua55ArgvCacheGet(ectx, j, obj_s, obj_len);
        } else {
            lua_argv[j] = ValkeyModule_CreateString(ctx, obj_s, obj_len);
        }
    }

    lua_pop(lua, *argc);

    if (j != *argc) {
        freeLua55ServerArgv(ectx, ctx, lua_argv, j);
        lua55PushError(lua, "Command arguments must be strings or integers");
        return NULL;
    }

    return lua_argv;
}

static void lua55ProcessReplyError(const char *err, int err_no, lua_State *lua) {
    int push_error = 1;

    if (err_no == ESPIPE) {
        if (strncmp(err, "ERR command ", strlen("ERR command ")) == 0) {
            lua55PushError(lua, "This Valkey command is not allowed from script");
            push_error = 0;
        }
    } else if (err_no == EINVAL) {
        if (strncmp(err, "ERR wrong number of arguments for ", strlen("ERR wrong number of arguments for ")) == 0) {
            lua55PushError(lua, "Wrong number of args calling command from script");
            push_error = 0;
        }
    } else if (err_no == ENOENT) {
        if (strncmp(err, "ERR unknown command '", strlen("ERR unknown command '")) == 0) {
            lua55PushError(lua, "Unknown command called from script");
            push_error = 0;
        }
    } else if (err_no == EACCES) {
        if (strncmp(err, "NOPERM ", strlen("NOPERM ")) == 0) {
            char *err_msg = lua55_asprintf("ACL failure in script: %s", err + strlen("NOPERM "));
            lua55PushError(lua, err_msg);
            ValkeyModule_Free(err_msg);
            push_error = 0;
        }
    }

    if (push_error) {
        char *err_with_dash = lua55_asprintf("-%s", err);
        lua55PushError(lua, err_with_dash);
        ValkeyModule_Free(err_with_dash);
    }
    lua_pushstring(lua, "ignore_error_stats_update");
    lua_pushboolean(lua, 1);
    lua_rawset(lua, -3);
}

#define LUA55_COLLECTION_ARRAY 1
#define LUA55_COLLECTION_MAP 2
#define LUA55_COLLECTION_SET 3

typedef struct lua55ReplyFrame {
    int type;
    size_t count;
} lua55ReplyFrame;

typedef struct lua55ReplyCtx {
    lua_State *lua;
    int resp;
    int base_top;
    int depth;
    int attr_skip;
    int top_error;
    int failed;
    lua55ReplyFrame frames[LUA55_MAX_REPLY_DEPTH];
} lua55ReplyCtx;

static int lua55ReplyBegin(lua55ReplyCtx *rc, int slots, size_t size, size_t max_size) {
    if (rc->failed || rc->attr_skip) return 0;
    if (size > max_size) {
        rc->failed = LUA55_REPLY_TOO_BIG;
        return 0;
    }
    if (!lua_checkstack(rc->lua, slots + 1)) {
        rc->failed = LUA55_REPLY_TOO_DEEP;
        return 0;
    }
    return 1;
}

static int lua55ReplyBeginScalar(lua55ReplyCtx *rc, int slots) {
    return lua55ReplyBegin(rc, slots, 0, 0);
}

static void lua55ReplyEmit(lua55ReplyCtx *rc) {
    if (rc->depth == 0) return;
    lua_State *lua = rc->lua;
    lua55ReplyFrame *f = &rc->frames[rc->depth - 1];
    f->count++;

    switch (f->type) {
    case LUA55_COLLECTION_ARRAY:
        lua_rawseti(lua, -2, (int)f->count);
        break;
    case LUA55_COLLECTION_MAP:
        if (f->count % 2) break;
        if (lua_isnil(lua, -2)) {
            lua_pop(lua, 2);
        } else {
            lua_rawset(lua, -3);
        }
        break;
    case LUA55_COLLECTION_SET:
        if (lua_isnil(lua, -1)) {
            lua_pop(lua, 1);
            break;
        }
        lua_pushboolean(lua, 1);
        lua_rawset(lua, -3);
        break;
    }
}

static int lua55ReplyOpen(lua55ReplyCtx *rc, int type, size_t len, size_t max_len) {
    if (!lua55ReplyBegin(rc, 3, len, max_len)) return 0;
    if (rc->depth >= LUA55_MAX_REPLY_DEPTH) {
        rc->failed = LUA55_REPLY_TOO_DEEP;
        return 0;
    }
    rc->frames[rc->depth].type = type;
    rc->frames[rc->depth].count = 0;
    rc->depth++;
    return 1;
}

static void lua55ReplyNull(void *ctx) {
    lua55ReplyCtx *rc = ctx;
    if (!lua55ReplyBeginScalar(rc, 1)) return;
    if (rc->resp == 2) {
        lua_pushboolean(rc->lua, 0);
    } else {
        lua_pushnil(rc->lua);
    }
    lua55ReplyEmit(rc);
}

static void lua55ReplyBulkString(void *ctx, const char *str, size_t len) {
    lua55ReplyCtx *rc = ctx;
    if (!lua55ReplyBeginScalar(rc, 1)) return;
    lua_pushlstring(rc->lua, str, len);
    lua55ReplyEmit(rc);
}

static void lua55ReplySimpleString(void *ctx, const char *str, size_t len) {
    lua55ReplyCtx *rc = ctx;
    if (!lua55ReplyBeginScalar(rc, 3)) return;
    lua_newtable(rc->lua);
    lua_pushstring(rc->lua, "ok");
    lua_pushlstring(rc->lua, str, len);
    lua_rawset(rc->lua, -3);
    lua55ReplyEmit(rc);
}

static void lua55ReplyVerbatimString(void *ctx, const char *str, size_t len, const char *fmt) {
    lua55ReplyCtx *rc = ctx;
    lua_State *lua = rc->lua;
    if (!lua55ReplyBeginScalar(rc, 5)) return;
    lua_newtable(lua);
    lua_pushstring(lua, "verbatim_string");
    lua_newtable(lua);
    lua_pushstring(lua, "string");
    lua_pushlstring(lua, str, len);
    lua_rawset(lua, -3);
    lua_pushstring(lua, "format");
    lua_pushlstring(lua, fmt, 3);
    lua_rawset(lua, -3);
    lua_rawset(lua, -3);
    lua55ReplyEmit(rc);
}

static void lua55ReplyError(void *ctx, const char *msg, size_t len) {
    lua55ReplyCtx *rc = ctx;
    int err_no = errno;
    if (!lua55ReplyBeginScalar(rc, 5)) return;

    char *err = lua55_strncpy(msg, len);
    lua55ProcessReplyError(err, rc->depth == 0 ? err_no : 0, rc->lua);
    ValkeyModule_Free(err);

    if (rc->depth == 0) rc->top_error = 1;
    lua55ReplyEmit(rc);
}

static void lua55ReplyInteger(void *ctx, long long val) {
    lua55ReplyCtx *rc = ctx;
    if (!lua55ReplyBeginScalar(rc, 1)) return;
    lua_pushinteger(rc->lua, (lua_Integer)val);
    lua55ReplyEmit(rc);
}

static void lua55ReplyDouble(void *ctx, double val) {
    lua55ReplyCtx *rc = ctx;
    if (!lua55ReplyBeginScalar(rc, 3)) return;
    lua_newtable(rc->lua);
    lua_pushstring(rc->lua, "double");
    lua_pushnumber(rc->lua, val);
    lua_rawset(rc->lua, -3);
    lua55ReplyEmit(rc);
}

static void lua55ReplyBigNumber(void *ctx, const char *str, size_t len) {
    lua55ReplyCtx *rc = ctx;
    if (!lua55ReplyBeginScalar(rc, 3)) return;
    lua_newtable(rc->lua);
    lua_pushstring(rc->lua, "big_number");
    lua_pushlstring(rc->lua, str, len);
    lua_rawset(rc->lua, -3);
    lua55ReplyEmit(rc);
}

static void lua55ReplyBool(void *ctx, int val) {
    lua55ReplyCtx *rc = ctx;
    if (!lua55ReplyBeginScalar(rc, 1)) return;
    lua_pushboolean(rc->lua, val);
    lua55ReplyEmit(rc);
}

static void lua55ReplyAttributeStart(void *ctx, size_t len) {
    VALKEYMODULE_NOT_USED(len);
    ((lua55ReplyCtx *)ctx)->attr_skip++;
}

static void lua55ReplyAttributeEnd(void *ctx) {
    ((lua55ReplyCtx *)ctx)->attr_skip--;
}

static void lua55ReplyArrayStart(void *ctx, size_t len) {
    lua55ReplyCtx *rc = ctx;
    if (!lua55ReplyOpen(rc, LUA55_COLLECTION_ARRAY, len, LUA55_MAX_ARRAY_SIZE)) return;
    lua_createtable(rc->lua, (int)len, 0);
}

static void lua55ReplyArrayEnd(void *ctx) {
    lua55ReplyCtx *rc = ctx;
    if (rc->failed || rc->attr_skip) return;
    rc->depth--;
    lua55ReplyEmit(rc);
}

static void lua55ReplyKeyedStart(lua55ReplyCtx *rc, int type, const char *field, size_t len) {
    if (!lua55ReplyOpen(rc, type, len, LUA55_MAX_HASH_SIZE)) return;
    lua_newtable(rc->lua);
    lua_pushstring(rc->lua, field);
    lua_createtable(rc->lua, 0, (int)len);
}

static void lua55ReplyMapStart(void *ctx, size_t len) {
    lua55ReplyKeyedStart(ctx, LUA55_COLLECTION_MAP, "map", len);
}

static void lua55ReplySetStart(void *ctx, size_t len) {
    lua55ReplyKeyedStart(ctx, LUA55_COLLECTION_SET, "set", len);
}

static void lua55ReplyKeyedEnd(void *ctx) {
    lua55ReplyCtx *rc = ctx;
    if (rc->failed || rc->attr_skip) return;
    lua_rawset(rc->lua, -3);
    rc->depth--;
    lua55ReplyEmit(rc);
}

static void lua55ReplyParsingError(void *ctx) {
    lua55ReplyCtx *rc = ctx;
    if (!rc->failed) rc->failed = LUA55_REPLY_BAD_PROTO;
}

static const ValkeyModuleReplyHandlers lua55ReplyHandlers = {
    .version = 1,
    .null = lua55ReplyNull,
    .nullBulkString = lua55ReplyNull,
    .nullArray = lua55ReplyNull,
    .bulkString = lua55ReplyBulkString,
    .simpleString = lua55ReplySimpleString,
    .verbatimString = lua55ReplyVerbatimString,
    .error = lua55ReplyError,
    .integer = lua55ReplyInteger,
    .doubleVal = lua55ReplyDouble,
    .bigNumber = lua55ReplyBigNumber,
    .boolVal = lua55ReplyBool,
    .attributeStart = lua55ReplyAttributeStart,
    .attributeEnd = lua55ReplyAttributeEnd,
    .arrayStart = lua55ReplyArrayStart,
    .arrayEnd = lua55ReplyArrayEnd,
    .mapStart = lua55ReplyMapStart,
    .mapEnd = lua55ReplyKeyedEnd,
    .setStart = lua55ReplySetStart,
    .setEnd = lua55ReplyKeyedEnd,
    .replyParsingError = lua55ReplyParsingError,
};

static int lua55_callargv_rejected = 0;

static int lua55ServerCallLegacy(lua55FuncCallCtx *rctx,
                                 lua_State *lua,
                                 ValkeyModuleString **argv,
                                 int argc) {
    char fmt[12] = "v!EMSX";
    int fmt_idx = 6;

    if (!(rctx->replication_flags & PROPAGATE_AOF)) {
        fmt[fmt_idx++] = 'A';
    }
    if (!(rctx->replication_flags & PROPAGATE_REPL)) {
        fmt[fmt_idx++] = 'R';
    }
    if (!rctx->replication_flags) {
        fmt[fmt_idx++] = 'A';
        fmt[fmt_idx++] = 'R';
    }
    if (rctx->resp == 3) {
        fmt[fmt_idx++] = '3';
    }
    fmt[fmt_idx] = '\0';

    size_t cmdlen = 0;
    const char *cmdname = ValkeyModule_StringPtrLen(argv[0], &cmdlen);
    if (memchr(cmdname, '\0', cmdlen)) {
        freeLua55ServerArgv(rctx->engine_ctx, rctx->module_ctx, argv, argc);
        lua55PushError(lua, "Unknown command called from script");
        return 1;
    }

    errno = 0;
    ValkeyModuleCallReply *reply =
        ValkeyModule_Call(rctx->module_ctx, cmdname, fmt, argv + 1, argc - 1);
    int call_errno = errno;
    freeLua55ServerArgv(rctx->engine_ctx, rctx->module_ctx, argv, argc);

    if (reply == NULL) {
        lua55PushError(lua, "Unknown command called from script");
        return 1;
    }

    int is_error = 1;
    if (call_errno != 0) {
        size_t len = 0;
        const char *str = ValkeyModule_CallReplyStringPtr(reply, &len);
        char *err = lua55_strncpy(str ? str : "", str ? len : 0);
        ValkeyModule_Log(rctx->module_ctx, "debug", "command returned an error: %s errno=%d", err, call_errno);
        lua55ProcessReplyError(err, call_errno, lua);
        ValkeyModule_Free(err);
    } else {
        int base_top = lua_gettop(lua);
        is_error = ValkeyModule_CallReplyType(reply) == VALKEYMODULE_REPLY_ERROR;
        int failed = callReplyToLuaType(lua, reply, rctx->resp, 0);
        if (failed != LUA55_REPLY_OK) is_error = lua55ReplyFailed(lua, base_top, failed);
    }

    ValkeyModule_FreeCallReply(reply);
    return is_error;
}

static int lua55ServerCallArgv(lua55FuncCallCtx *rctx,
                               lua_State *lua,
                               ValkeyModuleString **argv,
                               int argc) {
    int flags = VALKEYMODULE_CALL_ARGV_SCRIPT_MODE | VALKEYMODULE_CALL_ARGV_REPLICATE |
                VALKEYMODULE_CALL_ARGV_ERRORS_AS_REPLIES |
                VALKEYMODULE_CALL_ARGV_RESPECT_DENY_OOM | VALKEYMODULE_CALL_ARGV_REPLY_EXACT;

    if (!(rctx->replication_flags & PROPAGATE_AOF)) flags |= VALKEYMODULE_CALL_ARGV_NO_AOF;
    if (!(rctx->replication_flags & PROPAGATE_REPL)) flags |= VALKEYMODULE_CALL_ARGV_NO_REPLICAS;
    if (rctx->resp == 3) flags |= VALKEYMODULE_CALL_ARGV_RESP_3;

    lua55ReplyCtx rc;
    rc.lua = lua;
    rc.resp = rctx->resp;
    rc.base_top = lua_gettop(lua);
    rc.depth = 0;
    rc.attr_skip = 0;
    rc.top_error = 0;
    rc.failed = LUA55_REPLY_OK;

    int res = ValkeyModule_CallArgv(rctx->module_ctx, argv, argc, flags, &lua55ReplyHandlers, &rc);
    if (res != VALKEYMODULE_OK) {
        int err_no = errno;
        lua_settop(lua, rc.base_top);
        lua55_callargv_rejected = 1;
        ValkeyModule_Log(rctx->module_ctx, "warning",
                         "ValkeyModule_CallArgv rejected server.call (%s); "
                         "server.call uses ValkeyModule_Call from now on",
                         strerror(err_no));
        return lua55ServerCallLegacy(rctx, lua, argv, argc);
    }
    freeLua55ServerArgv(rctx->engine_ctx, rctx->module_ctx, argv, argc);

    if (rc.failed != LUA55_REPLY_OK) return lua55ReplyFailed(lua, rc.base_top, rc.failed);
    if (lua_gettop(lua) == rc.base_top) {
        lua55PushError(lua, "Unknown command called from script");
        return 1;
    }
    return rc.top_error;
}

static int lua55ServerGenericCommand(lua_State *lua, int raise_error) {
    lua55FuncCallCtx *rctx = lua55GetRunCtx(lua, raise_error ? "server.call" : "server.pcall");
    if (!rctx) return lua55Error(lua);

    static int inuse = 0;

    if (inuse) {
        char *recursion_warning = "lua55ServerGenericCommand() recursive call detected. "
                                  "Are you doing funny stuff with Lua debug hooks?";
        ValkeyModule_Log(rctx->module_ctx, "warning", "%s", recursion_warning);
        lua55PushError(lua, recursion_warning);
        return 1;
    }

    int argc = 0;
    ValkeyModuleString **argv = lua55ArgsToServerArgv(rctx->engine_ctx, rctx->module_ctx, lua, &argc);
    if (argv == NULL) {
        return raise_error ? lua55Error(lua) : 1;
    }

    inuse++;
    int is_error = ValkeyModule_CallArgv && !lua55_callargv_rejected
                       ? lua55ServerCallArgv(rctx, lua, argv, argc)
                       : lua55ServerCallLegacy(rctx, lua, argv, argc);
    inuse--;

    if (raise_error && is_error) {
        return lua55Error(lua);
    }
    return 1;
}

/* Our implementation to lua pcall.
 * Catches the table-form error object and converts it to a string for
 * backward compatibility with pre-Redis-7 OSS behaviour. */
static int lua55RedisPcall(lua_State *lua) {
    int argc = lua_gettop(lua);
    lua_pushboolean(lua, 1);
    lua_insert(lua, 1);
    if (lua_pcall(lua, argc - 1, LUA_MULTRET, 0)) {
        lua_remove(lua, 1);
        if (lua_istable(lua, -1)) {
            lua_getfield(lua, -1, "err");
            if (lua_isstring(lua, -1)) {
                lua_replace(lua, -2);
            }
        }
        lua_pushboolean(lua, 0);
        lua_insert(lua, 1);
    }
    return lua_gettop(lua);
}

/* server.call() */
static int lua55RedisCallCommand(lua_State *lua) {
    return lua55ServerGenericCommand(lua, 1);
}

/* server.pcall() */
static int lua55RedisPCallCommand(lua_State *lua) {
    return lua55ServerGenericCommand(lua, 0);
}

static void sha1hex(char *digest, char *script, size_t len) {
    SHA1_CTX ctx;
    unsigned char hash[20];
    char *cset = "0123456789abcdef";
    int j;

    SHA1Init(&ctx);
    SHA1Update(&ctx, (unsigned char *)script, len);
    SHA1Final(hash, &ctx);

    for (j = 0; j < 20; j++) {
        digest[j * 2] = cset[((hash[j] & 0xF0) >> 4)];
        digest[j * 2 + 1] = cset[(hash[j] & 0xF)];
    }
    digest[40] = '\0';
}

static int lua55RedisSha1hexCommand(lua_State *lua) {
    int argc = lua_gettop(lua);
    char digest[41];
    size_t len;
    char *s;

    if (argc != 1) {
        lua55PushError(lua, "wrong number of arguments");
        return lua55Error(lua);
    }

    s = (char *)lua_tolstring(lua, 1, &len);
    sha1hex(digest, s, len);
    lua_pushstring(lua, digest);
    return 1;
}

static int lua55RedisReturnSingleFieldTable(lua_State *lua, char *field) {
    if (lua_gettop(lua) != 1 || lua_type(lua, -1) != LUA_TSTRING) {
        lua55PushError(lua, "wrong number or type of arguments");
        return 1;
    }

    lua_newtable(lua);
    lua_pushstring(lua, field);
    lua_pushvalue(lua, -3);
    lua_settable(lua, -3);
    return 1;
}

static int lua55RedisErrorReplyCommand(lua_State *lua) {
    if (lua_gettop(lua) != 1 || lua_type(lua, -1) != LUA_TSTRING) {
        lua55PushError(lua, "wrong number or type of arguments");
        return 1;
    }

    const char *err = lua_tostring(lua, -1);
    if (!err) {
        lua55PushError(lua, "ERR unable to convert error to string");
        return 1;
    }
    char *err_buff = NULL;
    if (err[0] != '-') {
        err_buff = lua55_asprintf("-%s", err);
    } else {
        err_buff = lua55_strcpy(err);
    }
    lua55PushErrorBuff(lua, err_buff);
    ValkeyModule_Free(err_buff);
    return 1;
}

static int lua55RedisStatusReplyCommand(lua_State *lua) {
    return lua55RedisReturnSingleFieldTable(lua, "ok");
}

static int lua55RedisSetReplCommand(lua_State *lua) {
    int flags, argc = lua_gettop(lua);

    lua55FuncCallCtx *rctx = lua55GetRunCtx(lua, "server.set_repl");
    if (!rctx) return lua55Error(lua);

    if (argc != 1) {
        lua55PushError(lua, "server.set_repl() requires one argument.");
        return lua55Error(lua);
    }

    flags = (int)lua_tointeger(lua, -1);
    if ((flags & ~(PROPAGATE_AOF | PROPAGATE_REPL)) != 0) {
        lua55PushError(lua, "Invalid replication flags. Use REPL_AOF, "
                            "REPL_REPLICA, REPL_ALL or REPL_NONE.");
        return lua55Error(lua);
    }

    rctx->replication_flags = flags;

    return 0;
}

static int lua55RedisAclCheckCmdPermissionsCommand(lua_State *lua) {
    lua55FuncCallCtx *rctx = lua55GetRunCtx(lua, "server.acl_check_cmd");
    if (!rctx) return lua55Error(lua);

    int raise_error = 0;

    int argc = 0;
    ValkeyModuleString **argv =
        lua55ArgsToServerArgv(rctx->engine_ctx, rctx->module_ctx, lua, &argc);

    if (argv == NULL)
        return lua55Error(lua);

    int dbid = ValkeyModule_GetSelectedDb(rctx->module_ctx);
    ValkeyModuleString *username =
        ValkeyModule_GetCurrentUserName(rctx->module_ctx);
    int res, err_no;
    if (username) {
        ValkeyModuleUser *user = ValkeyModule_GetModuleUserFromUserName(username);
        ValkeyModule_FreeString(rctx->module_ctx, username);
        res = ValkeyModule_ACLCheckPermissions(user, argv, argc, dbid, NULL);
        err_no = errno;
        ValkeyModule_FreeModuleUser(user);
    } else {
        int nkeys = 0;
        int *keys = ValkeyModule_GetCommandKeysWithFlags(rctx->module_ctx, argv,
                                                         argc, &nkeys, NULL);
        res = (keys == NULL && errno == ENOENT) ? VALKEYMODULE_ERR : VALKEYMODULE_OK;
        err_no = EINVAL;
        if (keys)
            ValkeyModule_Free(keys);
    }

    if (res != VALKEYMODULE_OK) {
        if (err_no == EINVAL) {
            lua55PushError(lua, "Invalid command passed to server.acl_check_cmd()");
            raise_error = 1;
        } else {
            ValkeyModule_Assert(err_no == EACCES);
            lua_pushboolean(lua, 0);
        }
    } else {
        lua_pushboolean(lua, 1);
    }

    freeLua55ServerArgv(rctx->engine_ctx, rctx->module_ctx, argv, argc);
    if (raise_error)
        return lua55Error(lua);
    else
        return 1;
}

/* server.log() */
static int lua55LogCommand(lua_State *lua) {
    lua55FuncCallCtx *rctx = lua55GetFromRegistry(lua, REGISTRY_RUN_CTX_NAME);

    int j, argc = lua_gettop(lua);
    int level;

    if (argc < 2) {
        lua55PushError(lua, "server.log() requires two arguments or more.");
        return lua55Error(lua);
    } else if (!lua_isnumber(lua, -argc)) {
        lua55PushError(lua, "First argument must be a number (log level).");
        return lua55Error(lua);
    }
    level = (int)lua_tointeger(lua, -argc);
    if (level < LL_DEBUG || level > LL_WARNING) {
        lua55PushError(lua, "Invalid log level.");
        return lua55Error(lua);
    }

    char *log = NULL;
    for (j = 1; j < argc; j++) {
        size_t len;
        char *s;

        s = (char *)lua_tolstring(lua, (-argc) + j, &len);
        if (s) {
            if (j != 1) {
                char *next_log = lua55_asprintf("%s %s", log, s);
                ValkeyModule_Free(log);
                log = next_log;
            } else {
                log = lua55_asprintf("%s", s);
            }
        }
    }

    const char *level_str = NULL;
    switch (level) {
    case LL_DEBUG:
        level_str = "debug";
        break;
    case LL_VERBOSE:
        level_str = "verbose";
        break;
    case LL_NOTICE:
        level_str = "notice";
        break;
    case LL_WARNING:
        level_str = "warning";
        break;
    default:
        ValkeyModule_Assert(0);
    }

    ValkeyModule_Log(rctx ? rctx->module_ctx : NULL, level_str, "%s", log ? log : "");
    ValkeyModule_Free(log);
    return 0;
}

static int lua55SetResp(lua_State *lua) {
    lua55FuncCallCtx *rctx = lua55GetRunCtx(lua, "server.setresp");
    if (!rctx) return lua55Error(lua);

    int argc = lua_gettop(lua);

    if (argc != 1) {
        lua55PushError(lua, "server.setresp() requires one argument.");
        return lua55Error(lua);
    }

    int resp = (int)lua_tointeger(lua, -argc);
    if (resp != 2 && resp != 3) {
        lua55PushError(lua, "RESP version must be 2 or 3.");
        return lua55Error(lua);
    }

    rctx->resp = resp;

    return 0;
}

extern int luaopen_cjson(lua_State *lua);
extern int luaopen_cjson_safe(lua_State *lua);
extern int luaopen_struct(lua_State *lua);
extern int luaopen_cmsgpack(lua_State *lua);
extern int luaopen_cmsgpack_safe(lua_State *lua);
extern int luaopen_bit(lua_State *lua);
extern int luaopen_coroutine_sharedhook(lua_State *lua);

/* ---------------------------------------------------------------------------
 * Lua engine initialization and reset.
 * ------------------------------------------------------------------------- */

static void lua55WrapLoadFunction(lua_State *lua) {
    const char *wrap_code =
        "do "
        "  local load_original = load "
        "  load = function(chunk, chunkname, mode, env) "
        "    return load_original(chunk, chunkname, 't', env) "
        "  end "
        "  loadstring = load "
        "end";

    if (luaL_dostring(lua, wrap_code) != LUA_OK) {
        const char *err = lua_tostring(lua, -1);
        ValkeyModule_Log(NULL, "warning", "Failed to wrap load() function: %s",
                         err ? err : "unknown");
        lua_pop(lua, 1);
    }
}

static void lua55RequireLib(lua_State *lua, const char *name, lua_CFunction openf, int global) {
    luaL_requiref(lua, name, openf, global);
    lua_pop(lua, 1);
}

static void lua55LoadLibraries(lua_State *lua) {
    lua55RequireLib(lua, "_G", luaopen_base, 1);
    lua55RequireLib(lua, LUA_TABLIBNAME, luaopen_table, 1);
    lua55RequireLib(lua, LUA_STRLIBNAME, luaopen_string, 1);
    lua55RequireLib(lua, LUA_COLIBNAME, luaopen_coroutine_sharedhook, 1);
    lua55RequireLib(lua, LUA_MATHLIBNAME, luaopen_math, 1);
    lua55RequireLib(lua, LUA_DBLIBNAME, luaopen_debug, 1);
    lua55RequireLib(lua, LUA_OSLIBNAME, luaopen_os, 1);

    lua_getglobal(lua, "os");
    lua_getfield(lua, -1, "clock");
    lua_newtable(lua);
    lua_pushvalue(lua, -2);
    lua_setfield(lua, -2, "clock");
    lua_setglobal(lua, "os");
    lua_pop(lua, 2);

    lua_pushcfunction(lua, luaopen_cjson);
    lua_call(lua, 0, 1);
    lua_setglobal(lua, "cjson");

    lua_pushcfunction(lua, luaopen_cjson_safe);
    lua_call(lua, 0, 1);
    lua_setglobal(lua, "cjson.safe");

    lua_pushcfunction(lua, luaopen_struct);
    lua_call(lua, 0, 1);
    lua_setglobal(lua, "struct");

    lua_pushcfunction(lua, luaopen_cmsgpack);
    lua_call(lua, 0, 1);
    lua_setglobal(lua, "cmsgpack");

    lua_pushcfunction(lua, luaopen_cmsgpack_safe);
    lua_call(lua, 0, 1);
    lua_setglobal(lua, "cmsgpack_safe");

    lua_pushcfunction(lua, luaopen_bit);
    lua_call(lua, 0, 1);
    lua_setglobal(lua, "bit");

    /* Lua 5.5 moved unpack() into table.unpack. Alias it. */
    lua_getglobal(lua, "table");
    lua_getfield(lua, -1, "unpack");
    lua_setglobal(lua, "unpack");
    lua_pop(lua, 1);

    lua55WrapLoadFunction(lua);
}

static void lua55RemoveUnsafeGlobals(lua_State *lua) {
    char *deny_list[] = {"dofile", "loadfile", "print", "setfenv",
                         "getfenv", "newproxy", NULL};

    for (char **p = deny_list; *p != NULL; p++) {
        lua_pushnil(lua);
        lua_setglobal(lua, *p);
    }
}

void lua55RegisterVersion(lua55EngineCtx *ctx, lua_State *lua) {
    lua_pushstring(lua, "REDIS_VERSION_NUM");
    lua_pushinteger(lua, ctx->redis_version_num);
    lua_settable(lua, -3);

    lua_pushstring(lua, "REDIS_VERSION");
    lua_pushstring(lua, ctx->redis_version);
    lua_settable(lua, -3);

    lua_pushstring(lua, "VALKEY_VERSION_NUM");
    lua_pushinteger(lua, ctx->valkey_version_num);
    lua_settable(lua, -3);

    lua_pushstring(lua, "VALKEY_VERSION");
    lua_pushstring(lua, ctx->valkey_version);
    lua_settable(lua, -3);

    lua_pushstring(lua, "SERVER_NAME");
    lua_pushstring(lua, ctx->server_name);
    lua_settable(lua, -3);
}

void lua55RegisterLogFunction(lua_State *lua) {
    lua_pushstring(lua, "log");
    lua_pushcfunction(lua, lua55LogCommand);
    lua_settable(lua, -3);

    lua_pushstring(lua, "LOG_DEBUG");
    lua_pushinteger(lua, LL_DEBUG);
    lua_settable(lua, -3);

    lua_pushstring(lua, "LOG_VERBOSE");
    lua_pushinteger(lua, LL_VERBOSE);
    lua_settable(lua, -3);

    lua_pushstring(lua, "LOG_NOTICE");
    lua_pushinteger(lua, LL_NOTICE);
    lua_settable(lua, -3);

    lua_pushstring(lua, "LOG_WARNING");
    lua_pushinteger(lua, LL_WARNING);
    lua_settable(lua, -3);
}

void lua55RegisterServerAPI(lua55EngineCtx *ctx, lua_State *lua) {
    lua55LoadLibraries(lua);

    lua55RemoveUnsafeGlobals(lua);

    lua55SaveOnRegistry(lua, REGISTRY_RUN_CTX_NAME, NULL);

    lua_pushcfunction(lua, lua55RedisPcall);
    lua_setglobal(lua, "pcall");

    lua_newtable(lua);
    lua_pushstring(lua, "call");
    lua_pushcfunction(lua, lua55RedisCallCommand);
    lua_settable(lua, -3);
    lua_pushstring(lua, "pcall");
    lua_pushcfunction(lua, lua55RedisPCallCommand);
    lua_settable(lua, -3);

    lua55RegisterLogFunction(lua);

    lua55RegisterVersion(ctx, lua);

    lua_pushstring(lua, "setresp");
    lua_pushcfunction(lua, lua55SetResp);
    lua_settable(lua, -3);

    lua_pushstring(lua, "sha1hex");
    lua_pushcfunction(lua, lua55RedisSha1hexCommand);
    lua_settable(lua, -3);

    lua_pushstring(lua, "error_reply");
    lua_pushcfunction(lua, lua55RedisErrorReplyCommand);
    lua_settable(lua, -3);
    lua_pushstring(lua, "status_reply");
    lua_pushcfunction(lua, lua55RedisStatusReplyCommand);
    lua_settable(lua, -3);

    lua_pushstring(lua, "set_repl");
    lua_pushcfunction(lua, lua55RedisSetReplCommand);
    lua_settable(lua, -3);

    lua_pushstring(lua, "REPL_NONE");
    lua_pushinteger(lua, PROPAGATE_NONE);
    lua_settable(lua, -3);

    lua_pushstring(lua, "REPL_AOF");
    lua_pushinteger(lua, PROPAGATE_AOF);
    lua_settable(lua, -3);

    lua_pushstring(lua, "REPL_SLAVE");
    lua_pushinteger(lua, PROPAGATE_REPL);
    lua_settable(lua, -3);

    lua_pushstring(lua, "REPL_REPLICA");
    lua_pushinteger(lua, PROPAGATE_REPL);
    lua_settable(lua, -3);

    lua_pushstring(lua, "REPL_ALL");
    lua_pushinteger(lua, PROPAGATE_AOF | PROPAGATE_REPL);
    lua_settable(lua, -3);

    lua_pushstring(lua, "acl_check_cmd");
    lua_pushcfunction(lua, lua55RedisAclCheckCmdPermissionsCommand);
    lua_settable(lua, -3);

    lua_setglobal(lua, SERVER_API_NAME);
    lua_getglobal(lua, SERVER_API_NAME);
    lua_setglobal(lua, REDIS_API_NAME);

    lua_getglobal(lua, "math");
    lua_pushstring(lua, "random");
    lua_pushcfunction(lua, server_math_random);
    lua_settable(lua, -3);
    lua_pushstring(lua, "randomseed");
    lua_pushcfunction(lua, server_math_randomseed);
    lua_settable(lua, -3);
    lua_setglobal(lua, "math");

    lua_pushinteger(lua, 0);
    lua_setfield(lua, LUA_REGISTRYINDEX, "__gc_count");
    lua_pushinteger(lua, 0);
    lua_setfield(lua, LUA_REGISTRYINDEX, "__full_gc_count");
}

static void lua55CreateArray(lua_State *lua, ValkeyModuleString **elev, int elec) {
    int j;

    lua_createtable(lua, elec, 0);
    for (j = 0; j < elec; j++) {
        size_t len = 0;
        const char *str = ValkeyModule_StringPtrLen(elev[j], &len);
        lua_pushlstring(lua, str, len);
        lua_rawseti(lua, -2, j + 1);
    }
}

static int server_math_random(lua_State *L) {
    lua_Number r = (lua_Number)(serverLrand48() % SERVER_LRAND48_MAX) /
                   (lua_Number)SERVER_LRAND48_MAX;
    switch (lua_gettop(L)) {
    case 0: {
        lua_pushnumber(L, r);
        break;
    }
    case 1: {
        lua_Integer u = luaL_checkinteger(L, 1);
        luaL_argcheck(L, 1 <= u, 1, "interval is empty");
        lua_pushinteger(L, (lua_Integer)floor(r * (lua_Number)u) + 1);
        break;
    }
    case 2: {
        lua_Integer l = luaL_checkinteger(L, 1);
        lua_Integer u = luaL_checkinteger(L, 2);
        luaL_argcheck(L, l <= u, 2, "interval is empty");
        lua_pushinteger(L, (lua_Integer)floor(r * (lua_Number)(u - l + 1)) + l);
        break;
    }
    default:
        return luaL_error(L, "wrong number of arguments");
    }
    return 1;
}

static int server_math_randomseed(lua_State *L) {
    serverSrand48((int32_t)luaL_checkinteger(L, 1));
    return 0;
}

static void lua55MaskCountHook(lua_State *lua, lua_Debug *ar) {
    VALKEYMODULE_NOT_USED(ar);

    lua55FuncCallCtx *rctx = lua55GetFromRegistry(lua, REGISTRY_RUN_CTX_NAME);
    ValkeyModule_Assert(rctx);

    ValkeyModuleScriptingEngineExecutionState state =
        ValkeyModule_GetFunctionExecutionState(rctx->run_ctx);
    if (state == VMSE_STATE_KILLED) {
        char *err = NULL;
        if (rctx->type == VMSE_EVAL) {
            err = "Script killed by user with SCRIPT KILL.";
        } else {
            err = "Script killed by user with FUNCTION KILL.";
        }
        ValkeyModule_Log(NULL, "notice", "%s", err);

        lua_sethook(lua, lua55MaskCountHook, LUA_MASKLINE, 0);

        lua55PushError(lua, err);
        lua55Error(lua);
    }
}

void lua55ErrorInformationDiscard(errorInfo *err_info) {
    if (err_info->msg)
        ValkeyModule_Free(err_info->msg);
    if (err_info->source)
        ValkeyModule_Free(err_info->source);
    if (err_info->line)
        ValkeyModule_Free(err_info->line);
}

static void lua55RawGetField(lua_State *lua, const char *key) {
    lua_pushstring(lua, key);
    lua_rawget(lua, -2);
}

void lua55ExtractErrorInformation(lua_State *lua, errorInfo *err_info) {
    if (lua_isstring(lua, -1)) {
        err_info->msg = lua55_asprintf("ERR %s", lua_tostring(lua, -1));
        err_info->line = NULL;
        err_info->source = NULL;
        err_info->ignore_err_stats_update = 0;
        return;
    }
    if (!lua_istable(lua, -1)) {
        err_info->msg = lua55_asprintf("ERR (error object is a %s value)", luaL_typename(lua, -1));
        err_info->line = NULL;
        err_info->source = NULL;
        err_info->ignore_err_stats_update = 0;
        return;
    }

    lua55RawGetField(lua, "err");
    if (lua_isstring(lua, -1)) {
        err_info->msg = lua55_strcpy(lua_tostring(lua, -1));
    }
    lua_pop(lua, 1);

    lua55RawGetField(lua, "source");
    if (lua_isstring(lua, -1)) {
        err_info->source = lua55_strcpy(lua_tostring(lua, -1));
    }
    lua_pop(lua, 1);

    lua55RawGetField(lua, "line");
    if (lua_isstring(lua, -1)) {
        err_info->line = lua55_strcpy(lua_tostring(lua, -1));
    }
    lua_pop(lua, 1);

    lua55RawGetField(lua, "ignore_error_stats_update");
    if (lua_isboolean(lua, -1)) {
        err_info->ignore_err_stats_update = lua_toboolean(lua, -1);
    }
    lua_pop(lua, 1);

    if (err_info->msg == NULL) {
        err_info->msg = lua55_strcpy("ERR unknown error");
    }
}

void lua55CallFunction(ValkeyModuleCtx *ctx,
                       ValkeyModuleScriptingEngineServerRuntimeCtx *run_ctx,
                       ValkeyModuleScriptingEngineSubsystemType type,
                       lua_State *lua,
                       ValkeyModuleString **keys,
                       size_t nkeys,
                       ValkeyModuleString **args,
                       size_t nargs,
                       lua55EngineCtx *engine_ctx) {
    int delhook = 0;

    lua55FuncCallCtx call_ctx = {
        .engine_ctx = engine_ctx,
        .module_ctx = ctx,
        .run_ctx = run_ctx,
        .type = type,
        .replication_flags = PROPAGATE_AOF | PROPAGATE_REPL,
        .resp = 2,
        .lua_enable_insecure_api = engine_ctx->lua_enable_insecure_api,
    };

    lua55SaveOnRegistry(lua, REGISTRY_RUN_CTX_NAME, &call_ctx);

    lua_sethook(lua, lua55MaskCountHook, LUA_MASKCOUNT, LUA_HOOK_CHECK_INTERVAL);
    delhook = 1;

    lua55CreateArray(lua, keys, nkeys);
    if (type == VMSE_EVAL) {
        lua_setglobal(lua, "KEYS");
    }
    lua55CreateArray(lua, args, nargs);
    if (type == VMSE_EVAL) {
        lua_setglobal(lua, "ARGV");
    }

    int err;
    if (type == VMSE_EVAL) {
        err = lua_pcall(lua, 0, 1, -2);
    } else {
        err = lua_pcall(lua, 2, 1, -4);
    }

    {
        int gc_count = 0;
        lua_getfield(lua, LUA_REGISTRYINDEX, "__gc_count");
        if (!lua_isnil(lua, -1)) {
            gc_count = (int)lua_tointeger(lua, -1);
        }
        lua_pop(lua, 1);

        int full_gc_count = 0;
        lua_getfield(lua, LUA_REGISTRYINDEX, "__full_gc_count");
        if (!lua_isnil(lua, -1)) {
            full_gc_count = (int)lua_tointeger(lua, -1);
        }
        lua_pop(lua, 1);

        gc_count++;
        full_gc_count++;

        if (gc_count >= LUA_GC_CYCLE_PERIOD) {
            lua_gc(lua, LUA_GCSTEP, LUA_GC_CYCLE_PERIOD);
            gc_count = 0;
        }

        if (full_gc_count >= LUA_FULL_GC_CYCLE) {
            lua_gc(lua, LUA_GCCOLLECT, 0);
            full_gc_count = 0;
        }

        lua_pushinteger(lua, gc_count);
        lua_setfield(lua, LUA_REGISTRYINDEX, "__gc_count");
        lua_pushinteger(lua, full_gc_count);
        lua_setfield(lua, LUA_REGISTRYINDEX, "__full_gc_count");
    }

    if (err) {
        if (!lua_istable(lua, -1)) {
            const char *msg = "execution failure";
            if (lua_isstring(lua, -1)) {
                msg = lua_tostring(lua, -1);
            }
            ValkeyModule_ReplyWithErrorFormat(
                ctx, "ERR Error running script, %.100s\n", msg);
        } else {
            errorInfo err_info = {0};
            lua55ExtractErrorInformation(lua, &err_info);
            if (err_info.line && err_info.source) {
                ValkeyModule_ReplyWithCustomErrorFormat(
                    ctx, !err_info.ignore_err_stats_update, "%s script: on %s:%s.",
                    err_info.msg, err_info.source, err_info.line);
            } else {
                ValkeyModule_ReplyWithCustomErrorFormat(
                    ctx, !err_info.ignore_err_stats_update, "%s", err_info.msg);
            }
            lua55ErrorInformationDiscard(&err_info);
        }
        lua_pop(lua, 1);
    } else {
        lua55ReplyToServerReply(ctx, call_ctx.resp, lua);
    }

    if (delhook)
        lua_sethook(lua, NULL, 0, 0);

    lua55SaveOnRegistry(lua, REGISTRY_RUN_CTX_NAME, NULL);
}

unsigned long lua55Memory(lua_State *lua) {
    return (unsigned long)lua_gc(lua, LUA_GCCOUNT, 0) * 1024LL;
}

static void lua55PickNamedArgKey(const char **winner, const char *key) {
    if (*winner == NULL || strcmp(key, *winner) > 0)
        *winner = key;
}

const char *lua55ReadNamedArgKeys(lua_State *lua, lua55NamedArgKeys *keys) {
    const char *err = NULL;
    memset(keys, 0, sizeof(*keys));

    if (!lua_istable(lua, 1)) {
        return "calling server.register_function with a single argument is only "
               "applicable to Lua table "
               "(representing named arguments).";
    }

    lua_pushnil(lua);
    while (!err && lua_next(lua, 1)) {
        const char *key = lua_isstring(lua, -2) ? lua_tostring(lua, -2) : NULL;
        if (!key) {
            err = "named argument key given to server.register_function is not a "
                  "string";
        } else if (!strcasecmp(key, "function_name")) {
            if (lua_isstring(lua, -1))
                lua55PickNamedArgKey(&keys->function_name, key);
            else
                err = "function_name argument given to server.register_function must "
                      "be a string";
        } else if (!strcasecmp(key, "description")) {
            if (lua_isstring(lua, -1))
                lua55PickNamedArgKey(&keys->description, key);
            else
                err = "description argument given to server.register_function must be "
                      "a string";
        } else if (!strcasecmp(key, "callback")) {
            if (lua_isfunction(lua, -1))
                lua55PickNamedArgKey(&keys->callback, key);
            else
                err = "callback argument given to server.register_function must be a "
                      "function";
        } else if (!strcasecmp(key, "flags")) {
            if (lua_istable(lua, -1))
                lua55PickNamedArgKey(&keys->flags, key);
            else
                err = "flags argument to server.register_function must be a table "
                      "representing function flags";
        } else {
            err = "unknown argument given to server.register_function";
        }
        lua_pop(lua, err ? 2 : 1);
    }
    return err;
}

void lua55PushNamedArg(lua_State *lua, const char *key) {
    lua_pushstring(lua, key);
    lua_rawget(lua, 1);
}

int lua55UserStateRegisterFunction(lua_State *lua) {
    int argc = lua_gettop(lua);

    if (argc < 1 || argc > 2) {
        lua_pushstring(lua,
                       "wrong number of arguments to server.register_function");
        return lua_error(lua);
    }

    if (argc == 1) {
        lua55NamedArgKeys keys;
        const char *err = lua55ReadNamedArgKeys(lua, &keys);
        if (!err && !keys.function_name)
            err = "server.register_function must get a function name argument";
        if (!err && !keys.callback)
            err = "server.register_function must get a callback argument";
        if (err) {
            lua_pushstring(lua, err);
            return lua_error(lua);
        }
        lua55PushNamedArg(lua, keys.function_name);
        lua55PushNamedArg(lua, keys.callback);
    } else {
        if (!lua_isstring(lua, 1)) {
            lua_pushstring(lua, "function name must be a string");
            return lua_error(lua);
        }
        if (!lua_isfunction(lua, 2)) {
            lua_pushstring(lua, "function argument must be a function");
            return lua_error(lua);
        }
        lua_pushvalue(lua, 1);
        lua_pushvalue(lua, 2);
    }

    const char *name = lua_tostring(lua, -2);

    lua_getfield(lua, LUA_REGISTRYINDEX, "__library_functions");
    if (lua_isnil(lua, -1)) {
        lua_pop(lua, 1);
        lua_newtable(lua);
        lua_pushvalue(lua, -1);
        lua_setfield(lua, LUA_REGISTRYINDEX, "__library_functions");
    }

    lua_pushvalue(lua, -2);
    lua_setfield(lua, -2, name);
    return 0;
}

#define REGISTRY_LIBRARY_LOAD_START_NAME "__LIBRARY_LOAD_START__"
#define LIBRARY_LOAD_TIMEOUT_MS 500

static void lua55LibraryLoadHook(lua_State *lua, lua_Debug *ar) {
    VALKEYMODULE_NOT_USED(ar);
    monotime *start = lua55GetFromRegistry(lua, REGISTRY_LIBRARY_LOAD_START_NAME);
    if (start && elapsedMs(*start) > LIBRARY_LOAD_TIMEOUT_MS) {
        lua_sethook(lua, lua55LibraryLoadHook, LUA_MASKLINE, 0);
        lua55PushError(lua, "FUNCTION LOAD timeout");
        lua55Error(lua);
    }
}

int lua55CompileLibraryInUserState(lua_State *lua, const char *code, size_t code_len, const char *library_name) {
    if (luaL_loadbuffer(lua, code, code_len, library_name) != 0) {
        return -1;
    }

    monotime start = getMonotonicUs();
    lua55SaveOnRegistry(lua, REGISTRY_LIBRARY_LOAD_START_NAME, &start);
    lua_sethook(lua, lua55LibraryLoadHook, LUA_MASKCOUNT, LUA_HOOK_CHECK_INTERVAL);
    int status = lua_pcall(lua, 0, 0, 0);
    lua_sethook(lua, NULL, 0, 0);
    lua55SaveOnRegistry(lua, REGISTRY_LIBRARY_LOAD_START_NAME, NULL);

    return status == 0 ? 0 : -1;
}

void lua55RemoveFunctionFromUserState(lua_State *lua,
                                      const char *function_name) {
    lua_getfield(lua, LUA_REGISTRYINDEX, "__library_functions");
    if (lua_istable(lua, -1)) {
        lua_pushnil(lua);
        lua_setfield(lua, -2, function_name);
    }
    lua_pop(lua, 1);
}
