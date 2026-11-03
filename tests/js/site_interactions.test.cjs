// Runs the actual contrast and disclosure scripts with a mock DOM. Layout, focus,
// scrolling limits, and native disclosure behavior require a browser check.
// Run after building the portal: node tests/js/site_interactions.test.cjs [portal directory]
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const root = path.resolve(__dirname, '../..');
const portalRoot = path.resolve(root, process.argv[2] || 'site/github/output/public');
const pages = [
  'site/cloudflare/public/index.html',
  'site/github/index.html',
  'site/github/en/index.html',
  path.join(portalRoot, 'docs/ja/Using_the_App.html'),
  path.join(portalRoot, 'docs/en/Using_the_App.html'),
];
const storageKey = 'pdf-note-workspace-high-contrast';

function run(source, saved, unavailable = false) {
  const dataset = {};
  const events = {};
  const frames = [];
  const writes = [];
  const scrolls = [];
  const toggle = { checked: false, addEventListener: (name, callback) => { events[name] = callback; } };
  vm.runInNewContext(source, {
    document: {
      documentElement: { dataset },
      querySelector: selector => {
        assert.equal(selector, '#contrast-toggle');
        return toggle;
      },
    },
    localStorage: {
      getItem: key => {
        assert.equal(key, storageKey);
        if (unavailable) throw new Error('Storage unavailable');
        return saved;
      },
      setItem: (key, value) => {
        assert.equal(key, storageKey);
        if (unavailable) throw new Error('Storage unavailable');
        writes.push(value);
      },
    },
    window: { scrollX: 17, scrollY: 825, scrollTo: (x, y) => { scrolls.push([x, y]); } },
    requestAnimationFrame: callback => { frames.push(callback); },
  }, { timeout: 1000 });
  assert.equal(typeof events.change, 'function');
  assert.equal(frames.length, 0, 'initial preference must not move the document');
  assert.equal(writes.length, 0, 'reading a preference must not write it');
  return { dataset, toggle, events, frames, writes, scrolls };
}

for (const page of pages) {
  const html = fs.readFileSync(path.resolve(root, page), 'utf8');
  const scripts = [...html.matchAll(/<script(?:\s[^>]*)?>([\s\S]*?)<\/script>/g)]
    .map(match => match[1]).filter(source => source.includes(storageKey));
  assert.equal(scripts.length, 1, `${page}: required contrast script missing or duplicated`);
  const source = scripts[0];
  for (const saved of [null, 'false', 'TRUE', 'true']) {
    const f = run(source, saved);
    assert.equal(f.toggle.checked, saved === 'true', page);
    assert.equal(f.dataset.contrast, saved === 'true' ? 'high' : '', page);
    for (const enabled of [true, false]) {
      f.toggle.checked = enabled;
      f.events.change();
      assert.equal(f.dataset.contrast, enabled ? 'high' : '');
      assert.equal(f.writes.at(-1), String(enabled));
      assert.equal(f.frames.length, 1);
      f.frames.shift()();
      assert.deepEqual(f.scrolls.at(-1), [17, 825], 'restore the observed scroll position');
    }
  }
  const unavailable = run(source, null, true);
  unavailable.toggle.checked = true;
  assert.doesNotThrow(() => unavailable.events.change(), `${page}: blocked storage must not disable the control`);
  assert.equal(unavailable.dataset.contrast, 'high');
  unavailable.frames.shift()();
  assert.deepEqual(unavailable.scrolls, [[17, 825]]);
}

// A link or search result can target a heading inside a collapsed details
// container. The generated page must reveal that container before scrolling.
const fragmentHtml = fs.readFileSync(pages[3], 'utf8');
const fragmentSource = [...fragmentHtml.matchAll(/<script(?:\s[^>]*)?>([\s\S]*?)<\/script>/g)]
  .map(match => match[1]).find(source => source.includes('revealFragment'));
assert.ok(fragmentSource, 'generated document: fragment reveal script missing');
const fragmentEvents = {};
const fragmentFrames = [];
const fragmentToggle = { checked: false, addEventListener: (name, callback) => { fragmentEvents[name] = callback; } };
const outerDetails = { tagName: 'DETAILS', open: false, parentElement: null };
const target = { parentElement: outerDetails, scrollIntoView: options => { target.scrollOptions = options; } };
vm.runInNewContext(fragmentSource, {
  document: {
    documentElement: { dataset: {} },
    querySelector: selector => { assert.equal(selector, '#contrast-toggle'); return fragmentToggle; },
    getElementById: identifier => identifier === 'linked-section' ? target : null,
  },
  localStorage: { getItem: () => null, setItem: () => {} },
  window: {
    scrollX: 0, scrollY: 0, scrollTo: () => {},
    location: { hash: '#linked-section' },
    addEventListener: (name, callback) => { fragmentEvents[name] = callback; },
  },
  requestAnimationFrame: callback => { fragmentFrames.push(callback); },
}, { timeout: 1000 });
assert.equal(outerDetails.open, true, 'fragment target must open each containing details element');
assert.equal(typeof fragmentEvents.hashchange, 'function');
fragmentFrames.shift()();
assert.equal(target.scrollOptions.block, 'start');
console.log(`Contrast script checks passed for ${pages.length} pages (mock DOM; browser verification still required).`);

