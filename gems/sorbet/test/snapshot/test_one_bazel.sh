#!/bin/bash

# Runs a single srb init test from gems/sorbet/test/snapshot/{partial,total}/*

# --- begin runfiles.bash initialization ---
# Copy-pasted from Bazel's Bash runfiles library https://github.com/bazelbuild/bazel/blob/defd737761be2b154908646121de47c30434ed51/tools/bash/runfiles/runfiles.bash
set -euo pipefail
if [[ ! -d "${RUNFILES_DIR:-/dev/null}" && ! -f "${RUNFILES_MANIFEST_FILE:-/dev/null}" ]]; then
  if [[ -f "$0.runfiles_manifest" ]]; then
    export RUNFILES_MANIFEST_FILE="$0.runfiles_manifest"
  elif [[ -f "$0.runfiles/MANIFEST" ]]; then
    export RUNFILES_MANIFEST_FILE="$0.runfiles/MANIFEST"
  elif [[ -f "$0.runfiles/bazel_tools/tools/bash/runfiles/runfiles.bash" ]]; then
    export RUNFILES_DIR="$0.runfiles"
  fi
fi
if [[ -f "${RUNFILES_DIR:-/dev/null}/bazel_tools/tools/bash/runfiles/runfiles.bash" ]]; then
  # shellcheck disable=SC1090
  source "${RUNFILES_DIR}/bazel_tools/tools/bash/runfiles/runfiles.bash"
elif [[ -f "${RUNFILES_MANIFEST_FILE:-/dev/null}" ]]; then
  # shellcheck disable=SC1090
  source "$(grep -m1 "^bazel_tools/tools/bash/runfiles/runfiles.bash " \
            "$RUNFILES_MANIFEST_FILE" | cut -d ' ' -f 2-)"
else
  echo >&2 "ERROR: cannot find @bazel_tools//tools/bash/runfiles:runfiles.bash"
  exit 1
fi
# --- end runfiles.bash initialization ---

# The name of the ruby bazel package to use
ruby_package=$1

# Add ruby to the path
PATH="$(dirname "$(rlocation "${ruby_package}/ruby")"):$PATH"

# Add bundler to the path
BUNDLER_LOC=$(dirname "$(rlocation "gems/bundler/bundle")")
GEMS_LOC="$BUNDLER_LOC/../gems"
PATH="$BUNDLER_LOC:$PATH"

export PATH

repo_root="$PWD"

# The test to run
test_root="${repo_root}/gems/sorbet/test/snapshot/$2"
test_src="${test_root}/src"

# Setup sorbet
SRB_SORBET_EXE="$PWD/main/sorbet"

# Setup the run environment
(
  echo "test_src: ${test_src}"
  cd $test_src

  HOME=$test_src
  export HOME

  # Setup the vendor/cache directory to include all gems required for any test
  mkdir vendor
  ln -s "$GEMS_LOC" "vendor/cache"

  # https://bundler.io/v2.0/man/bundle-install.1.html#DEPLOYMENT-MODE
  # Passing --local to never consult rubygems.org
  bundle install --deployment --local
)
