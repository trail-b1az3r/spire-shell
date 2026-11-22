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

## Compile / build Spire itself

To compile the Spire Z# program into a Whetstone bundle (`build/spire.wsb`):

```sh
sh ./compile-spire.sh
```

Use `--run` to launch the compiled bundle after a successful build:

```sh
sh ./compile-spire.sh --run
```

If Whetstone is not installed but a neighboring `../z-nd` checkout exists, build Whetstone and compile Spire in one step:

```sh
sh ./compile-spire.sh --build-whetstone
```

You can set `WHETSTONE=/path/to/whetstone` to select a specific compiler/runtime executable.
The script checks the project, builds it, verifies the expected bundle was created,
and optionally runs it. This produces a `.wsb` bundle (not a standalone native binary).

## What it does

- **Both syntaxes**: `if … else if … end` and `if …; then … elif … fi`; `while`/`until`, `for` (`end` or `do … done`), `switch`/`case`, `case … esac`, `function f -a x … end` and `f() { … }`, `begin … end`, `{ … }`, `cmd; and …`, `cmd; or …`, `not`/`!`, `&&`, `||`, `|`, `|&`, `&`.
- **Expansion**: quotes and escapes, `$var`, fish lists (`$list[1]`, `$list[-1]`, `$list[2..3]`), bash arrays (`a=(x y)`, `a+=(z)`, `${a[@]}`, `${a[1]}`, `${#a[@]}`), `${var:-default}` `:=` `:+` `:?`, `${#var}`, `${var#pat}` `##` `%` `%%`, `${var/pat/rep}` `//` `/#` `/%`, `${var:off:len}`, `${var^^}` `,,`, `${!ref}`, `$#`, `$@`, `$1`, `(cmd)` and `$(cmd)`, `$(( arithmetic ))`, `{a,b}` and `{1..5}`, `~`, `*` `?` `[…]` `**` globs.
- **Tests and arithmetic**: `[[ … ]]` with `==` globs, `=~` regexes (`BASH_REMATCH`), `<` `>`, `&&` `||` `!` and `( … )`; `(( … ))` with C operators, `++`, assignments and short-circuiting; `test` / `[`.
- **Redirection**: `>`, `>>`, `<`, `2>`, `2>>`, `&>`, `2>&1`, `>&2`, `<<<`, here-documents (`<<EOF`, `<<-EOF`, `<<'EOF'`), on commands and on blocks; `( subshells )`, process substitution `<(cmd)` / `>(cmd)`, `select`.
- **Pipelines** run all their stages at once through real pipes, builtins and functions included; a trailing builtin runs in the shell, so `… | read x` and `… | source` keep their effect.
- **Builtins**: `cd pwd echo printf set` (fish, and bash's `-e/-x/-u/-o pipefail/--`) `export unset local shift alias abbr source read type history functions status set_color ls ll la count contains string math test [ trap getopts complete commandline jobs fg return exit break continue emit fish_add_path prompt_pwd fish_vcs_prompt bind command builtin eval isatty help true false`. `trap` handles `EXIT`, `ERR` and signals; `printf` does widths, precision, `%b`, `%q`, `-v`.


## Versioned builds and system install

Spire tracks semantic versions in `VERSION`. Use `sh ./build-spire.sh` for a normal build; it automatically increments the patch version (for example, `0.1.0` to `0.1.1`). Select a manual major bump with `--major` (resets minor and patch) or manual minor bump with `--minor` (resets patch). Explicit `--patch` is also available.

- `sh ./build-spire.sh --major`: manual major increment
- `sh ./build-spire.sh --minor`: manual minor increment
- `sh ./build-spire.sh --patch`: automatic patch increment
- `sh ./build-spire.sh --install`: build, then install Spire and Whetstone to `/usr/local` (run with sudo)
- `sh ./build-spire.sh --run`: build and run the resulting bundle

After system installation, upgrade the installation and rebuild from the latest `z#nd` branch with `sudo spire-upgrade`. The upgrade script installs the Whetstone runtime alongside Spire, so the installed launcher is self-contained with respect to the runtime dependency.
