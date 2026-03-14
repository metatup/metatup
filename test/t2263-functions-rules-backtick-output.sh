#! /bin/sh -e
# tup - A file-based build system
#
# Copyright (C) 2026  handicraftsman
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.

. ./tup.sh
cat > Tupfile << 'HERE'
function gen {
  rules {
    !gen = |> touch %o |> `printf 'out.txt'`
  }
  : |> !gen |>
}

call gen({})
HERE

parse
tup_object_exist . 'touch out.txt'
tup_object_exist . out.txt

eotup
