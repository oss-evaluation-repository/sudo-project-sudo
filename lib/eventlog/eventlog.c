/*
 * SPDX-License-Identifier: ISC
 *
 * Copyright (c) 1994-1996, 1998-2020 Todd C. Miller <Todd.Miller@sudo.ws>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Sponsored in part by the Defense Advanced Research Projects
 * Agency (DARPA) and Air Force Research Laboratory, Air Force
 * Materiel Command, USAF, under agreement number F39502-99-1-0512.
 */

/*
 * This is an open source non-commercial project. Dear PVS-Studio, please check it.
 * PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
 */

#include <config.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <locale.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "pathnames.h"
#include "sudo_compat.h"
#include "sudo_debug.h"
#include "sudo_eventlog.h"
#include "sudo_fatal.h"
#include "sudo_gettext.h"
#include "sudo_json.h"
#include "sudo_queue.h"
#include "sudo_util.h"

#define	LL_HOST_STR	"HOST="
#define	LL_TTY_STR	"TTY="
#define	LL_CHROOT_STR	"CHROOT="
#define	LL_CWD_STR	"PWD="
#define	LL_USER_STR	"USER="
#define	LL_GROUP_STR	"GROUP="
#define	LL_ENV_STR	"ENV="
#define	LL_CMND_STR	"COMMAND="
#define	LL_TSID_STR	"TSID="

#define IS_SESSID(s) ( \
    isalnum((unsigned char)(s)[0]) && isalnum((unsigned char)(s)[1]) && \
    (s)[2] == '/' && \
    isalnum((unsigned char)(s)[3]) && isalnum((unsigned char)(s)[4]) && \
    (s)[5] == '/' && \
    isalnum((unsigned char)(s)[6]) && isalnum((unsigned char)(s)[7]) && \
    (s)[8] == '\0')

/* Eventlog config settings */
static struct eventlog_config evl_conf;

/*
 * Allocate and fill in a new logline.
 */
static char *
new_logline(int flags, const char *message, const char *errstr,
    const struct eventlog *details)
{
    char *line = NULL, *evstr = NULL;
    const char *iolog_file = details->iolog_file;
    const char *tty, *tsid = NULL;
    char sessid[7];
    size_t len = 0;
    int i;
    debug_decl(new_logline, SUDO_DEBUG_UTIL);

    if (ISSET(flags, EVLOG_RAW)) {
	if (errstr != NULL) {
	    if (asprintf(&line, "%s: %s", message, errstr) == -1)
		goto oom;
	} else {
	    if ((line = strdup(message)) == NULL)
		goto oom;
	}
	debug_return_str(line);
    }

    /* A TSID may be a sudoers-style session ID or a free-form string. */
    if (iolog_file != NULL) {
	if (IS_SESSID(iolog_file)) {
	    sessid[0] = iolog_file[0];
	    sessid[1] = iolog_file[1];
	    sessid[2] = iolog_file[3];
	    sessid[3] = iolog_file[4];
	    sessid[4] = iolog_file[6];
	    sessid[5] = iolog_file[7];
	    sessid[6] = '\0';
	    tsid = sessid;
	} else {
	    tsid = iolog_file;
	}
    }

    /* Sudo-format logs use the short form of the ttyname. */
    if ((tty = details->ttyname) != NULL) {
	if (strncmp(tty, _PATH_DEV, sizeof(_PATH_DEV) - 1) == 0)
	    tty += sizeof(_PATH_DEV) - 1;
    }

    /*
     * Compute line length
     */
    if (message != NULL)
	len += strlen(message) + 3;
    if (errstr != NULL)
	len += strlen(errstr) + 3;
    if (details->submithost != NULL && !evl_conf.omit_hostname)
	len += sizeof(LL_HOST_STR) + 2 + strlen(details->submithost);
    if (tty != NULL)
	len += sizeof(LL_TTY_STR) + 2 + strlen(tty);
    if (details->runchroot != NULL)
	len += sizeof(LL_CHROOT_STR) + 2 + strlen(details->runchroot);
    if (details->runcwd != NULL)
	len += sizeof(LL_CWD_STR) + 2 + strlen(details->runcwd);
    if (details->runuser != NULL)
	len += sizeof(LL_USER_STR) + 2 + strlen(details->runuser);
    if (details->rungroup != NULL)
	len += sizeof(LL_GROUP_STR) + 2 + strlen(details->rungroup);
    if (tsid != NULL)
	len += sizeof(LL_TSID_STR) + 2 + strlen(tsid);
    if (details->env_add != NULL) {
	size_t evlen = 0;
	char * const *ep;

	for (ep = details->env_add; *ep != NULL; ep++)
	    evlen += strlen(*ep) + 1;
	if (evlen != 0) {
	    if ((evstr = malloc(evlen)) == NULL)
		goto oom;
	    ep = details->env_add;
	    if (strlcpy(evstr, *ep, evlen) >= evlen)
		goto toobig;
	    while (*++ep != NULL) {
		if (strlcat(evstr, " ", evlen) >= evlen ||
		    strlcat(evstr, *ep, evlen) >= evlen)
		    goto toobig;
	    }
	    len += sizeof(LL_ENV_STR) + 2 + evlen;
	}
    }
    if (details->command != NULL) {
	len += sizeof(LL_CMND_STR) - 1 + strlen(details->command);
	for (i = 1; details->argv[i] != NULL; i++)
	    len += strlen(details->argv[i]) + 1;
    }

    /*
     * Allocate and build up the line.
     */
    if ((line = malloc(++len)) == NULL)
	goto oom;
    line[0] = '\0';

    if (message != NULL) {
	if (strlcat(line, message, len) >= len ||
	    strlcat(line, errstr ? " : " : " ; ", len) >= len)
	    goto toobig;
    }
    if (errstr != NULL) {
	if (strlcat(line, errstr, len) >= len ||
	    strlcat(line, " ; ", len) >= len)
	    goto toobig;
    }
    if (details->submithost != NULL && !evl_conf.omit_hostname) {
	if (strlcat(line, LL_HOST_STR, len) >= len ||
	    strlcat(line, details->submithost, len) >= len ||
	    strlcat(line, " ; ", len) >= len)
	    goto toobig;
    }
    if (tty != NULL) {
	if (strlcat(line, LL_TTY_STR, len) >= len ||
	    strlcat(line, tty, len) >= len ||
	    strlcat(line, " ; ", len) >= len)
	    goto toobig;
    }
    if (details->runchroot != NULL) {
	if (strlcat(line, LL_CHROOT_STR, len) >= len ||
	    strlcat(line, details->runchroot, len) >= len ||
	    strlcat(line, " ; ", len) >= len)
	    goto toobig;
    }
    if (details->runcwd != NULL) {
	if (strlcat(line, LL_CWD_STR, len) >= len ||
	    strlcat(line, details->runcwd, len) >= len ||
	    strlcat(line, " ; ", len) >= len)
	    goto toobig;
    }
    if (details->runuser != NULL) {
	if (strlcat(line, LL_USER_STR, len) >= len ||
	    strlcat(line, details->runuser, len) >= len ||
	    strlcat(line, " ; ", len) >= len)
	    goto toobig;
    }
    if (details->rungroup != NULL) {
	if (strlcat(line, LL_GROUP_STR, len) >= len ||
	    strlcat(line, details->rungroup, len) >= len ||
	    strlcat(line, " ; ", len) >= len)
	    goto toobig;
    }
    if (tsid != NULL) {
	if (strlcat(line, LL_TSID_STR, len) >= len ||
	    strlcat(line, tsid, len) >= len ||
	    strlcat(line, " ; ", len) >= len)
	    goto toobig;
    }
    if (evstr != NULL) {
	if (strlcat(line, LL_ENV_STR, len) >= len ||
	    strlcat(line, evstr, len) >= len ||
	    strlcat(line, " ; ", len) >= len)
	    goto toobig;
	free(evstr);
	evstr = NULL;
    }
    if (details->command != NULL) {
	if (strlcat(line, LL_CMND_STR, len) >= len)
	    goto toobig;
	if (strlcat(line, details->command, len) >= len)
	    goto toobig;
	for (i = 1; details->argv[i] != NULL; i++) {
	    if (strlcat(line, " ", len) >= len ||
		strlcat(line, details->argv[i], len) >= len)
		goto toobig;
	}
    }

    debug_return_str(line);
oom:
    free(evstr);
    sudo_warnx(U_("%s: %s"), __func__, U_("unable to allocate memory"));
    debug_return_str(NULL);
toobig:
    free(evstr);
    free(line);
    sudo_warnx(U_("internal error, %s overflow"), __func__);
    debug_return_str(NULL);
}

