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
  "toolchain/os": "windows",
  "toolchain/family": "msvc",
  "toolchain/cc": "cl",
  "toolchain/cxx": "cl",
  "toolchain/linker": "link",
  "flags/common": "/Iinclude",
  "flags/c": "/TC",
  "flags/cxx": "/TP /std:c++20",
  "flags/ld_shared": "/INCREMENTAL:NO"
})

call "@std//native" static_library({
  "native/static_library/name": "core",
  "native/static_library/c/sources": "foo.c",
  "toolchain/os": "windows",
  "toolchain/family": "msvc",
  "toolchain/cc": "cl",
  "toolchain/ar": "lib",
  "flags/common": "/Iinclude",
  "flags/c": "/TC",
  "flags/ar": "/MACHINE:X64"
})
HERE

parse
tup_object_exist . 'cl /nologo /Iinclude /TC /c foo.c /Fo./widget.objs/c/foo.obj'
tup_object_exist . 'cl /nologo /Iinclude /TP /std:c++20 /c bar.cpp /Fo./widget.objs/cxx/bar.obj'
tup_object_exist . 'link /nologo /DLL /INCREMENTAL:NO /OUT:./widget.dll ./widget.objs/c/foo.obj ./widget.objs/cxx/bar.obj'
tup_object_exist . 'cl /nologo /Iinclude /TC /c foo.c /Fo./core.objs/c/foo.obj'
tup_object_exist . 'lib /nologo /MACHINE:X64 /OUT:./core.lib ./core.objs/c/foo.obj'

eotup
