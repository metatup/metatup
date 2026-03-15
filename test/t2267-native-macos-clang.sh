#! /bin/sh -e
# tup - A file-based build system
#
# Copyright (C) 2026  handicraftsman
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.

. ./tup.sh

touch foo.c bar.cpp
cat > Tupfile << HERE
call "@std//native" shared_library({
  "native/shared_library/name": "widget",
  "native/shared_library/c/sources": "foo.c",
  "native/shared_library/cxx/sources": "bar.cpp",
  "toolchain/os": "macos",
  "toolchain/family": "clang",
  "toolchain/cc": "clang",
  "toolchain/cxx": "clang++",
  "toolchain/linker": "clang++",
  "flags/common": "-isysroot /SDK",
  "flags/c": "-std=c11",
  "flags/cxx": "-std=c++20",
  "flags/ld_shared": "-dynamiclib"
})

call "@std//native" static_library({
  "native/static_library/name": "core",
  "native/static_library/cxx/sources": "bar.cpp",
  "toolchain/os": "macos",
  "toolchain/family": "clang",
  "toolchain/cxx": "clang++",
  "toolchain/ar": "ar",
  "flags/common": "-isysroot /SDK",
  "flags/cxx": "-std=c++20",
  "flags/ar": "-s"
})
HERE

parse
tup_object_exist . 'clang -isysroot /SDK -std=c11 -c foo.c -o ./widget.objs/c/foo.o'
tup_object_exist . 'clang++ -isysroot /SDK -std=c++20 -c bar.cpp -o ./widget.objs/cxx/bar.o'
tup_object_exist . 'clang++ -isysroot /SDK -dynamiclib ./widget.objs/c/foo.o ./widget.objs/cxx/bar.o -o ./libwidget.dylib'
tup_object_exist . 'clang++ -isysroot /SDK -std=c++20 -c bar.cpp -o ./core.objs/cxx/bar.o'
tup_object_exist . 'ar rcs -s ./libcore.a ./core.objs/cxx/bar.o'

eotup