static void
closefrom_nodebug(int lowfd)
{
    unsigned char *debug_fds;
    int fd, startfd;
    debug_decl(closefrom_nodebug, SUDO_DEBUG_UTIL);

    startfd = sudo_debug_get_fds(&debug_fds) + 1;
    if (lowfd > startfd)
	startfd = lowfd;

    /* Close fds higher than the debug fds. */
    sudo_debug_printf(SUDO_DEBUG_DEBUG|SUDO_DEBUG_LINENO,
	"closing fds >= %d", startfd);
    closefrom(startfd);

    /* Close fds [lowfd, startfd) that are not in debug_fds. */
    for (fd = lowfd; fd < startfd; fd++) {
	if (sudo_isset(debug_fds, fd))
	    continue;
	sudo_debug_printf(SUDO_DEBUG_DEBUG|SUDO_DEBUG_LINENO,
	    "closing fd %d", fd);
#ifdef __APPLE__
	/* Avoid potential libdispatch crash when we close its fds. */
	(void) fcntl(fd, F_SETFD, FD_CLOEXEC);
#else
	(void) close(fd);
#endif
    }
    debug_return;
}

#define MAX_MAILFLAGS	63

static void __attribute__((__noreturn__))
exec_mailer(const struct eventlog *evlog, int pipein)
{
    char *last, *mflags, *p, *argv[MAX_MAILFLAGS + 1];
    const char *mpath = evl_conf.mailerpath;
    int i;
    char * const root_envp[] = {
	"HOME=/",
	"PATH=/usr/bin:/bin:/usr/sbin:/sbin",
	"LOGNAME=root",
	"USER=root",
# ifdef _AIX
	"LOGIN=root",
# endif
	NULL
    };
    debug_decl(exec_mailer, SUDO_DEBUG_UTIL);

    /* Set stdin to read side of the pipe. */
    if (dup3(pipein, STDIN_FILENO, 0) == -1) {
	syslog(LOG_ERR, _("unable to dup stdin: %m"));
	sudo_debug_printf(SUDO_DEBUG_ERROR,
	    "unable to dup stdin: %s", strerror(errno));
	sudo_debug_exit(__func__, __FILE__, __LINE__, sudo_debug_subsys);
	_exit(127);
    }

    /* Build up an argv based on the mailer path and flags */
    if ((mflags = strdup(evl_conf.mailerflags)) == NULL) {
	syslog(LOG_ERR, _("unable to allocate memory"));
	sudo_debug_exit(__func__, __FILE__, __LINE__, sudo_debug_subsys);
	_exit(127);
    }
    if ((argv[0] = strrchr(mpath, '/')))
	argv[0]++;
    else
	argv[0] = (char *)mpath;

    i = 1;
    if ((p = strtok_r(mflags, " \t", &last))) {
	do {
	    argv[i] = p;
	} while (++i < MAX_MAILFLAGS && (p = strtok_r(NULL, " \t", &last)));
    }
    argv[i] = NULL;

    /*
     * Depending on the config, either run the mailer as root
     * (so user cannot kill it) or as the user (for the paranoid).
     */
    if (setuid(ROOT_UID) != 0) {
	sudo_debug_printf(SUDO_DEBUG_ERROR, "unable to change uid to %u",
	    ROOT_UID);
    }
    if (evl_conf.mailuid != ROOT_UID) {
	if (setuid(evl_conf.mailuid) != 0) {
	    sudo_debug_printf(SUDO_DEBUG_ERROR, "unable to change uid to %u",
		(unsigned int)evl_conf.mailuid);
	}
    }
    sudo_debug_exit(__func__, __FILE__, __LINE__, sudo_debug_subsys);
    if (evl_conf.mailuid == ROOT_UID)
	execve(mpath, argv, root_envp);
    else
	execv(mpath, argv);
    syslog(LOG_ERR, _("unable to execute %s: %m"), mpath);
    sudo_debug_printf(SUDO_DEBUG_ERROR, "unable to execute %s: %s",
	mpath, strerror(errno));
    _exit(127);
}

