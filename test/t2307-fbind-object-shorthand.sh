#! /bin/sh -e
# tup - A file-based build system
#
# Copyright (C) 2026  handicraftsman
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.

. ./tup.sh

touch main.c
cat > Tupfile << 'HERE'
function base {
  return {
    appending "flags/common": "-Wall",
    overriding "toolchain/cc": "cc",
    overriding "toolchain/cxx": "c++"
  }
}

function wrap {
  fbind inner := call base({})
  return {
    ...inner,
    appending "flags/common": "-Wextra"
  }
}

fbind vals := call wrap({})

call "@std//native" binary({
  ...vals,
  "native/binary/name": "app",
  "native/binary/c/sources": "main.c"
})
HERE

parse
tup_object_exist . 'cc -Wall -Wextra  -c main.c -o ./app.objs/c/main.o'

eotup
