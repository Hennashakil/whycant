# whycant

[![CI](https://github.com/Hennashakil/whycant/actions/workflows/ci.yml/badge.svg)](https://github.com/Hennashakil/whycant/actions/workflows/ci.yml)
[![Latest release](https://img.shields.io/github/v/release/Hennashakil/whycant)](https://github.com/Hennashakil/whycant/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

**Website and live demo:** [hennashakil.github.io/whycant](https://hennashakil.github.io/whycant/)

**"Permission denied" tells you *that* access failed. `whycant` tells you *where*, *why*, and the *safest fix*.**

`whycant` is a small Linux command-line tool written in C. It walks a path one folder at a time, the same way the Linux kernel does, applies the kernel's permission rules at every step, and stops at the first one that blocks access. Then it explains the reason in plain English and suggests the narrowest fix — never `chmod 777`.

```
$ whycant -u bob team/f
Can bob (uid 30033) read team/f?

  [ ok ]  drwxr-xr-x  root:root   /srv/wtest       pass through needs x: as other you get r-x
  [DENY]  drwxr-x---  root:devs   /srv/wtest/team  pass through needs x: as other you get ---

  Why: bob is not the owner and not in group devs,
       so the 'other' bits apply, and they lack x.

  Narrowest fix first:
    1) setfacl -m u:bob:x /srv/wtest/team
       # only this user; needs the acl package
    2) usermod -aG devs bob
       # every member of the group; takes effect at next login
    3) chmod o+x /srv/wtest/team
       # every user on the system -- use only for public content
  Avoid chmod 777 and chmod -R: they open far more than this one step.

  => Denied.
```

## Install

Needs Linux and a C compiler (`sudo apt install gcc make` on Ubuntu/Debian).

```bash
git clone https://github.com/Hennashakil/whycant.git
cd whycant
make
sudo make install        # puts whycant in /usr/local/bin
```

To remove it: `sudo make uninstall`

## Usage

```
whycant [OPTION]... [PATH]...
```

Both are optional. With no `PATH`, `whycant` checks the folder you are in.

| Command | Question it answers |
|---|---|
| `whycant` | Can I read the folder I am in? |
| `whycant /etc/hosts` | Can I read this file? |
| `whycant -w notes.txt` | Can I edit it? |
| `whycant -x ./backup.sh` | Can I run it? |
| `whycant -c /opt/myapp` | Can I create it (mkdir, touch)? |
| `sudo whycant -u www-data /var/www/html` | Can another user access it? |

**Exit codes:** `0` allowed · `1` denied · `2` error — so it works in scripts and CI.

## Safety guards

`whycant` only reads; it never changes a file. On top of that:

- **Protected files.** Password, key and admin files (`/etc/shadow`, `/etc/sudoers`, SSH keys, `/root`, ...) get a **Stop** instead of a fix.
- **Root-level groups.** It never suggests joining `docker`, `disk`, `sudo`, `wheel`, `lxd`, `libvirt`, `kmem` or `shadow`, because membership in these is as good as root. Raw disks are blocked too.
- **Root only when needed.** It runs without `sudo`. If it truly cannot see further, it prints the exact `sudo` command to run — and under `sudo` it still checks for *you*, not for root (use `-u root` to check root itself).

## How it works

For each folder on the way, the user needs **x** (permission to pass through). At the destination, they need exactly what was asked: **r**, **w** or **x**. At every step, Linux asks four questions and **the first match wins**:

1. Is the user root? → allowed (except running a file with no x bit)
2. Does the user own it? → use the **owner** bits only
3. Is the user in its group? → use the **group** bits only
4. Otherwise → use the **other** bits

Rule 2 surprises many people: if you own a file but the owner bits are empty, Linux never falls back to the group or other bits.

Also handled: symlinks (up to 40, like the kernel), `.` and `..`, trailing slashes, read-only and `noexec` mounts, and file names containing quotes or newlines.

## Testing

Every push runs three checks on GitHub Actions:

| Check | What it proves |
|---|---|
| Build with gcc and clang, warnings as errors | The code compiles cleanly on both compilers |
| Kernel comparison (`tests/kernel_check.sh`) | For 5 users × 42 paths × read/write/run, plus create, whycant's answer matches the kernel's own decision: symlinks, loops, `..`, read-only and `noexec` mounts, odd file names |
| The same comparison under AddressSanitizer and UBSan | No memory errors or undefined behaviour along the way |

Run them yourself:

```bash
make test                 # quick checks, no root needed
sudo make kernel-test     # full kernel comparison (creates and removes test users)
```

## Limitations

- POSIX ACL entries are detected and warned about, but not fully evaluated
- SELinux, AppArmor and NFS rules such as `root_squash` are not modelled
- Linux only

## Background

Built while studying the source of GNU coreutils `mkdir.c`, following the same habits: small single-purpose functions, every system call checked, safe output for odd file names, and write errors reported instead of ignored.

## License

[MIT](LICENSE)
