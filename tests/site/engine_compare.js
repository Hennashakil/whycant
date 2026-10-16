// Reads cases (with the kernel's answer) as JSON on stdin and checks that the
// website's engine (docs/engine.js) gives the same answer for every one.
const path = require("path");
const E = require(path.join(__dirname, "..", "..", "docs", "engine.js"));

let input = "";
process.stdin.on("data", (d) => { input += d; });
process.stdin.on("end", () => {
  const cases = JSON.parse(input);
  let bad = 0;
  for (const c of cases) {
    const rows = c.rows.map((r, i) => ({ name: i === 0 ? "" : r[0], mode: r[1], owner: r[2], group: r[3] }));
    const steps = E.walk(c.user, c.groups, c.want, rows);
    const engine = E.allowed(steps, rows);
    if (engine !== c.kernel) {
      bad++;
      console.log(`MISMATCH user=${c.user} want=${c.want} kernel=${c.kernel} engine=${engine} rows=${JSON.stringify(c.rows)}`);
    }
  }
  console.log(`engine check: ${cases.length - bad} passed, ${bad} failed`);
  process.exit(bad ? 1 : 0);
});
