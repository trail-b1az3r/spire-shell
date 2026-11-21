seq 3 | while read l; do echo "L$l"; done
echo "a b" | read x y; echo "read: $y $x"
echo "function pf; echo piped-fn; end" | source; pf
f() { while read line; do echo "f:$line"; done; }
printf 'x\ny\n' | f | tr a-z A-Z
yes | head -2 | while read y; do echo got-$y; done
v=$(echo cap | f); echo "v=$v"
count_lines() { set -l n 0; while read l; set n (math $n + 1); end; echo $n; }
seq 100 | count_lines
echo done
