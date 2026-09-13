"""Run the actual embedded page script against a stalled request and recovery."""
import ast
import json
from pathlib import Path
import re
import subprocess
import sys

source = (Path(__file__).resolve().parent.parent /
          'platform/esp32_common/diagnostics.c').read_text()
page = source.split('static const char page[] =', 1)[1].split(';\n', 1)[0]
html = ''.join(ast.literal_eval(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', page))
script = html.split('<script>', 1)[1].split('</script>', 1)[0]
program = r'''
const vm = require('node:vm');
const assert = require('node:assert/strict');
const pending = new Map();
let id = 0, requests = 0;
const elements = {s: {}, l: {}};
const context = {
  AbortController, Date, Error,
  document: {getElementById: name => elements[name]},
  setTimeout: (fn, delay) => {pending.set(++id, {fn, delay}); return id;},
  clearTimeout: key => pending.delete(key),
  fetch: (url, options) => {
    assert.equal(url, '/logs');
    if (++requests === 1) return new Promise((resolve, reject) => {
      options.signal.addEventListener('abort', () => reject(Error('stalled')));
    });
    return Promise.resolve({ok: true, text: async () => '<script>untrusted log</script>'});
  }
};
vm.runInNewContext(SCRIPT, context);
assert.equal(requests, 1);
[...pending.values()].find(x => x.delay === 4000).fn();
setImmediate(() => {
  assert.match(elements.s.textContent, /Disconnected/);
  const retry = [...pending.values()].find(x => x.delay === 2000);
  assert.ok(retry, 'stalled request must schedule a retry');
  retry.fn();
  setImmediate(() => {
    assert.equal(requests, 2);
    assert.match(elements.s.textContent, /Connected/);
    assert.equal(elements.l.textContent, '<script>untrusted log</script>');
    assert.equal(elements.l.innerHTML, undefined);
    console.log('web page timeout/recovery PASS');
  });
});
'''.replace('SCRIPT', json.dumps(script))
subprocess.run([sys.argv[1] if len(sys.argv) > 1 else 'node', '-e', program], check=True)
