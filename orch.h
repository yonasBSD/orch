/*-
 * Copyright (c) 2024 Kyle Evans <kevans@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <sys/types.h>

#include <stdbool.h>

#include <lua.h>

#define	ORCHLUA_MODNAME	"orch_impl"

struct orch_interp_cfg {
	const char		*scriptf;
	int			 dirfd;
	int			 argc;
	const char		**argv;
};

struct orch_process {
	int			 cmdsock;
	pid_t			 pid;
	int			 termctl;
	bool			 released;
	bool			 eof;
};

/* orch.c */
int orch_spawn(int, const char *[], struct orch_process *);

/* orch_interp.c */
int orch_interp(const char *, int argc, const char *argv[]);

/* orch_lua.c */
void orchlua_configure(struct orch_interp_cfg *);
int luaopen_orch(lua_State *);