#! /bin/sh -e
# tup - A file-based build system
#
# Copyright (C) 2026  handicraftsman
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.

. ./tup.sh

mkdir pkg
cat > Tupfile << HERE
call "./pkg/Tupfile" exported({ "msg": "one" })
HERE
cat > pkg/Tupfile << HERE
include ./helpers.tup
HERE
cat > pkg/helpers.tup << HERE
function exported {
  bind msg := "msg" || "default"
  rules {
    !gen = |> echo \$(msg) |> out.txt
  }
  : |> !gen |>
}
HERE

parse
tup_object_exist . 'echo one'

cat > pkg/helpers.tup << HERE
function exported {
  bind msg := "msg" || "default"
  rules {
    !gen = |> echo changed-\$(msg) |> out.txt
  }
  : |> !gen |>
}
HERE

parse
tup_object_no_exist . 'echo one'
tup_object_exist . 'echo changed-one'

eotup
