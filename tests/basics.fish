echo hello world
set x 1 2 3; echo $x[2] (count $x) $x[-1]
for i in a b c; echo "item $i"; end
for i in 1 2; do echo bash $i; done
if test 1 -eq 1; echo yes; else; echo no; end
if [ 2 -gt 3 ]; then echo no; elif true; then echo elif-ok; else echo no; fi
function greet -a name; echo "hi $name"; end
greet bob
f() { echo "args: $# $1"; }
f one two
echo a b c | tr a-z A-Z
echo $(echo nested) (echo fish-subst)
ls /etc/hostname > /tmp/spire-zxx-test.txt; cat < /tmp/spire-zxx-test.txt
echo {x,y}{1,2} {1..3}
false; or echo fish-or
true; and echo fish-and
not false && echo not-ok
switch hello; case 'h*'; echo switched; case '*'; echo nope; end
case foo in f*) echo bash-case ;; *) echo nope ;; esac
set -l n 0
while test $n -lt 3; set n (math $n + 1 2>/dev/null; or expr $n + 1); end; echo n=$n
echo ${NOPE_UNSET:-none} ${nope:-default} $status
string upper shout
alias hi 'echo alias works'; hi
echo "piped" | read v; echo got $v
echo one two | string upper
nosuchcmd 2>/dev/null; echo st=$status
cd /tmp; pwd; rm -f /tmp/spire-zxx-test.txt
echo *.nonexistent-glob
math "(1 + 2) * 3"
command -q ls && echo command-q
echo "function piped; echo from-pipe; end" | source
piped
set -l words (echo a; echo b); echo (count $words)