/* Send a message to MAILTO user */
static bool
send_mail(const struct eventlog *evlog, const char *fmt, ...)
{
    const char *cp, *timefmt = evl_conf.time_fmt;
    char timebuf[1024];
    struct tm *tm;
    time_t now;
    FILE *mail;
    int fd, pfd[2], status;
    pid_t pid, rv;
    struct stat sb;
    va_list ap;
#if defined(HAVE_NL_LANGINFO) && defined(CODESET)
    char *locale;
#endif
    debug_decl(send_mail, SUDO_DEBUG_UTIL);

    /* If mailer is disabled just return. */
    if (evl_conf.mailerpath == NULL || evl_conf.mailto == NULL)
	debug_return_bool(true);

    /* Make sure the mailer exists and is a regular file. */
    if (stat(evl_conf.mailerpath, &sb) != 0 || !S_ISREG(sb.st_mode))
	debug_return_bool(false);

    time(&now);
    if ((tm = gmtime(&now)) == NULL)
	debug_return_bool(false);

    /* Fork and return, child will daemonize. */
    switch (pid = sudo_debug_fork()) {
	case -1:
	    /* Error. */
	    sudo_warn("%s", U_("unable to fork"));
	    debug_return_bool(false);
	    break;
	case 0:
	    /* Child. */
	    switch (fork()) {
		case -1:
		    /* Error. */
		    syslog(LOG_ERR, _("unable to fork: %m"));
		    sudo_debug_printf(SUDO_DEBUG_ERROR, "unable to fork: %s",
			strerror(errno));
		    sudo_debug_exit(__func__, __FILE__, __LINE__, sudo_debug_subsys);
		    _exit(EXIT_FAILURE);
		case 0:
		    /* Grandchild continues below. */
		    sudo_debug_enter(__func__, __FILE__, __LINE__, sudo_debug_subsys);
		    break;
		default:
		    /* Parent will wait for us. */
		    _exit(EXIT_SUCCESS);
	    }
	    break;
	default:
	    /* Parent. */
	    for (;;) {
		rv = waitpid(pid, &status, 0);
		if (rv == -1 && errno != EINTR)
		    break;
		if (rv != -1 && !WIFSTOPPED(status))
		    break;
	    }
	    sudo_debug_printf(SUDO_DEBUG_INFO|SUDO_DEBUG_LINENO,
		"child (%d) exit value %d", (int)rv, status);
	    debug_return_bool(true);
    }

    /* Daemonize - disassociate from session/tty. */
    if (setsid() == -1)
      sudo_warn("setsid");
    if (chdir("/") == -1)
      sudo_warn("chdir(/)");
    fd = open(_PATH_DEVNULL, O_RDWR, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
    if (fd != -1) {
	(void) dup2(fd, STDIN_FILENO);
	(void) dup2(fd, STDOUT_FILENO);
	(void) dup2(fd, STDERR_FILENO);
    }

    /* Close non-debug fds so we don't leak anything. */
    closefrom_nodebug(STDERR_FILENO + 1);

    if (pipe2(pfd, O_CLOEXEC) == -1) {
	syslog(LOG_ERR, _("unable to open pipe: %m"));
	sudo_debug_printf(SUDO_DEBUG_ERROR, "unable to open pipe: %s",
	    strerror(errno));
	sudo_debug_exit(__func__, __FILE__, __LINE__, sudo_debug_subsys);
	_exit(EXIT_FAILURE);
    }

    switch (pid = sudo_debug_fork()) {
	case -1:
	    /* Error. */
	    syslog(LOG_ERR, _("unable to fork: %m"));
	    sudo_debug_printf(SUDO_DEBUG_ERROR, "unable to fork: %s",
		strerror(errno));
	    sudo_debug_exit(__func__, __FILE__, __LINE__, sudo_debug_subsys);
	    _exit(EXIT_FAILURE);
	    break;
	case 0:
	    /* Child. */
	    exec_mailer(evlog, pfd[0]);
	    /* NOTREACHED */
    }

    (void) close(pfd[0]);
    mail = fdopen(pfd[1], "w");

    /* Pipes are all setup, send message. */
    (void) fprintf(mail, "To: %s\nFrom: %s\nAuto-Submitted: %s\nSubject: ",
	evl_conf.mailto,
	evl_conf.mailfrom ? evl_conf.mailfrom : evlog->submituser,
	"auto-generated");
    for (cp = _(evl_conf.mailsub); *cp; cp++) {
	/* Expand escapes in the subject */
	if (*cp == '%' && *(cp+1) != '%') {
	    switch (*(++cp)) {
		case 'h':
		    (void) fputs(evlog->submithost, mail);
		    break;
		case 'u':
		    (void) fputs(evlog->submituser, mail);
		    break;
		default:
		    cp--;
		    break;
	    }
	} else
	    (void) fputc(*cp, mail);
    }

#if defined(HAVE_NL_LANGINFO) && defined(CODESET)
    locale = setlocale(LC_ALL, NULL);
    if (locale[0] != 'C' || locale[1] != '\0')
	(void) fprintf(mail, "\nContent-Type: text/plain; charset=\"%s\"\nContent-Transfer-Encoding: 8bit", nl_langinfo(CODESET));
#endif /* HAVE_NL_LANGINFO && CODESET */

    strftime(timebuf, sizeof(timebuf), timefmt, tm);
    (void) fprintf(mail, "\n\n%s : %s : %s : ", evlog->submithost, timebuf,
	evlog->submituser);
    va_start(ap, fmt);
    (void) vfprintf(mail, fmt, ap);
    va_end(ap);
    fputs("\n\n", mail);

    fclose(mail);
    for (;;) {
	rv = waitpid(pid, &status, 0);
	if (rv == -1 && errno != EINTR)
	    break;
	if (rv != -1 && !WIFSTOPPED(status))
	    break;
    }
    sudo_debug_printf(SUDO_DEBUG_INFO|SUDO_DEBUG_LINENO,
	"child (%d) exit value %d", (int)rv, status);
    sudo_debug_exit(__func__, __FILE__, __LINE__, sudo_debug_subsys);
    _exit(EXIT_SUCCESS);
}

static bool
json_add_timestamp(struct json_container *json, const char *name,
    const struct timespec *ts)
{
    const char *timefmt = evl_conf.time_fmt;
    struct json_value json_value;
    time_t secs = ts->tv_sec;
    char timebuf[1024];
    struct tm *tm;
    debug_decl(json_add_timestamp, SUDO_DEBUG_PLUGIN);

    if ((tm = gmtime(&secs)) == NULL)
	debug_return_bool(false);

    if (!sudo_json_open_object(json, name))
	goto oom;

    json_value.type = JSON_NUMBER;
    json_value.u.number = ts->tv_sec;
    if (!sudo_json_add_value(json, "seconds", &json_value))
	goto oom;

    json_value.type = JSON_NUMBER;
    json_value.u.number = ts->tv_nsec;
    if (!sudo_json_add_value(json, "nanoseconds", &json_value))
	goto oom;

    strftime(timebuf, sizeof(timebuf), "%Y%m%d%H%M%SZ", tm);
    json_value.type = JSON_STRING;
    json_value.u.string = timebuf;
    if (!sudo_json_add_value(json, "iso8601", &json_value))
	goto oom;

    strftime(timebuf, sizeof(timebuf), timefmt, tm);
    json_value.type = JSON_STRING;
    json_value.u.string = timebuf;
    if (!sudo_json_add_value(json, "localtime", &json_value))
	goto oom;

    if (!sudo_json_close_object(json))
	goto oom;

    debug_return_bool(true);
oom:
    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_ERRNO|SUDO_DEBUG_LINENO,
	"%s: %s", __func__, "unable to allocate memory");
    debug_return_bool(false);
}

