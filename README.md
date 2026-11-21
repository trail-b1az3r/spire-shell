warning this is ai generated 

# spire

**S**yntax-**P**olyglot **I**nteractive **R**untime **E**nvironment

A shell that merges the parts of fish, zsh, and bash people actually miss
when they switch between them: fish's live, colorized syntax highlighting
(but fully customizable) and readable `if ... end` blocks, bash/zsh's
`if/then/fi` and `name() { }` syntax so your muscle memory and existing
scripts still work, and a zsh-style module system for extending it.

This is a real, from-scratch shell — a hand-written lexer, recursive-descent
parser, tree-walking executor with proper `fork`/`exec`/`pipe`/job-control,
and a raw-terminal line editor. Not a wrapper around `readline` and not a
toy. It is, however, a big project built in one sitting, so see
[Limitations](#limitations) before you make it your only shell.

## Highlights

### Syntax: fish and bash, in one parser

- **Both control-flow styles.** `if`/`else if`/`end` or `if`/`then`/`elif`/`fi`;
  `for`/`while`/`until` with `end` or `do ... done`; `switch`/`case`/`end` or
  `case`/`esac`; `function name ... end` or `name() { }`. Mix them freely.
- **fish extras**: `begin ... end` blocks, `cmd; and other`, `cmd; or other`,
  `not cmd`, `function f -a first second -d "description"` (named arguments),
  `$status`, list variables with 1-based indexing (`$list[1]`, `$list[-1]`,
  `$list[2..3]`), `(cmd)` command substitution, `**` recursive globs
  (`**/*.c`, `**.md`), and the `string`, `math`, `count`, `contains`,
  `random`, `status`, `abbr` and `set -l/-g/-x/-e/-q/-a/-p` builtins.
- **bash extras**: `{ cmd; cmd; }` groups, `( subshells )`, `!` negation,
  `time pipeline`, `[[ ]]` with `==` globs, `=~` regexes, `<`/`>`,
  `( grouping )`, `&&`/`||`/`!`; `(( arithmetic ))` with short-circuiting
  `&&`/`||`/`?:` and the `,` operator; `for x; do` over `"$@"`;
  `local`/`declare`/`typeset`/`readonly`/`export` (including `local a=(1 2)`).
- **Full parameter expansion**: `${v:-d}` `${v-d}` `${v:=d}` `${v:+a}`
  `${v:?msg}` `${#v}` `${v#pat}` `${v##pat}` `${v%pat}` `${v%%pat}`
  `${v/pat/rep}` `${v//pat/rep}` `${v/#pat/rep}` `${v/%pat/rep}`
  `${v:offset:length}` `${v^}` `${v^^}` `${v,}` `${v,,}` `${v@Q}` `${!ref}`
  `${!arr[@]}` `${!prefix*}`, nested (`${a:-${b:-c}}`), and the same
  operators applied to whole arrays (`${arr[@]^^}`, `${arr[@]:1:2}`, `${@:2}`).
- **Quoting & expansion**: `'...'`, `"..."`, `$'\t\x41\e'` ANSI-C strings,
  `"$@"` keeps each argument separate, brace expansion (`{a,b}`, `{01..10..2}`),
  `$(cmd)`, `` `cmd` ``, `(cmd)`, process substitution `<(cmd)` / `>(cmd)`,
  `~` / `~user`, and dynamic variables `$RANDOM $SECONDS $EPOCHSECONDS
  $EPOCHREALTIME $PPID $UID $SHLVL $!`, plus `PIPESTATUS`/`$pipestatus`.
- **Redirection**: `>` `>>` `<` `<>` with any fd (`3> file`, `2>> log`),
  duplication and closing (`>&2`, `2>&1`, `3>&-`, `<&3`), `&>` `&>>`,
  `<<EOF` `<<-EOF` `<<'EOF'`, here-strings `<<<`, `|&`, and redirections on
  compound commands (`while read l; do ...; done < file`, `{ ...; } > out`).
  Builtins keep their effect when redirected (`read x < file` works).
- **Shell options & traps**: `set -e`, `set -x` (colored trace), `set -u`,
  `set -o pipefail`, `set --`; `trap 'cmd' EXIT ERR INT TERM ...`.
- **History expansion** at the prompt: `!!`, `!$`, `!^`, `!*`, `!n`, `!-n`,
  `!prefix`, `!!:$`, and `^old^new`. A leading space keeps a command out of
  history.

### Your fish config, as is

If you have `~/.config/fish` (or `$XDG_CONFIG_HOME/fish`), interactive spire
loads it the way fish does: universal variables from `fish_variables`
(including `fish_user_paths` and a `fish_color_*` theme), every
`conf.d/*.fish` in order, then `config.fish`. Functions in `functions/` and
completions in `completions/` are autoloaded on first use. Your
`fish_prompt`, `fish_right_prompt` and `fish_greeting` are used, functions
declared `--on-event fish_prompt / fish_preexec / fish_postexec` or
`--on-variable PWD` fire, and the builtins and helpers configs reach for
exist: `set_color` (names, `br*`, hex, `--bold`, `--background=`),
`alias NAME 'cmd …'`, `abbr`, `bind`, `commandline`, `emit`, `psub`,
`fish_add_path`, `prompt_pwd`, `fish_vcs_prompt`/`fish_git_prompt`,
`type -q`, `command -q/-s`, `status --is-interactive`, `isatty`,
`string split -f`, `__fish_seen_subcommand_from` and friends. `cmd | source`
and `cmd | read` run in the shell, so `zoxide init fish | source`-style
lines work. spire's own `~/.config/spire/config.spire` runs afterwards and
can override anything. Turn it off with `fish_config = no`.

### Interactive: completion, editing, color

- **fish's colors, 1:1, by default.** Syntax highlighting, autosuggestions
  and the completion pager use fish's default theme (commands in the
  terminal's normal color, arguments cyan, quotes yellow, errors bright red,
  `;`/`|`/`&` green, redirections bold cyan, comments red, descriptions
  yellow italic, the typed prefix bold+underlined). `colorscheme spire`
  brings back spire's own palette.
