/*
 * Copyright 2010-2020, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "assoc.h"
#include "lua/utils.h"
#include "trivia/util.h"
#include "box/func.h"
#include "box/call.h"
#include "box/port.h"
#include "box/identifier.h"
#include "box/session.h"
#include "box/user.h"
#include "say.h"
#include "diag.h"
#include "tt_static.h"

#include "box/lua/cfunc.h"
#include "box/lua/call.h"

static struct mh_strnptr_t *cfunc_hash;
static unsigned int nr_cfunc = 0;

struct cfunc {
	struct module_sym	mod_sym;
	size_t			name_len;
	char			name[0];
};

struct cfunc *
cfunc_lookup(const char *name, size_t len)
{
	if (name != NULL && len > 0) {
		mh_int_t e = mh_strnptr_find_inp(cfunc_hash, name, len);
		if (e != mh_end(cfunc_hash))
			return mh_strnptr_node(cfunc_hash, e)->val;
	}
	return NULL;
}

static int
cfunc_add(struct cfunc *cfunc)
{
	const struct mh_strnptr_node_t nd = {
		.str	= cfunc->name,
		.len	= cfunc->name_len,
		.hash	= mh_strn_hash(cfunc->name, cfunc->name_len),
		.val	= cfunc,
	};

	mh_int_t h = mh_strnptr_put(cfunc_hash, &nd, NULL, NULL);
	if (h == mh_end(cfunc_hash)) {
		diag_set(OutOfMemory, sizeof(nd), "cfunc_hash_add", "h");
		return -1;
	}
	return 0;
}

static void
luaT_param_type_error(struct lua_State *L, int idx, const char *func_name,
		      const char *param, const char *exp)
{
	const char *typename = idx == 0 ?
		"<unknown>" : lua_typename(L, lua_type(L, idx));
	static const char *fmt =
		"%s: wrong parameter \"%s\": expected %s, got %s";
	diag_set(IllegalParams, fmt, func_name, param, exp, typename);
}

static int
lbox_cfunc_create(struct lua_State *L)
{
	static const char method[] = "cfunc.create";
	struct cfunc *cfunc = NULL;

	if (lua_gettop(L) != 1) {
		static const char *fmt =
			"%s: expects %s(\'name\')";
		diag_set(IllegalParams, fmt, method, method);
		goto out;
	}

	if (lua_type(L, 1) != LUA_TSTRING) {
		luaT_param_type_error(L, 1, method,
				      lua_tostring(L, 1),
				      "function name");
		goto out;
	}

	size_t name_len;
	const char *name = lua_tolstring(L, 1, &name_len);

	if (identifier_check(name, name_len) != 0)
		goto out;

	if (cfunc_lookup(name, name_len) != NULL) {
		const char *fmt = tnt_errcode_desc(ER_FUNCTION_EXISTS);
		diag_set(IllegalParams, fmt, name);
		goto out;
	}

	cfunc = malloc(sizeof(*cfunc) + name_len + 1);
	if (cfunc == NULL) {
		diag_set(OutOfMemory, sizeof(*cfunc), "malloc", "cfunc");
		goto out;
	}

	cfunc->mod_sym.addr	= NULL;
	cfunc->mod_sym.module	= NULL;
	cfunc->mod_sym.name	= cfunc->name;
	cfunc->name_len		= name_len;

	memcpy(cfunc->name, name, name_len);
	cfunc->name[name_len] = '\0';

	if (cfunc_add(cfunc) != 0)
		goto out;

	nr_cfunc++;
	return 0;

out:
	free(cfunc);
	return luaT_error(L);
}

static int
lbox_cfunc_drop(struct lua_State *L)
{
	static const char method[] = "cfunc.drop";
	const char *name = NULL;

	if (lua_gettop(L) != 1) {
		static const char *fmt =
			"%s: expects %s(\'name\')";
		diag_set(IllegalParams, fmt, method, method);
		return luaT_error(L);
	}

	if (lua_type(L, 1) != LUA_TSTRING) {
		luaT_param_type_error(L, 1, method,
				      lua_tostring(L, 1),
				      "function name or id");
		return luaT_error(L);
	}

	size_t name_len;
	name = lua_tolstring(L, 1, &name_len);

	mh_int_t e = mh_strnptr_find_inp(cfunc_hash, name, name_len);
	if (e == mh_end(cfunc_hash)) {
		const char *fmt = tnt_errcode_desc(ER_NO_SUCH_FUNCTION);
		diag_set(IllegalParams, fmt, name);
		return luaT_error(L);
	}

	struct cfunc *cfunc = mh_strnptr_node(cfunc_hash, e)->val;
	mh_strnptr_del(cfunc_hash, e, NULL);

	nr_cfunc--;
	free(cfunc);

	return 0;
}

static int
lbox_cfunc_exists(struct lua_State *L)
{
	static const char method[] = "cfunc.exists";
	if (lua_gettop(L) != 1) {
		static const char *fmt =
			"%s: expects %s(\'name\') but no name passed";
		diag_set(IllegalParams, fmt, method, method);
		return luaT_error(L);
	}

	size_t name_len;
	const char *name = lua_tolstring(L, 1, &name_len);

	struct cfunc *cfunc = cfunc_lookup(name, name_len);
	lua_pushboolean(L, cfunc != NULL ? true : false);

	return 1;
}

static int
lbox_cfunc_reload(struct lua_State *L)
{
	static const char method[] = "cfunc.reload";
	if (lua_gettop(L) != 1 || !lua_isstring(L, 1)) {
		static const char *fmt =
			"%s: expects %s(\'name\') but no name passed";
		diag_set(IllegalParams, fmt, method, method);
		return luaT_error(L);
	}

	size_t name_len;
	const char *name = lua_tolstring(L, 1, &name_len);

	/*
	 * Since we use module engine do not allow to
	 * access arbitrary names only the ones we've
	 * really created.
	 */
	struct cfunc *cfunc = cfunc_lookup(name, name_len);
	if (cfunc == NULL) {
		const char *fmt = tnt_errcode_desc(ER_NO_SUCH_FUNCTION);
		diag_set(IllegalParams, fmt, name);
		return luaT_error(L);
	}

	struct module *module = NULL;
	struct func_name n;

	func_split_name(name, &n);
	if (module_reload(n.package, n.package_end, &module) == 0) {
		if (module != NULL)
			return 0;
		diag_set(ClientError, ER_NO_SUCH_MODULE, name);
	}

	return luaT_error(L);
}