/*
 * Store the contents of struct eventlog as JSON.
 * The submit_time and iolog_path members are not stored, they should
 * be stored and formatted by the caller.
 */
bool
eventlog_store_json(struct json_container *json, const struct eventlog *evlog)
{
    struct json_value json_value;
    size_t i;
    char *cp;
    debug_decl(eventlog_store_json, SUDO_DEBUG_UTIL);

    /* Required settings. */
    if (evlog->command == NULL || evlog->submituser == NULL ||
	    evlog->runuser == NULL)
	debug_return_bool(false);

    /* Note: submit_time and iolog_path must be stored by caller. */

    json_value.type = JSON_NUMBER;
    json_value.u.number = evlog->columns;
    if (!sudo_json_add_value(json, "columns", &json_value))
        goto oom;

    json_value.type = JSON_STRING;
    json_value.u.string = evlog->command;
    if (!sudo_json_add_value(json, "command", &json_value))
        goto oom;

    json_value.type = JSON_NUMBER;
    json_value.u.number = evlog->lines;
    if (!sudo_json_add_value(json, "lines", &json_value))
        goto oom;

    if (evlog->argv != NULL) {
	if (!sudo_json_open_array(json, "runargv"))
	    goto oom;
	for (i = 0; (cp = evlog->argv[i]) != NULL; i++) {
	    json_value.type = JSON_STRING;
	    json_value.u.string = cp;
	    if (!sudo_json_add_value(json, NULL, &json_value))
		goto oom;
	}
	if (!sudo_json_close_array(json))
	    goto oom;
    }

    if (evlog->envp != NULL) {
	if (!sudo_json_open_array(json, "runenv"))
	    goto oom;
	for (i = 0; (cp = evlog->envp[i]) != NULL; i++) {
	    json_value.type = JSON_STRING;
	    json_value.u.string = cp;
	    if (!sudo_json_add_value(json, NULL, &json_value))
		goto oom;
	}
	if (!sudo_json_close_array(json))
	    goto oom;
    }

    if (evlog->rungroup!= NULL) {
	if (evlog->rungid != (gid_t)-1) {
	    json_value.type = JSON_ID;
	    json_value.u.id = evlog->rungid;
	    if (!sudo_json_add_value(json, "rungid", &json_value))
		goto oom;
	}

	json_value.type = JSON_STRING;
	json_value.u.string = evlog->rungroup;
	if (!sudo_json_add_value(json, "rungroup", &json_value))
	    goto oom;
    }

    if (evlog->runuid != (uid_t)-1) {
	json_value.type = JSON_ID;
	json_value.u.id = evlog->runuid;
	if (!sudo_json_add_value(json, "runuid", &json_value))
	    goto oom;
    }

    if (evlog->runchroot != NULL) {
	json_value.type = JSON_STRING;
	json_value.u.string = evlog->runchroot;
	if (!sudo_json_add_value(json, "runchroot", &json_value))
	    goto oom;
    }

    if (evlog->runcwd != NULL) {
	json_value.type = JSON_STRING;
	json_value.u.string = evlog->runcwd;
	if (!sudo_json_add_value(json, "runcwd", &json_value))
	    goto oom;
    }

    json_value.type = JSON_STRING;
    json_value.u.string = evlog->runuser;
    if (!sudo_json_add_value(json, "runuser", &json_value))
	goto oom;

    if (evlog->cwd != NULL) {
	json_value.type = JSON_STRING;
	json_value.u.string = evlog->cwd;
	if (!sudo_json_add_value(json, "submitcwd", &json_value))
	    goto oom;
    }

    if (evlog->submithost != NULL) {
	json_value.type = JSON_STRING;
	json_value.u.string = evlog->submithost;
	if (!sudo_json_add_value(json, "submithost", &json_value))
	    goto oom;
    }

    json_value.type = JSON_STRING;
    json_value.u.string = evlog->submituser;
    if (!sudo_json_add_value(json, "submituser", &json_value))
	goto oom;

    if (evlog->ttyname != NULL) {
	json_value.type = JSON_STRING;
	json_value.u.string = evlog->ttyname;
	if (!sudo_json_add_value(json, "ttyname", &json_value))
	    goto oom;
    }

    debug_return_bool(true);

oom:
    sudo_warnx(U_("%s: %s"), __func__, U_("unable to allocate memory"));
    debug_return_bool(false);
}

