#!/bin/sh
# Run by build-packages.sh inside the Ubuntu 22.04 container: the source at /src (read-only), the
# build in /out/build. ci/build-linux.sh installs what SDL needs, builds, and checks that the
# binary needs nothing beyond the C library.
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update -q >/dev/null
apt-get install -y -q --no-install-recommends build-essential cmake git ca-certificates binutils >/dev/null
# a second run finds the fetched sources owned by the host's user
git config --global --add safe.directory '*'
# upstream's script names upstream's binary and makes a build for this processor
sed -e 's#\$build/picocalc-sim"#$build/pbconpc"#' \
    -e "s#-DCMAKE_BUILD_TYPE=Release#-DCMAKE_BUILD_TYPE=Release -DPBCONPC_NATIVE=OFF -DPICOCALC_SIM_VERSION=$VER#" \
    /src/ci/build-linux.sh > /tmp/build-linux.sh
grep -q 'build/pbconpc"' /tmp/build-linux.sh && grep -q PBCONPC_NATIVE=OFF /tmp/build-linux.sh
sh /tmp/build-linux.sh /src /out/build
chown -R "$HOST_ID" /out
