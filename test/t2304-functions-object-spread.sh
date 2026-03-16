#! /bin/sh -e
# tup - A file-based build system
#
# Copyright (C) 2026  handicraftsman
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.

. ./tup.sh

touch main.c top.c
cat > Tupfile << 'HERE'
function base {
  return {
    overriding "flags/common": "-Winvalid-pch",
    overriding "flags/ld_exe": "-pthread"
  }
}

function pkg {
  return {
    appending "flags/common": "-Ipkg/include",
    appending "flags/ld_exe": "-Lpkg/lib -lpkg"
  }
}

function combined {
  fbind { }: base_vals := call base({
    ...,
  })
  fbind {
    appending "flags/common": pkg_common,
    appending "flags/ld_exe": pkg_ld
  }: pkg_vals := call pkg({
    ...,
  })

  return {
    ...base_vals,
    ...pkg_vals,
    appending "flags/common": "-Wall"
  }
}

function app {
  fbind { }: vals := call combined({
    ...,
  })

  call "@std//native" binary({
    ...,
    ...vals,
    "native/binary/name": "app",
    "native/binary/c/sources": "main.c"
  })
}

call app({})

fbind { }: top_vals := call combined({})

call "@std//native" binary({
  ...top_vals,
  "native/binary/name": "topapp",
  "native/binary/c/sources": "top.c"
})
HERE

parse
tup_object_exist . 'cc -Winvalid-pch -Ipkg/include -Wall  -c main.c -o ./app.objs/c/main.o'
tup_object_exist . 'c++ -Winvalid-pch -Ipkg/include -Wall -pthread -Lpkg/lib -lpkg ./app.objs/c/main.o -o ./app'
tup_object_exist . 'cc -Winvalid-pch -Ipkg/include -Wall  -c top.c -o ./topapp.objs/c/top.o'
tup_object_exist . 'c++ -Winvalid-pch -Ipkg/include -Wall -pthread -Lpkg/lib -lpkg ./topapp.objs/c/top.o -o ./topapp'

eotup