static bool
default_json_cb(struct json_container *json, void *v)
{
    return eventlog_store_json(json, v);
}

static char *
format_json(int event_type, const char *reason, const char *errstr,
    const struct eventlog *details, const struct timespec *event_time,
    eventlog_json_callback_t info_cb, void *info, bool compact)
{
    const char *type_str;
    const char *time_str;
    struct json_container json = { 0 };
    struct json_value json_value;
    struct timespec now;
    debug_decl(format_json, SUDO_DEBUG_UTIL);

    if (info_cb == NULL) {
	info_cb = default_json_cb;
	info = (void *)details;
    }

    if (sudo_gettime_real(&now) == -1) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "unable to read the clock");
	debug_return_str(NULL);
    }

    switch (event_type) {
    case EVLOG_ACCEPT:
	type_str = "accept";
	time_str = "submit_time";
	break;
    case EVLOG_REJECT:
	type_str = "reject";
	time_str = "submit_time";
	break;
    case EVLOG_ALERT:
	type_str = "alert";
	time_str = "alert_time";
	break;
    default:
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
	    "unexpected event type %d", event_type);
	debug_return_str(NULL);
    }

    if (!sudo_json_init(&json, 4, compact, false))
	goto bad;
    if (!sudo_json_open_object(&json, type_str))
	goto bad;

    /* Reject and Alert events include a reason */
    if (reason != NULL) {
	json_value.type = JSON_STRING;
	json_value.u.string = reason;
	if (!sudo_json_add_value(&json, "reason", &json_value))
	    goto bad;
    }
    if (errstr != NULL) {
	json_value.type = JSON_STRING;
	json_value.u.string = errstr;
	if (!sudo_json_add_value(&json, "error", &json_value))
	    goto bad;
    }

    /* XXX - create and log uuid? */

    /* Log event time on server (set earlier) */
    if (!json_add_timestamp(&json, "server_time", &now)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_ERRNO|SUDO_DEBUG_LINENO,
	    "unable format timestamp");
	goto bad;
    }

    /* Log event time from client */
    if (!json_add_timestamp(&json, time_str, event_time)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_ERRNO|SUDO_DEBUG_LINENO,
	    "unable format timestamp");
	goto bad;
    }

    if (details->iolog_path != NULL) {
	json_value.type = JSON_STRING;
	json_value.u.string = details->iolog_path;
	if (!sudo_json_add_value(&json, "iolog_path", &json_value))
	    goto bad;
    }

    /* Write log details. */
    if (!info_cb(&json, info))
	goto bad;

    if (!sudo_json_close_object(&json))
	goto bad;

    /* Caller is responsible for freeing the buffer. */
    debug_return_str(sudo_json_get_buf(&json));

