/*-
 * Copyright (c) 2024 Kyle Evans <kevans@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>

#include <assert.h>
#include <err.h>
#include <stdlib.h>
#include <string.h>

#include "porch.h"
#include "porch_bin.h"

#include <lauxlib.h>
#include <lualib.h>

static const char porchlua_path[] = PORCHLUA_PATH;

static const char *
porch_interp_script(const char *porch_invoke_path)
{
	static char buf[MAXPATHLEN];

	/* Populate buf first... we cache the script path */
	if (buf[0] == '\0') {
		const char *path;

		path = getenv("PORCHLUA_PATH");
		if (path != NULL && (path[0] == '\0' || path[0] != '/')) {
			fprintf(stderr,
			    "Ignoring empty or relative PORCHLUA_PATH in the environment ('%s')\n",
			    path);
			path = NULL;
		}

		/* Fallback to what's built-in, if no env override. */
		if (path == NULL)
			path = porchlua_path;

		/* If PORCHLUA_PATH is empty, it's in the same path as our binary. */
		if (path[0] == '\0') {
			char *slash;

			if (realpath(porch_invoke_path, buf) == NULL)
				err(1, "realpath %s", porch_invoke_path);

			/* buf now a path to our binary, strip it. */
			slash = strrchr(buf, '/');
			if (slash == NULL)
				errx(1, "failed to resolve porch binary path");

			slash++;
			assert(*slash != '\0');
			*slash = '\0';
		} else {
			strlcpy(buf, path, sizeof(buf));
		}

		strlcat(buf, "/porch.lua", sizeof(buf));
	}

	return (&buf[0]);
}

static int
porch_interp_error(lua_State *L)
{
	const char *err;

	err = lua_tostring(L, -1);
	if (err == NULL)
		err = "unknown";

	fprintf(stderr, "%s\n", err);
	return (1);
}

int
porch_interp(const char *scriptf, const char *porch_invoke_path,
    int argc, const char * const argv[])
{
	lua_State *L;
	int status;

	L = luaL_newstate();
	if (L == NULL)
		errx(1, "luaL_newstate: out of memory");

	/* Open lua's standard library */
	luaL_openlibs(L);

	/* As well as our internal library */
	luaL_requiref(L, PORCHLUA_MODNAME, luaopen_porch_core, 0);
	lua_pop(L, 1);

	if (luaL_dofile(L, porch_interp_script(porch_invoke_path)) != LUA_OK) {
		status = porch_interp_error(L);
	} else {
		/*
		 * porch table is now at the top of stack, fetch run_script()
		 * and call it.  run_script(scriptf[, config])
		 */
		lua_getfield(L, -1, "run_script");
		lua_pushstring(L, scriptf);

		/* config */
		lua_createtable(L, 0, 1);

		/* config.alter_path */
		lua_pushboolean(L, 1);
		lua_setfield(L, -2, "alter_path");

		if (argc > 0) {
			/* config.command */
			lua_createtable(L, argc, 0);
			for (int i = 0; i < argc; i++) {
				lua_pushstring(L, argv[i]);
				lua_rawseti(L, -2, i + 1);
			}

			lua_setfield(L, -2, "command");
		}

		if (lua_pcall(L, 2, 1, 0) == LUA_OK)
			status = lua_toboolean(L, -1) ? 0 : 1;
		else
			status = porch_interp_error(L);
	}

	lua_close(L);
	return (status);
}
