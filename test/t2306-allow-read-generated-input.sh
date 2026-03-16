#! /bin/sh -e
. ./tup.sh

cat > Tupfile << 'HERE'
: |> echo generated > dep.txt |> dep.txt
: |> ^allow_read=dep.txt^ cat dep.txt > out.txt |> out.txt
HERE

update
check_exist dep.txt out.txt
grep -qx 'generated' out.txt

eotup
