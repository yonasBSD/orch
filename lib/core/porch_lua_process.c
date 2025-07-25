/*-
 * Copyright (c) 2024, 2025 Kyle Evans <kevans@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define	_FILE_OFFSET_BITS	64	/* Linux ino64 */

#include <sys/param.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "porch_lua.h"

#ifndef INFTIM
#define	INFTIM	(-1)
#endif

#define	ORCHLUA_PSTATUSHANDLE	"porchlua_process_status"
static void porchlua_register_pstatus_metatable(lua_State *L);

struct process_status {
	int		status;
	int		raw_status;
	bool		is_exited;
	bool		is_signaled;
	bool		is_stopped;
};

static void
porchlua_process_close_alarm(int signo __unused)
{
	/* XXX Ignored, just don't terminate us. */
}

static int
porchlua_process_wait(struct porch_process *self, int wflags)
{
	int wpid;

	assert((wflags & (WUNTRACED | WCONTINUED)) != (WUNTRACED | WCONTINUED));
	for (;;) {
		while ((wpid = waitpid(self->pid, &self->status, wflags)) == -1) {
			if (errno == EINTR)
				continue;
			return (-1);
		}

		/*
		 * If we're specifically waiting for either a stopped or continued
		 * process, then we'll keep looping until we've observed the
		 * correct status.  Odds are that won't happen, but the caller might
		 * have specified WNOHANG for some reason.
		 */
		if ((wflags & WUNTRACED) != 0 && WIFSTOPPED(self->status))
			break;
		if ((wflags & WCONTINUED) != 0 && WIFCONTINUED(self->status))
			break;

		/*
		 * Of course, if we just reaped the child, then we can't really
		 * come back from that.
		 */
		if (WIFEXITED(self->status) || WIFSIGNALED(self->status))
			break;
	}

	return (0);
}

static bool
porchlua_process_killed(struct porch_process *self, int *signo, bool hang)
{
	int flags = hang ? 0 : WNOHANG;

	assert(self->pid != 0);
	if (waitpid(self->pid, &self->status, flags) != self->pid)
		return (false);

	if (signo != NULL) {
		if (WIFSIGNALED(self->status))
			*signo = WTERMSIG(self->status);
		else
			*signo = 0;
	}

	self->pid = 0;

	return (true);
}

static void
porchlua_process_drain(lua_State *L, struct porch_process *self)
{

	/*
	 * Caller should have failed gracefully if the lua bits didn't set us up
	 * right.
	 */
	assert(lua_gettop(L) >= 2);

	/*
	 * Make a copy of the drain function, we may need to call it multiple times.
	 */
	self->draining = true;
	lua_pushvalue(L, -1);
	lua_call(L, 0, 0);
	self->draining = false;
}

static int
porchlua_process_chdir(lua_State *L)
{
	struct porch_ipc_msg *msg;
	struct porch_process *self;
	const char *dir;
	void *mdir;
	size_t dirsz;
	int error;

	self = luaL_checkudata(L, 1, ORCHLUA_PROCESSHANDLE);
	dir = lua_tolstring(L, 2, &dirsz);
	if (!porch_ipc_okay(self->ipc)) {
		luaL_pushfail(L);
		lua_pushstring(L, "process already released");
		return (2);
	}

	msg = porch_ipc_msg_alloc(IPC_CHDIR, dirsz + 1, &mdir);
	if (msg == NULL)
		goto err;

	memcpy(mdir, dir, dirsz);
	error = porch_lua_ipc_send_acked_errno(L, self, msg, IPC_CHDIR_ACK);
	if (error < 0)
		goto err;
	else if (error > 0)
		return (error);

	lua_pushboolean(L, 1);
	return (1);
err:
	luaL_pushfail(L);
	lua_pushstring(L, strerror(errno));
	return (2);
}

static int
porchlua_process_close(lua_State *L)
{
	struct porch_process *self;
	pid_t wret;
	int sig;
	bool failed;

	failed = false;
	self = luaL_checkudata(L, 1, ORCHLUA_PROCESSHANDLE);
	if (self->pid != 0 && porchlua_process_killed(self, &sig, false) &&
	    sig != 0) {
		luaL_pushfail(L);
		lua_pushfstring(L, "spawned process killed with signal '%d'", sig);
		return (2);
	}

	if (lua_gettop(L) < 2 || lua_isnil(L, 2)) {
		luaL_pushfail(L);
		lua_pushstring(L, "missing drain callback");
		return (2);
	}

	if (self->pid != 0) {
		struct sigaction sigalrm = {
			.sa_handler = porchlua_process_close_alarm,
		};

		sigaction(SIGALRM, &sigalrm, NULL);

		sig = SIGTERM;
again:
		/*
		 * We would still want an error if we terminate as a result of this
		 * signal.
		 */
		self->last_signal = -1;
		if (kill(self->pid, sig) == -1)
			warn("kill %d", sig);

		/* XXX Configurable? */
		if (sig != SIGKILL)
			alarm(5);
		if (sig == SIGKILL) {
			/*
			 * Once we've sent SIGKILL, we're tired of it; just drop the pty and
			 * anything that might've been added to the buffer after our SIGTERM.
			 */
			if (self->termctl != -1) {
				close(self->termctl);
				self->termctl = -1;
			}
		} else {
			/*
			 * Some systems (e.g., Darwin/XNU) will wait for us to drain the tty
			 * when the controlling process exits.  We'll do that before we
			 * attempt to signal it, just in case.
			 */
			porchlua_process_drain(L, self);
		}

		wret = waitpid(self->pid, &self->status, 0);
		alarm(0);
		if (wret != self->pid) {
			failed = true;

			/* If asking nicely didn't work, just kill it. */
			if (sig != SIGKILL) {
				sig = SIGKILL;
				goto again;
			}
		}

		signal(SIGALRM, SIG_DFL);
		self->pid = 0;
	}

	porch_ipc_close(self->ipc);
	self->ipc = NULL;

	if (self->termctl != -1)
		close(self->termctl);
	self->termctl = -1;

	if (failed) {
		luaL_pushfail(L);
		lua_pushstring(L, "could not kill process with SIGTERM");
		return (2);
	}

	lua_pushboolean(L, 1);
	return (1);
}

