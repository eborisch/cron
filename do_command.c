/* Copyright 1988,1990,1993,1994 by Paul Vixie
 * All rights reserved
 */

/*
 * Copyright (c) 1997,2000 by Internet Software Consortium, Inc.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM DISCLAIMS
 * ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE
 * CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

#if !defined(lint) && !defined(LINT)
static char rcsid[] = "$Id: do_command.c,v 1.4 2000/01/02 20:53:41 vixie Exp $";
#endif

#include "cron.h"

static void		child_process(entry *, user *);
static int		safe_p(const char *, const char *);

void
do_command(entry *e, user *u) {
	Debug(DPROC, ("[%ld] do_command(%s, (%s,%ld,%ld))\n",
		      (long)getpid(), e->cmd, u->name,
		      (long)e->uid, (long)e->gid))

	/* fork to become asynchronous -- parent process is done immediately,
	 * and continues to run the normal cron code, which means return to
	 * tick().  the child and grandchild don't leave this function, alive.
	 *
	 * vfork() is unsuitable, since we have much to do, and the parent
	 * needs to be able to run off and fork other processes.
	 */
	switch (fork()) {
	case -1:
		log_it("CRON", getpid(), "error", "can't fork");
		break;
	case 0:
		/* child process */
		acquire_daemonlock(1);
		child_process(e, u);
		Debug(DPROC, ("[%ld] child process done, exiting\n",
			      (long)getpid()))
		_exit(OK_EXIT);
		break;
	default:
		/* parent process */
		break;
	}
	Debug(DPROC, ("[%ld] main process returning to work\n",(long)getpid()))
}

