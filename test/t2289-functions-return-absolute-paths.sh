#! /bin/sh -e
# tup - A file-based build system
#
# Copyright (C) 2026  handicraftsman
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.

. ./tup.sh

mkdir src
touch src/glob.txt
cat > Tupfile << HERE
: |> printf 'group\n' > %o |> group.txt {grp}
: |> printf 'abs\n' > %o |> abs.txt
: |> printf '%%s\n' \$(abs abs.txt) > %o |> abs-path.txt
: |> printf '%%s\n' \$(globs src/*.txt) > %o |> globs-path.txt
: |> printf '%%s\n' \$(groups {grp}) > %o |> groups-path.txt
HERE

update
grep -qx "$PWD/abs.txt" abs-path.txt
grep -qx "$PWD/src/glob.txt" globs-path.txt
grep -qx "$PWD/group.txt" groups-path.txt

eotup