- **The prompt** is `user ❯ ` followed by what you type; the `❯` turns red
  after a failed command, and inside a git repository the branch shows on
  the right. Each new prompt fades in over ~60 ms (skipped when you're
  typing ahead; `prompt.animate = no` turns it off).
- **`ls`, `ll` and `la` behave like fish's functions**: color and `-F` type
  indicators on a terminal, plain when piped, `ll` = `ls -lh`,
  `la` = `ls -lAh` — so `ll` or `ls -la` is colored with no aliases. With
  no `$LS_COLORS`, spire loads the full palette from `dircolors`.

- **Context-aware Tab completion** with a fish/zsh-style menu: candidates
  are laid out in a grid with **descriptions**, colored by kind (files use
  your `LS_COLORS`), and repeated Tab / Shift-Tab / arrow keys move a
  highlighted selection (Enter accepts, Ctrl-G cancels). Filenames with
  spaces are escaped (or completed inside the quote you opened).
  It knows about:
  - commands (builtins, functions with their descriptions, aliases,
    abbreviations, keywords, `$PATH`), `$variables` / `${vars}` (with their
    values), `~users`, directories only after `cd`/`pushd`/`mkdir` (and
    `$CDPATH`), files after redirections and `--opt=` / `VAR=`
  - **options parsed from `--help`** (`ls --col<Tab>` → `--color`), cached
    and limited to native binaries so scripts are never run
  - `git` subcommands, branches, tags, remotes, changed files and your git
    aliases; `make` targets; `ssh`/`scp` hosts from `~/.ssh/config`,
    `known_hosts` and `/etc/hosts`; `kill` signals, jobs and processes;
    `npm`/`yarn`/`pnpm run` scripts from `package.json`; `man`/`which`/`type`;
    `export`/`unset`/`set`; `fg`/`bg`; `trap`; `module`; `colorscheme`; …
  - **your own completions** with a `complete` builtin that speaks both
    dialects:
    ```sh
    complete -c deploy -f -a 'staging production' -d 'environment'   # fish
    complete -c deploy -l force -d 'skip safety checks'
    complete -W "start stop status" svc                              # bash
    _mytool() { COMPREPLY=(alpha beta gamma); }; complete -F _mytool mytool
    ```
  - and a case-insensitive, then fuzzy (subsequence) fallback when nothing
    matches literally: `cfgfile<Tab>` finds `my_config_file.txt`.
- **A wrap- and Unicode-aware line editor** (no readline): long lines wrap
  correctly, wide characters and emoji count as two columns, plus
  fish-style autosuggestions from history (→ / End / Ctrl-E accepts all,
  Alt-→ / Alt-F accepts one word), Up/Down **prefix search** through history,
  Ctrl-R incremental search with the match highlighted, undo (Ctrl-Z /
  Ctrl-_), a kill ring (Ctrl-U/K/W, Alt-D, Alt-Backspace, Ctrl-Y), Alt-.
  (insert the last argument of earlier commands), Alt-U/L/C (upper/lower/
  capitalize word), Alt-T (transpose words), Alt-S (toggle `sudo`),
  Ctrl-X Ctrl-E / Alt-E (edit the line in `$EDITOR`).
- **Abbreviations** (`abbr -a gco git checkout`) expand in place when you
  press space or Enter, like fish.
