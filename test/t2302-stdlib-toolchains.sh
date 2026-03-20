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
fbind { }: gcc_vals := call "@std//toolchains" gcc({
  "toolchain/os": "linux"
})

fbind { }: clang_vals := call "@std//toolchains" clang({
  "toolchain/os": "macos"
})

fbind { }: msvc_vals := call "@std//toolchains" msvc({ ..., })

fbind { }: host_vals := call "@std//toolchains" host({ ..., })

fbind { }: posix_pkg := call "@std//toolchains" package_flags({
  "package/include_dir": "/deps/include",
  "package/lib_dir": "/deps/lib",
  "package/posix_link_flags": "-lsqlite3"
})

fbind { }: msvc_pkg := call "@std//toolchains" package_flags({
  "toolchain/family": "msvc",
  "package/include_dir": "C:/deps/include",
  "package/lib_dir": "C:/deps/lib",
  "package/msvc_link_flags": "sqlite3.lib"
})

call "@std//native" binary({
  ...gcc_vals,
  ...posix_pkg,
  "native/binary/name": "gccapp",
  "native/binary/c/sources": "gcc.c"
})

call "@std//native" binary({
  ...clang_vals,
  ...posix_pkg,
  "native/binary/name": "clangapp",
  "native/binary/c/sources": "clang.c"
})

call "@std//native" binary({
  ...msvc_vals,
  ...msvc_pkg,
  "native/binary/name": "msvcapp",
  "native/binary/c/sources": "msvc.c"
})

call "@std//native" binary({
  "native/binary/name": "hostapp",
  "native/binary/c/sources": "host.c"
})
HERE

touch gcc.c clang.c msvc.c host.c
parse
tup_object_exist . 'gcc  -I/deps/include -c gcc.c -o ./gccapp.objs/c/gcc.o'
tup_object_exist . 'g++  -L/deps/lib -lsqlite3 ./gccapp.objs/c/gcc.o -o ./gccapp'
tup_object_exist . 'clang  -I/deps/include -c clang.c -o ./clangapp.objs/c/clang.o'
tup_object_exist . 'clang++  -L/deps/lib -lsqlite3 ./clangapp.objs/c/clang.o -o ./clangapp'
tup_object_exist . 'cl /nologo  /IC:/deps/include /c msvc.c /Fo./msvcapp.objs/c/msvc.obj'
tup_object_exist . 'link /nologo /LIBPATH:C:/deps/lib sqlite3.lib /OUT:./msvcapp.exe ./msvcapp.objs/c/msvc.obj'

if uname -s | grep Linux > /dev/null; then
	tup_object_exist . 'gcc   -c host.c -o ./hostapp.objs/c/host.o'
	tup_object_exist . 'g++   ./hostapp.objs/c/host.o -o ./hostapp'
fi

eotup
