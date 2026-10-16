#!/bin/bash
# Compare whycant's verdict with the Linux kernel's own decision.
#
# Builds a small tree of folders and files with tricky permissions (owner
# lockout, groups, symlinks, loops, read-only and noexec mounts, odd names),
# then for every user x path x access asks the kernel (test -r/-w/-x, mkdir)
# while running as that user, and checks that whycant gives the same answer.
#
# Needs root, because it creates test users, groups and mounts.
# Usage: sudo tests/kernel_check.sh [path/to/whycant]

set -u
W=$(readlink -f "${1:-./whycant}")
T=/srv/whycant-test

if [ "$(id -u)" -ne 0 ]; then echo "run as root: sudo $0" >&2; exit 2; fi
[ -x "$W" ] || { echo "whycant binary not found: $W (run make first)" >&2; exit 2; }

cleanup() {
  umount "$T/ro_mnt" "$T/noexec_mnt" 2>/dev/null
  rm -rf "$T"
  for u in wc_alice wc_bob wc_carol; do userdel "$u" 2>/dev/null; done
  groupdel wc_devs 2>/dev/null
}
trap cleanup EXIT
cleanup

# --- users: alice and carol are in wc_devs, bob is not
groupadd wc_devs
for u in wc_alice wc_bob wc_carol; do useradd -M -s /bin/sh "$u"; done
usermod -aG wc_devs wc_alice
usermod -aG wc_devs wc_carol

# --- the tree
mkdir -p "$T"; chmod 755 /srv "$T"; cd "$T" || exit 2
mkdir pub; echo hi > pub/f; chmod 755 pub; chmod 644 pub/f
mkdir team; echo s > team/f; chown root:wc_devs team team/f; chmod 750 team; chmod 640 team/f
mkdir ownzero; chown wc_alice:wc_devs ownzero; chmod 0070 ownzero; touch ownzero/f; chmod 644 ownzero/f
mkdir -p deep/a/b; chmod 711 deep; chmod 700 deep/a; chmod 755 deep/a/b; echo x > deep/a/b/f
printf '#!/bin/sh\necho ok\n' > s644; chmod 644 s644
printf '#!/bin/sh\necho ok\n' > s744; chmod 744 s744
echo r > ro444; chmod 444 ro444
mkdir writable; chmod 777 writable
mkdir sticky; chmod 1777 sticky
mkdir alicedir; chown wc_alice:wc_alice alicedir; chmod 755 alicedir
ln -s team/f link_team
ln -s "$T/pub/f" link_abs
ln -s ../whycant-test/pub link_rel_dir
ln -s loopA loopB; ln -s loopB loopA
ln -s nothere dangling
ln -s deep/a/b link_deep
mkdir "we'ird
name"; chmod 700 "we'ird
name"
mkdir ro_mnt noexec_mnt
mount -t tmpfs -o size=1m tmpfs ro_mnt && printf '#!/bin/sh\n' > ro_mnt/s && chmod 755 ro_mnt/s && mount -o remount,ro ro_mnt
mount -t tmpfs -o size=1m,noexec tmpfs noexec_mnt && printf '#!/bin/sh\n' > noexec_mnt/s && chmod 755 noexec_mnt/s

NL=$'\n'
paths=(
  / /srv "$T" "$T/pub" "$T/pub/f" "$T/team" "$T/team/f" "$T/ownzero" "$T/ownzero/f"
  "$T/deep" "$T/deep/a" "$T/deep/a/b/f" "$T/link_deep/f" "$T/link_deep/../b/f"
  "$T/link_team" "$T/link_abs" "$T/link_rel_dir/f" "$T/loopA" "$T/dangling"
  "$T/nothere" "$T/nothere/x" "$T/pub/f/" "$T/pub/f/x" "$T/pub/" "$T/pub/./f"
  "$T/pub/../team/f" "$T/s644" "$T/s744" "$T/ro444" "$T/writable" "$T/sticky"
  "$T/alicedir" "$T/ro_mnt" "$T/ro_mnt/s" "$T/noexec_mnt/s" "$T/we'ird${NL}name"
  "$T//pub///f" pub/f team/f ../whycant-test/team/f . ..
)
creates=(
  "$T/pub/new" "$T/writable/new" "$T/sticky/new" "$T/team/new" "$T/alicedir/new"
  "$T/ro_mnt/new" "$T/nothere/new" "$T/pub" "$T/pub/." "$T/dangling" "$T/deep/a/new"
)

pass=0; fail=0
for u in root wc_alice wc_bob wc_carol nobody; do
  if [ "$u" = root ]; then run=(); else
    run=(setpriv --reuid="$u" --regid="$(id -gn "$u")" --init-groups)
  fi
  for p in "${paths[@]}"; do
    for m in r w x; do
      (cd "$T" && "${run[@]}" /usr/bin/test "-$m" "$p"); k=$?
      (cd "$T" && "$W" -u "$u" "-$m" "$p" >/dev/null 2>&1); w=$?
      if [ "$k" -eq "$w" ]; then pass=$((pass+1)); else
        fail=$((fail+1)); printf 'MISMATCH user=%s access=%s path=%q kernel=%s whycant=%s\n' "$u" "$m" "$p" "$k" "$w"
      fi
    done
  done
  for p in "${creates[@]}"; do
    (cd "$T" && "${run[@]}" mkdir "$p" 2>/dev/null); k=$?; [ "$k" -eq 0 ] && rmdir "$p"
    "$W" -u "$u" -c "$p" >/dev/null 2>&1; w=$?
    if [ "$k" -eq "$w" ]; then pass=$((pass+1)); else
      fail=$((fail+1)); printf 'MISMATCH create user=%s path=%q kernel=%s whycant=%s\n' "$u" "$p" "$k" "$w"
    fi
  done
done

echo "kernel check: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
