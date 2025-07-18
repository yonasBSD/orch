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

static struct porch_incl {
	const char		*incl_path;
	struct porch_incl	*incl_next;
} *porch_incl_head, *porch_incl_last;
static size_t porch_incl_count = 0;

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

static void
porch_setup_pkgpath(lua_State *L)
{
	const char *pkg_path, *env_path;

	/*
	 * If PORCHLUA_PATH is specified in the environment, we need to also add
	 * it to package.path to get our scripts from there.  We can't do
	 * anything about the C modules that we statically compiled in, though.
	 */
	env_path = getenv("PORCHLUA_PATH");
	if (env_path == NULL || env_path[0] == '\0' || env_path[0] != '/')
		return;

	lua_getglobal(L, "package");
	lua_getfield(L, -1, "path");

	pkg_path = lua_tostring(L, -1);

	lua_pushfstring(L, "%s/?.lua;%s", env_path, pkg_path);
	lua_setfield(L, -3, "path");

	/* Pop both the package table, and the original package.path value. */
	lua_pop(L, 2);
}

void
porch_interp_include(const char *scriptf)
{
	struct porch_incl *next_incl;

	next_incl = calloc(1, sizeof(*next_incl));
	if (next_incl == NULL)
		err(1, "malloc");

	next_incl->incl_path = scriptf;
	if (porch_incl_last == NULL) {
		porch_incl_head = porch_incl_last = next_incl;
	} else {
		porch_incl_last->incl_next = next_incl;
		porch_incl_last = next_incl;
	}

	porch_incl_count++;
}

static void
porch_interp_include_table(lua_State *L)
{
	struct porch_incl *next, *walker;
	size_t idx = 1;

	lua_createtable(L, porch_incl_count, 0);
	walker = porch_incl_head;
	while (walker != NULL) {
		next = walker->incl_next;

		lua_pushstring(L, walker->incl_path);
		lua_rawseti(L, -2, idx);

		idx++;
		porch_incl_count--;

		free(walker);
		walker = next;
	}

	porch_incl_head = porch_incl_last = NULL;
	assert(porch_incl_count == 0);
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

	porch_setup_pkgpath(L);

	/* As well as our internal library */
	luaL_requiref(L, PORCHLUA_MODNAME, luaopen_porch_core, 0);
	lua_pop(L, 1);

	if (luaL_dofile(L, porch_interp_script(porch_invoke_path)) != LUA_OK) {
		status = porch_interp_error(L);
	} else {
		int nargs;

		/*
		 * porch table is now at the top of stack, fetch the appropriate
		 * function and call it.
		 *
		 * porchgen: generate_script(config)
		 * porch and rporch: run_script(scriptf[, config])
		 */
		if (porch_mode == PMODE_GENERATE)
			lua_getfield(L, -1, "generate_script");
		else
			lua_getfield(L, -1, "run_script");
		lua_pushstring(L, scriptf);
		nargs = 2;	/* scriptf, config */

		/* config */
		lua_createtable(L, 0, 3);

		/* config.allow_exit */
		lua_pushboolean(L, 1);
		lua_setfield(L, -2, "allow_exit");

		/* config.alter_path */
		lua_pushboolean(L, 1);
		lua_setfield(L, -2, "alter_path");

		if (porch_incl_count > 0) {
			/* config.includes */
			porch_interp_include_table(L);
			lua_setfield(L, -2, "includes");
		}

		switch (porch_mode) {
		case PMODE_REMOTE:
			/* config.remote */
			lua_createtable(L, 0, 2);

			if (argc == 1 && argv[0][0] != '\0') {
				/* config.remote[host] */
				lua_pushstring(L, argv[0]);
				lua_setfield(L, -2, "host");
			}

			/* config.remote[rsh] */
			lua_pushstring(L, porch_rsh);
			lua_setfield(L, -2, "rsh");

			lua_setfield(L, -2, "remote");
			break;
		case PMODE_GENERATE:
		case PMODE_LOCAL:
			if (argc > 0) {
				/* config.command */
				lua_createtable(L, argc, 0);
				for (int i = 0; i < argc; i++) {
					lua_pushstring(L, argv[i]);
					lua_rawseti(L, -2, i + 1);
				}

				lua_setfield(L, -2, "command");
			}

			break;
		}

		if (lua_pcall(L, nargs, 2, 0) == LUA_OK && !lua_isnil(L, -2))
			status = lua_toboolean(L, -2) ? 0 : 1;
		else
			status = porch_interp_error(L);
	}

	lua_close(L);
	return (status);
}