static int
porchlua_process_continue(lua_State *L)
{
	struct porch_process *self;
	int error;
	bool sendsig = true;

	/*
	 * The caller can choose to avoid sending SIGCONT by passing a falsey
	 * value in.  We assume they expect an external force to resume it.
	 */
	self = luaL_checkudata(L, 1, ORCHLUA_PROCESSHANDLE);
	if (lua_gettop(L) >= 2)
		sendsig = lua_toboolean(L, 2);

	if (sendsig && kill(self->pid, SIGCONT) != 0)
		goto failed;

	error = porchlua_process_wait(self, WCONTINUED);
	if (error == -1)
		goto failed;
	if (!WIFCONTINUED(self->status)) {
		luaL_pushfail(L);
		lua_pushstring(L, "Process seems to have terminated");
		return (2);
	}

	lua_pushboolean(L, 1);
	return (1);
failed:
	error = errno;

	luaL_pushfail(L);
	lua_pushstring(L, strerror(error));
	return (2);
}

static int
porchlua_process_eof(lua_State *L)
{
	struct porch_process *self;
	struct process_status *pstatus;
	int timeout;

	self = luaL_checkudata(L, 1, ORCHLUA_PROCESSHANDLE);

	/*
	 * We take a timeout in case we need to wait(2) on the process.  Just
	 * beccause we've observed EOF, that doesn't strictly mean that the
	 * process will be exiting; perhaps it closed stdout/stderr for some
	 * other reason.
	 */
	timeout = -1;
	if (lua_gettop(L) >= 2 && !lua_isnil(L, 2))
		timeout = luaL_checkinteger(L, 2);

	if (!self->eof) {
		lua_pushboolean(L, 0);
		return (1);
	}

	lua_pushboolean(L, 1);

	/*
	 * If we hit EOF, we'll generate a pstatus object that the caller can
	 * either discard or pass around for examination.
	 */
	if (self->pid != 0) {
		bool hang, killed;

		hang = true;
		if (timeout > 0) {
			struct sigaction sigalrm = {
				.sa_handler = porchlua_process_close_alarm,
			};

			sigaction(SIGALRM, &sigalrm, NULL);
			alarm(timeout);
		} else if (timeout == 0) {
			hang = false;
		}

		killed = porchlua_process_killed(self, NULL, hang);

		if (timeout > 0) {
			alarm(0);
			signal(SIGALRM, SIG_DFL);
		}

		/*
		 * It's possible that we hit EOF without having exited yet, in
		 * which case we'll just return true rather than a wait status.
		 */
		if (!killed)
			return (1);
	}

	assert(self->pid == 0);

	pstatus = lua_newuserdata(L, sizeof(*pstatus));
	pstatus->raw_status = self->status;
	pstatus->is_exited = WIFEXITED(self->status);
	pstatus->is_signaled = WIFSIGNALED(self->status);
	pstatus->is_stopped = WIFSTOPPED(self->status);

	if (pstatus->is_exited) {
		pstatus->status = WEXITSTATUS(self->status);
	} else if (pstatus->is_signaled) {
		pstatus->status = WTERMSIG(self->status);
	} else if (pstatus->is_stopped) {
		pstatus->status = WSTOPSIG(self->status);
	}

	luaL_setmetatable(L, ORCHLUA_PSTATUSHANDLE);

	return (2);
}

static int
porchlua_process_gid(lua_State *L)
{
	struct porch_process *self;

	self = luaL_checkudata(L, 1, ORCHLUA_PROCESSHANDLE);
	lua_pushinteger(L, self->gid);
	return (1);
}

static int
porchlua_process_proxy_read(lua_State *L, int fd, int fn, bool *eof)
{
	char buf[4096];
	ssize_t readsz;

retry:
	readsz = read(fd, buf, sizeof(buf));
	if (readsz == -1 && errno == EINTR)
		goto retry;
	if (readsz == -1) {
		int serrno = errno;

		luaL_pushfail(L);
		lua_pushstring(L, strerror(serrno));
		return (2);
	}

	lua_pushvalue(L, fn);
	if (readsz == 0) {
		*eof = true;
		lua_pushnil(L);
	} else {
		lua_pushlstring(L, buf, readsz);
	}

	lua_call(L, 1, 0);
	return (0);
}

