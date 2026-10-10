const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../../src/network/html/shared/PluginHost.js.inc'), 'utf8');
const js = source.slice(source.indexOf('R"CPJS(') + 7, source.lastIndexOf(')CPJS"'));
const scripts = [], posts = [], nodes = new Map();
function element(tag) {
  return { tag, children: [], append(...values) { this.children.push(...values); }, appendChild(value) {
    this.children.push(value); if (value.id) nodes.set(value.id, value);
    if (value.tag === 'script') { scripts.push(value.src); value.onload(); }
  }};
}
const body = element('body');
const plugins = [
  {name:'disabled', available:true, approved:false, enabled:false, script:'main.js', systemEnabled:true},
  {name:'changed', available:true, approved:false, enabled:true, script:'main.js', systemEnabled:true},
  {name:'globalOff', available:true, approved:true, enabled:true, script:'main.js', systemEnabled:false},
  {name:'trusted', available:true, approved:true, enabled:true, script:'plugin.js', systemEnabled:true}
];
let claim = null, failCompletion = false;
const context = {
  window: {}, TextEncoder, encodeURIComponent, setInterval: () => 1,
  document: {body, getElementById:id => nodes.get(id), querySelector:() => null, createElement:element},
  confirm:() => false, location:{reload:() => {}},
  async fetch(url, options = {}) {
    let value, status = 200;
    if (url === '/api/plugins') value = plugins;
    else if (url.startsWith('/api/plugin-jobs/claim')) { value = claim || {}; claim = null; }
    else {
      posts.push({url, body:JSON.parse(options.body), headers:options.headers}); value = {};
      if (failCompletion) {status = 503; failCompletion = false;}
    }
    return {ok:status === 200, status, json:async () => value,
      headers:{get:name => name === 'X-Plugin-Session' ? 'nonce' : null}};
  }
};
vm.runInNewContext(js, context);
(async () => {
  await context.window.loadPlugins('settings');
  assert.equal(scripts.length, 1); assert.match(scripts[0], /name=trusted&file=plugin.js&session=nonce$/);
  const host = context.window.CrossPoint;
  host.registerAction('trusted', 'sync', async () => ({text:'中'.repeat(70)}));
  claim = {id:1, claim:42, action:'sync', args:{}};
  await host._pollJobs();
  assert.equal(posts.at(-1).body.ok, false); assert.equal(posts.at(-1).headers['X-Plugin-Session'], 'nonce');
  assert.ok(new TextEncoder().encode(JSON.stringify(posts.at(-1).body.result)).length < 192);
  host.registerAction('trusted', 'small', async () => ({ok:true}));
  claim = {id:2, claim:43, action:'small', args:{}}; failCompletion = true;
  await host._pollJobs(); assert.equal(host._completion.id, 2);
  await host._pollJobs(); assert.equal(host._completion, null);
  assert.equal(posts.at(-1).body.id, 2); assert.equal(posts.at(-1).body.claim, 43);
  console.log('BrowserJS approval, UTF-8 bounds and completion retry checks passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
