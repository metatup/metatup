#! /bin/sh -e
# tup - A file-based build system
#
# Copyright (C) 2026  handicraftsman
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.

. ./tup.sh

mkdir -p extrepo/sub extrepo/shared
cat > MetaTupRepo.yaml << HERE
meta:
  name: root

dependencies:
  - name: extrepo
    path: ./extrepo
HERE

cat > Tupfile << HERE
call root_fn({})

function root_fn {
  call "@extrepo//sub" sub_fn({})
}
HERE

cat > extrepo/sub/Tupfile << HERE
function sub_fn {
  call "//shared" call_gen({})
  spawn "//shared" spawn_gen({})
}
HERE

cat > extrepo/shared/Tupfile << HERE
function call_gen {
  : |> echo repo-call > %o |> call.txt
}

function spawn_gen {
  : |> echo repo-spawn > %o |> spawn.txt
}
HERE

update
check_exist call.txt
check_exist spawn.txt
grep -qx "repo-call" call.txt
grep -qx "repo-spawn" spawn.txt

eotup
