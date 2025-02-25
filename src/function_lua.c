/*
 * Copyright (c) 2021, Redis Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * function_lua.c unit provides the Lua engine functionality.
 * Including registering the engine and implementing the engine
 * callbacks:
 * * Create a function from blob (usually text)
 * * Invoke a function
 * * Free function memory
 * * Get memory usage
 *
 * Uses script_lua.c to run the Lua code.
 */

#include "functions.h"
#include "script_lua.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#define LUA_ENGINE_NAME "LUA"
#define REGISTRY_ENGINE_CTX_NAME "__ENGINE_CTX__"
#define REGISTRY_ERROR_HANDLER_NAME "__ERROR_HANDLER__"

/* Lua engine ctx */
typedef struct luaEngineCtx {
    lua_State *lua;
} luaEngineCtx;

/* Lua function ctx */
typedef struct luaFunctionCtx {
    /* Special ID that allows getting the Lua function object from the Lua registry */
    int lua_function_ref;
} luaFunctionCtx;

/*
 * Compile a given blob and save it on the registry.
 * Return a function ctx with Lua ref that allows to later retrieve the
 * function from the registry.
 *
 * Return NULL on compilation error and set the error to the err variable
 */
static void* luaEngineCreate(void *engine_ctx, sds blob, sds *err) {
    luaEngineCtx *lua_engine_ctx = engine_ctx;
    lua_State *lua = lua_engine_ctx->lua;
    if (luaL_loadbuffer(lua, blob, sdslen(blob), "@user_function")) {
        *err = sdsempty();
        *err = sdscatprintf(*err, "Error compiling function: %s",
                lua_tostring(lua, -1));
        lua_pop(lua, 1);
        return NULL;
    }

    serverAssert(lua_isfunction(lua, -1));

    int lua_function_ref = luaL_ref(lua, LUA_REGISTRYINDEX);

    luaFunctionCtx *f_ctx = zmalloc(sizeof(*f_ctx));
    *f_ctx = (luaFunctionCtx ) { .lua_function_ref = lua_function_ref, };

    return f_ctx;
}

/*
 * Invole the give function with the given keys and args
 */
static void luaEngineCall(scriptRunCtx *run_ctx,
                          void *engine_ctx,
                          void *compiled_function,
                          robj **keys,
                          size_t nkeys,
                          robj **args,
                          size_t nargs)
{
    luaEngineCtx *lua_engine_ctx = engine_ctx;
    lua_State *lua = lua_engine_ctx->lua;
    luaFunctionCtx *f_ctx = compiled_function;

    /* Push error handler */
    lua_pushstring(lua, REGISTRY_ERROR_HANDLER_NAME);
    lua_gettable(lua, LUA_REGISTRYINDEX);

    lua_rawgeti(lua, LUA_REGISTRYINDEX, f_ctx->lua_function_ref);

    serverAssert(lua_isfunction(lua, -1));

    luaCallFunction(run_ctx, lua, keys, nkeys, args, nargs, 0);
    lua_pop(lua, 1); /* Pop error handler */
}

static size_t luaEngineGetUsedMemoy(void *engine_ctx) {
    luaEngineCtx *lua_engine_ctx = engine_ctx;
    return luaMemory(lua_engine_ctx->lua);
}

static size_t luaEngineFunctionMemoryOverhead(void *compiled_function) {
    return zmalloc_size(compiled_function);
}

static size_t luaEngineMemoryOverhead(void *engine_ctx) {
    luaEngineCtx *lua_engine_ctx = engine_ctx;
    return zmalloc_size(lua_engine_ctx);
}

static void luaEngineFreeFunction(void *engine_ctx, void *compiled_function) {
    luaEngineCtx *lua_engine_ctx = engine_ctx;
    lua_State *lua = lua_engine_ctx->lua;
    luaFunctionCtx *f_ctx = compiled_function;
    lua_unref(lua, f_ctx->lua_function_ref);
    zfree(f_ctx);
}

/* Initialize Lua engine, should be called once on start. */
int luaEngineInitEngine() {
    luaEngineCtx *lua_engine_ctx = zmalloc(sizeof(*lua_engine_ctx));
    lua_engine_ctx->lua = lua_open();

    luaRegisterRedisAPI(lua_engine_ctx->lua);

    /* Save error handler to registry */
    lua_pushstring(lua_engine_ctx->lua, REGISTRY_ERROR_HANDLER_NAME);
    char *errh_func =       "local dbg = debug\n"
                            "local error_handler = function (err)\n"
                            "  local i = dbg.getinfo(2,'nSl')\n"
                            "  if i and i.what == 'C' then\n"
                            "    i = dbg.getinfo(3,'nSl')\n"
                            "  end\n"
                            "  if i then\n"
                            "    return i.source .. ':' .. i.currentline .. ': ' .. err\n"
                            "  else\n"
                            "    return err\n"
                            "  end\n"
                            "end\n"
                            "return error_handler";
    luaL_loadbuffer(lua_engine_ctx->lua, errh_func, strlen(errh_func), "@err_handler_def");
    lua_pcall(lua_engine_ctx->lua,0,1,0);
    lua_settable(lua_engine_ctx->lua, LUA_REGISTRYINDEX);

    /* save the engine_ctx on the registry so we can get it from the Lua interpreter */
    luaSaveOnRegistry(lua_engine_ctx->lua, REGISTRY_ENGINE_CTX_NAME, lua_engine_ctx);

    luaEnableGlobalsProtection(lua_engine_ctx->lua, 0);


    engine *lua_engine = zmalloc(sizeof(*lua_engine));
    *lua_engine = (engine) {
        .engine_ctx = lua_engine_ctx,
        .create = luaEngineCreate,
        .call = luaEngineCall,
        .get_used_memory = luaEngineGetUsedMemoy,
        .get_function_memory_overhead = luaEngineFunctionMemoryOverhead,
        .get_engine_memory_overhead = luaEngineMemoryOverhead,
        .free_function = luaEngineFreeFunction,
    };
    return functionsRegisterEngine(LUA_ENGINE_NAME, lua_engine);
}
