/* whycant's permission rules for the browser: the same ones whycant.c applies.
   Pure functions, no page access, so tests can run them against the real kernel
   (see tests/site/engine_check.py in the whycant repository). */
(function (root) {
  "use strict";

  var BIT = { r: 4, w: 2, x: 1 };

  // "750", "0750", "rwxr-x---" or "drwxr-x---" -> { bits: [7, 5, 0], dir: true | false | null }
  function parseMode(s) {
    s = (s || "").trim();
    if (/^[0-7]{3,4}$/.test(s)) {
      return { bits: s.slice(-3).split("").map(Number), dir: null };
    }
    var m = /^([-dlbcps])?([r-][w-][xsS-])([r-][w-][xsS-])([r-][w-][xtT-])$/.exec(s);
    if (!m) return null;
    var bits = [m[2], m[3], m[4]].map(function (t) {
      return (t[0] === "r" ? 4 : 0) | (t[1] === "w" ? 2 : 0) | (/[xst]/.test(t[2]) ? 1 : 0);
    });
    return { bits: bits, dir: m[1] ? m[1] === "d" : null };
  }

  function triplet(v) { return (v & 4 ? "r" : "-") + (v & 2 ? "w" : "-") + (v & 1 ? "x" : "-"); }
  function modeString(p, isDir) { return (isDir ? "d" : "-") + p.bits.map(triplet).join(""); }

  var EXACT = ["/etc/shadow", "/etc/shadow-", "/etc/gshadow", "/etc/gshadow-", "/etc/sudoers"];
  var TREES = ["/etc/sudoers.d", "/etc/ssl/private", "/root"];
  function isSensitive(path) {
    if (EXACT.indexOf(path) !== -1) return true;
    for (var i = 0; i < TREES.length; i++) {
      var t = TREES[i];
      if (path === t || path.indexOf(t + "/") === 0) return true;
    }
    return path.indexOf("/.ssh/") !== -1 || /\/\.ssh$/.test(path) || /^\/etc\/ssh\/ssh_host_.*_key$/.test(path);
  }

  // Groups whose members are as good as root: never suggest joining them
  var PRIVILEGED = ["sudo", "wheel", "admin", "docker", "lxd", "libvirt", "disk", "kmem", "shadow"];

  // Walk rows ([{ name, mode, owner, group }], the first one is "/") the way the
  // kernel does: x on every folder, then `want` (r, w or x) on the last row.
  // Stops at the first step that fails.
  function walk(user, groups, want, rows) {
    var steps = [], last = rows.length - 1, path = "";
    for (var i = 0; i < rows.length; i++) {
      var row = rows[i], p = parseMode(row.mode);
      path = i === 0 ? "/" : (path === "/" ? "/" : path + "/") + row.name;
      var isLast = i === last, isDir = !isLast ? true : (p.dir === null ? false : p.dir);
      var need = isLast ? want : "x", cls, have;
      if (user === "root") { cls = "root"; have = 6 | ((isDir || (p.bits[0] | p.bits[1] | p.bits[2]) & 1) ? 1 : 0); }
      else if (row.owner === user) { cls = "owner"; have = p.bits[0]; }
      else if (groups.indexOf(row.group) !== -1) { cls = "group"; have = p.bits[1]; }
      else { cls = "other"; have = p.bits[2]; }
      var ok = (have & BIT[need]) !== 0;
      steps.push({ row: row, p: p, path: path, need: need, cls: cls, have: have, ok: ok, isDir: isDir });
      if (!ok) break;
    }
    return steps;
  }

  function allowed(steps, rows) {
    return steps.length === rows.length && steps[steps.length - 1].ok;
  }

  // The narrowest fixes for a failed step, or a reason to stop
  function fixesFor(user, s) {
    var b = s.need, path = s.path, g = s.row.group;
    if (isSensitive(path)) return { stop: "This is closed on purpose: it holds passwords, keys or admin rules. Opening it would be a security hole, so whycant suggests no fix." };
    if (g !== "root" && PRIVILEGED.indexOf(g) !== -1)
      return { stop: "This is guarded by group " + g + ", and access to it is as good as root. whycant will not suggest joining it." };
    if (s.cls === "root") return { list: [["chmod u+x " + path, "even root needs one x bit to run a file"]] };
    if (s.cls === "owner") return { list: [["chmod u+" + b + " " + path, "the owner's own bits"]] };
    var list = [["setfacl -m u:" + user + ":" + b + " " + path, "only this user"]];
    if (s.cls === "group") list.push(["chmod g+" + b + " " + path, "every member of group " + g]);
    else {
      // A group named after its owner is that user's personal group: never suggest joining it
      if (g !== "root" && g !== s.row.owner) {
        var groupHas = (s.p.bits[1] & BIT[b]) !== 0;
        list.push(["usermod -aG " + g + " " + user + (groupHas ? "" : " && chmod g+" + b + " " + path), "every member of the group"]);
      }
      list.push(["chmod o+" + b + " " + path, "every user on the system"]);
    }
    return { list: list };
  }

  var api = {
    BIT: BIT, parseMode: parseMode, triplet: triplet, modeString: modeString,
    isSensitive: isSensitive, PRIVILEGED: PRIVILEGED, walk: walk, allowed: allowed, fixesFor: fixesFor
  };
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.WhyEngine = api;
})(this);
