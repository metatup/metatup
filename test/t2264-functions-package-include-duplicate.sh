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
call "./pkg/Tupfile" exported({})
HERE
cat > pkg/Tupfile << HERE
include ./one.tup
include ./two.tup
HERE
cat > pkg/one.tup << HERE
function exported {
  rules {
    !one = |> echo one |> out.txt
  }
  : |> !one |>
}
HERE
cat > pkg/two.tup << HERE
function exported {
  rules {
    !two = |> echo two |> out.txt
  }
  : |> !two |>
}
HERE

parse_fail_msg "Duplicate function 'exported'"

eotup
