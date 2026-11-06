/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "lauxlib.h"
#include "lstate.h"
#include "lua.h"
#include "lualib.h"

static void movehook(lua_State *from, lua_State *to) {
  int settraps = from->hookmask && !to->hookmask;
  to->hook = from->hook;
  to->hookmask = from->hookmask;
  to->basehookcount = from->basehookcount;
  to->hookcount = from->hookcount;
  if (settraps) {
    for (CallInfo *ci = to->ci; ci != NULL; ci = ci->previous)
      if (isLua(ci))
        ci->u.l.trap = 1;
  }
}

static int auxresume(lua_State *L, lua_State *co, int narg) {
  int status, nres;
  if (!lua_checkstack(co, narg)) {
    lua_pushliteral(L, "too many arguments to resume");
    return -1;
  }
  lua_xmove(L, co, narg);
  movehook(L, co);
  status = lua_resume(co, L, narg, &nres);
  movehook(co, L);
  if (status == LUA_OK || status == LUA_YIELD) {
    if (!lua_checkstack(L, nres + 1)) {
      lua_pop(co, nres);
      lua_pushliteral(L, "too many results to resume");
      return -1;
    }
    lua_xmove(co, L, nres);
    return nres;
  }
  lua_xmove(co, L, 1);
  return -1;
}

static int luaB_coresume(lua_State *L) {
  lua_State *co = lua_tothread(L, 1);
  int r;
  luaL_argexpected(L, co, 1, "thread");
  r = auxresume(L, co, lua_gettop(L) - 1);
  if (r < 0) {
    lua_pushboolean(L, 0);
    lua_insert(L, -2);
    return 2;
  }
  lua_pushboolean(L, 1);
  lua_insert(L, -(r + 1));
  return r + 1;
}

static int luaB_auxwrap(lua_State *L) {
  lua_State *co = lua_tothread(L, lua_upvalueindex(1));
  int r = auxresume(L, co, lua_gettop(L));
  if (r < 0) {
    int stat = lua_status(co);
    if (stat != LUA_OK && stat != LUA_YIELD) {
      stat = lua_closethread(co, L);
      lua_xmove(co, L, 1);
    }
    if (stat != LUA_ERRMEM && lua_type(L, -1) == LUA_TSTRING) {
      luaL_where(L, 1);
      lua_insert(L, -2);
      lua_concat(L, 2);
    }
    return lua_error(L);
  }
  return r;
}

static int luaB_cowrap(lua_State *L) {
  lua_State *NL;
  luaL_checktype(L, 1, LUA_TFUNCTION);
  NL = lua_newthread(L);
  lua_pushvalue(L, 1);
  lua_xmove(L, NL, 1);
  lua_pushcclosure(L, luaB_auxwrap, 1);
  return 1;
}

int luaopen_coroutine_sharedhook(lua_State *L) {
  luaopen_coroutine(L);
  lua_pushcfunction(L, luaB_coresume);
  lua_setfield(L, -2, "resume");
  lua_pushcfunction(L, luaB_cowrap);
  lua_setfield(L, -2, "wrap");
  return 1;
}
