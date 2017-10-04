#!/bin/bash

set -e

cd $(dirname $0)/../..

REPO=https://github.com/bazelbuild/buildifier
SHA=dd416ef7e19824ad568468fb7cf9f2e9d49c3e8f

if [ ! -x "$(which buildifier)" ]; then

  if [[ ! -x "$(which go)" ]]; then
    if [[ ! -x "$(which brew)" ]]; then
        echo -ne "\033[0;31m"
        echo "You need to install golang."
        echo -ne "\033[0m"
        exit 1
    fi
    echo "Installing a golang for you."
    brew install golang
  fi

  if [[ -z "$GOPATH" ]]; then
    export GOPATH=~/golang
    export PATH=$GOPATH/bin:$PATH
    mkdir -p $GOPATH
  fi

  if [ ! -x "$(which buildifier)" ]; then
    go get -d -u github.com/bazelbuild/buildifier/buildifier
    pushd $GOPATH/src/github.com/bazelbuild/buildifier/
    git reset --hard $SHA
    git clean -fdx --quiet
    go generate github.com/bazelbuild/buildifier/build
    go install github.com/bazelbuild/buildifier/buildifier
    popd
  fi
fi
if [ "$1" == "-t" ]; then
  OUTPUT=$(find . -name BUILD -not -path "./bazel-*" | xargs buildifier -v -mode=check || :)
  if [ -n "$OUTPUT" ]; then
    echo -ne "\e[1;31m"
    echo "☢️☢️  Some bazel files need to be reformatted! ☢️☢️"
    echo -ne "\e[0m"
    echo -e "✨✨ Run \e[97;1;42m ./tools/scripts/format_build_files.sh\e[0m to fix them up.  ✨✨"
    exit 1
  fi
else
  exec find . -name BUILD -not -path "./bazel-*" | xargs buildifier -v -mode=fix
fi
