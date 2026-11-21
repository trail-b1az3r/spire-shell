trap 'echo bye from EXIT' EXIT
trap 'echo caught USR1' USR1
kill -USR1 $$
echo after-signal
trap 'echo err-trap' ERR
false
trap - ERR
printf "%-6s|%5.2f|%03d|%x|%q\n" name 3.14159 7 255 "a b"
printf "%s-" a b c; echo
printf -v out "%05d" 42; echo $out
printf '%b\n' 'tab\there'
printf '%c%c\n' hello world
printf "%d\n" "'A"
OPTIND=1
while getopts "ab:c" o -a -b val -c; do echo "opt $o $OPTARG"; done
f() { local x=inner; echo $x; shift; echo "rest: $@"; }
x=outer; f one two three; echo $x
echo end
