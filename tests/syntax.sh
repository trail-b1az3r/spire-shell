x=5
echo $(( x * 2 + 1 )) $((2**10)) $(( 7 / 2 )) $(( -7 % 3 )) $(( 1 < 2 ? 10 : 20 )) $(( 0x1f )) "$(( x > 3 && x < 9 ))"
(( x++ )); echo after=$x
(( x > 100 )) || echo not-big
i=0; while (( i < 3 )); do echo loop $i; (( i += 1 )); done
y=0; (( 0 && y++ )); echo short=$y
[[ abc == a* ]] && echo glob-ok
[[ foo.txt =~ ^([a-z]+)\.(txt)$ ]] && echo "re $BASH_REMATCH[3]"
[[ -d /tmp && ! -f /tmp ]] && echo files-ok
[[ ( a == b ) || ( 3 -gt 2 ) ]] && echo group-ok
[[ abc < abd ]] && echo order-ok
[ 3 -eq 3 -a -n x ] && echo test-ok
test -z "" && echo z-ok
f=/usr/local/lib/archive.tar.gz
echo ${f#*/} ${f##*/} ${f%.*} ${f%%.*}
echo ${f/lib/LIB} ${f//a/A} ${f/#\/usr/U} ${f/%gz/xz}
s=HelloWorld; echo ${s:5} ${s:0:5} ${s: -3} ${s^^} ${s,,}
ref=s; echo ${!ref} ${#s}
cat <<EOF
heredoc $x $(echo sub) $((1+1))
  "quotes" stay
EOF
echo after-heredoc
(cd /; echo in-sub $PWD); echo out-unchanged
v=$(cd /usr; pwd); echo cap=$v
echo end
