/*  Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        J.Wielemaker@vu.nl
    WWW:           http://www.swi-prolog.org
    Copyright (c)  2011-2016, University of Amsterdam
                              VU University Amsterdam
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:

    1. Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.

    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in
       the documentation and/or other materials provided with the
       distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
    FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
    COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
    INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
    BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
    CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
    LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
    ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.
*/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
This module binds the  SWI-Prolog  terminal   I/O  to  the  GNU readline
library. Existence of this  this  library   is  detected  by  configure.
Binding is achieved by rebinding the read function of the Sinput stream.

This  module  only  depends  on  the  public  interface  as  defined  by
SWI-Prolog.h and SWI-Stream.h
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include <string.h>
#include <stdlib.h>
#include "SWI-Stream.h"
#include "SWI-Prolog.h"
#include <config.h>
#include <signal.h>

#define DEBUG(level, g) (void)0

static IOFUNCTIONS	rl_functions;	/* IO+Terminal+Readline functions */

#if defined(HAVE_POLL_H) && defined(HAVE_POLL)
#include <poll.h>
#elif defined(HAVE_SYS_SELECT_H)
#include <sys/select.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_UXNT_H
#include <uxnt.h>
#endif
#ifdef HAVE_CLOCK
#include <time.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef __WINDOWS__
#include <io.h>
#endif
#ifdef O_RLC
#include "win32/console/console.h"
#endif

#ifdef HAVE_RL_INSERT_CLOSE
#define PAREN_MATCHING 1
#endif

#undef ESC				/* will be redefined ... */
#include <stdio.h>			/* readline needs it */
#include <errno.h>
#define savestring(x)			/* avoid definition there */
#include <readline/readline.h>
extern int rl_done;			/* should be in readline.h, but */
					/* isn't in some versions ... */
#ifdef HAVE_READLINE_HISTORY_H
#include <readline/history.h>
#else
extern void add_history(char *);	/* should be in readline.h */
#endif
					/* missing prototypes in older */
					/* readline.h versions */
extern int rl_begin_undo_group(void);	/* delete when conflict arrises! */
extern int rl_end_undo_group(void);
#ifndef HAVE_RL_FILENAME_COMPLETION_FUNCTION
#define rl_filename_completion_function filename_completion_function
extern char *filename_completion_function(const char *, int);
#endif

#ifndef HAVE_RL_COMPLETION_MATCHES
#define rl_completion_matches completion_matches
#endif

#ifndef RL_STATE_INITIALIZED
int rl_readline_state = 0;
#define RL_STATE_INITIALIZED 0
#endif
#ifndef HAVE_RL_SET_PROMPT
#define rl_set_prompt(x) (void)0
#endif
#ifndef HAVE_RL_CLEAR_PENDING_INPUT
#define rl_clear_pending_input() (void)0
#endif
#ifndef HAVE_RL_CLEANUP_AFTER_SIGNAL
#define rl_cleanup_after_signal() (void)0
#endif

#if !defined(HAVE_RL_DONE) && defined(HAVE_DECL_RL_DONE) && !HAVE_DECL_RL_DONE
/* surely not provided, so we provide a dummy.  We do this as
   a global symbol, so if there is one in a dynamic library it
   will work anyway.
*/
int rl_done;
#endif


