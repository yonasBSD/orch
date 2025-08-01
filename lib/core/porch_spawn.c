/*-
 * Copyright (c) 2024 Kyle Evans <kevans@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "porch.h"
#include "porch_lib.h"

#ifdef __OpenBSD__
#define	POSIX_OPENPT_FLAGS	(O_RDWR | O_NOCTTY)
#else
#define	POSIX_OPENPT_FLAGS	(O_RDWR | O_NOCTTY | O_CLOEXEC)
#endif

/* A bit lazy, but meh. */
#if defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
#define	SOCKPAIR_ATTRS	(SOCK_CLOEXEC | SOCK_NONBLOCK)
#elif defined(SOCK_CLOEXEC)
#define	SOCKPAIR_ATTRS	(SOCK_CLOEXEC)
#elif defined(SOCK_NONBLOCK)
#define	SOCKPAIR_ATTRS	(SOCK_NONBLOCK)
#else
#define	SOCKPAIR_ATTRS	(0)
#endif

extern char **environ;

/* Parent */
static int porch_newpt(void);

/* Child */
static pid_t porch_newsess(porch_ipc_t);
static void porch_usept(porch_ipc_t, pid_t, int, struct termios *);
static void porch_child_error(porch_ipc_t, const char *, ...) __printflike(2, 3);
static void porch_exec(porch_ipc_t, int, const char *[], struct termios *);

/* Both */
static int porch_wait(porch_ipc_t);

int
porch_spawn(int argc, const char *argv[], struct porch_process *p,
    porch_ipc_handler *child_error_handler)
{
	int cmdsock[2];
	pid_t pid, sess;

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCKPAIR_ATTRS, 0,
	    &cmdsock[0]) == -1)
		err(1, "socketpair");
#if (SOCKPAIR_ATTRS & SOCK_CLOEXEC) == 0
	if (fcntl(cmdsock[0], F_SETFD, fcntl(cmdsock[0], F_GETFD) |
	    FD_CLOEXEC) == -1)
		err(1, "fcntl");
	if (fcntl(cmdsock[1], F_SETFD, fcntl(cmdsock[1], F_GETFD) |
	    FD_CLOEXEC) == -1)
		err(1, "fcntl");
#endif
#if (SOCKPAIR_ATTRS & SOCK_NONBLOCK) == 0
	if (fcntl(cmdsock[0], F_SETFL, fcntl(cmdsock[0], F_GETFL) |
	    O_NONBLOCK) == -1)
		err(1, "fcntl");
	if (fcntl(cmdsock[1], F_SETFL, fcntl(cmdsock[1], F_GETFL) |
	    O_NONBLOCK) == -1)
		err(1, "fcntl");
#endif

	p->termctl = porch_newpt();

	pid = fork();
	if (pid == -1) {
		err(1, "fork");
	} else if (pid == 0) {
		struct termios t;
		porch_ipc_t ipc;

		/* Child */
		close(cmdsock[0]);
		ipc = porch_ipc_open(cmdsock[1]);
		if (ipc == NULL) {
			close(cmdsock[1]);
			fprintf(stderr, "child out of memory\n");
			_exit(1);
		}

		sess = porch_newsess(ipc);

		porch_usept(ipc, sess, p->termctl, &t);
		assert(p->termctl >= 0);
		close(p->termctl);
		p->termctl = -1;

		porch_exec(ipc, argc, argv, &t);
	}

	p->released = false;
	p->pid = pid;
	p->ipc = porch_ipc_open(cmdsock[0]);

	/* Parent */
	close(cmdsock[1]);

	if (p->ipc == NULL) {
		int status;

		assert(p->termctl >= 0);
		close(p->termctl);
		close(cmdsock[0]);

		kill(pid, SIGKILL);
		while (waitpid(pid, &status, 0) != pid) {
			continue;
		}

		errno = ENOMEM;
		return (-1);
	}

	porch_ipc_register(p->ipc, IPC_ERROR, child_error_handler, p);

	/*
	 * Stalls until the tty is configured, completely side step races from
	 * script writing to the tty before, e.g., echo is disabled.
	 */
	return (porch_wait(p->ipc));
}