static int
lbox_cfunc_list(struct lua_State *L)
{
	if (nr_cfunc == 0)
		return 0;

	lua_createtable(L, nr_cfunc, 0);

	int nr = 1;
	mh_int_t i;
	mh_foreach(cfunc_hash, i) {
		struct cfunc *c = mh_strnptr_node(cfunc_hash, i)->val;
		lua_pushstring(L, c->name);
		lua_rawseti(L, -2, nr++);
	}

	return 1;
}

static int
lbox_cfunc_call(struct lua_State *L)
{
	static const char method[] = "cfunc.call";
	if (lua_gettop(L) < 1 || !lua_isstring(L, 1)) {
		static const char *fmt =
			"%s: expects %s(\'name\')";
		diag_set(IllegalParams, fmt, method, method);
	}

	size_t name_len;
	const char *name = lua_tolstring(L, 1, &name_len);

	struct cfunc *cfunc = cfunc_lookup(name, name_len);
	if (cfunc == NULL) {
		const char *fmt = tnt_errcode_desc(ER_NO_SUCH_FUNCTION);
		diag_set(IllegalParams, fmt, name);
		return luaT_error(L);
	}

	lua_State *args_L = luaT_newthread(tarantool_L);
	if (args_L == NULL)
		return luaT_error(L);

	int coro_ref = luaL_ref(tarantool_L, LUA_REGISTRYINDEX);
	lua_xmove(L, args_L, lua_gettop(L) - 1);

	struct port args;
	port_lua_create(&args, args_L);
	((struct port_lua *)&args)->ref = coro_ref;

	struct port ret;
	if (module_sym_call(&cfunc->mod_sym,  &args, &ret) != 0) {
		port_destroy(&args);
		return luaT_error(L);
	}

	int top = lua_gettop(L);
	port_dump_lua(&ret, L, true);
	int cnt = lua_gettop(L) - top;

	port_destroy(&ret);
	port_destroy(&args);
	return cnt;
}

int
cfunc_init(void)
{
	cfunc_hash = mh_strnptr_new();
	if (cfunc_hash == NULL) {
		diag_set(OutOfMemory, sizeof(*cfunc_hash), "malloc",
			  "cfunc hash table");
		return -1;
	}
	return 0;
}

void
cfunc_free(void)
{
	while (mh_size(cfunc_hash) > 0) {
		mh_int_t e = mh_first(cfunc_hash);
		struct cfunc *c = mh_strnptr_node(cfunc_hash, e)->val;
		module_sym_unload(&c->mod_sym);
		mh_strnptr_del(cfunc_hash, e, NULL);
	}
	mh_strnptr_delete(cfunc_hash);
	cfunc_hash = NULL;
}

void
box_lua_cfunc_init(struct lua_State *L)
{
	static const struct luaL_Reg cfunclib[] = {
		{ "create",	lbox_cfunc_create },
		{ "drop",	lbox_cfunc_drop },
		{ "exists",	lbox_cfunc_exists },
		{ "call",	lbox_cfunc_call },
		{ "reload",	lbox_cfunc_reload },
		{ "list",	lbox_cfunc_list },
		{ }
	};

	luaL_register_module(L, "cfunc", cfunclib);
	lua_pop(L, 1);
}
