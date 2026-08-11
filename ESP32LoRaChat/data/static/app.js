const messagesEl = document.getElementById('messages');
const nodesEl = document.getElementById('nodes');
const nodeCountEl = document.getElementById('nodeCount');
const statusEl = document.getElementById('status');
const nameEl = document.getElementById('name');
const nodeFormEl = document.getElementById('nodeForm');
const nodeNameEl = document.getElementById('nodeName');
const saveNodeEl = document.getElementById('saveNode');
const messageEl = document.getElementById('message');
const sendEl = document.getElementById('send');
const linkPillEl = document.getElementById('linkPill');
const routeTitleEl = document.getElementById('routeTitle');
const backendValueEl = document.getElementById('backendValue');
const tabs = Array.from(document.querySelectorAll('.tab'));
const panels = Array.from(document.querySelectorAll('.panel'));
const fields = {
  node: document.getElementById('nodeValue'),
  dest: document.getElementById('destValue'),
  mode: document.getElementById('modeValue'),
  crypto: document.getElementById('cryptoValue'),
  queue: document.getElementById('queueValue'),
  txQueue: document.getElementById('txQueueValue'),
  relayQueue: document.getElementById('relayQueueValue'),
  radio: document.getElementById('radioValue'),
  signal: document.getElementById('signalValue'),
  rf: document.getElementById('rfValue'),
  aux: document.getElementById('auxValue'),
  lastPacket: document.getElementById('lastPacketValue'),
  clients: document.getElementById('clientsValue')
};
let lastMessageRender = '';
let lastNodeRender = '';
let localNodeName = '';

nameEl.value = localStorage.getItem('loraChatName') || 'User';

function timeLabel(ms) {
  if (ms == null) return '--';
  const totalSeconds = Math.max(0, Math.floor(Number(ms) / 1000));
  if (totalSeconds < 60) return `${totalSeconds}s`;
  const minutes = Math.floor(totalSeconds / 60);
  if (minutes < 60) return `${minutes}m`;
  const hours = Math.floor(minutes / 60);
  return `${hours}h ${minutes % 60}m`;
}

function bandwidthLabel(value) {
  const hz = Number(value) || 0;
  if (!hz) return '--';
  if (hz >= 1000) return `${Math.round(hz / 1000)} kHz`;
  return `${hz} Hz`;
}

function setText(el, value) {
  if (!el) return;
  el.textContent = value == null || value === '' ? '--' : value;
}

function clear(el) {
  while (el.firstChild) el.removeChild(el.firstChild);
}

function div(className, text) {
  const node = document.createElement('div');
  if (className) node.className = className;
  if (text != null) node.textContent = text;
  return node;
}

function switchTab(name) {
  tabs.forEach(tab => tab.classList.toggle('active', tab.dataset.tab === name));
  panels.forEach(panel => panel.classList.toggle('active', panel.dataset.panel === name));
}

function renderMessages(items) {
  const signature = JSON.stringify(items);
  if (signature === lastMessageRender) return;
  const wasNearBottom = messagesEl.scrollHeight - messagesEl.scrollTop - messagesEl.clientHeight < 80;
  lastMessageRender = signature;
  clear(messagesEl);

  if (!items.length) {
    messagesEl.appendChild(div('empty', 'No messages yet'));
    return;
  }

  items.forEach(item => {
    const row = document.createElement('section');
    row.className = `msg ${item.outgoing ? 'out' : 'in'}`;

    const meta = div('message-meta');
    const sender = document.createElement('span');
    sender.className = 'sender';
    sender.textContent = item.sender || (item.outgoing ? 'Me' : 'Peer');
    meta.appendChild(sender);
    meta.appendChild(document.createElement('span')).textContent = timeLabel(item.ageMs);
    row.appendChild(meta);

    row.appendChild(div('body', item.text));

    const state = div(`message-state ${item.status || ''}`);
    const hopText = `${item.hops || 0} ${(item.hops || 0) === 1 ? 'hop' : 'hops'}`;
    const signalText = item.signalAvailable === false ? 'signal n/a' : `rssi ${item.rssi} dBm | snr ${item.snr}`;
    state.textContent = item.outgoing ? item.status : `${item.from} | ${signalText} | ${hopText}`;
    row.appendChild(state);

    messagesEl.appendChild(row);
  });

  if (wasNearBottom) messagesEl.scrollTop = messagesEl.scrollHeight;
}

function renderStatus(status) {
  const txQueue = Number(status.txQueue) || 0;
  const relayQueue = Number(status.relayQueue) || 0;
  const queueTotal = Number(status.queue) || 0;
  const hasPacket = status.radio && status.radio !== 'no packets';
  const signalAvailable = status.signalAvailable !== false;
  const rssi = Number(status.rssi) || 0;
  const snr = Number(status.snr) || 0;

  if (status.name) {
    localNodeName = status.name;
    if (!nodeNameEl.value || document.activeElement !== nodeNameEl) nodeNameEl.value = status.name;
  }

  setText(fields.node, status.name ? `${status.name} | ${status.node}` : status.node);
  setText(fields.dest, status.destination);
  setText(fields.mode, status.mode);
  setText(fields.crypto, status.crypto);
  setText(fields.queue, `${queueTotal} total`);
  setText(fields.txQueue, `${txQueue} pending`);
  setText(fields.relayQueue, `${relayQueue} pending`);
  setText(fields.radio, status.radio);
  setText(fields.signal, signalAvailable && hasPacket ? `${rssi} dBm | ${snr.toFixed(1)} dB` : '--');
  setText(fields.rf, status.rf || `SF${status.spreadingFactor} | ${bandwidthLabel(status.bandwidth)} | ${status.txPower} dBm | hop ${status.hopLimit}`);
  setText(fields.aux, status.auxReady === false ? 'busy' : 'ready');
  setText(fields.lastPacket, status.radio);
  setText(fields.clients, status.clients);
  setText(backendValueEl, status.backend || 'Radio backend');

  fields.signal.className = 'signal';
  if (signalAvailable && hasPacket) {
    fields.signal.classList.add(rssi < -115 ? 'bad' : rssi < -105 ? 'warn' : 'good');
  }

  routeTitleEl.textContent = status.destination === 'broadcast'
    ? 'Broadcast mesh'
    : `Direct to ${status.destination}`;
  statusEl.classList.remove('offline');
  statusEl.textContent = `${status.name || status.node} to ${status.destination} | ${status.radio}`;
  linkPillEl.classList.remove('online', 'offline');
  if (hasPacket) linkPillEl.classList.add('online');
}

