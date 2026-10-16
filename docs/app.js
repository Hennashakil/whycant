/* whycant website: the interactive parts. The permission rules live in engine.js. */
(function () {
  "use strict";

  var E = window.WhyEngine;
  var parseMode = E.parseMode, triplet = E.triplet, modeString = E.modeString, BIT = E.BIT,
      walk = E.walk, fixesFor = E.fixesFor;

  function esc(s) {
    return String(s).replace(/[&<>"']/g, function (c) {
      return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c];
    });
  }

  var VERB = { r: "read", w: "write to", x: "run" };
  var DOIT = { r: "read it", w: "change it", x: "run it" };

  // Who may do this, in plain words, for one folder or file
  function whoMay(row, p, bit) {
    var o = p.bits[0] & bit, g = p.bits[1] & bit, e = p.bits[2] & bit;
    var sameAsOwner = row.group === row.owner;
    if (e) {
      var except = [];
      if (!o) except.push(row.owner === "root" ? "root’s own bits" : row.owner);
      if (!g && !sameAsOwner) except.push("the " + row.group + " group");
      return except.length ? "Everyone except " + except.join(" and ") : "Everyone";
    }
    var list = [];
    if (o) list.push(row.owner);
    if (g && !(sameAsOwner && o)) list.push("the " + row.group + " group");
    if (!list.length) return "Nobody but root";
    return "Only " + list.join(" and ");
  }
  function plainRow(row, p, isLast, want) {
    if (!p) return "";
    var bit = isLast ? BIT[want] : 1;
    var verb = !isLast || (p.dir === true && want === "x") ? "can enter" : "can " + DOIT[want];
    return whoMay(row, p, bit) + " " + verb;
  }

  function renderPath(el, rows, steps, want, animate) {
    var html = "";
    want = want || "r";
    rows.forEach(function (row, i) {
      var st = steps[i], state = !st ? "unreached" : st.ok ? "ok" : "deny";
      var word = !st ? "not reached" : !st.ok ? "stops here" : i === rows.length - 1 ? "allowed" : "passes";
      var label = i === 0 ? "/" : row.name + (i < rows.length - 1 ? "/" : "");
      var p = parseMode(row.mode), isLast = i === rows.length - 1;
      var meta = (p ? modeString(p, !isLast || p.dir === true) : row.mode) + "  " + row.owner + ":" + row.group;
      html += '<li class="step ' + state + '" style="--depth:' + i + ";--i:" + i + '"><span class="name">' + esc(label) +
        '<span class="plain">' + esc(plainRow(row, p, isLast, want)) + "</span></span>" +
        '<span class="meta">' + esc(meta) + '</span><span class="state">' + word + "</span></li>";
    });
    el.classList.remove("walk");
    el.innerHTML = html;
    if (animate) { void el.offsetWidth; el.classList.add("walk"); }
  }

  function renderVerdict(el, user, want, rows, steps) {
    var lastStep = steps[steps.length - 1], d = "";
    var atTarget = steps.length === rows.length;
    var s = lastStep, owner = s.row.owner, group = s.row.group;
    var verb = !atTarget || (s.isDir && s.need === "x") ? "enter" : DOIT[s.need];
    if (s.ok && atTarget) {
      var how = s.cls === "root" ? "root can open almost anything." :
        s.cls === "owner" ? user + " owns it, and the owner may " + verb + "." :
        s.cls === "group" ? user + " is in the " + group + " group, and that group may " + verb + "." :
        user + " counts as “everyone else”, and everyone else may " + verb + ".";
      d += '<dt class="v-ok">Allowed</dt><dd>' + esc(user) + " can " + VERB[want] + " <code>" + esc(s.path) + "</code>. " + esc(how) + "</dd>";
      el.innerHTML = d;
      return;
    }
    var reason;
    if (s.cls === "root") reason = "Even root can only run a file that has at least one x (run) switch on.";
    else if (s.cls === "owner") reason = user + " owns it, but the owner’s own switches don’t allow this. Linux never gives the owner the group’s or everyone else’s rights instead.";
    else if (s.cls === "group") reason = user + " is in the " + group + " group, but that group may not " + verb + ". Group members never get the “everyone else” rights instead.";
    else reason = user + " is not the owner (" + owner + ")" + (group === owner ? "" : " and not in the " + group + " group") + ", so Linux treats them as “everyone else”, and everyone else may not " + verb + ".";
    d += '<dt class="v-deny">Blocked</dt><dd>' + esc(user) + " cannot " + (atTarget ? VERB[s.need] : "get past") + " <code>" + esc(s.path) + "</code></dd>";
    d += "<dt>Why</dt><dd>" + esc(reason) + " <small>(" + esc(user) + "’s switches here: " + triplet(s.have) + ")</small></dd>";
    var f = fixesFor(user, s);
    if (f.stop) d += '<dt class="v-stop">Stop</dt><dd>' + esc(f.stop) + "</dd>";
    else d += "<dt>Fix</dt><dd><ol>" + f.list.map(function (x) {
      return "<li><code>" + esc(x[0]) + '</code><button class="mini-copy" type="button" data-copy="' + esc(x[0]) + '">Copy</button> <small>' + esc(x[1]) + "</small></li>";
    }).join("") + "</ol><small>Pick the first one that works for you: it opens the least. Never use chmod 777.</small></dd>";
    el.innerHTML = d;
  }

  function command(user, want, rows) {
    var flag = want === "r" ? "" : " -" + want;
    var path = "/" + rows.slice(1).map(function (r) { return r.name; }).join("/");
    return "$ whycant -u " + user + flag + " " + path;
  }

  /* ---------- Hero demo ---------- */

  var demoRows = [
    { name: "",           mode: "drwxr-xr-x", owner: "root",  group: "root" },
    { name: "srv",        mode: "drwxr-xr-x", owner: "root",  group: "root" },
    { name: "demo",       mode: "drwxr-xr-x", owner: "root",  group: "root" },
    { name: "team",       mode: "drwxr-x---", owner: "root",  group: "devs" },
    { name: "report.txt", mode: "----r-----", owner: "alice", group: "devs" }
  ];
  var demoGroups = { bob: ["bob"], carol: ["carol", "devs"], alice: ["alice", "devs"], root: ["root"] };

  function renderDemo(user) {
    var steps = walk(user, demoGroups[user], "r", demoRows);
    document.getElementById("demo-cmd").textContent = command(user, "r", demoRows);
    renderPath(document.getElementById("demo-path"), demoRows, steps, "r", true);
    renderVerdict(document.getElementById("demo-verdict"), user, "r", demoRows, steps);
  }
  document.querySelectorAll('input[name="who"]').forEach(function (el) {
    el.addEventListener("change", function () { if (el.checked) renderDemo(el.value); });
  });
  renderDemo("bob");

  /* ---------- Playground ---------- */

  var PRESETS = {
    web: { u: "www-data", g: "www-data", a: "r", rows: [
      ["", "755", "root", "root"], ["var", "755", "root", "root"], ["www", "755", "root", "root"],
      ["html", "750", "henna", "henna"], ["index.html", "644", "henna", "henna"]] },
    owner: { u: "henna", g: "henna, devs", a: "r", rows: [
      ["", "755", "root", "root"], ["home", "755", "root", "root"], ["henna", "750", "henna", "henna"],
      ["notes.txt", "044", "henna", "devs"]] },
    docker: { u: "bob", g: "bob", a: "w", rows: [
      ["", "755", "root", "root"], ["run", "755", "root", "root"], ["docker.sock", "660", "root", "docker"]] },
    shadow: { u: "henna", g: "henna", a: "r", rows: [
      ["", "755", "root", "root"], ["etc", "755", "root", "root"], ["shadow", "640", "root", "shadow"]] }
  };

  var state;
  var userEl = document.getElementById("pg-user"), groupsEl = document.getElementById("pg-groups");
  var rowsEl = document.getElementById("pg-rows");

  function load(p) {
    state = { u: p.u, g: p.g, a: p.a, rows: p.rows.map(function (r) { return r.slice(); }) };
    userEl.value = state.u; groupsEl.value = state.g;
    document.querySelectorAll('input[name="pg-access"]').forEach(function (el) { el.checked = el.value === state.a; });
    drawRows();
    update();
  }

  function drawRows() {
    var html = "";
    state.rows.forEach(function (r, i) {
      var nameCell = i === 0
        ? '<span class="depth">/</span>'
        : '<input class="t" data-i="' + i + '" data-k="0" value="' + esc(r[0]) + '" aria-label="Name of folder ' + (i + 1) + '" autocomplete="off" spellcheck="false">';
      html += "<tr>" +
        '<td class="n" data-label="Name">' + nameCell + "</td>" +
        '<td class="m" data-label="Permission"><input class="t" data-i="' + i + '" data-k="1" value="' + esc(r[1]) + '" aria-label="Permission of ' + (i === 0 ? "/" : esc(r[0])) + '" autocomplete="off" spellcheck="false"></td>' +
        '<td class="o" data-label="Owner"><input class="t" data-i="' + i + '" data-k="2" value="' + esc(r[2]) + '" aria-label="Owner" autocomplete="off" spellcheck="false"></td>' +
        '<td class="g" data-label="Group"><input class="t" data-i="' + i + '" data-k="3" value="' + esc(r[3]) + '" aria-label="Group" autocomplete="off" spellcheck="false"></td>' +
        '<td class="x" data-label="">' + (i === 0 || state.rows.length <= 2 ? "" :
          '<button class="rm" type="button" data-rm="' + i + '" aria-label="Remove ' + esc(r[0] || "row") + '" title="Remove">×</button>') + "</td></tr>";
    });
    rowsEl.innerHTML = html;
  }

  function update() {
    var user = userEl.value.trim(), groups = groupsEl.value.split(",").map(function (s) { return s.trim(); }).filter(Boolean);
    var problems = [];
    if (!/^[a-z_][a-z0-9_.-]{0,31}$/i.test(user)) problems.push("User needs a name, like bob or www-data.");
    rowsEl.querySelectorAll("input").forEach(function (el) {
      var i = +el.dataset.i, k = +el.dataset.k, v = el.value.trim(), bad = false;
      if (k === 1) bad = !parseMode(v);
      else if (k === 0) bad = !v || /\//.test(v) || v === "." || v === "..";
      else bad = !v || /\s/.test(v);
      el.setAttribute("aria-invalid", bad ? "true" : "false");
      if (bad) problems.push(k === 1 ? "Permission “" + v + "” isn’t valid: use 3–4 digits (750) or 9 letters (rwxr-x---)."
        : k === 0 ? "Each name needs some text and no slash." : "Owner and group need a name.");
    });
    var rows = state.rows.map(function (r, i) { return { name: i === 0 ? "" : r[0].trim(), mode: r[1].trim(), owner: r[2].trim(), group: r[3].trim() }; });
    var out = document.getElementById("pg-verdict"), pathEl = document.getElementById("pg-path"), cmdEl = document.getElementById("pg-cmd");
    if (problems.length) {
      cmdEl.textContent = "";
      pathEl.innerHTML = "";
      out.innerHTML = '<dt class="v-deny">Fix this</dt><dd>' + esc(problems[0]) + "</dd>";
      return;
    }
    var steps = walk(user, groups, state.a, rows);
    cmdEl.textContent = command(user, state.a, rows);
    renderPath(pathEl, rows, steps, state.a, false);
    renderVerdict(out, user, state.a, rows, steps);
    try { history.replaceState(null, "", "#try=" + encodeURIComponent(JSON.stringify(state))); } catch (e) {}
  }

  rowsEl.addEventListener("input", function (e) {
    var el = e.target; if (!el.dataset || el.dataset.i === undefined) return;
    state.rows[+el.dataset.i][+el.dataset.k] = el.value;
    update();
  });
  rowsEl.addEventListener("click", function (e) {
    var b = e.target.closest("[data-rm]"); if (!b) return;
    state.rows.splice(+b.dataset.rm, 1);
    drawRows(); update();
  });
  userEl.addEventListener("input", function () { state.u = userEl.value; update(); });
  groupsEl.addEventListener("input", function () { state.g = groupsEl.value; update(); });
  document.querySelectorAll('input[name="pg-access"]').forEach(function (el) {
    el.addEventListener("change", function () { if (el.checked) { state.a = el.value; update(); } });
  });
  document.getElementById("pg-add").addEventListener("click", function () {
    if (state.rows.length >= 40) return;
    var last = state.rows[state.rows.length - 1];
    state.rows.push(["new-folder", "755", last[2], last[3]]);
    drawRows(); update();
    var inputs = rowsEl.querySelectorAll('input[data-k="0"]');
    if (inputs.length) { inputs[inputs.length - 1].focus(); inputs[inputs.length - 1].select(); }
  });
  document.querySelectorAll("[data-preset]").forEach(function (b) {
    b.addEventListener("click", function () { load(PRESETS[b.dataset.preset]); });
  });
  document.getElementById("pg-share").addEventListener("click", function () {
    var btn = this, url = location.href;
    function done(m) { btn.textContent = m; setTimeout(function () { btn.textContent = "Copy link to this example"; }, 1800); }
    if (navigator.clipboard && window.isSecureContext) navigator.clipboard.writeText(url).then(function () { done("Link copied"); }, function () { done("Copy it from the address bar"); });
    else done("Copy it from the address bar");
  });

  // Start from a shared link if there is one, otherwise from the web server example
  function fromHash() {
    var m = /^#try=(.+)$/.exec(location.hash);
    if (!m) return null;
    try {
      var s = JSON.parse(decodeURIComponent(m[1]));
      var okRows = Array.isArray(s.rows) && s.rows.length >= 2 && s.rows.length <= 40 && s.rows.every(function (r) {
        return Array.isArray(r) && r.length === 4 && r.every(function (x) { return typeof x === "string" && x.length <= 64; });
      });
      if (!okRows || typeof s.u !== "string" || typeof s.g !== "string" || ["r", "w", "x"].indexOf(s.a) === -1) return null;
      return { u: s.u.slice(0, 32), g: s.g.slice(0, 200), a: s.a, rows: s.rows };
    } catch (e) { return null; }
  }
  load(fromHash() || PRESETS.web);

  /* ---------- Paste real output: namei -l and id ---------- */

  // "drwxr-x--- root devs team" lines -> playground rows. Symlink lines are skipped:
  // namei lists the folders it walks through for the link target right after them.
  function parseNamei(text) {
    var rows = [];
    String(text).split(/\r?\n/).forEach(function (line) {
      var m = /^\s*([-dlbcps])([r-][w-][xsS-][r-][w-][xsS-][r-][w-][xtT-])\+?\s+(\S+)\s+(\S+)\s+(.+?)\s*$/.exec(line);
      if (!m || m[1] === "l") return;
      var name = m[5];
      if (name === "/") { rows = [["", m[1] + m[2], m[3], m[4]]]; return; }
      if (!rows.length) rows.push(["", "drwxr-xr-x", "root", "root"]);
      if (name === "." ) return;
      rows.push([name.slice(0, 64), m[1] + m[2], m[3].slice(0, 64), m[4].slice(0, 64)]);
    });
    return rows.slice(0, 40);
  }
  // "uid=1001(prince) gid=1001(prince) groups=1001(prince),27(sudo)" -> user and groups
  function parseId(text) {
    var t = String(text).trim(), u = /uid=\d+\(([^)]+)\)/.exec(t);
    if (!u) return null;
    var groups = [], g = /gid=\d+\(([^)]+)\)/.exec(t);
    if (g) groups.push(g[1]);
    var gm = /groups=(.*)$/.exec(t);
    if (gm) gm[1].replace(/\d+\(([^)]+)\)/g, function (_, name) { if (groups.indexOf(name) === -1) groups.push(name); });
    return { user: u[1].slice(0, 32), groups: groups.slice(0, 50) };
  }

  document.getElementById("real-go").addEventListener("click", function () {
    var err = document.getElementById("real-err");
    var rows = parseNamei(document.getElementById("real-namei").value);
    var who = parseId(document.getElementById("real-id").value);
    if (rows.length < 2) { err.textContent = "Couldn’t read the namei output. Paste everything namei -l printed, including the lines that start with drwx…"; return; }
    if (!who) { err.textContent = "Couldn’t read the id output. It should start with uid=, like uid=1001(prince) gid=1001(prince) …"; return; }
    err.textContent = "";
    var a = (document.querySelector('input[name="pg-access"]:checked') || { value: "r" }).value;
    load({ u: who.user, g: who.groups.join(", "), a: a, rows: rows });
    document.getElementById("pg-out-anchor").scrollIntoView({ behavior: "smooth", block: "start" });
  });

  /* ---------- Permission switches ---------- */

  var swBoxes = document.querySelectorAll("input.swc"), swNum = document.getElementById("sw-num");
  function swValue() { var v = 0; swBoxes.forEach(function (c) { if (c.checked) v |= +c.dataset.b; }); return v; }
  function swSet(v) { swBoxes.forEach(function (c) { c.checked = (v & +c.dataset.b) !== 0; }); }
  function flagsFor(special, o, g, e) {
    var f = [];
    if (special === 0 && o === 7 && g === 7 && e === 7) f.push(["danger", "777 lets every user on the system do anything. Find the smallest set of switches that still works."]);
    else if (e & 2) f.push(["danger", "Everyone can change this. On a folder, anyone can also delete other people’s files inside it."]);
    if (special & 4) f.push(["danger", "setuid: the program runs with its owner’s rights. If root owns it, any bug in it can become a root exploit."]);
    if (special & 2) f.push(["warn", "setgid: a program runs with the file’s group rights; in a folder, new files join the folder’s group."]);
    if (special & 1) f.push(["info", "Sticky: in a shared folder like /tmp, people may only delete their own files."]);
    if ((g & ~o) || (e & ~o)) f.push(["warn", "The owner has fewer switches on than the group or everyone else. The owner only ever gets the owner’s switches."]);
    if (!f.length) f.push(["ok", "No red flags."]);
    return f;
  }
  function swRender(v, fromBox) {
    var sp = (v >> 9) & 7, o = (v >> 6) & 7, g = (v >> 3) & 7, e = v & 7;
    var octal = (sp ? String(sp) : "") + o + g + e;
    if (fromBox) swNum.value = octal;
    var sym = modeString({ bits: [o, g, e] }, false).slice(1).split("");
    if (sp & 4) sym[2] = o & 1 ? "s" : "S";
    if (sp & 2) sym[5] = g & 1 ? "s" : "S";
    if (sp & 1) sym[8] = e & 1 ? "t" : "T";
    sym = sym.join("");
    document.getElementById("sw-sym").innerHTML = "<span>" + sym.slice(0, 3) + "</span><span>" + sym.slice(3, 6) + "</span><span>" + sym.slice(6) + "</span>";
    function can(bits) {
      var a = [];
      if (bits & 4) a.push("read"); if (bits & 2) a.push("change"); if (bits & 1) a.push("run / enter");
      return a.length ? a.join(", ") : "nothing";
    }
    document.getElementById("sw-plain").innerHTML =
      "<li><b>Owner</b> can " + can(o) + "</li><li><b>Group</b> can " + can(g) + "</li><li><b>Everyone else</b> can " + can(e) + "</li>";
    var L = function (bits, extra) { var t = (bits & 4 ? "r" : "") + (bits & 2 ? "w" : "") + (bits & 1 ? "x" : "") + extra; return t; };
    document.getElementById("sw-c1").textContent = "chmod " + octal + " FILE";
    document.getElementById("sw-c2").textContent = "chmod u=" + L(o, sp & 4 ? "s" : "") + ",g=" + L(g, sp & 2 ? "s" : "") + ",o=" + L(e, sp & 1 ? "t" : "") + " FILE";
    var LV = { danger: "Danger", warn: "Careful", info: "Note", ok: "OK" };
    document.getElementById("sw-flags").innerHTML = flagsFor(sp, o, g, e).map(function (x) {
      return '<li class="' + x[0] + '"><b>' + LV[x[0]] + "</b> " + esc(x[1]) + "</li>";
    }).join("");
  }
  swBoxes.forEach(function (c) { c.addEventListener("change", function () { swRender(swValue(), true); }); });
  swNum.addEventListener("input", function () {
    var t = swNum.value.trim();
    swNum.setAttribute("aria-invalid", /^[0-7]{3,4}$/.test(t) ? "false" : "true");
    if (!/^[0-7]{3,4}$/.test(t)) return;
    var v = parseInt(t, 8); swSet(v); swRender(v, false);
  });
  swSet(parseInt("750", 8)); swRender(parseInt("750", 8), false);

  /* ---------- Quiz ---------- */

  var QUIZ = [
    { user: "bob", groups: ["bob"], want: "r", rows: [["", "755", "root", "root"], ["home", "755", "root", "root"], ["alice", "700", "alice", "alice"], ["notes.txt", "644", "alice", "alice"]] },
    { user: "carol", groups: ["carol", "devs"], want: "w", rows: [["", "755", "root", "root"], ["srv", "755", "root", "root"], ["team", "770", "root", "devs"], ["plan.txt", "664", "root", "devs"]] },
    { user: "alice", groups: ["alice", "devs"], want: "r", rows: [["", "755", "root", "root"], ["srv", "755", "root", "root"], ["report.txt", "070", "alice", "devs"]] },
    { user: "bob", groups: ["bob"], want: "x", rows: [["", "755", "root", "root"], ["opt", "755", "root", "root"], ["backup.sh", "754", "root", "ops"]] },
    { user: "root", groups: ["root"], want: "x", rows: [["", "755", "root", "root"], ["usr", "755", "root", "root"], ["tool", "644", "root", "root"]] }
  ];
  var quizBox = document.getElementById("quiz-box"), qi = 0, qmarks = [];
  function qRows(q) { return q.rows.map(function (r, i) { return { name: i === 0 ? "" : r[0], mode: r[1], owner: r[2], group: r[3] }; }); }
  function qPath(q) { return "/" + q.rows.slice(1).map(function (r) { return r[0]; }).join("/"); }
  function qTruth(q) { var rows = qRows(q), st = walk(q.user, q.groups, q.want, rows); return { rows: rows, steps: st, yes: st.length === rows.length && st[st.length - 1].ok }; }
  function qDots() {
    return '<div class="q-dots">' + QUIZ.map(function (_, i) {
      return "<i class=\"" + (qmarks[i] === true ? "right" : qmarks[i] === false ? "wrong" : i === qi ? "now" : "") + "\"></i>";
    }).join("") + "</div>";
  }
  function qShow() {
    if (qi >= QUIZ.length) {
      var right = qmarks.filter(Boolean).length;
      quizBox.innerHTML = '<div class="q-top"><span>Done</span>' + qDots() + '</div><p class="q-score">' + right + " / " + QUIZ.length + "</p>" +
        "<p>" + (right === QUIZ.length ? "Perfect. You think like the kernel." : right >= 3 ? "Nice! Look again at the ones you missed: the owner rule catches most people." : "That’s how everyone starts. Read the basics above and try again.") + "</p>" +
        '<button class="btn" type="button" id="q-again">Try again</button>';
      document.getElementById("q-again").onclick = function () { qi = 0; qmarks = []; qShow(); };
      return;
    }
    var q = QUIZ[qi], groupsText = q.groups.filter(function (g) { return g !== q.user; });
    var facts = q.rows.map(function (r, i) {
      var p = parseMode(r[1]), last = i === q.rows.length - 1;
      return "<li>" + esc(i === 0 ? "/" : r[0] + (last ? "" : "/")) + "  <span>" + esc(modeString(p, !last)) + " " + esc(r[2] + ":" + r[3]) + "</span></li>";
    }).join("");
    quizBox.innerHTML = '<div class="q-top"><span>Question ' + (qi + 1) + " of " + QUIZ.length + "</span>" + qDots() + "</div>" +
      '<p class="q-ask">Can <b>' + esc(q.user) + "</b> " + VERB[q.want] + " <code>" + esc(qPath(q)) + "</code>?</p>" +
      '<p class="muted" style="margin:0 0 8px">' + esc(q.user) + (q.user === "root" ? " is the administrator." : groupsText.length ? " is in the " + esc(groupsText.join(", ")) + " group." : " is in no extra groups.") + " The path:</p>" +
      '<ul class="q-facts">' + facts + "</ul>" +
      '<div class="q-btns"><button class="btn" type="button" data-a="yes">Yes</button><button class="btn quiet" type="button" data-a="no">No</button></div>' +
      '<div class="q-result" id="q-result"></div>';
    quizBox.querySelectorAll("[data-a]").forEach(function (b) {
      b.onclick = function () {
        var t = qTruth(q), guess = b.dataset.a === "yes", ok = guess === t.yes;
        qmarks[qi] = ok;
        quizBox.querySelectorAll("[data-a]").forEach(function (x) { x.disabled = true; });
        var res = document.getElementById("q-result");
        res.innerHTML = '<p class="tag ' + (ok ? "right" : "wrong") + '">' + (ok ? "Right!" : "Not quite.") + " The answer is " + (t.yes ? "yes" : "no") + ".</p>" +
          '<ol class="path small" id="q-path"></ol><dl class="verdict" id="q-verdict"></dl>' +
          '<p style="margin-top:16px"><button class="btn" type="button" id="q-next">' + (qi + 1 < QUIZ.length ? "Next question" : "See my score") + "</button></p>";
        renderPath(document.getElementById("q-path"), t.rows, t.steps, q.want, true);
        renderVerdict(document.getElementById("q-verdict"), q.user, q.want, t.rows, t.steps);
        quizBox.querySelector(".q-top").outerHTML = '<div class="q-top"><span>Question ' + (qi + 1) + " of " + QUIZ.length + "</span>" + qDots() + "</div>";
        document.getElementById("q-next").onclick = function () { qi++; qShow(); quizBox.scrollIntoView({ block: "nearest" }); };
        document.getElementById("q-next").focus({ preventScroll: true });
      };
    });
  }
  qShow();

  /* ---------- Copy buttons next to fix commands ---------- */
  document.addEventListener("click", function (e) {
    var b = e.target.closest && e.target.closest(".mini-copy"); if (!b) return;
    var t = b.dataset.copy;
    function done(m) { b.textContent = m; setTimeout(function () { b.textContent = "Copy"; }, 1500); }
    if (navigator.clipboard && window.isSecureContext) navigator.clipboard.writeText(t).then(function () { done("Copied"); }, function () { done("Select it"); });
    else done("Select it");
  });

  /* ---------- Comments (giscus, GitHub Discussions) ---------- */
  // The giscus script is loaded only when the comments come near the screen,
  // so visitors who never scroll that far make no request to giscus.app.
  var GISCUS = { repo: "Hennashakil/whycant", repoId: "R_kgDOVLI-kQ", category: "General", categoryId: "DIC_kwDOVLI-kc4DIpMz" };
  var commentsEl = document.getElementById("comments");
  function loadGiscus() {
    if (loadGiscus.done) return;
    loadGiscus.done = true;
    var gs = document.createElement("script");
    gs.src = "https://giscus.app/client.js"; gs.async = true; gs.crossOrigin = "anonymous";
    var attrs = { "data-repo": GISCUS.repo, "data-repo-id": GISCUS.repoId, "data-category": GISCUS.category, "data-category-id": GISCUS.categoryId,
      "data-mapping": "pathname", "data-strict": "1", "data-reactions-enabled": "1", "data-emit-metadata": "0", "data-input-position": "top",
      "data-theme": "preferred_color_scheme", "data-lang": "en" };
    Object.keys(attrs).forEach(function (k) { gs.setAttribute(k, attrs[k]); });
    document.getElementById("giscus-box").appendChild(gs);
  }
  if (GISCUS.repoId && GISCUS.categoryId) {
    commentsEl.hidden = false;
    if ("IntersectionObserver" in window) {
      new IntersectionObserver(function (es, obs) {
        if (es.some(function (x) { return x.isIntersecting; })) { loadGiscus(); obs.disconnect(); }
      }, { rootMargin: "600px" }).observe(commentsEl);
    } else loadGiscus();
  }

  /* ---------- Live project status from the GitHub API ---------- */

  var REPO = "Hennashakil/whycant";
  function ago(iso) {
    var s = (new Date(iso).getTime() - Date.now()) / 1000, a = Math.abs(s);
    var rtf = window.Intl && Intl.RelativeTimeFormat ? new Intl.RelativeTimeFormat("en", { numeric: "auto" }) : null;
    var u = a < 3600 ? ["minute", 60] : a < 86400 ? ["hour", 3600] : a < 2592000 ? ["day", 86400] : ["month", 2592000];
    return rtf ? rtf.format(Math.round(s / u[1]), u[0]) : new Date(iso).toDateString();
  }
  function day(iso) { return new Date(iso).toLocaleDateString("en-GB", { day: "numeric", month: "short", year: "numeric" }); }
  function getJSON(url) {
    return fetch(url, { headers: { Accept: "application/vnd.github+json" } }).then(function (r) {
      if (!r.ok) throw new Error("GitHub said " + r.status);
      return r.json();
    });
  }
  function cacheGet() {
    try { var c = JSON.parse(localStorage.getItem("whycant-status") || "null"); if (c && Date.now() - c.t < 600000) return c.d; } catch (e) {}
    return null;
  }
  function cacheSet(d) { try { localStorage.setItem("whycant-status", JSON.stringify({ t: Date.now(), d: d })); } catch (e) {} }

  function showStatus(d) {
    var rel = d.releases.filter(function (r) { return !r.draft; });
    var latest = rel[0];
    var downloads = rel.reduce(function (n, r) {
      return n + (r.assets || []).reduce(function (m, a) { return m + (a.download_count || 0); }, 0);
    }, 0);
    if (latest) {
      document.getElementById("st-version").textContent = latest.tag_name;
      document.getElementById("st-released").innerHTML = esc(day(latest.published_at)) + "<small>" + esc(ago(latest.published_at)) + "</small>";
      var btn = document.getElementById("dl-btn");
      btn.textContent = "Download " + latest.tag_name;
      btn.href = latest.html_url;
    }
    document.getElementById("st-downloads").textContent = downloads.toLocaleString("en-IN");
    document.getElementById("st-stars").textContent = String(d.repo.stargazers_count);
    var c = d.commits[0];
    if (c) {
      var msg = String(c.commit.message).split("\n")[0].slice(0, 80);
      document.getElementById("st-last").innerHTML = "Last change " + esc(ago(c.commit.author.date)) +
        ': <a href="' + esc(c.html_url) + '"><code>' + esc(msg) + "</code></a>";
    }
    document.getElementById("live-dot").classList.add("on");
    document.getElementById("live-text").textContent = "Live from GitHub";
  }

  (function loadStatus() {
    var cached = cacheGet();
    if (cached) { showStatus(cached); return; }
    var base = "https://api.github.com/repos/" + REPO;
    Promise.all([getJSON(base), getJSON(base + "/releases?per_page=30"), getJSON(base + "/commits?per_page=1")])
      .then(function (r) {
        var d = { repo: { stargazers_count: r[0].stargazers_count }, releases: r[1].map(function (x) {
          return { tag_name: x.tag_name, published_at: x.published_at, html_url: x.html_url, draft: x.draft,
                   assets: (x.assets || []).map(function (a) { return { download_count: a.download_count }; }) };
        }), commits: r[2].slice(0, 1).map(function (x) {
          return { html_url: x.html_url, commit: { message: x.commit.message, author: { date: x.commit.author.date } } };
        }) };
        cacheSet(d); showStatus(d);
      })
      .catch(function () {
        document.getElementById("live-text").textContent = "Couldn’t reach GitHub just now, so these numbers may be out of date.";
      });
  })();

  /* ---------- Copy buttons ---------- */
  document.querySelectorAll(".copy").forEach(function (btn) {
    btn.addEventListener("click", function () {
      var text = btn.previousElementSibling.innerText;
      function done(msg) { btn.textContent = msg; setTimeout(function () { btn.textContent = "Copy"; }, 1600); }
      if (navigator.clipboard && window.isSecureContext) navigator.clipboard.writeText(text).then(function () { done("Copied"); }, function () { done("Select and copy"); });
      else done("Select and copy");
    });
  });
})();