// Bulk disclosure must affect only the feature catalogue. Native details
// remain usable if scripting or the catalogue controls are unavailable.
const introductionHtml = fs.readFileSync(path.join(root, pages[0]), 'utf8');
const bulkScript = introductionHtml.match(/<script id="feature-disclosure-script">([\s\S]*?)<\/script>/);
assert.ok(bulkScript, 'feature bulk-disclosure script missing');
function mockDisclosure(open = false) {
  const summary = {
    events: {},
    addEventListener(event, callback) { assert.equal(event, 'click'); this.events[event] = callback; },
  };
  return {
    open, dataset: {}, events: {}, summary,
    querySelector(selector) { assert.equal(selector, ':scope > summary'); return summary; },
    addEventListener(event, callback) { assert.equal(event, 'toggle'); this.events[event] = callback; },
  };
}
function runDisclosure(missingCatalogue = false) {
  const entries = Array.from({ length: 18 }, () => mockDisclosure());
  const controls = { hidden: true };
  const attributes = {};
  const label = { textContent: '' };
  const clicks = {};
  const selectors = {
    '#feature-catalogue': missingCatalogue ? null : {
      querySelectorAll: selector => { assert.equal(selector, 'details.feature-entry'); return entries; },
    },
    '#feature-controls': controls,
    '#features-toggle': {
      addEventListener: (event, callback) => { assert.equal(event, 'click'); clicks.toggle = callback; },
      setAttribute: (name, value) => { attributes[name] = value; },
    },
    '#features-toggle-label': label,
  };
  vm.runInNewContext(bulkScript[1], {
    document: { querySelector: selector => { assert.ok(Object.hasOwn(selectors, selector)); return selectors[selector]; } },
  }, { timeout: 1000 });
  return { entries, controls, clicks, attributes, label };
}
const bulk = runDisclosure();
assert.equal(bulk.controls.hidden, false);
assert.equal(bulk.entries.filter(entry => entry.open).length, 0, 'initialization must keep the native closed state');
assert.equal(bulk.attributes['aria-expanded'], 'false');
assert.equal(bulk.label.textContent, 'すべて開く');
bulk.clicks.toggle();
assert.equal(bulk.entries.filter(entry => entry.open).length, 18);
assert.equal(bulk.attributes['aria-expanded'], 'true');
assert.equal(bulk.label.textContent, 'すべて閉じる');
bulk.entries[4].open = false; // Simulate an individual native disclosure.
bulk.entries[4].events.toggle();
assert.equal(bulk.attributes['aria-expanded'], 'false');
assert.equal(bulk.label.textContent, 'すべて開く');
bulk.clicks.toggle();
assert.equal(bulk.entries.filter(entry => entry.open).length, 18, 'mixed state must expand completely');
bulk.clicks.toggle();
assert.equal(bulk.entries.filter(entry => entry.open).length, 0);
assert.equal(bulk.label.textContent, 'すべて開く');
assert.ok(bulk.entries.every(entry => entry.dataset.opened === undefined), 'bulk opening and closing must not mark items');
bulk.entries[0].dataset.opened = 'true';
bulk.entries[1].dataset.readPending = 'true';
bulk.clicks.toggle();
assert.equal(bulk.entries[0].dataset.opened, 'true', 'bulk operations retain completed individual history');
assert.ok(bulk.entries.every(entry => entry.dataset.readPending === undefined), 'bulk operations interrupt pending individual cycles');
assert.equal(runDisclosure(true).controls.hidden, true, 'unavailable controls must stay hidden');
console.log('Feature bulk-disclosure checks passed (18 items, mixed state, label, state, history preserved, missing catalogue).');