/*
 * proxy(file, outputfn, inputfn[, pulsefn]) -- signal that we're proxy'ing the
 * `file` stream into the process.  Lines read from `file` will be passed into
 * the `inputfn` for processing, and lines read from the process will be passed
 * into the `outputfn` for processing.  This function will put the `file`
 * stream into unbuffered mode.  The `pulsefn` will be invoked every second if
 * there is no input or output.
 */
static int
porchlua_process_proxy(lua_State *L)
{
	struct pollfd pfd[2];
	struct porch_process *self;
	luaL_Stream *p;
	FILE *inf;
	struct termios term;
	int infd, outfd, ready, ret, timeout;
	bool bailed, eof, has_pulse;

	bailed = eof = false;
	self = luaL_checkudata(L, 1, ORCHLUA_PROCESSHANDLE);
	p = (luaL_Stream *)luaL_checkudata(L, 2, LUA_FILEHANDLE);
	luaL_checktype(L, 3, LUA_TFUNCTION);	/* outputfn */
	luaL_checktype(L, 4, LUA_TFUNCTION);	/* inputfn */
	has_pulse = lua_gettop(L) >= 5 && !lua_isnil(L, 5);
	if (has_pulse) {
		luaL_checktype(L, 5, LUA_TFUNCTION);	/* pulsefn */
		timeout = 1000;	/* pulsefn invoked every second. */
	} else {
		timeout = INFTIM;
	}

	inf = p->f;
	outfd = self->termctl;

	infd = dup(fileno(inf));
	if (infd == -1) {
		int serrno = errno;

		luaL_pushfail(L);
		lua_pushstring(L, strerror(serrno));
		return (2);
	}

	if (tcgetattr(infd, &term) == 0) {
		term.c_lflag &= ~(ICANON | ISIG);

		if (tcsetattr(infd, TCSANOW, &term) != 0) {
			int serrno = errno;

			luaL_pushfail(L);
			lua_pushstring(L, strerror(serrno));
			return (2);
		}
	} else if (errno != ENOTTY) {
		int serrno = errno;

		luaL_pushfail(L);
		lua_pushstring(L, strerror(serrno));
		return (2);
	}

	pfd[0].fd = outfd;
	pfd[0].events = POLLIN;

	pfd[1].fd = infd;
	pfd[1].events = POLLIN;

	while (!eof) {
		ready = poll(pfd, 2, timeout);
		if (ready == -1 && errno == EINTR)
			continue;
		if (ready == -1) {
			int serrno = errno;

			luaL_pushfail(L);
			lua_pushstring(L, strerror(serrno));
			return (2);
		}

		if (ready == 0) {
			assert(has_pulse);

			lua_pushvalue(L, 5);
			lua_call(L, 0, 1);
			bailed = !lua_toboolean(L, -1);
			lua_pop(L, 1);

			if (bailed)
				break;

			continue;
		}

		if ((pfd[0].revents & POLLIN) != 0) {
			ret = porchlua_process_proxy_read(L, outfd, 3, &eof);

			if (ret > 0)
				return (ret);

			if (eof) {
				if (self->pid == 0 ||
				    porchlua_process_killed(self, NULL, true)) {
					bailed = !WIFEXITED(self->status) ||
					    WEXITSTATUS(self->status) != 0;
				} else {
					bailed = true;
				}
			}
		}

		if ((pfd[1].revents & POLLIN) != 0) {
			ret = porchlua_process_proxy_read(L, infd, 4, &eof);
			if (ret > 0)
				return (ret);

			if (eof)
				bailed = true;
		} else if (eof) {
			/*
			 * Signal EOF to the input function if we didn't have
			 * any input, so that it can wrap up the script.
			 */
			lua_pushvalue(L, 4);
			lua_pushnil(L);
			lua_call(L, 1, 0);
		}
	}

	lua_pushboolean(L, !bailed);
	return (1);
}

/*
 * read(callback[, timeout]) -- returns true if we finished, false if we
 * hit EOF, or a fail, error pair otherwise.
 */