static void
child_process(entry *e, user *u) {
	int stdin_pipe[2], stdout_pipe[2];
	char *input_data, *usernm, *mailto;
	int children = 0;

	Debug(DPROC, ("[%ld] child_process('%s')\n", (long)getpid(), e->cmd))

#ifdef CAPITALIZE_FOR_PS
	/* mark ourselves as different to PS command watchers by upshifting
	 * our program name.  This has no effect on some kernels.
	 */
	/*local*/{
		char	*pch;

		for (pch = ProgramName;  *pch;  pch++)
			*pch = MkUpper(*pch);
	}
#endif /* CAPITALIZE_FOR_PS */

	/* discover some useful and important environment settings
	 */
	usernm = env_get("LOGNAME", e->envp);
	mailto = env_get("MAILTO", e->envp);

	/* our parent is watching for our death by catching SIGCHLD.  we
	 * do not care to watch for our children's deaths this way -- we
	 * use wait() explictly.  so we have to disable the signal (which
	 * was inherited from the parent).
	 */
	(void) signal(SIGCHLD, SIG_IGN);

	/* create some pipes to talk to our future child
	 */
	pipe(stdin_pipe);	/* child's stdin */
	pipe(stdout_pipe);	/* child's stdout */
	
	/* since we are a forked process, we can diddle the command string
	 * we were passed -- nobody else is going to use it again, right?
	 *
	 * if a % is present in the command, previous characters are the
	 * command, and subsequent characters are the additional input to
	 * the command.  Subsequent %'s will be transformed into newlines,
	 * but that happens later.
	 */
	/*local*/{
		int escaped = FALSE;
		int ch;

		for (input_data = e->cmd;
		     (ch = *input_data) != '\0';
		     input_data++) {
			if (escaped) {
				escaped = FALSE;
				continue;
			}
			if (ch == '\\') {
				escaped = TRUE;
				continue;
			}
			if (ch == '%') {
				*input_data++ = '\0';
				break;
			}
		}
	}

	/* fork again, this time so we can exec the user's command.
	 */
	switch (vfork()) {
	case -1:
		log_it("CRON", getpid(), "error", "can't vfork");
		exit(ERROR_EXIT);
		/*NOTREACHED*/
	case 0:
		Debug(DPROC, ("[%ld] grandchild process Vfork()'ed\n",
			      (long)getpid()))

		/* write a log message.  we've waited this long to do it
		 * because it was not until now that we knew the PID that
		 * the actual user command shell was going to get and the
		 * PID is part of the log message.
		 */
		if ((e->flags & DONT_LOG) == 0) {
			char *x = mkprints((u_char *)e->cmd, strlen(e->cmd));

			log_it(usernm, getpid(), "CMD", x);
			free(x);
		}

		/* that's the last thing we'll log.  close the log files.
		 */
#ifdef SYSLOG
		closelog();
#endif

		/* get new pgrp, void tty, etc.
		 */
		(void) setsid();

		/* close the pipe ends that we won't use.  this doesn't affect
		 * the parent, who has to read and write them; it keeps the
		 * kernel from recording us as a potential client TWICE --
		 * which would keep it from sending SIGPIPE in otherwise
		 * appropriate circumstances.
		 */
		close(stdin_pipe[WRITE_PIPE]);
		close(stdout_pipe[READ_PIPE]);

		/* grandchild process.  make std{in,out} be the ends of
		 * pipes opened by our daddy; make stderr go to stdout.
		 */
		close(STDIN);	dup2(stdin_pipe[READ_PIPE], STDIN);
		close(STDOUT);	dup2(stdout_pipe[WRITE_PIPE], STDOUT);
		close(STDERR);	dup2(STDOUT, STDERR);

		/* close the pipes we just dup'ed.  The resources will remain.
		 */
		close(stdin_pipe[READ_PIPE]);
		close(stdout_pipe[WRITE_PIPE]);

		/* set our directory, uid and gid.  Set gid first, since once
		 * we set uid, we've lost root privledges.
		 */
#if defined(__bsdi__) && (_BSDI_VERSION > 199510)
		{
			struct passwd *pwd;
			char *ep, *np;

			pwd = getpwuid(e->uid);
			if (pwd == NULL) {
				fprintf(stderr, "getpwuid: couldn't get entry for %d\n", e->uid);
				_exit(ERROR_EXIT);
			}
			if (setusercontext(0, pwd, e->uid, LOGIN_SETALL) < 0) {
				fprintf(stderr, "setusercontext failed for %d\n", e->uid);
				_exit(ERROR_EXIT);
			}
			if (auth_approve(0, pwd->pw_name, "cron") <= 0) {
				fprintf(stderr, "approval failed for %d\n", e->uid);
				_exit(ERROR_EXIT);
			}
			if (env_get("PATH", e->envp) == NULL &&
			    (ep = getenv("PATH"))) {
				np = malloc(strlen(ep) + 6);
				if (np) {
					strcpy(np, "PATH=");
					strcat(np, ep);
					env_set(e->envp, np);
				}
			}
			
		}
#else
		setgid(e->gid);
		initgroups(env_get("LOGNAME", e->envp), e->gid);
		setuid(e->uid);		/* we aren't root after this... */

#endif /* __bsdi__ */
		chdir(env_get("HOME", e->envp));

		/*
		 * Exec the command.
		 */
		{
			char	*shell = env_get("SHELL", e->envp);

# if DEBUGGING
			if (DebugFlags & DTEST) {
				fprintf(stderr,
				"debug DTEST is on, not exec'ing command.\n");
				fprintf(stderr,
				"\tcmd='%s' shell='%s'\n", e->cmd, shell);
				_exit(OK_EXIT);
			}
# endif /*DEBUGGING*/
			execle(shell, shell, "-c", e->cmd, (char *)0, e->envp);
			fprintf(stderr, "execl: couldn't exec `%s'\n", shell);
			perror("execl");
			_exit(ERROR_EXIT);
		}
		break;
	default:
		/* parent process */
		break;
	}

	children++;

	/* middle process, child of original cron, parent of process running
	 * the user's command.
	 */

	Debug(DPROC, ("[%ld] child continues, closing pipes\n",(long)getpid()))

	/* close the ends of the pipe that will only be referenced in the
	 * grandchild process...
	 */
	close(stdin_pipe[READ_PIPE]);
	close(stdout_pipe[WRITE_PIPE]);

	/*
	 * write, to the pipe connected to child's stdin, any input specified
	 * after a % in the crontab entry.  while we copy, convert any
	 * additional %'s to newlines.  when done, if some characters were
	 * written and the last one wasn't a newline, write a newline.
	 *
	 * Note that if the input data won't fit into one pipe buffer (2K
	 * or 4K on most BSD systems), and the child doesn't read its stdin,
	 * we would block here.  thus we must fork again.
	 */

	if (*input_data && fork() == 0) {
		FILE *out = fdopen(stdin_pipe[WRITE_PIPE], "w");
		int need_newline = FALSE;
		int escaped = FALSE;
		int ch;

		Debug(DPROC, ("[%ld] child2 sending data to grandchild\n",
			      (long)getpid()))

		/* close the pipe we don't use, since we inherited it and
		 * are part of its reference count now.
		 */
		close(stdout_pipe[READ_PIPE]);

		/* translation:
		 *	\% -> %
		 *	%  -> \n
		 *	\x -> \x	for all x != %
		 */
		while ((ch = *input_data++) != '\0') {
			if (escaped) {
				if (ch != '%')
					putc('\\', out);
			} else {
				if (ch == '%')
					ch = '\n';
			}

			if (!(escaped = (ch == '\\'))) {
				putc(ch, out);
				need_newline = (ch != '\n');
			}
		}
		if (escaped)
			putc('\\', out);
		if (need_newline)
			putc('\n', out);

		/* close the pipe, causing an EOF condition.  fclose causes
		 * stdin_pipe[WRITE_PIPE] to be closed, too.
		 */
		fclose(out);

		Debug(DPROC, ("[%ld] child2 done sending to grandchild\n",
			      (long)getpid()))
		exit(0);
	}

	/* close the pipe to the grandkiddie's stdin, since its wicked uncle
	 * ernie back there has it open and will close it when he's done.
	 */
	close(stdin_pipe[WRITE_PIPE]);

	children++;

	/*
	 * read output from the grandchild.  it's stderr has been redirected to
	 * it's stdout, which has been redirected to our pipe.  if there is any
	 * output, we'll be mailing it to the user whose crontab this is...
	 * when the grandchild exits, we'll get EOF.
	 */

	Debug(DPROC, ("[%ld] child reading output from grandchild\n",
		      (long)getpid()))

	/*local*/{
		FILE	*in = fdopen(stdout_pipe[READ_PIPE], "r");
		int	ch = getc(in);

		if (ch != EOF) {
			FILE	*mail;
			int	bytes = 1;
			int	status = 0;

			Debug(DPROC|DEXT,
			      ("[%ld] got data (%x:%c) from grandchild\n",
			       (long)getpid(), ch, ch))

			/* get name of recipient.  this is MAILTO if set to a
			 * valid local username; USER otherwise.
			 */
			if (mailto) {
				/* MAILTO was present in the environment
				 */
				if (!*mailto) {
					/* ... but it's empty. set to NULL
					 */
					mailto = NULL;
				}
			} else {
				/* MAILTO not present, set to USER.
				 */
				mailto = usernm;
			}
		
			/* if we are supposed to be mailing, MAILTO will
			 * be non-NULL.  only in this case should we set
			 * up the mail command and subjects and stuff...
			 */

			if (mailto && safe_p(usernm, mailto)) {
				char	**env;
				char	mailcmd[MAX_COMMAND];
				char	hostname[MAXHOSTNAMELEN];

				gethostname(hostname, MAXHOSTNAMELEN);
				if (strlens(MAILFMT, MAILARG, NULL) + 1
				    >= sizeof mailcmd) {
					fprintf(stderr, "mailcmd too long\n");
					(void) _exit(ERROR_EXIT);
				}
				(void)sprintf(mailcmd, MAILFMT, MAILARG);
				if (!(mail = cron_popen(mailcmd, "w"))) {
					perror(mailcmd);
					(void) _exit(ERROR_EXIT);
				}
				fprintf(mail, "From: root (Cron Daemon)\n");
				fprintf(mail, "To: %s\n", mailto);
				fprintf(mail, "Subject: Cron <%s@%s> %s\n",
					usernm, first_word(hostname, "."),
					e->cmd);
#ifdef MAIL_DATE
				fprintf(mail, "Date: %s\n",
					arpadate(&TargetTime));
#endif /*MAIL_DATE*/
				for (env = e->envp;  *env;  env++)
					fprintf(mail, "X-Cron-Env: <%s>\n",
						*env);
				fprintf(mail, "\n");

				/* this was the first char from the pipe
				 */
				putc(ch, mail);
			}

			/* we have to read the input pipe no matter whether
			 * we mail or not, but obviously we only write to
			 * mail pipe if we ARE mailing.
			 */

			while (EOF != (ch = getc(in))) {
				bytes++;
				if (mailto)
					putc(ch, mail);
			}

			/* only close pipe if we opened it -- i.e., we're
			 * mailing...
			 */

			if (mailto) {
				Debug(DPROC, ("[%ld] closing pipe to mail\n",
					      (long)getpid()))
				/* Note: the pclose will probably see
				 * the termination of the grandchild
				 * in addition to the mail process, since
				 * it (the grandchild) is likely to exit
				 * after closing its stdout.
				 */
				status = cron_pclose(mail);
			}

			/* if there was output and we could not mail it,
			 * log the facts so the poor user can figure out
			 * what's going on.
			 */
			if (mailto && status) {
				char buf[MAX_TEMPSTR];

				sprintf(buf,
			"mailed %d byte%s of output but got status 0x%04x\n",
					bytes, (bytes==1)?"":"s",
					status);
				log_it(usernm, getpid(), "MAIL", buf);
			}

		} /*if data from grandchild*/

		Debug(DPROC, ("[%ld] got EOF from grandchild\n",
			      (long)getpid()))

		fclose(in);	/* also closes stdout_pipe[READ_PIPE] */
	}

	/* wait for children to die.
	 */
	for (; children > 0; children--) {
		WAIT_T waiter;
		PID_T pid;

		Debug(DPROC, ("[%ld] waiting for grandchild #%d to finish\n",
			      (long)getpid(), children))
		pid = wait(&waiter);
		if (pid < OK) {
			Debug(DPROC,
			      ("[%ld] no more grandchildren--mail written?\n",
			       (long)getpid()))
			break;
		}
		Debug(DPROC, ("[%ld] grandchild #%ld finished, status=%04x",
			      (long)getpid(), (long)pid, WEXITSTATUS(waiter)))
		if (WIFSIGNALED(waiter) && WCOREDUMP(waiter))
			Debug(DPROC, (", dumped core"))
		Debug(DPROC, ("\n"))
	}
}

static int
safe_p(const char *usernm, const char *s) {
	static const char safe_delim[] = "@!:%-.,";     /* conservative! */
	const char *t;
	int ch, first;

	for (t = s, first = 1; (ch = *t++) != '\0'; first = 0) {
		if (isascii(ch) && isprint(ch) &&
		    (isalnum(ch) || (!first && strchr(safe_delim, ch))))
			continue;
		log_it(usernm, getpid(), "UNSAFE", s);
		return (FALSE);
	}
	return (TRUE);
}
