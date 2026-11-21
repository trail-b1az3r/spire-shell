diff <(printf 'a\nb\n') <(printf 'a\nc\n') | grep -c '^[<>]'
cat <(echo procsub)
x=$(set -e; false; echo not-reached); echo "errexit=[$x]"
if false; then :; fi; echo cond-ok
set -o pipefail; false | true; echo pipefail=$?; set +o pipefail
false | true; echo no-pipefail=$?
(set -x; echo traced) 2>&1
printf '2\n' | (select fruit in apple banana; do echo "picked $fruit ($REPLY)"; break; done) 2>/dev/null
echo end
