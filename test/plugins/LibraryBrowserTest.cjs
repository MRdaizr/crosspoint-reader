const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, '../../src/network/html/FilesPage.html'), 'utf8');
for (const match of html.matchAll(/<script(?:\s[^>]*)?>([\s\S]*?)<\/script>/g)) new vm.Script(match[1]);
const js = html.match(/<script id="library-browser-script">([\s\S]*?)<\/script>/)[1];
const nodes = new Map(), calls = [];
function node(id = '') {
  return {id, value:'', children:[], listeners:{}, disabled:false, textContent:'',
    append(...values) {this.children.push(...values);}, appendChild(child) {this.children.push(child);},
    replaceChildren() {this.children = [];}, addEventListener(name, fn) {this.listeners[name] = fn;}};
}
for (const id of ['library-query','library-sort','library-direction','library-status','library-results',
                 'library-previous','library-next','library-rebuild','library-cancel','library-page','library-search-form'])
  nodes.set(id, node(id));
nodes.get('library-sort').value = 'recent'; nodes.get('library-direction').value = 'desc';
let confirmation = false, pending = false;
const context = { TextEncoder, URLSearchParams, AbortController,
  confirm:() => confirmation,
  document:{getElementById:id => nodes.get(id), createElement:() => node(), createDocumentFragment:() => node()},
  fetch:async (url, options = {}) => {
    calls.push({url, options});
    if (pending) return new Promise((_, reject) => {
      options.signal.addEventListener('abort', () => {const error = new Error('cancelled'); error.name = 'AbortError'; reject(error);});
    });
    const offset = Number(new URL('http://reader' + url).searchParams.get('offset')) || 0;
    return {ok:true, json:async () => options.method === 'POST' ? {books:20} :
      {offset,total:20,count:1,dirty:false,items:[{title:'<img onerror=alert(1)>中文',author:'"<script>"',path:'/Books/中文 &?.epub'}]}};
  }
};
const settle = () => new Promise(setImmediate);
vm.runInNewContext(js, context);
(async () => {
  await settle();
  assert.equal(calls.length, 1); assert.equal(calls[0].options.method, undefined);
  assert.equal(new URL('http://reader' + calls[0].url).searchParams.get('limit'), '16');
  const row = nodes.get('library-results').children[0].children[0], link = row.children[0];
  assert.equal(link.textContent, '<img onerror=alert(1)>中文');
  assert.equal(link.href, '/download?path=' + encodeURIComponent('/Books/中文 &?.epub'));
  assert.equal(row.children[1].textContent, '"<script>"');
  nodes.get('library-query').value = '中文搜索';
  nodes.get('library-query').listeners.compositionstart();
  nodes.get('library-search-form').listeners.submit({preventDefault(){}}); await settle();
  assert.equal(calls.length, 1);
  nodes.get('library-query').listeners.compositionend();
  nodes.get('library-sort').value = 'title'; nodes.get('library-direction').value = 'asc';
  nodes.get('library-search-form').listeners.submit({preventDefault(){}}); await settle();
  const params = new URL('http://reader' + calls.at(-1).url).searchParams;
  assert.equal(params.get('query'), '中文搜索'); assert.equal(params.get('sort'), 'title'); assert.equal(params.get('direction'), 'asc');
  nodes.get('library-next').listeners.click(); await settle();
  assert.equal(new URL('http://reader' + calls.at(-1).url).searchParams.get('offset'), '16');
  const before = calls.length;
  nodes.get('library-rebuild').listeners.click(); await settle(); assert.equal(calls.length, before);
  confirmation = true; nodes.get('library-rebuild').listeners.click(); await settle();
  assert.equal(calls.at(-2).url, '/api/library/rebuild'); assert.equal(calls.at(-2).options.method, 'POST');
  assert.equal(new URL('http://reader' + calls.at(-1).url).searchParams.get('offset'), '0');
  pending = true; nodes.get('library-search-form').listeners.submit({preventDefault(){}}); await settle();
  assert.equal(nodes.get('library-cancel').disabled, false);
  nodes.get('library-cancel').listeners.click(); await settle();
  assert.match(nodes.get('library-status').textContent, /cancelled/);
  assert.equal(nodes.get('library-cancel').disabled, true);
  assert.ok(calls.every(call => !call.url.includes('/status')));
  console.log('Library browser IME, XSS-safe rows, pagination, explicit POST and cancellation checks passed');
})().catch(error => {console.error(error); process.exitCode = 1;});