bad:
    sudo_json_free(&json);
    debug_return_str(NULL);
}

/*
 * Log a message to syslog, pre-pending the username and splitting the
 * message into parts if it is longer than syslog_maxlen.
 */
static bool
do_syslog_sudo(int pri, char *logline, const struct eventlog *details)
{
    size_t len, maxlen;
    char *p, *tmp, save;
    const char *fmt;
    debug_decl(do_syslog_sudo, SUDO_DEBUG_UTIL);

    evl_conf.open_log(EVLOG_SYSLOG, NULL);

    /*
     * Log the full line, breaking into multiple syslog(3) calls if necessary
     */
    fmt = _("%8s : %s");
    maxlen = evl_conf.syslog_maxlen -
	(strlen(fmt) - 5 + strlen(details->submituser));
    for (p = logline; *p != '\0'; ) {
	len = strlen(p);
	if (len > maxlen) {
	    /*
	     * Break up the line into what will fit on one syslog(3) line
	     * Try to avoid breaking words into several lines if possible.
	     */
	    tmp = memrchr(p, ' ', maxlen);
	    if (tmp == NULL)
		tmp = p + maxlen;

	    /* NULL terminate line, but save the char to restore later */
	    save = *tmp;
	    *tmp = '\0';

	    syslog(pri, fmt, details->submituser, p);

	    *tmp = save;			/* restore saved character */

	    /* Advance p and eliminate leading whitespace */
	    for (p = tmp; *p == ' '; p++)
		continue;
	} else {
	    syslog(pri, fmt, details->submituser, p);
	    p += len;
	}
	fmt = _("%8s : (command continued) %s");
	maxlen = evl_conf.syslog_maxlen -
	    (strlen(fmt) - 5 + strlen(details->submituser));
    }
    evl_conf.close_log(EVLOG_SYSLOG, NULL);

    debug_return_bool(true);
}

static bool
do_syslog_json(int pri, int event_type, const char *reason,
    const char *errstr, const struct eventlog *details,
    const struct timespec *event_time,
    eventlog_json_callback_t info_cb, void *info)
{
    char *json_str;
    debug_decl(do_syslog_json, SUDO_DEBUG_UTIL);

    /* Format as a compact JSON message (no newlines) */
    json_str = format_json(event_type, reason, errstr, details, event_time,
	info_cb, info, true);
    if (json_str == NULL)
	debug_return_bool(false);

    /* Syslog it with a @cee: prefix */
    /* TODO: use evl_conf.syslog_maxlen to break up long messages. */
    evl_conf.open_log(EVLOG_SYSLOG, NULL);
    syslog(pri, "@cee:{%s }", json_str);
    evl_conf.close_log(EVLOG_SYSLOG, NULL);
    free(json_str);
    debug_return_bool(true);
}

/*
 * Log a message to syslog in either sudo or JSON format.
 */
static bool
do_syslog(int event_type, int flags, const char *reason, const char *errstr,
    const struct eventlog *details, const struct timespec *event_time,
    eventlog_json_callback_t info_cb, void *info)
{
    char *logline = NULL;
    bool ret = false;
    int pri;
    debug_decl(do_syslog, SUDO_DEBUG_UTIL);

    /* Sudo format logs and mailed logs use the same log line format. */
    if (evl_conf.format == EVLOG_SUDO || ISSET(flags, EVLOG_MAIL)) {
	logline = new_logline(flags, reason, errstr, details);
	if (logline == NULL)
	    debug_return_bool(false);

	if (ISSET(flags, EVLOG_MAIL)) {
	    if (!send_mail(details, "%s", logline)) {
		sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
		    "unable to mail log line");
	    }
	    if (ISSET(flags, EVLOG_MAIL_ONLY)) {
		free(logline);
		debug_return_bool(true);
	    }
	}
    }

    switch (event_type) {
    case EVLOG_ACCEPT:
	pri = evl_conf.syslog_acceptpri;
	break;
    case EVLOG_REJECT:
	pri = evl_conf.syslog_rejectpri;
	break;
    case EVLOG_ALERT:
	pri = evl_conf.syslog_alertpri;
	break;
    default:
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
	    "unexpected event type %d", event_type);
	pri = -1;
	break;
    }
    if (pri == -1) {
	/* syslog disabled for this message type */
	free(logline);
	debug_return_bool(true);
    }

    switch (evl_conf.format) {
    case EVLOG_SUDO:
	ret = do_syslog_sudo(pri, logline, details);
	break;
    case EVLOG_JSON:
	ret = do_syslog_json(pri, event_type, reason, errstr, details,
	    event_time, info_cb, info);
	break;
    default:
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
	    "unexpected eventlog format %d", evl_conf.format);
	break;
    }
    free(logline);

    debug_return_bool(ret);
}