static foreign_t
pl_rl_read_init_file(term_t file)
{ char *f;

  if ( PL_get_file_name(file, &f, 0) )
  {
#ifdef O_XOS
    char buf[PATH_MAX];
    rl_read_init_file(_xos_os_filename(f, buf));
#else
    rl_read_init_file(f);
#endif

    PL_succeed;
  }

  PL_fail;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
This is not really clear using   wide-character  handling. We now assume
that the readline library can only do   wide characters using UTF-8. Not
sure this is true, but is certainly covers most installations.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static char *hist_last = NULL;

static foreign_t
pl_rl_add_history(term_t text)
{ char *line;

  if ( PL_get_chars(text, &line, CVT_ATOM|CVT_STRING|REP_MB|CVT_EXCEPTION) )
  { if ( hist_last && strcmp(hist_last,line) == 0 )
      return FALSE;
    if ( hist_last )
      free(hist_last);
    hist_last = strdup(line);
    add_history(line);

    return TRUE;
  }

  return FALSE;
}


static int
file_error(term_t fn, const char *op, int rc)
{ switch(rc)
  { case EPERM:
      return PL_permission_error("file", op, fn);
    case ENOENT:
      return PL_existence_error("file", fn);
    default:
      return FALSE;				/* TBD */
  }
}


static foreign_t
pl_rl_write_history(term_t fn)
{ char *s;
  int rc;

  if ( !PL_get_file_name(fn, &s, 0) )
    return FALSE;

  if ( (rc=write_history(s)) == 0 )
    return TRUE;

  return file_error(fn, "write", rc);
}



static foreign_t
pl_rl_read_history(term_t fn)
{ char *s;
  int rc;

  if ( !PL_get_file_name(fn, &s, 0) )
    return FALSE;

  if ( (rc=read_history(s)) == 0 )
    return TRUE;

  return file_error(fn, "read", rc);
}



static char *my_prompt    = NULL;
static int   in_readline  = 0;
static int   sig_at_level = -1;

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Signal handling wrapper.

This is tricky. The GNU readline   library places signal handlers around
the   below   given   signals.   They    re-send   all   signals   using
kill(getpid(),sig). This goes wrong in our  multi-threaded context as it
will send signals meant for a thread (using pthread_kill()) to the wrong
thread. The library time.pl from the  clib   package  was victim of this
behaviour.

We disable readline's signal handling  using   rl_catch_signals  = 0 and
redo the work ourselves, where we call   the handler directly instead of
re-sending the signal. See  "info  readline"   for  details  on readline
signal handling issues.

One of the problems is that the signal   handler may not return after ^C
<abort>. Earlier versions uses PL_abort_handler()   to reset the basics,
but  since  the  introduction  of  multi-threading  and  exception-based
aborts, this no longer works. We set sig_at_level to the current nesting
level if we receive a signal. If this is still the current nesting level
if we reach readling again we assumed we broke out of the old invocation
in a non-convential manner.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

typedef struct
{ int			signo;		/* number of the signal */
  struct sigaction	old_state;	/* old state for the signal */
} sigstate;

static void rl_sighandler(int sig);

static sigstate signals[] =
{ { SIGINT },
#ifdef SIGTSTP
  { SIGTSTP },
  { SIGTTOU },
  { SIGTTIN },
#endif
  { SIGALRM },
  { SIGTERM },
  { SIGQUIT },
  { -1 },
};


static void
prepare_signals(void)
{ sigstate *s;

  for(s=signals; s->signo != -1; s++)
  { struct sigaction new;

    memset(&new, 0, sizeof(new));
    new.sa_handler = rl_sighandler;
    sigaction(s->signo, &new, &s->old_state);
  }
}


static void
restore_signals(void)
{ sigstate *s;

  for(s=signals; s->signo != -1; s++)
  { sigaction(s->signo, &s->old_state, NULL);
  }
}


static void
rl_sighandler(int sig)
{ sigstate *s;

  DEBUG(3, Sdprintf("Signal %d in readline\n", sig));

  sig_at_level = in_readline;

  if ( sig == SIGINT )
    rl_free_line_state ();
  rl_cleanup_after_signal ();
  restore_signals();
  Sreset();

  for(s=signals; s->signo != -1; s++)
  { if ( s->signo == sig )
    { void (*func)(int) = s->old_state.sa_handler;

      if ( func == SIG_DFL )
      { // TBD: unblockSignal(sig);
	DEBUG(3, Sdprintf("Re-sending signal\n"));
	PL_raise(sig);			/* was: kill(getpid(), sig); */
      } else if ( func != SIG_IGN )
      { (*func)(sig);
      }

      break;
    }
  }

  DEBUG(3, Sdprintf("Resetting after signal\n"));
  prepare_signals();
  rl_reset_after_signal();
}


static char *
pl_readline(const char *prompt)
{ char *line;

  prepare_signals();
  line = readline(prompt);
  restore_signals();

  return line;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
The GNU-readline library is not reentrant (or does not appear to be so).
Therefore we will detect this and simply   call  the default function if
reentrant access is tried.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifdef HAVE_RL_EVENT_HOOK
static int
input_on_fd(int fd)
{
#ifdef HAVE_POLL
  struct pollfd fds[1];

  fds[0].fd = fd;
  fds[0].events = POLLIN;

  return poll(fds, 1, 0) != 0;
#else
  fd_set rfds;
  struct timeval tv;

#if defined(FD_SETSIZE) && !defined(__WINDOWS__)
  if ( fd >= FD_SETSIZE )
  { Sdprintf("input_on_fd(%d) > FD_SETSIZE\n", fd);
    return 1;
  }
#endif

  FD_ZERO(&rfds);
  FD_SET(fd, &rfds);
  tv.tv_sec = 0;
  tv.tv_usec = 0;

  return select(fd+1, &rfds, NULL, NULL, &tv) != 0;
#endif
}

static int
event_hook(void)
{ if ( Sinput->position )
  { int64_t c0 = Sinput->position->charno;

    while( !input_on_fd(0) )
    { PL_dispatch(0, PL_DISPATCH_NOWAIT);
      if ( Sinput->position->charno != c0 )
      { if ( my_prompt )
	  rl_set_prompt(my_prompt);
	rl_forced_update_display();
	c0 = Sinput->position->charno;
	rl_done = FALSE;
      }
    }
  } else
    PL_dispatch(0, PL_DISPATCH_WAIT);

  return TRUE;
}
#endif


static void
reset_readline(void)
{ if ( in_readline )
  { restore_signals();
  }

  if ( my_prompt )
  { free(my_prompt);
    my_prompt = NULL;
  }
  in_readline = 0;
}


static ssize_t
Sread_readline(void *handle, char *buf, size_t size)
{ intptr_t h = (intptr_t)handle;
  int fd = (int) h;
  int ttymode = PL_ttymode(Suser_input); /* Not so nice */
  int rval;
#ifdef HAVE_CLOCK
  intptr_t oldclock = clock();
#endif

  PL_write_prompt(ttymode == PL_NOTTY);

  switch( ttymode )
  { case PL_RAWTTY:			/* get_single_char/1 */
#ifdef O_RLC
    { int chr = getkey();

      if ( chr == 04 || chr == 26 )
	return 0;			/* EOF */

      buf[0] = chr & 0xff;
      return 1;
    }
#endif
    case PL_NOTTY:			/* -tty */
#ifdef RL_NO_REENTRANT
    notty:
#endif
    { PL_dispatch(Suser_input, PL_DISPATCH_WAIT);
      rval = read(fd, buf, size);
      if ( rval > 0 && buf[rval-1] == '\n' )
	PL_prompt_next(Suser_input);

      break;
    }
    case PL_COOKEDTTY:
    default:
    { char *line;
      const char *prompt;

#ifdef RL_NO_REENTRANT
      if ( in_readline )
      { Sprintf("[readline disabled] ");
	PL_write_prompt(TRUE);
	goto notty;			/* avoid reentrance */
      }
#endif

#ifdef HAVE_RL_EVENT_HOOK
      if ( PL_dispatch(Suser_input, PL_DISPATCH_INSTALLED) )
	rl_event_hook = event_hook;
      else
	rl_event_hook = NULL;
#endif

      prompt = PL_prompt_string(Suser_input);
      if ( prompt )
	PL_add_to_protocol(prompt, strlen(prompt));

      { char *oldp = my_prompt;

	my_prompt = prompt ? strdup(prompt) : (char *)NULL;

	if ( sig_at_level == in_readline )
	{ sig_at_level = -1;
	  reset_readline();
	}

	if ( in_readline++ )
	{ int state = rl_readline_state;

	  rl_clear_pending_input();
	  rl_discard_argument();
	  rl_deprep_terminal();
	  rl_readline_state = (RL_STATE_INITIALIZED);
	  line = pl_readline(prompt);
	  rl_prep_terminal(FALSE);
	  rl_readline_state = state;
	  rl_done = 0;
	} else
	  line = pl_readline(prompt);
	in_readline--;

	if ( my_prompt )
	  free(my_prompt);
	my_prompt = oldp;
      }

      if ( line )
      { size_t l = strlen(line);

	if ( l >= size )
	{ PL_warning("Input line too long");	/* must be tested! */
	  l = size-1;
	}
	memcpy(buf, line, l);
	buf[l++] = '\n';
	rval = l;

	/*Sdprintf("Read: '%s'\n", line);*/
	free(line);
      } else if ( PL_exception(0) )
      { errno = EPLEXCEPTION;
	return -1;
      } else
	rval = 0;
    }
  }

  return rval;
}


static int
prolog_complete(int ignore, int key)
{ if ( rl_point > 0 && rl_line_buffer[rl_point-1] != ' ' )
  { rl_begin_undo_group();
    rl_complete(ignore, key);
    if ( rl_point > 0 && rl_line_buffer[rl_point-1] == ' ' )
    {
#ifdef HAVE_RL_INSERT_CLOSE		/* actually version >= 1.2 */
      rl_delete_text(rl_point-1, rl_point);
      rl_point -= 1;
#else
      rl_delete(-1, key);
#endif
    }
    rl_end_undo_group();
  } else
    rl_complete(ignore, key);

  return 0;
}


static char *
atom_generator(const char *prefix, int state)
{ char *s = PL_atom_generator(prefix, state);

  if ( s )
  { char *copy = malloc(1 + strlen(s));

    if ( copy )				/* else pretend no completion */
      strcpy(copy, s);
    s = copy;
  }

  return s;
}


static char **
prolog_completion(const char *text, int start, int end)
{ char **matches = NULL;

  if ( (start == 1 && rl_line_buffer[0] == '[') ||	/* [file */
       (start == 2 && strncmp(rl_line_buffer, "['", 2)) )
    matches = rl_completion_matches((char *)text,	/* for pre-4.2 */
				    rl_filename_completion_function);
  else
    matches = rl_completion_matches((char *)text,
				    atom_generator);

  return matches;
}

#undef read				/* UXNT redefinition */

static foreign_t
pl_rl_wrap(void)
{ if ( isatty(0) )
  { rl_catch_signals = 0;
    rl_readline_name = "Prolog";
    rl_attempted_completion_function = prolog_completion;
#ifdef __WINDOWS__
    rl_basic_word_break_characters = "\t\n\"\\'`@$><= [](){}+*!,|%&?";
#else
    rl_basic_word_break_characters = ":\t\n\"\\'`@$><= [](){}+*!,|%&?";
#endif
    rl_add_defun("prolog-complete", prolog_complete, '\t');
#if HAVE_RL_INSERT_CLOSE
    rl_add_defun("insert-close", rl_insert_close, ')');
#endif
#if HAVE_RL_SET_KEYBOARD_INPUT_TIMEOUT	/* see (*) */
    rl_set_keyboard_input_timeout(20000);
#endif

    rl_functions = *Sinput->functions;	/* structure copy */
    rl_functions.read = Sread_readline;	/* read through readline */

    Sinput->functions  = &rl_functions;
    Soutput->functions = &rl_functions;
    Serror->functions  = &rl_functions;

    PL_set_prolog_flag("readline",    PL_ATOM, "readline");
    PL_set_prolog_flag("tty_control", PL_BOOL, TRUE);
  }

  return TRUE;
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
For some obscure reasons,  notably  libreadline   6  can  show  very bad
interactive behaviour. There is a timeout set   to  100000 (0.1 sec). It
isn't particularly clear what this timeout is doing. I _think_ it should
be synchronized PL_dispatch_hook(),  and  set  to   0  if  this  hook is
non-null.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifdef __WINDOWS__
#define isatty(fd) TRUE
#endif

install_t
install_readline4pl(void)
{ PL_license("gpl", "GNU Readline library");

#define PRED(name, arity, func, attr) \
	PL_register_foreign_in_module("system", name, arity, func, attr)

  PRED("rl_read_init_file", 1, pl_rl_read_init_file, 0);
  PRED("rl_add_history",    1, pl_rl_add_history,    PL_FA_NOTRACE);
  PRED("rl_write_history",  1, pl_rl_write_history,  0);
  PRED("rl_read_history",   1, pl_rl_read_history,   0);
  PRED("rl_wrap",           0, pl_rl_wrap,           0);
}