static int
porch_wait(porch_ipc_t ipc)
{
	struct porch_ipc_msg *msg;
	bool stop = false;

	while (!stop) {
		if (porch_ipc_wait(ipc, &stop) == -1)
			return (-1);
		else if (stop)
			break;

		if (porch_ipc_recv(ipc, &msg) != 0)
			return (-1);
		if (msg == NULL)
			continue;

		stop = porch_ipc_msg_tag(msg) == IPC_RELEASE;

		porch_ipc_msg_free(msg);
		msg = NULL;
	}

	return (0);
}

int
porch_release(porch_ipc_t ipc)
{

	return (porch_ipc_send_nodata(ipc, IPC_RELEASE));
}

static void
porch_child_error(porch_ipc_t ipc, const char *fmt, ...)
{
	struct porch_ipc_msg *errmsg;
	char *str, *msgstr;
	va_list ap;
	int sz;

	errmsg = NULL;
	va_start(ap, fmt);
	if ((sz = vasprintf(&str, fmt, ap)) == -1)
		goto out;
	va_end(ap);

	errmsg = porch_ipc_msg_alloc(IPC_ERROR, sz + 1, (void **)&msgstr);
	if (errmsg == NULL)
		goto out;

	strlcpy(msgstr, str, sz + 1);

	free(str);
	str = NULL;

	porch_ipc_send(ipc, errmsg);

out:
	porch_ipc_msg_free(errmsg);
	free(str);
	porch_ipc_close(ipc);
	_exit(1);
}

static int
porch_child_termios_inquiry(porch_ipc_t ipc, struct porch_ipc_msg *inmsg __unused,
    void *cookie)
{
	struct porch_ipc_msg *msg;
	struct termios *child_termios = cookie, *parent_termios;
	int error, serr;

	/* Send term attributes back over the wire. */
	msg = porch_ipc_msg_alloc(IPC_TERMIOS_SET, sizeof(*child_termios),
	    (void **)&parent_termios);
	if (msg == NULL) {
		errno = EINVAL;
		return (-1);
	}

	memcpy(parent_termios, child_termios, sizeof(*child_termios));

	error = porch_ipc_send(ipc, msg);
	serr = errno;

	porch_ipc_msg_free(msg);
	if (error != 0)
		errno = serr;
	return (error);
}

static int
porch_child_termios_set(porch_ipc_t ipc, struct porch_ipc_msg *msg, void *cookie)
{
	struct termios *updated_termios;
	struct termios *current_termios = cookie;
	size_t datasz;

	updated_termios = porch_ipc_msg_payload(msg, &datasz);
	if (updated_termios == NULL || datasz != sizeof(*updated_termios)) {
		errno = EINVAL;
		return (-1);
	}

	/*
	 * We don't need to keep track of the updated state, but we do so
	 * anyways.
	 */
	memcpy(current_termios, updated_termios, sizeof(*updated_termios));

	if (tcsetattr(STDIN_FILENO, TCSANOW, current_termios) == -1)
		porch_child_error(ipc, "tcsetattr");

	return (porch_ipc_send_nodata(ipc, IPC_TERMIOS_ACK));
}

static int
porch_clearenv(void)
{
#if (defined(__FreeBSD__) && __FreeBSD_version >= 1400041) || defined(__linux__)
	return (clearenv());
#else
	extern char **environ;

	while (*environ != NULL) {
		const char *envp = *environ;
		char *eq, *name;

		eq = strchr(envp, '=');
		assert(eq != NULL);

		name = strndup(envp, eq - envp);
		if (name == NULL)
			return (-1);

		unsetenv(name);
		free(name);
	}

	return (0);
#endif
}

static int
porch_child_env_setup(porch_ipc_t ipc, struct porch_ipc_msg *msg,
    void *cookie __unused)
{
	struct porch_env *penv;
	size_t envsz;

	penv = porch_ipc_msg_payload(msg, &envsz);
	if (penv == NULL || envsz < sizeof(*penv)) {
		errno = EINVAL;
		return (-1);
	}

	if (penv->clear && porch_clearenv() != 0)
		return (-1);

	if (penv->setsz != 0) {
		const char *env, *last;

		last = &penv->envstr[penv->setsz];
		env = &penv->envstr[0];
		while (env < last) {
			char *cp;

			cp = strdup(env);
			if (cp == NULL)
				_exit(1);
			putenv(cp);
			env = strchr(env, '\0') + 1;
		}
	}

	return (porch_ipc_send_nodata(ipc, IPC_ENV_ACK));
}