static bool
do_logfile_sudo(const char *logline, const struct eventlog *details)
{
    const char *timefmt = evl_conf.time_fmt;
    const char *logfile = evl_conf.logpath;
    char timebuf[8192], *timestr = NULL;
    struct tm *timeptr;
    bool ret = false;
    FILE *fp;
    debug_decl(do_logfile_sudo, SUDO_DEBUG_UTIL);

    if ((fp = evl_conf.open_log(EVLOG_FILE, logfile)) == NULL)
	debug_return_bool(false);

    if (!sudo_lock_file(fileno(fp), SUDO_LOCK)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "unable to lock log file %s", logfile);
	goto done;
    }

    if ((timeptr = localtime(&details->submit_time.tv_sec)) != NULL) {
	/* strftime() does not guarantee to NUL-terminate so we must check. */
	timebuf[sizeof(timebuf) - 1] = '\0';
	if (strftime(timebuf, sizeof(timebuf), timefmt, timeptr) != 0 &&
		timebuf[sizeof(timebuf) - 1] == '\0') {
	    timestr = timebuf;
	}
    }
    (void)fprintf(fp, "%s : %s : %s\n", timestr ? timestr : "invalid date",
	details->submituser, logline);
    (void)fflush(fp);
    if (ferror(fp)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "unable to write log file %s", logfile);
	goto done;
    }
    ret = true;

done:
    (void)sudo_lock_file(fileno(fp), SUDO_UNLOCK);
    evl_conf.close_log(EVLOG_FILE, fp);
    debug_return_bool(ret);
}

static bool
do_logfile_json(int event_type, const char *reason, const char *errstr,
    const struct eventlog *details, const struct timespec *event_time,
    eventlog_json_callback_t info_cb, void *info)
{
    const char *logfile = evl_conf.logpath;
    struct stat sb;
    char *json_str;
    int ret = false;
    FILE *fp;
    debug_decl(do_logfile_json, SUDO_DEBUG_UTIL);

    if ((fp = evl_conf.open_log(EVLOG_FILE, logfile)) == NULL)
	debug_return_bool(false);

    json_str = format_json(event_type, reason, errstr, details, event_time,
	info_cb, info, false);
    if (json_str == NULL)
	goto done;

    if (!sudo_lock_file(fileno(fp), SUDO_LOCK)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "unable to lock log file %s", logfile);
	goto done;
    }

    /* Note: assumes file ends in "\n}\n" */
    if (fstat(fileno(fp), &sb) == -1) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_ERRNO|SUDO_DEBUG_LINENO,
	    "unable to stat %s", logfile);
	goto done;
    }
    if (sb.st_size == 0) {
	/* New file */
	putc('{', fp);
    } else if (fseeko(fp, -3, SEEK_END) == 0) {
	/* Continue file, overwrite the final "\n}\n" */
	putc(',', fp);
    } else {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_ERRNO|SUDO_DEBUG_LINENO,
	    "unable to seek %s", logfile);
	goto done;
    }
    fputs(json_str, fp);
    fputs("\n}\n", fp);			/* close JSON */
    fflush(fp);
    /* XXX - check for file error and recover */

    ret = true;

done:
    free(json_str);
    (void)sudo_lock_file(fileno(fp), SUDO_UNLOCK);
    evl_conf.close_log(EVLOG_FILE, fp);
    debug_return_bool(ret);
}

static bool
do_logfile(int event_type, int flags, const char *reason, const char *errstr,
    const struct eventlog *details, const struct timespec *event_time,
    eventlog_json_callback_t info_cb, void *info)
{
    bool ret = false;
    char *logline = NULL;
    debug_decl(do_logfile, SUDO_DEBUG_UTIL);

    /* Sudo format logs and mailed logs use the same log line format. */
    if (evl_conf.format == EVLOG_SUDO || ISSET(flags, EVLOG_MAIL)) {
	logline = new_logline(flags, reason, errstr, details);
	if (logline == NULL)
	    debug_return_bool(false);

	if (ISSET(flags, EVLOG_MAIL)) {
	    if (!send_mail(details, "%s", logline)) {
		sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
		    "unable to mail log line");
	    }
	    if (ISSET(flags, EVLOG_MAIL_ONLY)) {
		free(logline);
		debug_return_bool(true);
	    }
	}
    }

    switch (evl_conf.format) {
    case EVLOG_SUDO:
	ret = do_logfile_sudo(logline ? logline : reason, details);
	break;
    case EVLOG_JSON:
	ret = do_logfile_json(event_type, reason, errstr, details,
	    event_time, info_cb, info);
	break;
    default:
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
	    "unexpected eventlog format %d", evl_conf.format);
	break;
    }
    free(logline);

    debug_return_bool(ret);
}

