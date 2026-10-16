#!/bin/sh
# Build whycant_<version>_<arch>.deb from the hardened binary.
# Usage: packaging/build-deb.sh        (run from the repository root)
set -eu

cd "$(dirname "$0")/.."

version=$(sed -n 's/^#define PROGRAM_VERSION "\(.*\)"$/\1/p' whycant.c)
arch=$(dpkg --print-architecture)
[ -n "$version" ] || { echo "build-deb: cannot find PROGRAM_VERSION" >&2; exit 1; }

make release

# The oldest C library this binary can run on, read from its own symbols.
glibc=$(objdump -T whycant | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -V | tail -n 1)

pkg=$(mktemp -d)
trap 'rm -rf "$pkg"' EXIT
root="$pkg/whycant_${version}_${arch}"

install -D -m 755 whycant "$root/usr/bin/whycant"
install -d "$root/usr/share/man/man1" "$root/usr/share/doc/whycant" "$root/DEBIAN"
gzip -9n < whycant.1 > "$root/usr/share/man/man1/whycant.1.gz"
chmod 644 "$root/usr/share/man/man1/whycant.1.gz"

{
  echo "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/"
  echo "Upstream-Name: whycant"
  echo "Source: https://github.com/Hennashakil/whycant"
  echo
  echo "Files: *"
  echo "Copyright: $(sed -n 's/^Copyright (c) //p' LICENSE)"
  echo "License: MIT"
  sed 's/^$/./; s/^/ /' LICENSE
} > "$root/usr/share/doc/whycant/copyright"
chmod 644 "$root/usr/share/doc/whycant/copyright"

size=$(du -sk "$root/usr" | cut -f1)
cat > "$root/DEBIAN/control" <<EOF
Package: whycant
Version: $version
Architecture: $arch
Maintainer: Henna Shakil <Hennashakil@users.noreply.github.com>
Installed-Size: $size
Depends: libc6 (>= $glibc)
Recommends: acl
Section: utils
Priority: optional
Homepage: https://hennashakil.github.io/whycant/
Description: explain why a user can or cannot access a path
 "Permission denied" says that access failed, but not where or why.
 whycant walks a path one folder at a time, the way the Linux kernel does,
 applies the kernel's permission rules (including POSIX ACLs) at every step,
 and stops at the first one that blocks access. It explains the reason in
 plain words and suggests the narrowest fix, never chmod 777.
EOF

dpkg-deb --root-owner-group -Zxz --build "$root" . >/dev/null
echo "built whycant_${version}_${arch}.deb (needs libc6 >= $glibc)"
