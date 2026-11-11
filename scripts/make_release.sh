#!/usr/bin/env bash
# make_release.sh TAG: builds the install zip (scripts/make_package.sh) and publishes it as a GitHub release of this repository, the
# download that update.bat (install_photon.ps1 -Latest) fetches. Publishing is public: the repository owner runs this, after the
# build and a laptop check. Needs gh logged in with rights on the repository.
set -euo pipefail
TAG=${1:?usage: make_release.sh TAG (e.g. v0.1.0)}
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
scripts/make_package.sh
Z=$(ls -t dist/photon-*.zip | head -1)
gh release create "$TAG" "$Z" --title "Photon $TAG" --notes "Install: unzip, run install.bat. Update an existing install: update.bat. Uninstall: uninstall.bat. See README.md."