static int
porch_child_chdir(porch_ipc_t ipc, struct porch_ipc_msg *msg,
    void *cookie __unused)
{
	const char *dir;
	size_t dirsz;
	int error, *errorp;

	dir = porch_ipc_msg_payload(msg, &dirsz);
	if (dir == NULL || dirsz == 0) {
		errno = EINVAL;
		return (-1);
	}

	error = 0;
	if (chdir(dir) != 0)
		error = errno;

	msg = porch_ipc_msg_alloc(IPC_CHDIR_ACK, sizeof(error),
	    (void **)&errorp);

	*errorp = error;
	error = porch_ipc_send(ipc, msg);
	porch_ipc_msg_free(msg);

	return (error);
}

static int
porch_child_setgroups(porch_ipc_t ipc, struct porch_ipc_msg *msg,
    void *cookie __unused)
{
	struct porch_setgroups *sgrp;
	size_t sgrpsz;
	const gid_t *gids;
	int error, *errorp;

	sgrp = porch_ipc_msg_payload(msg, &sgrpsz);
	if (sgrp == NULL || sgrpsz < sizeof(*sgrp)) {
		errno = EINVAL;
		return (-1);
	} else if (sgrpsz != PORCH_SETGROUPS_SIZE(sgrp->setgroups_cnt)) {
		errno = EINVAL;
		return (-1);
	}

	if (sgrp->setgroups_cnt > 0)
		gids = &sgrp->setgroups_gids[0];
	else
		gids = NULL;

	error = 0;
	if (setgroups(sgrp->setgroups_cnt, gids) != 0)
		error = errno;

	msg = porch_ipc_msg_alloc(IPC_SETGROUPS_ACK, sizeof(error),
	    (void **)&errorp);

	*errorp = error;
	error = porch_ipc_send(ipc, msg);
	porch_ipc_msg_free(msg);

	return (error);
}

static int
porch_child_setid(porch_ipc_t ipc, struct porch_ipc_msg *msg,
    void *cookie __unused)
{
	struct porch_setid *sid;
	size_t sidsz;
	int error, *errorp;

	sid = porch_ipc_msg_payload(msg, &sidsz);
	if (sid == NULL || sidsz != sizeof(*sid)) {
		errno = EINVAL;
		return (-1);
	}

	error = 0;
	if ((sid->setid_flags & SID_SETGID) != 0 &&
	    setgid(sid->setid_gid) != 0) {
		error = errno;
		goto done;
	}

	if ((sid->setid_flags & SID_SETUID) != 0 &&
	    setuid(sid->setid_uid) != 0) {
		error = errno;
		goto done;
	}

done:
	msg = porch_ipc_msg_alloc(IPC_SETID_ACK, sizeof(error),
	    (void **)&errorp);

	*errorp = error;
	error = porch_ipc_send(ipc, msg);
	porch_ipc_msg_free(msg);

	return (error);
}

static int
porch_child_setmask(porch_ipc_t ipc, struct porch_ipc_msg *msg,
    void *cookie __unused)
{
	const sigset_t *newmask;
	size_t masksz;
	int error, *errorp;

	newmask = porch_ipc_msg_payload(msg, &masksz);
	if (newmask == NULL || masksz != sizeof(*newmask)) {
		errno = EINVAL;
		return (-1);
	}

	error = 0;
	if (sigprocmask(SIG_SETMASK, newmask, NULL) != 0)
		error = errno;

	msg = porch_ipc_msg_alloc(IPC_SETMASK_ACK, sizeof(error),
	    (void **)&errorp);

	*errorp = error;
	error = porch_ipc_send(ipc, msg);
	porch_ipc_msg_free(msg);

	return (error);
}

