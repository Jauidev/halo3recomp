// Node version of sync_dir.py (for machines without Python): makes <dst> an exact copy of <src>,
// touching only files whose content changed, so ninja rebuilds just those (a fresh XenonRecomp
// output otherwise has new timestamps on all ~230 files). --dry lists the changes without copying.
// Usage: node sync_dir.js <src> <dst> [--dry]
const fs = require('fs');
const path = require('path');
const [src, dst, flag] = process.argv.slice(2);
const dry = flag === '--dry';
const names = fs.readdirSync(src);
if (!names.length) { console.error(`${src} is empty: refusing to wipe ${dst}`); process.exit(1); }
if (!dry) fs.mkdirSync(dst, { recursive: true });
const changed = [], removed = [];
for (const name of names.sort()) {
  const s = path.join(src, name), d = path.join(dst, name);
  const a = fs.readFileSync(s);
  if (!fs.existsSync(d) || !a.equals(fs.readFileSync(d))) {
    changed.push(name);
    if (!dry) fs.writeFileSync(d, a);
  }
}
const keep = new Set(names);
for (const name of (fs.existsSync(dst) ? fs.readdirSync(dst) : []).sort()) {
  if (!keep.has(name)) { removed.push(name); if (!dry) fs.unlinkSync(path.join(dst, name)); }
}
console.log(`${dst}: ${changed.length} files changed, ${removed.length} removed, ${names.length} total${dry ? ' (dry run)' : ''}`);
if (changed.length) console.log('changed: ' + changed.join(' '));
if (removed.length) console.log('removed: ' + removed.join(' '));
