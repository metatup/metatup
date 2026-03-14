#! /bin/sh -e
# tup - A file-based build system
#
# Copyright (C) 2026  handicraftsman
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.

. ./tup.sh

mkdir toolsrepo build

cat > MetaTupRepo.yaml << HERE
meta:
  name: root

dependencies:
  - name: tools
    path: ./toolsrepo
HERE

cat > Tupfile << HERE
function build_app {
  bind builddir := "builddir"
  bind tool := "tool"
  : \$(tool)/toolsrepo/tool.txt |> cp %f %o |> \$(builddir)/app.txt
}
HERE

cat > MetaTup.yaml << HERE
components:
  - name: app
    tupfile: ./Tupfile
    function: build_app
    dependencies:
      - name: "@tools//:tool"
HERE

cat > toolsrepo/Tupfile << HERE
function build_tool {
  bind brdir := "brdir"
  : |> echo tool > %o |> \$(brdir)/tool.txt
}
HERE

cat > toolsrepo/MetaTup.yaml << HERE
components:
  - name: tool
    tupfile: ./Tupfile
    function: build_tool
HERE

cd build
tup gen app
grep -q '^  - name: app$' TupBuild.yaml
grep -q '^    tupfile: \.\./\./Tupfile$' TupBuild.yaml
grep -q '^  - name: app__toolsrepo_tool$' TupBuild.yaml
grep -q '^    tupfile: \.\./toolsrepo/\./Tupfile$' TupBuild.yaml
cd ..

update
check_exist build/app.build/app.txt
check_exist build/app__toolsrepo_tool.build/toolsrepo/tool.txt
grep -qx "tool" build/app.build/app.txt

eotup