for (const relative of pages) {
  const html = fs.readFileSync(path.resolve(root, relative), 'utf8');
  const script = html.match(/<script id="navigation-state-script">([\s\S]*?)<\/script>/);
  assert.ok(script, `${relative}: navigation-state script missing`);
  const canonicalTag = html.match(/<link\b[^>]*rel="canonical"[^>]*>/)[0];
  const canonical = canonicalTag.match(/href="([^"]+)"/)[1];
  const origin = new URL(canonical).origin;
  const links = ['#content', 'next.html', `${origin}/same-site`, 'https://example.net/other', '//example.net/other', 'http://[invalid'].map(href => ({
    dataset: {}, getAttribute: name => { assert.equal(name, 'href'); return href; },
  }));
  const entries = [false, true, false, false].map(open => mockDisclosure(open));
  const timers = [];
  vm.runInNewContext(script[1], {
    URL, setTimeout: (callback, delay) => { assert.equal(delay, 0); timers.push(callback); },
    document: {
      querySelector: selector => { assert.equal(selector, 'link[rel="canonical"]'); return { href: canonical }; },
      querySelectorAll: selector => {
        if (selector === 'a[href]') return links;
        assert.equal(selector, 'details'); return entries;
      },
    },
  }, { timeout: 1000 });
  assert.deepEqual(links.map(link => link.dataset.linkKind), ['internal', 'internal', 'internal', 'external', 'external', undefined]);
  assert.equal(entries[0].dataset.opened, undefined);
  assert.equal(entries[1].dataset.opened, undefined, 'initially open sections do not count as individually read');
  const activate = (entry, { prevented = false, nestedLink = false } = {}) => {
    const event = { defaultPrevented: prevented, target: { closest: () => nestedLink ? {} : null } };
    entry.summary.events.click(event);
    if (!prevented && !nestedLink) entry.open = !entry.open; // Native summary default action.
    entry.events.toggle();
    while (timers.length) timers.shift()();
  };
  // Programmatic and fragment-driven changes must not create a read cycle.
  entries[0].open = true; entries[0].events.toggle();
  entries[0].open = false; entries[0].events.toggle();
  assert.equal(entries[0].dataset.opened, undefined);
  activate(entries[0]);
  assert.equal(entries[0].dataset.opened, undefined, 'individual opening alone must not change color');
  activate(entries[0]);
  assert.equal(entries[0].dataset.opened, 'true', 'individual open then close completes the cycle');
  activate(entries[0]);
  assert.equal(entries[0].dataset.opened, 'true', 'reopening preserves the completed cycle');
  activate(entries[1]);
  assert.equal(entries[1].dataset.opened, undefined, 'closing an initially open section is not a complete individual cycle');
  activate(entries[2], { prevented: true });
  activate(entries[2], { nestedLink: true });
  assert.equal(entries[2].dataset.readPending, undefined, 'cancelled or nested-link clicks do not start a cycle');
  activate(entries[2]);
  entries[2].open = false; entries[2].events.toggle();
  assert.equal(entries[2].dataset.opened, undefined, 'programmatic closing does not complete a pending cycle');
  assert.equal(entries[2].dataset.readPending, undefined);
  // Integrate the actual bulk script to check interrupted and bulk-origin cycles.
  const controls = { hidden: true };
  let bulkClick;
  vm.runInNewContext(bulkScript[1], { document: { querySelector: selector => ({
    '#feature-catalogue': { querySelectorAll: () => entries },
    '#feature-controls': controls,
    '#features-toggle': { setAttribute() {}, addEventListener: (_, callback) => { bulkClick = callback; } },
    '#features-toggle-label': { textContent: '' },
  })[selector] } }, { timeout: 1000 });
  activate(entries[3]);
  bulkClick(); // Expand all, interrupting the individual open.
  activate(entries[3]);
  assert.equal(entries[3].dataset.opened, undefined, 'individual close after bulk expansion does not count');
  bulkClick(); // Expand all.
  bulkClick(); // Close all.
  assert.equal(entries[2].dataset.opened, undefined, 'bulk open then close must not count');
  assert.equal(entries[0].dataset.opened, 'true', 'bulk operations preserve existing completed cycles');
  activate(entries[3]); activate(entries[3]);
  assert.equal(entries[3].dataset.opened, 'true', 'a fresh individual cycle works after bulk operations');
  entries[2].summary.events.click({ defaultPrevented: false, target: { closest: () => null } });
  entries[2].open = true; // Leave the native action's timer pending.
  bulkClick();
  while (timers.length) timers.shift()();
  assert.equal(entries[2].dataset.readPending, undefined, 'bulk action invalidates a delayed individual-opening callback');
  activate(entries[2]);
  assert.equal(entries[2].dataset.opened, undefined, 'a stale callback must not create a completed cycle');
}
console.log(`Navigation colors and individual read-cycle checks passed for ${pages.length} pages.`);