- **Live syntax highlighting** with configurable colors for commands,
  invalid commands, builtins, functions, keywords, strings, variables,
  escapes, operators, redirections, comments, numbers, flags, and paths —
  arguments naming **existing files are underlined**, redirection targets are
  recognized, and `$(...)` contents are highlighted recursively.
- **A richer prompt**: zsh-style escapes including exit status (`%?`, `%E`),
  conditionals (`%(?.green.red)`), git branch with dirty marker (`%G`),
  command duration for slow commands (`%c`), jobs (`%J`), time (`%T`,
  `%D{fmt}`), virtualenv (`%v`), background colors (`%K{}`), bold and
  underline — plus a **right prompt** (`rprompt`). The default prompt turns
  red after a failed command and shows `✘ status`, durations and job counts on
  the right. Output that didn't end with a newline is marked with a dim `⏎`.
- **Helpful errors**: `command not found` in red with a **did-you-mean**
  suggestion (`gti` → `git`), `autocd` (type a directory name to `cd` into
  it), and `command_not_found_handle` / `fish_command_not_found` hooks.
- **Colors everywhere**: colored `jobs`, `history`, `type`, `set`, `abbr`,
  `functions`, `help`, `set -x` traces, job notifications, and an `aliases`
  module that turns on color for `ls`, `grep`, `diff` and `ip`. Colors accept
  names, 256-color numbers, `#rrggbb` truecolor and `bold-`/`underline-`/
  `italic-`/`dim-`/`reverse` modifiers; `colorscheme` switches between seven
  presets.

### Builtins

`cd pwd exit export unset alias unalias source . echo printf read test [
type which command builtin eval exec : shift getopts set declare typeset
readonly local let jobs fg bg wait kill disown trap umask dirs pushd popd
history functions true false break continue return string math count
contains random status abbr complete set_color module colorscheme help`

`printf` supports widths, precision, `%d %i %u %o %x %X %f %e %g %c %s %b %q`,
`*` widths and `-v var`; `read` supports `-p -s -r -n -d -t -a`; `test`/`[`
supports `! ( ) -a -o` and every bash file operator.

## Building & installing

```sh
./install.sh                 # build, install to /usr/local/bin (uses sudo if needed)
./install.sh --user          # install to ~/.local/bin, no sudo, no /etc/shells edit
./install.sh --prefix=/opt   # install into /opt/bin instead
```

This builds with `make` (plain C11, no dependencies beyond libc), installs
the binary, and — without ever clobbering files you already have — writes
a starter `~/.config/spire/spire.conf` and the two example modules into
`~/.config/spire/modules/`.

To make it your login shell:

```sh
chsh -s "$(command -v spire)"    # or the full path install.sh printed
```

To remove it:

```sh
./uninstall.sh            # removes the binary, keeps your config/history
./uninstall.sh --purge    # also removes ~/.config/spire and history
```

You can also just `make` and run `./spire` directly without installing
anything.

## Using it

```sh
spire                    # interactive shell (also: spire -i, spire -l)
spire -c 'echo hi'       # run a one-liner and exit
spire script.spire a b   # run a script, with $1=a $2=b
```

### Control flow, either syntax

```sh
if test -f config.toml
    echo "found it"
else if test -d config.d
    echo "found a directory instead"
else
    echo "nothing"
end
```

```sh
if [ -f config.toml ]; then
    echo "found it"
elif [ -d config.d ]; then
    echo "found a directory instead"
else
    echo "nothing"
fi
```

### Functions, either syntax

```sh
function greet
    echo "hello $argv"
end
```

```sh
greet() {
    echo "hello $1"
}
```

### Loops

```sh
for f in *.txt
    echo $f
end

for f in *.txt; do echo $f; done

while test -f lockfile
    sleep 1
end
```

## Configuration

`~/.config/spire/spire.conf`, plain `key = value` (see `config/spire.conf` for
every key with comments):

```sh
prompt  = "%F{cyan}%n%f@%F{cyan}%m%f %F{yellow}%~%f %F{brightblack}%G%f%(?.%F{green}.%F{red})▲❯%f "
rprompt = "%(?..%F{red}✘ %?%f )%F{yellow}%c%f%F{brightblack}%J%f"

color.command         = green
color.command_invalid = red
color.function        = brightblue
color.redirect        = brightmagenta
color.description     = brightblack     # completion menu descriptions
color.selection       = reverse         # selected completion
# ... and builtin string variable operator comment keyword number path
#     array flag escape suggestion text

autocd = yes
autosuggest = yes
highlight.valid_paths = yes
complete.parse_help = yes
prompt.duration_threshold = 2000        # ms before %c shows a duration
history.size = 5000
modules = aliases, git
```

Commands to run at startup — aliases, abbreviations, functions, `complete`
rules, `set -o` options — go in `~/.config/spire/config.spire` (or
`~/.spirerc`), which interactive shells source after loading modules.

