#! /bin/sh -e
# tup - A file-based build system
#
# Copyright (C) 2026  handicraftsman
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.

. ./tup.sh

touch top.c app.c
cat > Tupfile << 'HERE'
function base {
  return {
    appending "flags/common": "-Wall",
    overriding "toolchain/cc": "cc",
    overriding "toolchain/cxx": "c++"
  }
}

function pkg {
  return {
    appending "flags/common": "-Wextra"
  }
}

function wrap {
  fbind foo := call base({})
  fbind bar := call pkg({})
  fbind merged := merge {
    ...foo,
    ...bar,
    appending "flags/common": "-Werror"
  }
  return {
    ...merged
  }
}

fbind foo := call base({})
fbind bar := call pkg({})
fbind vals := merge {
  ...foo,
  ...bar,
  appending "flags/common": "-Wpedantic"
}

fbind { "native/binary/name": out_name } := merge {
  overriding "native/binary/name": "bound"
}

call "@std//native" binary({
  ...vals,
  "native/binary/name": "top",
  "native/binary/c/sources": "top.c"
})

fbind nested := call wrap({})

call "@std//native" binary({
  ...nested,
  "native/binary/name": "app",
  "native/binary/c/sources": "app.c"
})

: |> touch %o |> $(out_name).txt
HERE

parse
tup_object_exist . 'cc -Wall -Wextra -Wpedantic  -c top.c -o ./top.objs/c/top.o'
tup_object_exist . 'cc -Wall -Wextra -Werror  -c app.c -o ./app.objs/c/app.o'
tup_object_exist . 'touch bound.txt'

eotup