bool
eventlog_accept(const struct eventlog *details, int flags,
    eventlog_json_callback_t info_cb, void *info)
{
    const int log_type = evl_conf.type;
    bool ret = true;
    debug_decl(log_accept, SUDO_DEBUG_UTIL);

    if (log_type == EVLOG_NONE)
	debug_return_bool(true);

    if (ISSET(log_type, EVLOG_SYSLOG)) {
	if (!do_syslog(EVLOG_ACCEPT, flags, NULL, NULL, details,
		&details->submit_time, info_cb, info))
	    ret = false;
	CLR(flags, EVLOG_MAIL);
    }
    if (ISSET(log_type, EVLOG_FILE)) {
	if (!do_logfile(EVLOG_ACCEPT, flags, NULL, NULL, details,
		&details->submit_time, info_cb, info))
	    ret = false;
    }

    debug_return_bool(ret);
}

bool
eventlog_reject(const struct eventlog *details, int flags, const char *reason,
    eventlog_json_callback_t info_cb, void *info)
{
    const int log_type = evl_conf.type;
    bool ret = true;
    debug_decl(log_reject, SUDO_DEBUG_UTIL);

    if (ISSET(log_type, EVLOG_SYSLOG)) {
	if (!do_syslog(EVLOG_REJECT, flags, reason, NULL, details,
		&details->submit_time, info_cb, info))
	    ret = false;
	CLR(flags, EVLOG_MAIL);
    }
    if (ISSET(log_type, EVLOG_FILE)) {
	if (!do_logfile(EVLOG_REJECT, flags, reason, NULL, details,
		&details->submit_time, info_cb, info))
	    ret = false;
    }

    debug_return_bool(ret);
}

bool
eventlog_alert(const struct eventlog *details, int flags,
    struct timespec *alert_time, const char *reason, const char *errstr)
{
    const int log_type = evl_conf.type;
    bool ret = true;
    debug_decl(log_alert, SUDO_DEBUG_UTIL);

    if (ISSET(log_type, EVLOG_SYSLOG)) {
	if (!do_syslog(EVLOG_ALERT, flags, reason, errstr, details, alert_time,
		NULL, NULL))
	    ret = false;
	CLR(flags, EVLOG_MAIL);
    }
    if (ISSET(log_type, EVLOG_FILE)) {
	if (!do_logfile(EVLOG_ALERT, flags, reason, errstr, details, alert_time,
		NULL, NULL))
	    ret = false;
    }

    debug_return_bool(ret);
}

/*
 * Free the strings in a struct eventlog.
 */
void
eventlog_free(struct eventlog *evlog)
{
    int i;
    debug_decl(eventlog_free, SUDO_DEBUG_UTIL);

    if (evlog != NULL) {
	free(evlog->iolog_path);
	free(evlog->command);
	free(evlog->cwd);
	free(evlog->runchroot);
	free(evlog->runcwd);
	free(evlog->rungroup);
	free(evlog->runuser);
	free(evlog->submithost);
	free(evlog->submituser);
	free(evlog->submitgroup);
	free(evlog->ttyname);
	if (evlog->argv != NULL) {
	    for (i = 0; evlog->argv[i] != NULL; i++)
		free(evlog->argv[i]);
	    free(evlog->argv);
	}
	if (evlog->envp != NULL) {
	    for (i = 0; evlog->envp[i] != NULL; i++)
		free(evlog->envp[i]);
	    free(evlog->envp);
	}
	free(evlog);
    }

    debug_return;
}

static FILE *
eventlog_stub_open_log(int type, const char *logfile)
{
    debug_decl(eventlog_stub_open_log, SUDO_DEBUG_UTIL);
    sudo_debug_printf(SUDO_DEBUG_WARN|SUDO_DEBUG_LINENO,
	"open_log not set, using stub");
    debug_return_ptr(NULL);
}

static void
eventlog_stub_close_log(int type, FILE *fp)
{
    debug_decl(eventlog_stub_close_log, SUDO_DEBUG_UTIL);
    sudo_debug_printf(SUDO_DEBUG_WARN|SUDO_DEBUG_LINENO,
	"close_log not set, using stub");
    debug_return;
}

/*
 * Set eventlog config settings.
 */
bool
eventlog_setconf(struct eventlog_config *conf)
{
    debug_decl(eventlog_setconf, SUDO_DEBUG_UTIL);

    if (conf != NULL) {
	memcpy(&evl_conf, conf, sizeof(evl_conf));
    } else {
	memset(&evl_conf, 0, sizeof(evl_conf));
    }

    /* Apply default values where possible. */
    if (evl_conf.syslog_maxlen == 0)
	evl_conf.syslog_maxlen = MAXSYSLOGLEN;
    if (evl_conf.logpath == NULL)
	evl_conf.logpath = _PATH_SUDO_LOGFILE;
    if (evl_conf.time_fmt == NULL)
	evl_conf.time_fmt = "%h %e %T";
    if (evl_conf.mailerpath == NULL)
	evl_conf.mailerpath = _PATH_SUDO_SENDMAIL;
    if (evl_conf.mailerflags == NULL)
	evl_conf.mailerflags = "-t";
    if (evl_conf.mailto == NULL)
	evl_conf.mailto = MAILTO;
    if (evl_conf.mailsub == NULL)
	evl_conf.mailsub = N_(MAILSUBJECT);
    if (evl_conf.open_log == NULL)
	evl_conf.open_log = eventlog_stub_open_log;
    if (evl_conf.close_log == NULL)
	evl_conf.close_log = eventlog_stub_close_log;

    debug_return_bool(true);
}