static int
porchlua_process_read(lua_State *L)
{
	char buf[LINE_MAX];
	fd_set rfd;
	struct porch_process *self;
	struct timeval tv, *tvp;
	time_t start, now;
	ssize_t readsz;
	int fd, ret;
	lua_Number timeout;

	self = luaL_checkudata(L, 1, ORCHLUA_PROCESSHANDLE);
	luaL_checktype(L, 2, LUA_TFUNCTION);

	if (lua_gettop(L) > 2) {
		timeout = luaL_checknumber(L, 3);
		if (timeout < 0) {
			luaL_pushfail(L);
			lua_pushstring(L, "Invalid timeout");
			return (2);
		} else {
			timeout = MAX(timeout, 1);
		}

		tv.tv_sec = timeout;
		tv.tv_usec = 0;
		tvp = &tv;
	} else {
		/* No timeout == block */
		tvp = NULL;
	}

	fd = self->termctl;
	FD_ZERO(&rfd);

	/* Crude */
	if (tvp != NULL)
		start = now = time(NULL);
	else
		start = now = timeout = 0;
	while ((tvp == NULL || now - start < timeout) && !self->error) {
		FD_SET(fd, &rfd);
		ret = select(fd + 1, &rfd, NULL, NULL, tvp);
		if (ret == -1 && errno == EINTR) {
			/*
			 * Rearm the timeout and go again; we'll just let the loop
			 * terminate with a negative tv.tv_sec if timeout seconds
			 * have elapsed.
			 */
			if (tvp != NULL) {
				now = time(NULL);
				tv.tv_sec = timeout - (now - start);
			}

			if (!self->draining)
				continue;

			/* Timeout */
			ret = 0;
		}

		if (ret == -1) {
			int err = errno;

			luaL_pushfail(L);
			lua_pushstring(L, strerror(err));
			return (2);
		} else if (ret == 0) {
			/* Timeout -- not the end of the world. */
			lua_pushboolean(L, 1);
			return (1);
		}

		/* Read it */
		readsz = read(fd, buf, sizeof(buf));

		/*
		 * Some platforms will return `0` when the slave side of a pty
		 * has gone away, while others will return -1 + EIO.  Convert
		 * the latter to the former.
		 */
		if (readsz == -1 && errno == EIO)
			readsz = 0;
		if (readsz < 0) {
			int err = errno;

			luaL_pushfail(L);
			lua_pushstring(L, strerror(err));
			return (2);
		} else {
			int nargs = 0;

			/*
			 * Duplicate the function value, it'll get popped by the call.
			 */
			lua_settop(L, 3);
			lua_copy(L, -2, -1);

			/* callback([data]) -- nil data == EOF */
			if (readsz > 0) {
				lua_pushlstring(L, buf, readsz);
				nargs++;
			}

			/*
			 * Callback should return true if it's done, false if it
			 * wants more.
			 */
			lua_call(L, nargs, 1);

			if (readsz == 0) {
				int signo;

				self->eof = true;

				assert(self->termctl >= 0);
				close(self->termctl);
				self->termctl = -1;

				if (!self->draining &&
				    porchlua_process_killed(self, &signo, false) &&
				    signo != 0 && signo != self->last_signal) {
					luaL_pushfail(L);
					lua_pushfstring(L,
						"spawned process killed with signal '%d'", signo);
					return (2);
				}

				/*
				 * We need to be able to distinguish between a disaster
				 * scenario and possibly business as usual, so we'll
				 * return true if we hit EOF.  This lets us assert()
				 * on the return value and catch bad program exits.
				 */
				lua_pushboolean(L, 1);
				return (1);
			}

			if (lua_toboolean(L, -1))
				break;
		}
	}

	lua_pushboolean(L, 1);
	return (1);
}

static bool
porchlua_do_env(lua_State *L, struct porch_process *self, int index)
{
	struct porch_env *penv;
	struct porch_ipc_msg *msg;
	const char *setstr, *unsetstr;
	size_t envsz, setsz, unsetsz;
	bool clear = false;

	/* Push the "expand" method */
	lua_getfield(L, index, "expand");
	lua_pushvalue(L, index);
	lua_call(L, 1, 3);

	clear = lua_toboolean(L, -1);
	lua_pop(L, 1);

	/* Remaining: unset @ -1, set @ -2 */
	unsetstr = lua_tolstring(L, -1, &unsetsz);
	setstr = lua_tolstring(L, -2, &setsz);

	if (setsz > 0 && setstr[setsz - 1] != '\0') {
		luaL_pushfail(L);
		lua_pushstring(L, "Malformed env string");
		return (2);
	}

	assert(setsz != 0 || unsetsz != 0 || clear);

	envsz = sizeof(*penv) + setsz + unsetsz;
	msg = porch_ipc_msg_alloc(IPC_ENV_SETUP, envsz, (void **)&penv);
	if (msg == NULL) {
		luaL_pushfail(L);
		lua_pushstring(L, strerror(ENOMEM));
		return (2);
	}

	penv->clear = clear;
	penv->setsz = setsz;
	penv->unsetsz = unsetsz;
	if (setsz > 0)
		memcpy(&penv->envstr[0], setstr, setsz);
	if (unsetsz > 0)
		memcpy(&penv->envstr[setsz], unsetstr, unsetsz);
	lua_pop(L, 2);

	return (porch_lua_ipc_send_acked(L, self, msg, IPC_ENV_ACK));
}

static int
porchlua_process_release(lua_State *L)
{
	struct porch_process *self;
	int error;

	self = luaL_checkudata(L, 1, ORCHLUA_PROCESSHANDLE);

	if (lua_istable(L, 2)) {
		int ret;

		ret = porchlua_do_env(L, self, 2);
		if (ret != 0)
			return (ret);
	}

	error = porch_release(self->ipc);
	porch_ipc_close(self->ipc);
	self->ipc = NULL;

	if (error != 0) {
		error = errno;

		luaL_pushfail(L);
		lua_pushstring(L, strerror(error));
		return (2);
	}

	self->released = true;

	lua_pushboolean(L, 1);
	return (1);
}

