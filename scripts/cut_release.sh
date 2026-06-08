#!/usr/bin/env bash
#
# Cut a release. Validates the version, creates an annotated semver tag, and
# pushes it. Pushing the tag triggers .github/workflows/release.yml, which
# builds the Axon .clap, gates on the unit tests, and publishes a GitHub
# Release with the zipped bundle + SHA256SUMS.
#
#   scripts/cut_release.sh 1.2.3
#   scripts/cut_release.sh v1.2.3      # leading "v" optional
#
# Requirements: a clean working tree and a tag that doesn't already exist.
set -euo pipefail

die() { echo "error: $*" >&2; exit 1; }

ver="${1:-}"
[ -n "$ver" ] || die "usage: $(basename "$0") <version>   (e.g. 1.2.3)"
ver="${ver#v}" # tolerate a leading v

# Semver MAJOR.MINOR.PATCH with an optional -prerelease suffix.
printf '%s' "$ver" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$' \
  || die "'$ver' is not valid semver (expected MAJOR.MINOR.PATCH[-pre])"
tag="v$ver"

git rev-parse --is-inside-work-tree >/dev/null 2>&1 || die "not inside a git repo"
[ -z "$(git status --porcelain)" ] || die "working tree is dirty — commit or stash first"
git rev-parse -q --verify "refs/tags/$tag" >/dev/null \
  && die "tag $tag already exists"

branch="$(git rev-parse --abbrev-ref HEAD)"
sha="$(git rev-parse --short HEAD)"
remote="$(git config --get branch."$branch".remote || echo origin)"

echo "About to cut release:"
echo "  tag:    $tag"
echo "  ref:    $branch @ $sha"
echo "  remote: $remote"
printf 'Proceed? [y/N] '
read -r reply
case "$reply" in
  [yY] | [yY][eE][sS]) : ;;
  *) die "aborted" ;;
esac

git tag -a "$tag" -m "Axon $tag"
git push "$remote" "$tag"

echo
echo "Pushed $tag. The release workflow is now building Axon.clap:"
echo "  https://github.com/stevemurr/axon/actions/workflows/release.yml"