function renderNodes(nodes) {
  const signature = JSON.stringify(nodes);
  if (signature === lastNodeRender) return;
  lastNodeRender = signature;
  clear(nodesEl);

  const heardCount = nodes.filter(node => node.local || node.heard).length;
  nodeCountEl.textContent = `${heardCount}/${nodes.length} active`;

  nodes.forEach(node => {
    const card = document.createElement('article');
    card.className = `node-card ${node.local ? 'local' : ''} ${!node.local && !node.heard ? 'silent' : ''}`;

    const title = div('node-title');
    const avatar = div('node-avatar', (node.name || node.address || '?').replace('Node ', '').slice(0, 1));
    const titleText = div();
    titleText.appendChild(div('node-name', node.name || node.address));
    titleText.appendChild(div('node-meta', `${node.address} | ${node.role || 'node'}`));
    title.appendChild(avatar);
    title.appendChild(titleText);
    card.appendChild(title);

    const age = node.local
      ? 'local node'
      : node.heard
        ? `last ${timeLabel(node.ageMs)} ago via ${node.lastVia || '--'}`
        : 'not heard';
    card.appendChild(div('node-meta', age));

    const stats = div('node-stats');
    [
      ['RX', node.rx || 0],
      ['TX', node.tx || 0],
      ['Relay', node.relay || 0]
    ].forEach(([label, value]) => {
      const cell = div();
      cell.appendChild(document.createElement('span')).textContent = label;
      cell.appendChild(document.createElement('strong')).textContent = value;
      stats.appendChild(cell);
    });
    card.appendChild(stats);

    const signal = node.signalAvailable === false || !node.heard
      ? 'signal n/a'
      : `${node.rssi} dBm | ${Number(node.snr || 0).toFixed(1)} dB`;
    card.appendChild(div('node-meta', `${signal} | ${node.lastHops || 0} hops`));
    nodesEl.appendChild(card);
  });
}

async function refresh() {
  try {
    const [messagesRes, statusRes, nodesRes] = await Promise.all([
      fetch('/api/messages', { cache: 'no-store' }),
      fetch('/api/status', { cache: 'no-store' }),
      fetch('/api/nodes', { cache: 'no-store' })
    ]);
    const messages = await messagesRes.json();
    const status = await statusRes.json();
    const nodes = await nodesRes.json();
    renderMessages(messages.messages || []);
    renderStatus(status);
    renderNodes(nodes.nodes || []);
  } catch (err) {
    statusEl.classList.add('offline');
    statusEl.textContent = 'ESP32 connection lost';
    linkPillEl.classList.remove('online');
    linkPillEl.classList.add('offline');
  }
}

async function loadConfig() {
  try {
    const res = await fetch('/api/config', { cache: 'no-store' });
    const config = await res.json();
    localNodeName = config.name || '';
    nodeNameEl.value = localNodeName;
  } catch (err) {
    statusEl.textContent = 'config unavailable';
  }
}

async function saveNodeName(event) {
  event.preventDefault();
  const name = nodeNameEl.value.trim();
  if (!name) return;

  saveNodeEl.disabled = true;
  try {
    const body = new URLSearchParams({ name });
    const res = await fetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body
    });
    if (!res.ok) throw new Error(await res.text());
    const saved = await res.json();
    localNodeName = saved.name || name;
    nodeNameEl.value = localNodeName;
    await refresh();
  } catch (err) {
    statusEl.textContent = 'name save rejected';
  } finally {
    saveNodeEl.disabled = false;
  }
}

async function sendMessage() {
  const sender = nameEl.value.trim() || 'User';
  const text = messageEl.value.trim();
  if (!text) return;
  localStorage.setItem('loraChatName', sender);
  sendEl.disabled = true;

  try {
    const body = new URLSearchParams({ sender, message: text });
    const res = await fetch('/api/send', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body
    });
    if (!res.ok) throw new Error(await res.text());
    messageEl.value = '';
    await refresh();
  } catch (err) {
    statusEl.textContent = 'send rejected';
  } finally {
    sendEl.disabled = false;
    messageEl.focus();
  }
}

tabs.forEach(tab => {
  tab.addEventListener('click', () => switchTab(tab.dataset.tab));
});
nodeFormEl.addEventListener('submit', saveNodeName);
sendEl.addEventListener('click', sendMessage);
messageEl.addEventListener('keydown', event => {
  if (event.key === 'Enter') sendMessage();
});
setInterval(refresh, 1000);
loadConfig();
refresh();