static int
porchlua_process_released(lua_State *L)
{
	struct porch_process *self;

	self = luaL_checkudata(L, 1, ORCHLUA_PROCESSHANDLE);
	lua_pushboolean(L, self->released);
	return (1);
}

static bool
porch_resolve_gid(const char *idstr, gid_t *gid)
{
	struct group *grp;

	errno = 0;
	grp = getgrnam(idstr);
	if (grp == NULL) {
		if (errno == 0)
			errno = ENOENT;
		return (false);
	}

	*gid = grp->gr_gid;
	return (true);
}

static bool
porch_resolve_uid(const char *idstr, uid_t *uid)
{
	struct passwd *pwd;

	errno = 0;
	pwd = getpwnam(idstr);
	if (pwd == NULL) {
		if (errno == 0)
			errno = ENOENT;
		return (false);
	}

	*uid = pwd->pw_uid;
	return (true);
}

static int
porchlua_process_setgroups(lua_State *L)
{
	struct porch_process *self;
	struct porch_setgroups *sgrp = NULL;
	struct porch_ipc_msg *msg;
	void *msgrp;
	size_t sgrpsz;
	int error, nargs, serrno;

	self = luaL_checkudata(L, 1, ORCHLUA_PROCESSHANDLE);
	nargs = lua_gettop(L) - 1;

	sgrpsz = PORCH_SETGROUPS_SIZE(nargs);
	sgrp = malloc(sgrpsz);
	if (sgrp == NULL)
		goto err;

	sgrp->setgroups_cnt = nargs;
	for (int idx = 0; idx < nargs; idx++) {
		const char *idstr;

		if (!lua_isinteger(L, 2 + idx)) {
			idstr = luaL_checkstring(L, 2 + idx);
			if (!porch_resolve_gid(idstr, &sgrp->setgroups_gids[idx]))
				goto err;
		} else {
			sgrp->setgroups_gids[idx] = luaL_checkinteger(L, 2 + idx);
		}
	}

	msg = porch_ipc_msg_alloc(IPC_SETGROUPS, sgrpsz, &msgrp);
	if (msg == NULL)
		goto err;

	memcpy(msgrp, sgrp, sgrpsz);
	error = porch_lua_ipc_send_acked_errno(L, self, msg, IPC_SETGROUPS_ACK);
	if (error < 0)
		goto err;
	else if (error > 0)
		return (error);

#ifdef __FreeBSD__
	/*
	 * FreeBSD seems to be the only OS in 2025 that will change the egid
	 * based on a setgroups(2) call; the rest that have been examined will
	 * exclusively touch secondary groups.
	 */
	if (nargs > 0)
		self->gid = sgrp->setgroups_gids[0];
#endif
	free(sgrp);

	lua_pushboolean(L, 1);
	return (1);

err:
	serrno = errno;

	free(sgrp);
	luaL_pushfail(L);
	lua_pushstring(L, strerror(serrno));
	return (2);
}

static int
porchlua_process_setid(lua_State *L)
{
	const char *idstr;
	struct porch_ipc_msg *msg;
	struct porch_process *self;
	void *msid;
	struct porch_setid sid;
	int error, flags = 0, serrno;

	self = luaL_checkudata(L, 1, ORCHLUA_PROCESSHANDLE);
	if (!lua_isnoneornil(L, 2))
		flags |= SID_SETUID;
	if (!lua_isnoneornil(L, 3))
		flags |= SID_SETGID;

	if (flags == 0)
		goto done;

	if ((flags & SID_SETUID) != 0) {
		if (!lua_isinteger(L, 2)) {
			idstr = luaL_checkstring(L, 2);
			if (!porch_resolve_uid(idstr, &sid.setid_uid))
				goto err;
		} else {
			sid.setid_uid = luaL_checkinteger(L, 2);
		}

		if (sid.setid_uid == self->uid)
			flags &= ~SID_SETUID;
	}

	if ((flags & SID_SETGID) != 0) {
		if (!lua_isinteger(L, 3)) {
			idstr = luaL_checkstring(L, 3);
			if (!porch_resolve_gid(idstr, &sid.setid_gid))
				goto err;
		} else {
			sid.setid_gid = luaL_checkinteger(L, 3);
		}

		if (sid.setid_gid == self->gid)
			flags &= ~SID_SETGID;
	}

	if (flags == 0)
		goto done;

	sid.setid_flags = flags;
	msg = porch_ipc_msg_alloc(IPC_SETID, sizeof(sid), &msid);
	if (msg == NULL)
		goto err;

	memcpy(msid, &sid, sizeof(sid));
	error = porch_lua_ipc_send_acked_errno(L, self, msg, IPC_SETID_ACK);
	if (error < 0)
		goto err;
	else if (error > 0)
		return (error);

	if ((flags & SID_SETUID) != 0)
		self->uid = sid.setid_uid;
	if ((flags & SID_SETGID) != 0)
		self->gid = sid.setid_gid;
done:
	lua_pushinteger(L, self->uid);
	lua_pushinteger(L, self->gid);
	return (2);

err:
	serrno = errno;

	luaL_pushfail(L);
	lua_pushstring(L, strerror(serrno));
	return (2);
}