static int
porch_child_sigcatch(porch_ipc_t ipc, struct porch_ipc_msg *msg,
    void *cookie __unused)
{
	const struct porch_sigcatch *catchmsg;
	size_t catchmsgsz;
	int error, *errorp, sigmax;
	void (*new_action)(int);

	catchmsg = porch_ipc_msg_payload(msg, &catchmsgsz);
	if (catchmsg == NULL || catchmsgsz != sizeof(*catchmsg)) {
		errno = EINVAL;
		return (-1);
	}

	if (catchmsg->catch)
		new_action = SIG_DFL;
	else
		new_action = SIG_IGN;
	sigmax = porch_sigmax();
	for (int signo = 1; signo < sigmax; signo++) {
		error = sigismember(&catchmsg->mask, signo);
		if (error == -1)
			continue;	/* Failed (internal signal?) */
		if (!error)
			continue;	/* Not a member */

		(void)signal(signo, new_action);
	}

	error = 0;
	msg = porch_ipc_msg_alloc(IPC_SIGCATCH_ACK, sizeof(error),
	    (void **)&errorp);

	*errorp = error;
	error = porch_ipc_send(ipc, msg);
	porch_ipc_msg_free(msg);

	return (error);
}

static void
porch_exec(porch_ipc_t ipc, int argc __unused, const char *argv[],
    struct termios *t)
{
	int error;

	signal(SIGTERM, SIG_DFL);

	/*
	 * Register a couple of events that the script may want to use:
	 * - IPC_TERMIOS_INQUIRY: sent our terminal attributes back over.
	 * - IPC_TERMIOS_SET: update our terminal attributes
	 */
	porch_ipc_register(ipc, IPC_TERMIOS_INQUIRY, porch_child_termios_inquiry,
	    t);
	porch_ipc_register(ipc, IPC_TERMIOS_SET, porch_child_termios_set, t);
	porch_ipc_register(ipc, IPC_ENV_SETUP, porch_child_env_setup, NULL);
	porch_ipc_register(ipc, IPC_CHDIR, porch_child_chdir, NULL);
	porch_ipc_register(ipc, IPC_SETGROUPS, porch_child_setgroups, NULL);
	porch_ipc_register(ipc, IPC_SETID, porch_child_setid, NULL);
	porch_ipc_register(ipc, IPC_SETMASK, porch_child_setmask, NULL);
	porch_ipc_register(ipc, IPC_SIGCATCH, porch_child_sigcatch, NULL);

	/* Let the script commence. */
	if (porch_release(ipc) != 0)
		_exit(1);

	/*
	 * The child waits here for the script to release it.  It will typically be
	 * released on first match, but we provide an explicit release() function to
	 * do it manually in case the script doesn't want to queue up input before
	 * execution starts for some reason.
	 *
	 * For now this is just a simple int, in the future it may grow a more
	 * extensive protocol so that the script can, e.g., reconfigure the tty.
	 */
	error = porch_wait(ipc);
	porch_ipc_close(ipc);

	if (error != 0)
		_exit(1);

	execvp(argv[0], (char * const *)(const void *)argv);

	_exit(1);
}

static int
porch_newpt(void)
{
	int newpt;

	newpt = posix_openpt(POSIX_OPENPT_FLAGS);
	if (newpt == -1)
		err(1, "posix_openpt");
#if (POSIX_OPENPT_FLAGS & O_CLOEXEC) == 0
	if (fcntl(newpt, F_SETFD, fcntl(newpt, F_GETFD) | FD_CLOEXEC) == -1)
		err(1, "fcntl");
#endif

	if (grantpt(newpt) == -1)
		err(1, "grantpt");
	if (unlockpt(newpt) == -1)
		err(1, "unlockpt");

	return (newpt);
}

static pid_t
porch_newsess(porch_ipc_t ipc)
{
	pid_t sess;

	sess = setsid();
	if (sess == -1)
		porch_child_error(ipc, "setsid");

	return (sess);
}

static void
porch_usept(porch_ipc_t ipc, pid_t sess, int termctl, struct termios *t)
{
	const char *name;
	int target;

	name = ptsname(termctl);
	if (name == NULL)
		porch_child_error(ipc, "ptsname: %s", strerror(errno));

	target = open(name, O_RDWR);
	if (target == -1)
		porch_child_error(ipc, "open %s: %s", name, strerror(errno));

	if (tcsetsid(target, sess) == -1)
		porch_child_error(ipc, "tcsetsid");

	if (tcgetattr(target, t) == -1)
		porch_child_error(ipc, "tcgetattr");

	/* XXX Accept mask, buffering? */
	dup2(target, STDIN_FILENO);
	dup2(target, STDOUT_FILENO);
	dup2(target, STDERR_FILENO);
	if (target > STDERR_FILENO)
		close(target);
}
