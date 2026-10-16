#!/usr/bin/env python3
"""Check the website's permission engine (docs/engine.js) against the Linux kernel.

Builds random folder paths with random owners, groups and modes, asks the
kernel whether test users may read, write or run the last one (test -r/-w/-x
as that user), then has engine_compare.js check that the engine agrees.

Needs root (it creates test users and folders) and node. Everything it creates
is removed at the end.  Usage:  sudo python3 tests/site/engine_check.py [CASES]
"""
import json
import os
import pwd
import random
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = "/srv/whycant-engine-test"
USERS = {"wce_alice": ["wce_devs"], "wce_bob": [], "wce_carol": ["wce_devs", "wce_web"]}
GROUPS = ["wce_devs", "wce_web"]
OWNERS = ["root", "wce_alice", "wce_bob", "wce_carol"]
FILE_GROUPS = ["root", "wce_devs", "wce_web", "wce_alice", "wce_bob"]


def run(*cmd):
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)


def setup():
    for g in GROUPS:
        run("groupadd", "-f", g)
    for u, extra in USERS.items():
        if subprocess.run(["id", u], capture_output=True).returncode != 0:
            run("useradd", "-M", "-U", "-s", "/usr/sbin/nologin", u)
        run("usermod", "-G", ",".join(extra), u)


def cleanup():
    shutil.rmtree(BASE, ignore_errors=True)
    for u in USERS:
        subprocess.run(["userdel", u], capture_output=True)
    for g in GROUPS + list(USERS):
        subprocess.run(["groupdel", g], capture_output=True)


def groups_of(user):
    if user == "root":
        return ["root"]
    return [user] + USERS[user]


def main():
    if os.geteuid() != 0:
        sys.exit("engine_check: run as root (it creates test users)")
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 2000
    rnd = random.Random(2026)
    try:
        setup()
        cases = []
        for _ in range(n):
            rows = [["", "755", "root", "root"]]
            for d in range(rnd.randint(1, 4)):
                mode = "".join(str(rnd.choice([0, 1, 4, 5, 6, 7])) for _ in range(3))
                rows.append([f"d{d}", mode, rnd.choice(OWNERS), rnd.choice(FILE_GROUPS)])
            user = rnd.choice(["root"] + list(USERS))
            want = rnd.choice("rwx")

            # Build it under BASE; /srv and BASE are open (755 root), like "/" in the engine
            shutil.rmtree(BASE, ignore_errors=True)
            os.makedirs(BASE)
            os.chmod(BASE, 0o755)
            path = BASE
            for i, (name, mode, owner, group) in enumerate(rows[1:], 1):
                path += "/" + name
                if i == len(rows) - 1:
                    open(path, "w").close()
                else:
                    os.mkdir(path)
                shutil.chown(path, owner, group)
                os.chmod(path, int(mode, 8))

            as_user = [] if user == "root" else [
                "setpriv", f"--reuid={user}", f"--regid={pwd.getpwnam(user).pw_gid}", "--init-groups"]
            kernel = subprocess.run(as_user + ["/usr/bin/test", "-" + want, path]).returncode == 0
            cases.append({"rows": rows, "user": user, "groups": groups_of(user), "want": want, "kernel": kernel})

        result = subprocess.run(["node", os.path.join(HERE, "engine_compare.js")],
                                input=json.dumps(cases), text=True)
        allowed = sum(c["kernel"] for c in cases)
        print(f"({allowed} of {len(cases)} cases were allowed by the kernel)")
        sys.exit(result.returncode)
    finally:
        cleanup()


if __name__ == "__main__":
    main()