static int
porchlua_process_term_set(porch_ipc_t ipc __unused, struct porch_ipc_msg *msg,
    void *cookie)
{
	struct porch_term *term = cookie;
	struct termios *child_termios;
	struct termios *parent_termios = &term->term;
	size_t datasz;

	child_termios = porch_ipc_msg_payload(msg, &datasz);
	if (child_termios == NULL || datasz != sizeof(*child_termios)) {
		errno = EINVAL;
		return (-1);
	}

	memcpy(parent_termios, child_termios, sizeof(*child_termios));
	term->initialized = true;
	term->winsz_valid = false;

	return (0);
}

static int
porch_sigset2table(lua_State *L, const sigset_t *sigset)
{
	int ret, sigmax;

	sigmax = porch_sigmax();
	lua_newtable(L);
	for (int signo = 1; signo < sigmax; signo++) {
		ret = sigismember(sigset, signo);
		if (ret == -1)
			ret = 0;
		lua_pushboolean(L, ret);
		lua_rawseti(L, -2, signo);
	}

	return (1);
}

static void
porch_table2sigset(lua_State *L, int idx, sigset_t *sigset)
{
	int sigmax;

	assert(idx > 0);
	sigmax = porch_sigmax();
	for (int signo = 1; signo < sigmax; signo++) {
		bool present;

		lua_geti(L, idx, signo);
		present = lua_toboolean(L, -1);
		lua_pop(L, 1);

		if (!present)
			continue;

		/*
		 * This may fail if we had an internal signal,
		 * but we'll just ignore that here.
		 */
		(void)sigaddset(sigset, signo);
	}
}

static int
porchlua_process_sigcatch(lua_State *L)
{
	struct porch_process *self;
	struct porch_ipc_msg *msg;
	struct porch_sigcatch *catchmsg;
	sigset_t newmask;
	int error;
	bool catch;

	self = luaL_checkudata(L, 1, ORCHLUA_PROCESSHANDLE);
	if (lua_gettop(L) < 2 || lua_isnil(L, 2)) {
		/* Fetch the signal caught mask in table form. */
		return (porch_sigset2table(L, &self->sigcaughtmask));
	}

	catch = lua_toboolean(L, 2);
	luaL_checktype(L, 3, LUA_TTABLE);

	sigemptyset(&newmask);
	porch_table2sigset(L, 3, &newmask);

	/* Mask was valid, now to apply it if we're not too late. */
	if (!porch_ipc_okay(self->ipc)) {
		luaL_pushfail(L);
		lua_pushstring(L, "process already released");
		return (2);
	}

	msg = porch_ipc_msg_alloc(IPC_SIGCATCH, sizeof(*catchmsg),
	    (void **)&catchmsg);
	if (msg == NULL)
		goto err;

	memcpy(&catchmsg->mask, &newmask, sizeof(catchmsg->mask));
	catchmsg->catch = catch;
	error = porch_lua_ipc_send_acked_errno(L, self, msg, IPC_SIGCATCH_ACK);
	if (error < 0)
		goto err;
	else if (error > 0)
		return (error);

	porch_mask_apply(!catch, &self->sigcaughtmask, &newmask);

	lua_pushboolean(L, 1);
	return (1);
err:
	luaL_pushfail(L);
	lua_pushstring(L, strerror(errno));
	return (2);
}

static int
porchlua_process_sigmask(lua_State *L)
{
	struct porch_process *self;
	struct porch_ipc_msg *msg;
	sigset_t newmask, *sendmask;
	int error;

	self = luaL_checkudata(L, 1, ORCHLUA_PROCESSHANDLE);
	if (lua_gettop(L) < 2 || lua_isnil(L, 2)) {
		/* Fetch the current mask in table form. */
		return (porch_sigset2table(L, &self->sigmask));
	}

	sigemptyset(&newmask);
	if (lua_isinteger(L, 2)) {
		lua_Number n;

		n = luaL_checkinteger(L, 2);
		if (n != 0) {
			luaL_pushfail(L);
			lua_pushfstring(L, "Expected table or 0, got %d", n);
			return (2);
		}
	} else {
		luaL_checktype(L, 2, LUA_TTABLE);
		porch_table2sigset(L, 2, &newmask);
	}

	/* Mask was valid, now to apply it if we're not too late. */
	if (!porch_ipc_okay(self->ipc)) {
		luaL_pushfail(L);
		lua_pushstring(L, "process already released");
		return (2);
	}

	msg = porch_ipc_msg_alloc(IPC_SETMASK, sizeof(*sendmask),
	    (void **)&sendmask);
	if (msg == NULL)
		goto err;

	memcpy(sendmask, &newmask, sizeof(*sendmask));
	error = porch_lua_ipc_send_acked_errno(L, self, msg, IPC_SETMASK_ACK);
	if (error < 0)
		goto err;
	else if (error > 0)
		return (error);

	memcpy(&self->sigmask, &newmask, sizeof(self->sigmask));
	lua_pushboolean(L, 1);
	return (1);
err:
	luaL_pushfail(L);
	lua_pushstring(L, strerror(errno));
	return (2);
}

