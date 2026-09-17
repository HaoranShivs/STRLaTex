#!/usr/bin/env bash
# Builds the PaperForge portable TeX Live runtime (plan §33).
#
# The runtime is installed in user mode (no root) into the repository so the
# application can provide its own fixed TeX environment instead of depending
# on the user's PATH, system TeX Live or MiKTeX (plan §3, §12).
#
# Usage:
#   tools/build-runtime/build_runtime.sh
#
# The package set is defined in packages.txt; the install profile keeps the
# runtime small instead of pulling in the full TeX Live distribution (plan §5).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../.." && pwd)"
RUNTIME_ROOT="${REPO_ROOT}/runtime"
TEXLIVE_ROOT="${RUNTIME_ROOT}/texlive"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# Pin the package list so a rebuild reproduces the same runtime (plan §34).
PACKAGES="${HERE}/packages.txt"
PROFILE="${HERE}/texlive.profile"
YEAR="${TEXLIVE_YEAR:-2026}"

# The TeX Live installer (~5MB). It is *not* committed, so fetch it when it is
# missing instead of failing with a bare tar error. Override with
# TEXLIVE_INSTALLER_TARBALL to reuse a local/offline copy, or
# TEXLIVE_INSTALLER_URL to pick a faster mirror.
INSTALLER_TARBALL="${TEXLIVE_INSTALLER_TARBALL:-/tmp/install-tl-unx.tar.gz}"
INSTALLER_URL="${TEXLIVE_INSTALLER_URL:-https://mirror.ctan.org/systems/texlive/tlnet/install-tl-unx.tar.gz}"

if [ ! -f "${INSTALLER_TARBALL}" ]; then
  echo "==> downloading installer: ${INSTALLER_URL}"
  if command -v curl >/dev/null 2>&1; then
    curl -fL --retry 3 --connect-timeout 20 -o "${INSTALLER_TARBALL}" "${INSTALLER_URL}"
  elif command -v wget >/dev/null 2>&1; then
    wget -O "${INSTALLER_TARBALL}" "${INSTALLER_URL}"
  else
    echo "need curl or wget to download ${INSTALLER_URL}" >&2
    exit 1
  fi
fi

echo "==> TeX Live ${YEAR} user-mode install into ${TEXLIVE_ROOT}"
mkdir -p "${TEXLIVE_ROOT}"

tar -xzf "${INSTALLER_TARBALL}" -C "${WORK}"
INSTALLER_DIR="$(find "${WORK}" -maxdepth 1 -type d -name 'install-tl-*' | head -1)"
echo "==> installer: ${INSTALLER_DIR}"

# install-tl supports env-var substitution in the profile only for the
# destination; every other path is expanded here from the profile template so
# the profile stays the single source of truth for the layout.
EXPANDED_PROFILE="${WORK}/texlive.profile"
sed -e "s|__TEXLIVE_ROOT__|${TEXLIVE_ROOT}|g" "${PROFILE}" \
  > "${EXPANDED_PROFILE}"

TEXLIVE_INSTALL_PREFIX="${TEXLIVE_ROOT}" \
  perl "${INSTALLER_DIR}/install-tl" --profile "${EXPANDED_PROFILE}" \
  --no-interaction > "${WORK}/install.log" 2>&1 || {
    tail -40 "${WORK}/install.log"
    exit 1
  }
echo "==> core install done"

# The named packages (plan §5) are added on top of the minimal scheme.
BIN_DIR="$(ls -d "${TEXLIVE_ROOT}/bin/"* 2>/dev/null | head -1)"
if [ -z "${BIN_DIR}" ]; then
  echo "no bin dir produced by install-tl" >&2
  exit 1
fi
while IFS= read -r package; do
  case "${package}" in ''|'#'*) continue ;; esac
  echo "==> tlmgr install ${package}"
  "${BIN_DIR}/tlmgr" --repository "https://mirror.ctan.org/systems/texlive/tlnet" \
    install "${package}" >> "${WORK}/tlmgr.log" 2>&1 || {
      echo "   (failed: ${package}; see tlmgr.log)"
    }
done < "${PACKAGES}"

echo "==> runtime built at ${TEXLIVE_ROOT}"
"${BIN_DIR}/latexmk" --version | head -1 || true
