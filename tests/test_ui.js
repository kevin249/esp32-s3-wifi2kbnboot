// 使用页面真实脚本验证键鼠事件，不连接真实设备或触发主板按键。
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const test = require('node:test');

function page() {
  const source = fs.readFileSync('firmware/main/index.html', 'utf8').match(/<script>([\s\S]*?)<\/script>/)[1];
  const elements = new Map(), calls = [], intervals = [];
  class Element {
    constructor() { this.events = {}; this.children = []; this.value = 'test-access-token'; }
    addEventListener(name, callback) { this.events[name] = callback; }
    replaceChildren() { this.children = []; }
    append(item) { this.children.push(item); }
    setPointerCapture() {}
  }
  const get = id => { if (!elements.has(id)) elements.set(id, new Element()); return elements.get(id); };
  let deferMouse = false, finishMouse;
  const context = vm.createContext({
    document: { getElementById: get, createElement: () => new Element() },
    window: { addEventListener() {} },
    URL: { createObjectURL: () => 'blob:test', revokeObjectURL() {} },
    setInterval: (callback, ms) => intervals.push({callback, ms}),
    setTimeout: callback => callback(),
    fetch: async (path, options) => {
      calls.push({path, body: options.body ? JSON.parse(options.body) : null});
      if (deferMouse && path === '/api/hid/mouse') {
        deferMouse = false;
        await new Promise(resolve => { finishMouse = resolve; });
      }
      let body = { queued: true };
      if (path === '/api/manifest') body = {iso: null, transfer: {mode: 'host', owned: true}};
      if (path.startsWith('/api/library')) body = {path: '.', items: []};
      return {ok: true, headers: {get: () => 'application/json'}, json: async () => body};
    },
  });
  vm.runInContext(source, context);
  return {context, get, calls, defer: () => {deferMouse = true;}, finish: () => finishMouse()};
}
const keyEvent = (code, type) => ({code, type, repeat: false, preventDefault() {}});

test('键盘按下、释放和右修饰键映射正确', async () => {
  const p = page(), input = p.get('keyboard');
  input.events.keydown(keyEvent('ControlRight', 'keydown'));
  input.events.keydown(keyEvent('KeyA', 'keydown'));
  input.events.keyup(keyEvent('KeyA', 'keyup'));
  input.events.keyup(keyEvent('ControlRight', 'keyup'));
  await vm.runInContext('hidChain', p.context);
  assert.deepEqual(p.calls.map(c => c.body), [
    {modifiers: 16, keys: []}, {modifiers: 16, keys: [4]},
    {modifiers: 16, keys: []}, {modifiers: 0, keys: []},
  ]);
});
test('快速点击在网络请求未完成时也会补发鼠标释放', async () => {
  const p = page(), mouse = p.get('mouse');
  p.defer();
  mouse.events.pointerdown({buttons: 1, pointerId: 1, preventDefault() {}});
  for (let i=0;i<5;++i) await Promise.resolve();
  mouse.events.pointerup({buttons: 0, pointerId: 1, preventDefault() {}});
  p.finish();
  await vm.runInContext('hidChain', p.context);
  await Promise.resolve();
  await vm.runInContext('flushMouse()', p.context);
  assert.deepEqual(p.calls.filter(c => c.path === '/api/hid/mouse').map(c => c.body.buttons), [1, 0]);
});
test('网页主板按钮发送独立脉冲参数', async () => {
  const p = page();
  await p.get('pwr').onclick();
  await p.get('reset').onclick();
  await p.get('hold').onclick();
  assert.deepEqual(p.calls.map(c => c.body), [
    {button: 'pwr', duration_ms: 200}, {button: 'reset', duration_ms: 200},
    {button: 'pwr', duration_ms: 5000},
  ]);
});
