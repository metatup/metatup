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
call "@std//" _build_marker({
  "brdir": ".",
})

: |> echo generated > %o |> out.txt
HERE

update
check_exist out.txt _marker.txt .metatup/stdlib-hash .metatup/repos/std/Tupfile
grep -qx 'generated' out.txt
grep -qx 'stdlib' _marker.txt
echo stale > .metatup/repos/std/stale.txt

printf 'stale-stdlib-hash\n' > .metatup/stdlib-hash

update
check_exist out.txt _marker.txt .metatup/stdlib-hash .metatup/repos/std/Tupfile
check_not_exist .metatup/repos/std/stale.txt
if grep -qx 'stale-stdlib-hash' .metatup/stdlib-hash; then
	echo "*** stdlib hash was not refreshed after invalidation" 1>&2
	exit 1
fi

eotup
