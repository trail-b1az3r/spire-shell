warning this is ai generated

# spire — Zsharpend edition (branch `z#nd`)

**S**yntax-**P**olyglot **I**nteractive **R**untime **E**nvironment, rewritten in
[Zsharpend](https://github.com/trail-b1az3r/z-nd) (`.zxx`). The C version of
spire lives on the other branches of this repository (`main`, and the newest on
`claude/sleepy-shannon-ggm7uu`); this branch is the same
shell as a Zsharpend program, run by Whetstone with the `os` script extender
(the terminal, processes and files).

## Running it

You need Whetstone with the `os` extender, from the Zsharpend repository
(currently on its `claude/spire-in-zsharpend` branch):

```sh
git clone -b claude/spire-in-zsharpend https://github.com/trail-b1az3r/z-nd
cargo install --path z-nd/crates/whetstone      # or just: cargo build --release -p whetstone
```

Then, from this checkout:

```sh
./spire                          # interactive
./spire -c 'ls | wc -l'          # one command
./spire script.fish a b          # a script, with $argv
tests/run.sh                     # the test scripts
```

`./spire` finds Whetstone as `$WHETSTONE`, on `$PATH`, or in a `z-nd`
checkout next to this one (`../z-nd/target/release/whetstone`).

## What it does

- **Both syntaxes**: `if … else if … end` and `if …; then … elif … fi`; `while`/`until`, `for` (`end` or `do … done`), `switch`/`case`, `case … esac`, `function f -a x … end` and `f() { … }`, `begin … end`, `{ … }`, `cmd; and …`, `cmd; or …`, `not`/`!`, `&&`, `||`, `|`, `|&`, `&`.
- **Expansion**: quotes and escapes, `$var`, fish lists (`$list[1]`, `$list[-1]`, `$list[2..3]`), bash arrays (`a=(x y)`, `a+=(z)`, `${a[@]}`, `${a[1]}`, `${#a[@]}`), `${var:-default}` `:=` `:+` `:?`, `${#var}`, `${var#pat}` `##` `%` `%%`, `${var/pat/rep}` `//` `/#` `/%`, `${var:off:len}`, `${var^^}` `,,`, `${!ref}`, `$#`, `$@`, `$1`, `(cmd)` and `$(cmd)`, `$(( arithmetic ))`, `{a,b}` and `{1..5}`, `~`, `*` `?` `[…]` `**` globs.
- **Tests and arithmetic**: `[[ … ]]` with `==` globs, `=~` regexes (`BASH_REMATCH`), `<` `>`, `&&` `||` `!` and `( … )`; `(( … ))` with C operators, `++`, assignments and short-circuiting; `test` / `[`.
- **Redirection**: `>`, `>>`, `<`, `2>`, `2>>`, `&>`, `2>&1`, `>&2`, `<<<`, here-documents (`<<EOF`, `<<-EOF`, `<<'EOF'`), on commands and on blocks; `( subshells )`, process substitution `<(cmd)` / `>(cmd)`, `select`.
- **Pipelines** run all their stages at once through real pipes, builtins and functions included; a trailing builtin runs in the shell, so `… | read x` and `… | source` keep their effect.
- **Builtins**: `cd pwd echo printf set (fish, and bash's -e/-x/-u/-o pipefail/--) export unset local shift alias abbr source read type history functions status set_color ls ll la count contains string math test [ trap getopts complete commandline jobs fg return exit break continue emit fish_add_path prompt_pwd fish_vcs_prompt bind command builtin eval isatty help true false`. `trap` handles `EXIT`, `ERR` and signals; `printf` does widths, precision, `%b`, `%q`, `-v` and reuses its format.
- **Completion rules**: `complete -c CMD -s/-l/-a/-d/-n/-f` (fish) and `complete -W/-F` (bash), fish's `completions/CMD.fish` files, `commandline`, the `__fish_*` helpers, and options learned from `CMD --help`, shown with descriptions in the menu.
- **The prompt**: `user ❯ ` then what you type; the `❯` turns red after a failure, the git branch shows on the right inside a repository, and each new prompt fades in (`set spire_animate no` turns that off).
- **Editing**: fish's colors for syntax highlighting (unknown commands red, valid paths underlined), autosuggestions from history (→ accepts, Alt-→ one word), ↑/↓ history search by prefix, Ctrl-R, Tab completion of commands, files and `$variables` with a menu (Tab again to choose), abbreviations expanding on space, "did you mean" for typos, and `cd`-less directory names.
- **Fish configs**: at startup it runs `~/.config/fish/conf.d/*.fish` and `config.fish`, autoloads `functions/NAME.fish`, and uses your `fish_prompt`, `fish_right_prompt` and `fish_greeting`. Functions with `--on-event fish_prompt/fish_preexec/fish_postexec` or `--on-variable PWD` fire.
- **Key bindings**: `bind` with fish's key notation (old `\\cf`, `\\e[A`; new `ctrl-f`, `alt-x`, `up`; `-k` names), binding keys to fish input functions (`forward-char`, `history-search-backward`, `accept-autosuggestion`, …) or to commands that edit the line with `commandline -r/-i/-a/-t/-C/-f`; `functions --copy`.
- **ls, ll, la** behave like fish's: color and `-F` indicators on a terminal, plain when piped.

## Files

| File | |
|---|---|
| `src/main.zxx` | start-up, the prompt loop, history, fish config loading |
| `src/lex.zxx`, `src/parse.zxx` | words and operators; one parser for both syntaxes |
| `src/vars.zxx`, `src/expand.zxx` | variables (fish-style lists) and word expansion |
| `src/exec.zxx` | running commands, pipelines, functions, redirections |
| `src/builtins.zxx` | the builtins |
| `src/edit.zxx` | the line editor, highlighting, completion, prompt |
| `tests/` | scripts and their expected output; `tests/run.sh` checks them |

## Compared with the C spire (branch `claude/sleepy-shannon-ggm7uu`)

The two editions now cover the same ground — process substitution, `select`, `set -e` / `-x` / `-u` / `-o pipefail`, `trap`, here-documents, `[[ ]]`, `$(( ))` and programmable completion included. Neither has associative arrays. Command substitution and `( … )` run in a forked copy of the shell, as in bash.