static int
porchlua_process_signal(lua_State *L)
{
	struct porch_process *self;
	int signal;

	self = luaL_checkudata(L, 1, ORCHLUA_PROCESSHANDLE);

	/*
	 * We don't bother validating anything here in case they're wanting to
	 * use a signal that we don't know about.  kill(2) can validate this
	 * stuff better than we can.
	 */
	signal = luaL_checkinteger(L, 2);

	if (self->ipc != NULL) {
		/*
		 * We don't accept signaling processes before they're released
		 * for reasons, including because it doesn't seem useful to test
		 * how porch(1) itself handles signals.
		 */
		luaL_pushfail(L);
		lua_pushstring(L, "process not yet released");
		return (2);
	} else if (self->pid == 0) {
		luaL_pushfail(L);
		lua_pushstring(L, "process has already terminated");
	}

	assert(self->pid > 0);
	self->last_signal = signal;
	if (kill(self->pid, signal) != 0) {
		int serrno = errno;

		luaL_pushfail(L);
		lua_pushstring(L, strerror(serrno));
		return (2);
	}

	lua_pushboolean(L, 1);
	return (1);
}

static int
porchlua_process_stop(lua_State *L)
{
	struct porch_process *self;
	int error;

	/*
	 * We'll send a SIGSTOP to the child process, then wait for it to report
	 * having stopped.
	 */
	self = luaL_checkudata(L, 1, ORCHLUA_PROCESSHANDLE);
	if (kill(self->pid, SIGSTOP) != 0)
		goto failed;

	error = porchlua_process_wait(self, WUNTRACED);
	if (error == -1)
		goto failed;
	if (!WIFSTOPPED(self->status)) {
		luaL_pushfail(L);
		lua_pushstring(L, "Process seems to have terminated");
		return (2);
	}

	lua_pushboolean(L, 1);
	return (1);
failed:
	error = errno;

	luaL_pushfail(L);
	lua_pushstring(L, strerror(error));
	return (2);
}

static int
porchlua_process_term(lua_State *L)
{
	struct porch_term sterm;
	struct porch_ipc_msg *cmsg;
	struct porch_process *self;
	int error, retvals;

	retvals = 0;
	self = luaL_checkudata(L, 1, ORCHLUA_PROCESSHANDLE);
	if (!porch_ipc_okay(self->ipc)) {
		luaL_pushfail(L);
		lua_pushstring(L, "process already released");
		return (2);
	} else if (self->term != NULL) {
		luaL_pushfail(L);
		lua_pushstring(L, "process term already generated");
		return (2);
	}

	sterm.proc = self;
	sterm.initialized = false;
	porch_ipc_register(self->ipc, IPC_TERMIOS_SET, porchlua_process_term_set,
	    &sterm);

	/*
	 * The client is only responding to our messages up until we release, so
	 * there shouldn't be anything in the queue.  We'll just fire this off,
	 * and wait for a response to become ready.
	 */
	if ((error = porch_ipc_send_nodata(self->ipc,
	    IPC_TERMIOS_INQUIRY)) != 0) {
		error = errno;
		goto out;
	}

	if (porch_ipc_wait(self->ipc, NULL) == -1) {
		error = errno;
		goto out;
	}

	if (porch_ipc_recv(self->ipc, &cmsg) != 0) {
		error = errno;
		goto out;
	}

	if (cmsg != NULL) {
		luaL_pushfail(L);
		lua_pushfstring(L, "unexpected message type '%d'",
		    porch_ipc_msg_tag(cmsg));

		porch_ipc_msg_free(cmsg);
		cmsg = NULL;

		retvals = 2;
		goto out;
	} else if (!sterm.initialized) {
		luaL_pushfail(L);
		lua_pushstring(L, "unknown unexpected message received");
		retvals = 2;
		goto out;
	}

	retvals = porchlua_tty_alloc(L, &sterm, &self->term);

out:
	/* Deallocate the slot */
	porch_ipc_register(self->ipc, IPC_TERMIOS_SET, NULL, NULL);
	if (error != 0 && retvals == 0) {
		luaL_pushfail(L);
		lua_pushstring(L, strerror(error));

		retvals = 2;
	}

	return (retvals);
}

static int
porchlua_process_uid(lua_State *L)
{
	struct porch_process *self;

	self = luaL_checkudata(L, 1, ORCHLUA_PROCESSHANDLE);
	lua_pushinteger(L, self->uid);
	return (1);
}

static int
porchlua_process_write(lua_State *L)
{
	struct porch_process *self;
	const char *buf;
	size_t bufsz, totalsz;
	ssize_t writesz;
	int fd;

	self = luaL_checkudata(L, 1, ORCHLUA_PROCESSHANDLE);
	buf = luaL_checklstring(L, 2, &bufsz);

	fd = self->termctl;
	totalsz = 0;
	while (totalsz < bufsz) {
		writesz = write(fd, &buf[totalsz], bufsz - totalsz);
		if (writesz == -1 && errno == EINTR) {
			continue;
		} else if (writesz == -1) {
			int err = errno;

			luaL_pushfail(L);
			lua_pushstring(L, strerror(err));
			return (2);
		}

		totalsz += writesz;
	}

	lua_pushnumber(L, totalsz);
	return (1);
}


