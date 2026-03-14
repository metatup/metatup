#! /bin/sh -e
# tup - A file-based build system
#
# Copyright (C) 2026 handicraftsman
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

# Test that compile_commands.json uses project-root-relative source paths
# even when generated for a variant build directory.

. ./tup.sh

mkdir build
mkdir sub

cat > Tupfile << HERE
: foreach *.c |> ^j^ gcc -c %f -o %o |> %B.o
HERE
cat > sub/Tupfile << HERE
: foreach *.c |> ^j^ gcc -c %f -o %o |> %B.o
HERE
echo "CONFIG_FOO=y" > build/tup.config
touch foo.c sub/bar.c

update
compiledb

check_exist build/compile_commands.json

if ! grep '"file": "foo\.c"' build/compile_commands.json > /dev/null; then
	echo "Error: Expected project-root-relative top-level file path." 1>&2
	exit 1
fi

if ! grep '"file": "sub/bar\.c"' build/compile_commands.json > /dev/null; then
	echo "Error: Expected project-root-relative subdir file path." 1>&2
	exit 1
fi

if grep '"file": "\.\./sub/bar\.c"' build/compile_commands.json > /dev/null; then
	echo "Error: File path should not be relative to the variant build directory." 1>&2
	exit 1
fi

eotup
