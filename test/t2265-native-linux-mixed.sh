#! /bin/sh -e
# tup - A file-based build system
#
# Copyright (C) 2026  handicraftsman
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.

. ./tup.sh

touch a.c b.cpp
cat > Tupfile << HERE
call "@std//native" binary({
  "native/binary/name": "app",
  "native/binary/c/sources": "a.c",
  "native/binary/cxx/sources": "b.cpp",
  "toolchain/os": "linux",
  "toolchain/family": "gcc",
  "toolchain/cc": "gcc",
  "toolchain/cxx": "clang++",
  "toolchain/linker": "clang++",
  "flags/common": "-Iinclude",
  "flags/c": "-std=c11",
  "flags/cxx": "-std=c++20",
  "flags/ld_exe": "-pthread"
})
HERE

parse
tup_object_exist . 'gcc -Iinclude -std=c11 -c a.c -o ./app.objs/c/a.o'
tup_object_exist . 'clang++ -Iinclude -std=c++20 -c b.cpp -o ./app.objs/cxx/b.o'
tup_object_exist . 'clang++ -Iinclude -pthread ./app.objs/c/a.o ./app.objs/cxx/b.o -o ./app'

eotup