Prompt escapes: `%n` user, `%m`/`%M` short/full host, `%~` cwd with `~`,
`%/` full cwd, `%.` last cwd component, `%g` git branch, `%G` `(branch*)`
with a dirty marker, `%?` last exit status, `%E` `✘ N` only after a failure,
`%j`/`%J` job count (always / only when nonzero), `%c` duration of the last
command if it was slow, `%T` `%*` `%t` time, `%D{strftime}`, `%v`
virtualenv, `%h` history number, `%#` `#`/`$`, `%F{color}…%f` and
`%K{color}…%k` foreground/background, `%B…%b` bold, `%U…%u` underline,
`%r` git branch name, `%(g.in-repo.not)`, `%P` fish-style shortened cwd (`~/s/project`), `%(?.yes.no)` conditionals (`?` success, `j` jobs, `#` root, `N` status N),
`%%` a literal `%`.

Color values: `black red green yellow blue magenta cyan white default`,
their `bright*` variants, `gray orange purple`, a 256-color number,
`#rrggbb`, the attributes `bold underline italic dim reverse`, and
modifier prefixes such as `bold-red` or `underline-cyan`.

There's also a `colorscheme` builtin with seven built-in presets
(`colorscheme dark|light|mono|solarized|nord|gruvbox|dracula`; plain
`colorscheme` shows the current colors as a swatch).

## Writing a module

A module is just a `.spire` file, sourced into the shell at startup:

```sh
# ~/.config/spire/modules/mymodule.spire
function hello
    echo "hi from mymodule, argv = $argv"
end

alias reload='source ~/.config/spire/spire.conf'
```

Enable it with `modules = mymodule` in `spire.conf`, or load it on demand
with `module load mymodule`. `module list` shows what's enabled.

## Project layout

```
src/            all shell source (C11, no external dependencies)
  lexer.c/h       tokenizer (word/operator/comment scanning, quote-aware)
  parser.c/h      recursive-descent parser, both syntaxes -> one AST
  ast.c/h         AST node definitions
  expand.c/h      variable/command-substitution/glob/quote expansion
  exec.c/h        the tree-walking executor: fork/exec/pipe/redirect/jobs
  builtins.c/h    builtin commands
  vars.c/h        shell variable table (+ environment sync)
  funcs.c/h       user-defined function table
  aliases.c/h     alias table
  jobs.c/h        background job tracking
  history.c/h     command history (in-memory + append-only file)
  lineedit.c/h    raw-terminal line editor (the "not-readline" part)
  highlight.c/h   live syntax highlighter used by the line editor
  builtins_extra.c  set/declare/printf/read/test/trap/getopts/string/math/abbr/...
  complete.c/h    tab completion engine, `complete` rules, --help parsing
  colors.c/h      color specs, LS_COLORS, terminal display width
  config.c/h      spire.conf loading
  prompt.c/h      prompt template rendering (%n %m %~ %F{}... etc)
  modules.c/h     module loader
  main.c          entry point, argument handling, REPL loop
config/
  spire.conf        starter config, copied to ~/.config/spire on install
  modules/          starter modules (aliases, git, fish)
tests/
  run.sh            regression suite: `make test` (`tests/run.sh --update` to regenerate)
  cases/            NAME.spire scripts and their NAME.expected output
install.sh, uninstall.sh, Makefile
```

## Testing

```sh
make test
```

runs every script in `tests/cases/` and diffs its output against the
matching `.expected` file.

## Limitations

This was built to be genuinely useful, not to pass a POSIX conformance
suite. Known gaps, so nothing surprises you:

- No associative arrays (maps) and no sparse arrays — only flat,
  integer-indexed arrays (`unset arr[2]` blanks the element).
- No `select` loops, `coproc`, or `case` fall-through (`;&`, `;;&`).
- `readonly`, `declare -i`/`-r`/`-l`/`-u` are accepted but don't enforce
  their attribute.
- The line editor is single-line: multi-line commands are entered one line
  at a time with a continuation prompt, and bracketed paste isn't used
  (pasting several lines runs them one by one, like bash).
- Aliases are argument-prefix macros (`alias ll='ls -la'`), not full
  re-parsed command lines — an alias can't itself contain a pipe or `&&`
  (use a function or an `abbr` for that).
- Word splitting/globbing is a close, pragmatic approximation of
  POSIX rules, not a byte-for-byte match to bash.
- `set -e` follows the common cases (simple commands, pipelines, subshells
  outside conditions), not every bash corner case.
- Job control works (`jobs`/`fg`/`bg`/`kill %n`/`disown`, real `tcsetpgrp`
  terminal handoff, stopped-job reporting) but is simpler than a mature
  shell's.

Patches welcome in spirit, even though this is a from-scratch personal
project rather than a maintained package — read the source, it's meant
to be readable.
