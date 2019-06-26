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


# ----- Option parsing -----

# these positional arguments are supplied in snapshot.bzl
ruby_package=$1
test_name=$2

if [[ "test_name" =~ "partial/*" ]]; then
  is_partial=1
else
  is_partial=
fi

# ----- Environment setup -----

# Add ruby to the path
PATH="$(dirname "$(rlocation "${ruby_package}/ruby")"):$PATH"

# Put the bundler library into RUBYLIB
source $(rlocation "gems/bundler/bundle-env")

# Add bundler to the path
BUNDLER_LOC=$(dirname "$(rlocation "gems/bundler/bundle")")
GEMS_LOC="$BUNDLER_LOC/../gems"
PATH="$BUNDLER_LOC:$PATH"

export PATH

repo_root="$PWD"

test_root="${repo_root}/gems/sorbet/test/snapshot/$2"

srb="${repo_root}/gems/sorbet/bin/srb"

# Use the sorbet executable built by bazel
SRB_SORBET_EXE="$PWD/main/sorbet"

HOME=$test_root
export HOME

# ----- Run the test -----

(
  cd $test_root/src

  # Setup the vendor/cache directory to include all gems required for any test
  mkdir vendor
  ln -s "$GEMS_LOC" "vendor/cache"

  # https://bundler.io/v2.0/man/bundle-install.1.html#DEPLOYMENT-MODE
  # Passing --local to never consult rubygems.org
  bundle install --deployment --local

  bundle check

  bundle exec ruby -e 'puts $:'

  # run `srb init`
  SRB_YES=1 bundle exec "$srb" init \
    || find . -type f | xargs -L 1 -t bundle exec "$srb" tc --no-config --error-white-list 1000
)
