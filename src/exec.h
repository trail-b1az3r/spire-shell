#ifndef SPIRE_EXEC_H
#define SPIRE_EXEC_H

#include "common.h"
#include "ast.h"
#include <sys/types.h>

void exec_init(void);

/* Execute a parsed top-level program (an N_SEQ root). Returns the exit
 * status of the last command run ($?). */
int exec_program(Node *root);

/* Execute a block Node (used for function bodies / control-flow bodies). */
int exec_block(Node *block);

/* Parse+execute `src` with stdout captured into a malloc'd, newline-trimmed
 * buffer (*out_buf, caller frees). Used for $(...) / `...` / (...) command
 * substitution. Returns the exit status. */
int exec_capture_stdout(const char *src, char **out_buf);

/* Parse+execute the contents of a file in the current shell (source/.), or
 * as a module. Returns exit status; -1 if the file could not be opened. */
int exec_source_file(const char *path);

/* Parse+execute a raw string as if typed at the prompt (used by -c). */
int exec_source_string(const char *src);

/* Invoke a user-defined function body with the given argv (argv[0] is the
 * function name). Sets up $1.., $#, $@, $argv for the duration of the call. */
int exec_call_function(Node *body, int argc, char **argv);

/* current recursion depth of function calls / command substitutions, used
 * to guard against runaway recursion */
extern int g_exec_depth;

/* set by `exit`/loop control builtins via longjmp-free flags checked between
 * statements */
extern volatile bool g_should_exit;
extern int g_exit_code;

/* whether the shell owns the terminal (real tty + interactive REPL) */
extern bool g_shell_is_interactive;
extern pid_t g_shell_pgid;

/* called by the `break` / `continue` / `return` builtins */
void exec_signal_break(void);
void exec_signal_continue(void);
void exec_signal_return(int code);

/* ---- positional parameters ($1.. $# $@ $* and fish's $argv list) ---- */
void exec_set_positional(int count, char **params); /* params[0] is $1 */
size_t exec_positional_count(void);
const char *exec_positional_get(size_t i);          /* 0-based: 0 is $1 */

/* ---- shell options (set -e / -x / -u / -o pipefail) ---- */
extern bool g_opt_errexit;
extern bool g_opt_xtrace;
extern bool g_opt_pipefail;
extern bool g_opt_nounset;

/* ---- traps: sig 0 = EXIT, TRAP_ERR = ERR, otherwise a signal number ---- */
#define TRAP_ERR 64
#define TRAP_SLOTS 65
void exec_set_trap(int slot, const char *cmd); /* NULL = reset to default, "" = ignore */
const char *exec_get_trap(int slot);
void exec_run_pending_traps(void);
void exec_run_exit_trap(void);

/* ---- process substitution: runs `src` with its stdout (want_read) or
 * stdin (!want_read) attached to a pipe and returns "/dev/fd/N" for the
 * other end. The fd is closed after the current command finishes. ---- */
char *exec_procsub(const char *src, bool want_read);

/* milliseconds the last interactive command took (for %D and $CMD_DURATION) */
extern long g_last_duration_ms;

/* parse+run `src` inside the current context (no flag resets): `eval` */
int exec_eval_string(const char *src);

/* runs argv as a builtin or external command, never a function (`command`) */
int exec_run_argv_nofunc(int argc, char **argv);

/* performs a raw assignment word ("x=1", "a=(1 2)", "a[2]=x", "s+=y"),
 * expanding its value exactly once */
void exec_assign(const char *raw);
bool exec_looks_like_assignment(const char *s);

/* fish events: runs every function declared `--on-event NAME` (or
 * `--on-variable NAME` for exec_emit_var) with the given arguments */
void exec_emit_event(const char *name, int argc, char **argv);
void exec_emit_var(const char *name);

#endif
