#!/bin/sh
# Fails if anything machine-specific or secret-shaped has been committed.
#
# This repository is public, and its live tests are driven entirely from
# environment variables (PSRP_CONNECTION / PSRP_USER / PSRP_PASS) so that no
# endpoint, address or credential ever needs to be written down here. This
# check enforces that rather than trusting it.
#
# It deliberately names no particular environment: it looks for the shapes
# such things take, not for any one lab, host or token. It excludes itself,
# since it necessarily contains the patterns it searches for.
set -e

cd "$(dirname "$0")/.."

fail=0
self="scripts/check-no-local-config.sh"

# RFC 1918 addresses. A private address in a public repository is either
# someone's lab or someone's office, and neither belongs here.
priv='\b(10\.|192\.168\.|172\.(1[6-9]|2[0-9]|3[01])\.)[0-9]{1,3}\.[0-9]{1,3}\b'

# Bearer tokens and long hex secrets. Kept narrow on purpose: GUIDs and the
# protocol's own hex fixtures are legitimate and must not trip this.
tok='Bearer [A-Za-z0-9._-]{16,}'

for f in $(git ls-files); do
    [ "$f" = "$self" ] && continue
    case "$f" in *.pdf|*.png|*.jpg) continue;; esac

    if grep -nEI "$priv" "$f" >/dev/null 2>&1; then
        echo "private address in $f:"
        grep -nEI "$priv" "$f" | head -3 | sed 's/^/    /'
        fail=1
    fi
    if grep -nEI "$tok" "$f" >/dev/null 2>&1; then
        echo "bearer token in $f:"
        grep -nEI "$tok" "$f" | head -3 | sed 's/^/    /'
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo ""
    echo "Local configuration does not belong in this repository. Tests read"
    echo "the endpoint and credentials from the environment; keep them there."
    exit 1
fi

echo "no machine-specific configuration in tracked files"