#define	PROCESS_SIMPLE(n)	{ #n, porchlua_process_ ## n }
static const luaL_Reg porchlua_process[] = {
	PROCESS_SIMPLE(chdir),
	PROCESS_SIMPLE(close),
	PROCESS_SIMPLE(continue),
	PROCESS_SIMPLE(eof),
	PROCESS_SIMPLE(gid),
	PROCESS_SIMPLE(proxy),
	PROCESS_SIMPLE(read),
	PROCESS_SIMPLE(release),
	PROCESS_SIMPLE(released),
	PROCESS_SIMPLE(setgroups),
	PROCESS_SIMPLE(setid),
	PROCESS_SIMPLE(sigcatch),
	PROCESS_SIMPLE(sigmask),
	PROCESS_SIMPLE(signal),
	PROCESS_SIMPLE(stop),
	PROCESS_SIMPLE(term),
	PROCESS_SIMPLE(uid),
	PROCESS_SIMPLE(write),
	{ NULL, NULL },
};

static const luaL_Reg porchlua_process_meta[] = {
	{ "__index", NULL },	/* Set during registration */
	{ "__gc", porchlua_process_close },
	{ "__close", porchlua_process_close },
	{ NULL, NULL },
};

void
porchlua_register_process_metatable(lua_State *L)
{
	luaL_newmetatable(L, ORCHLUA_PROCESSHANDLE);
	luaL_setfuncs(L, porchlua_process_meta, 0);

	luaL_newlibtable(L, porchlua_process);
	luaL_setfuncs(L, porchlua_process, 0);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);

	porchlua_register_pstatus_metatable(L);
}

static int
porchlua_pstatus_is_exited(lua_State *L)
{
	struct process_status *pstatus;

	pstatus = luaL_checkudata(L, 1, ORCHLUA_PSTATUSHANDLE);
	lua_pushboolean(L, pstatus->is_exited);
	return (1);
}

static int
porchlua_pstatus_is_signaled(lua_State *L)
{
	struct process_status *pstatus;

	pstatus = luaL_checkudata(L, 1, ORCHLUA_PSTATUSHANDLE);
	lua_pushboolean(L, pstatus->is_signaled);
	return (1);
}

static int
porchlua_pstatus_is_stopped(lua_State *L)
{
	struct process_status *pstatus;

	pstatus = luaL_checkudata(L, 1, ORCHLUA_PSTATUSHANDLE);
	lua_pushboolean(L, pstatus->is_stopped);
	return (1);
}

static int
porchlua_pstatus_status(lua_State *L)
{
	struct process_status *pstatus;
	int retvals = 0;

	pstatus = luaL_checkudata(L, 1, ORCHLUA_PSTATUSHANDLE);
	lua_pushinteger(L, pstatus->status);
	if (pstatus->status >= 0) {
		lua_pushinteger(L, pstatus->status);
		retvals = 1;
	} else {
		luaL_pushfail(L);
		lua_pushfstring(L, "unable to extract status from wait status: %x",
		    pstatus->raw_status);
		retvals = 2;
	}

	return (retvals);
}

static int
porchlua_pstatus_raw_status(lua_State *L)
{
	struct process_status *pstatus;

	pstatus = luaL_checkudata(L, 1, ORCHLUA_PSTATUSHANDLE);
	lua_pushinteger(L, pstatus->raw_status);
	return (1);
}

int
porchlua_process_wrap_status(lua_State *L)
{
	struct process_status *pstatus;
	const char *exit_type;
	int exit_code;

	exit_type = luaL_checkstring(L, 1);
	exit_code = luaL_checkinteger(L, 2);

	pstatus = lua_newuserdata(L, sizeof(*pstatus));
	memset(pstatus, 0, sizeof(*pstatus));
	pstatus->raw_status = -1;
	pstatus->status = exit_code;

	if (strcmp(exit_type, "exit") == 0) {
		pstatus->is_exited = true;
	} else if (strcmp(exit_type, "signal") == 0) {
		pstatus->is_signaled = true;
	} else {
		lua_pop(L, 1);

		luaL_pushfail(L);
		lua_pushfstring(L,
		    "unexpected exit type from file:close: %s", exit_type);
		return (2);
	}

	luaL_setmetatable(L, ORCHLUA_PSTATUSHANDLE);
	return (1);
}


#define	PSTATUS_SIMPLE(n)	{ #n, porchlua_pstatus_ ## n }
static const luaL_Reg porchlua_pstatus[] = {
	PSTATUS_SIMPLE(is_exited),
	PSTATUS_SIMPLE(is_signaled),
	PSTATUS_SIMPLE(is_stopped),
	PSTATUS_SIMPLE(status),
	PSTATUS_SIMPLE(raw_status),
	{ NULL, NULL },
};

static const luaL_Reg porchlua_pstatus_meta[] = {
	{ "__index", NULL },	/* Set during registration */
	{ NULL, NULL },
};

static void
porchlua_register_pstatus_metatable(lua_State *L)
{
	luaL_newmetatable(L, ORCHLUA_PSTATUSHANDLE);
	luaL_setfuncs(L, porchlua_pstatus_meta, 0);

	luaL_newlibtable(L, porchlua_pstatus);
	luaL_setfuncs(L, porchlua_pstatus, 0);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);
}
