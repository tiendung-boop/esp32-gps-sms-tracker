'use strict';

// Relay cho vi-tri-nhieu-thiet-bi: nhan vi tri tu cac may "phat" va phat lai
// cho cac trang "thu" (thu-vi-tri-tool.html) dang xem cung 1 ma phong. Khong
// luu gi xuong dia (chi giu trong RAM), reset het khi restart server - dung
// cho muc dich theo doi thoi gian thuc, khong phai kho luu lich su.
//
// 2 duong nhap vi tri cho ben "phat", cung ghi vao chung 1 cho (applyPos):
//   1. WebSocket /ws (dung cho phat-vi-tri-tool.html - dien thoai, dong ho
//      thong minh... bat ky thiet bi nao co trinh duyet).
//   2. HTTP GET/POST /api/pos (dung cho thiet bi KHONG co trinh duyet: mach
//      vi dieu khien nho nhu ESP32/ESP8266 gan module GPS, script tren may
//      tinh nhung, SIM module chi ho tro AT+HTTP don gian...). Khong can giu
//      ket noi lien tuc - moi lan co toa do moi thi ban 1 request roi thoi,
//      hop voi thiet bi it tai nguyen / muon tiet kiem pin. Xem HUONG_DAN.txt
//      muc "CONG CU THEO DOI VI TRI NHIEU THIET BI" de biet dinh dang request.

const http = require('http');
const fs = require('fs');
const path = require('path');
const os = require('os');
const crypto = require('crypto');
const { WebSocketServer, WebSocket } = require('ws');

const PORT = process.env.PORT || 8877; // 8080 hay bi phan mem khac (vd phan mem bao ve/antivirus) chiem san tren Windows
const HEARTBEAT_MS = 30000;      // chu ky ping kiem tra ket noi con song
const JOIN_TIMEOUT_MS = 10000;   // phai gui "join" trong 10s dau, khong thi ngat
const MIN_POS_INTERVAL_MS = 800; // chan spam: 1 sender toi da ~1.25 msg/giay
const MAX_PAYLOAD_BYTES = 4096;  // 1 ban tin vi tri chi vai tram byte la du
const MAX_CLOCK_SKEW_MS = 5 * 60 * 1000; // +/-5 phut - chan replay request /api/pos cu

// Secret dung chung giua server nay va tung thiet bi gui vi tri qua HTTP API
// (vd ESP32/GpsPosSender.ino - xem API_SECRET trong secrets.h cua sketch do).
// KHONG hardcode gia tri o day - luon doc tu bien moi truong, giong tinh
// than "khong commit secret vao repo" da ap dung cho secrets.h phia ESP32.
const API_SECRET = process.env.API_SECRET;
if (!API_SECRET) {
  console.error('LOI: thieu bien moi truong API_SECRET.');
  console.error('  Vi du (PowerShell):  $env:API_SECRET = "<gia-tri-dai-ngau-nhien>"; node server.js');
  console.error('  Gia tri nay phai KHOP CHINH XAC voi API_SECRET trong secrets.h cua tung thiet bi ESP32.');
  process.exit(1);
}

// "View key" rieng cho tung phong, tinh tu API_SECRET - biet view key KHONG
// the suy nguoc ra API_SECRET (HMAC 1 chieu), nen lo view key cua 1 nguoi
// xem KHONG cho phep gia mao thanh thiet bi gui vi tri that. Dung
// compute-view-key.js de lay view key can chia se cho 1 phong cu the.
function computeViewKey(room) {
  return crypto.createHmac('sha256', API_SECRET).update('view:' + room).digest('hex');
}

function timingSafeStringEqual(a, b) {
  const bufA = Buffer.from(String(a == null ? '' : a), 'utf8');
  const bufB = Buffer.from(String(b == null ? '' : b), 'utf8');
  if (bufA.length !== bufB.length) return false;
  return crypto.timingSafeEqual(bufA, bufB);
}

// Xac minh chu ky HMAC-SHA256 cua 1 request /api/pos. Chuoi ky phai khop
// CHINH XAC cach ESP32 dung (xem hmacSha256Hex()/sendPosition() trong
// GpsPosSender.ino): room|deviceId|ts|lat|lon, dung dung gia tri CHUOI da
// nhan duoc (khong format lai so) de tranh lech dinh dang float.
function verifySig(room, deviceId, ts, lat, lon, sig) {
  if (!sig || !ts) return false;
  const tsNum = Number(ts);
  if (!Number.isFinite(tsNum)) return false;
  if (Math.abs(Date.now() - tsNum * 1000) > MAX_CLOCK_SKEW_MS) return false;
  const toSign = room + '|' + deviceId + '|' + ts + '|' + lat + '|' + lon;
  const expected = crypto.createHmac('sha256', API_SECRET).update(toSign).digest('hex');
  return timingSafeStringEqual(expected, sig);
}

const STATIC_FILES = {
  '/thu': path.join(__dirname, '..', 'thu-vi-tri-tool.html'),
  '/phat': path.join(__dirname, '..', 'phat-vi-tri-tool.html')
};

// Thu muc phuc vu tinh theo tien to URL - dung cho ban do offline (pmtiles)
// va cac thu vien JS/CSS da tai ve local (Leaflet/markercluster/protomaps-leaflet)
// thay vi load tu CDN, de dashboard /thu chay duoc hoan toan khong can internet.
const STATIC_DIRS = {
  '/tiles/': path.join(__dirname, 'tiles'),
  '/vendor/': path.join(__dirname, '..', 'vendor')
};

const MIME_TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.png': 'image/png',
  '.map': 'application/json; charset=utf-8',
  '.pmtiles': 'application/octet-stream'
};

// Phuc vu 1 file tinh tu thu muc goc da biet truoc (baseDir), co ho tro HTTP
// Range (bat buoc cho .pmtiles - thu vien doc pmtiles doc tung doan byte rieng
// le trong file, khong tai nguyen file). Chan path traversal bang cach kiem
// tra duong dan sau resolve van nam trong baseDir.
function serveStaticFile(req, res, baseDir, relPath) {
  const filePath = path.normalize(path.join(baseDir, relPath));
  if (filePath !== baseDir && !filePath.startsWith(baseDir + path.sep)) {
    res.writeHead(400); res.end('Bad request'); return;
  }
  fs.stat(filePath, (err, stat) => {
    if (err || !stat.isFile()) {
      res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' }); res.end('Khong tim thay file.'); return;
    }
    const ext = path.extname(filePath).toLowerCase();
    const contentType = MIME_TYPES[ext] || 'application/octet-stream';
    const range = req.headers.range;

    if (range) {
      const m = /^bytes=(\d*)-(\d*)$/.exec(range);
      const start = m && m[1] ? parseInt(m[1], 10) : 0;
      const end = m && m[2] ? parseInt(m[2], 10) : stat.size - 1;
      if (!m || start > end || end >= stat.size) {
        res.writeHead(416, { 'Content-Range': 'bytes */' + stat.size }); res.end(); return;
      }
      res.writeHead(206, {
        'Content-Type': contentType,
        'Content-Length': end - start + 1,
        'Content-Range': 'bytes ' + start + '-' + end + '/' + stat.size,
        'Accept-Ranges': 'bytes'
      });
      fs.createReadStream(filePath, { start, end }).pipe(res);
      return;
    }

    res.writeHead(200, { 'Content-Type': contentType, 'Content-Length': stat.size, 'Accept-Ranges': 'bytes' });
    fs.createReadStream(filePath).pipe(res);
  });
}

// ---------- HTTP: phuc vu file HTML/tinh + API nhan vi tri tho ----------
const server = http.createServer((req, res) => {
  let parsedUrl;
  try {
    parsedUrl = new URL(req.url, 'http://localhost');
  } catch (e) {
    res.writeHead(400); res.end('Bad request'); return;
  }
  let pathname = parsedUrl.pathname;

  if (pathname === '/api/pos') { handleApiPos(req, res, parsedUrl); return; }

  for (const prefix in STATIC_DIRS) {
    if (pathname.startsWith(prefix)) {
      serveStaticFile(req, res, STATIC_DIRS[prefix], pathname.slice(prefix.length));
      return;
    }
  }

  if (pathname === '/') pathname = '/thu';
  const file = STATIC_FILES[pathname];
  if (!file) { res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' }); res.end('Khong tim thay trang.'); return; }
  fs.readFile(file, (err, data) => {
    if (err) { res.writeHead(500, { 'Content-Type': 'text/plain; charset=utf-8' }); res.end('Loi doc file: ' + err.message); return; }
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    res.end(data);
  });
});

// ---------- WebSocket: relay theo phong ----------
const wss = new WebSocketServer({ server, path: '/ws', maxPayload: MAX_PAYLOAD_BYTES });

// roomCode -> { clients: Set<ws>, senders: Map<deviceId, {ws, name, lat, lon, accuracy, speed, heading, ts}> }
const rooms = new Map();

function getRoom(code, create) {
  let room = rooms.get(code);
  if (!room && create) {
    room = { clients: new Set(), senders: new Map() };
    rooms.set(code, room);
  }
  return room;
}

function dropRoomIfEmpty(code) {
  const room = rooms.get(code);
  if (room && room.clients.size === 0) rooms.delete(code);
}

function send(ws, obj) {
  if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(obj));
}

function broadcast(room, obj, exceptWs) {
  const payload = JSON.stringify(obj);
  room.clients.forEach((client) => {
    if (client === exceptWs) return;
    if (client.readyState === WebSocket.OPEN) client.send(payload);
  });
}

function cleanString(v, maxLen) {
  if (typeof v !== 'string') return '';
  return v.trim().slice(0, maxLen);
}

function handleJoin(ws, msg) {
  const role = msg.role === 'sender' || msg.role === 'viewer' ? msg.role : null;
  const room = cleanString(msg.room, 64);
  const deviceId = cleanString(msg.deviceId, 100);
  let name = cleanString(msg.name, 60);

  if (!role || !room || !deviceId) {
    send(ws, { type: 'error', message: 'join thieu role/room/deviceId hop le.' });
    ws.close(1008, 'invalid join');
    return;
  }

  // Xem (role=viewer, dung cho /thu) can 1 "view key" rieng, khac voi ma
  // phong - nguoi chi co link/ma phong (vi du lam ro tu 1 thiet bi gui)
  // khong tu dong xem duoc danh sach vi tri neu khong co view key nay.
  // Sender (dien thoai/tablet qua phat-vi-tri-tool.html) van chi can biet
  // ma phong, giu nguyen thiet ke don gian da chon truoc do cho duong nay.
  if (role === 'viewer') {
    const viewKey = cleanString(msg.viewKey, 128);
    if (!timingSafeStringEqual(computeViewKey(room), viewKey)) {
      send(ws, { type: 'error', message: 'view key khong hop le.' });
      ws.close(1008, 'invalid view key');
      return;
    }
  }

  if (!name) name = 'Thiet bi ' + deviceId.slice(0, 6);

  ws.joined = true;
  ws.role = role;
  ws.room = room;
  ws.deviceId = deviceId;
  ws.name = name;

  const r = getRoom(room, true);
  r.clients.add(ws);

  if (role === 'sender') {
    const existing = r.senders.get(deviceId) || {};
    r.senders.set(deviceId, Object.assign({}, existing, { ws, name }));
  } else {
    const devices = [];
    r.senders.forEach((d, id) => {
      if (d.lat == null) return; // sender da join nhung chua co fix GPS nao
      devices.push({ deviceId: id, name: d.name, lat: d.lat, lon: d.lon, accuracy: d.accuracy, speed: d.speed, heading: d.heading, ts: d.ts });
    });
    send(ws, { type: 'roster', devices: devices });
  }

  send(ws, { type: 'joined', room: room });
}

// Ghi 1 vi tri moi vao room.senders + phat cho cac client dang xem (viewer).
// Dung chung cho ca 2 duong nhap: WebSocket (ownerWs la ket noi gui) va HTTP
// API (ownerWs = null, vi request khong phai 1 ket noi lau dai). Rate-limit
// gan theo deviceId (khong gan theo ws) de dung nhat cho ca 2 duong - 1 thiet
// bi gui qua nhanh du bang WS hay bang HTTP deu bi chan nhu nhau.
function applyPos(room, data, ownerWs) {
  const now = Date.now();
  const entry = room.senders.get(data.deviceId) || {};
  if (entry.lastPosAt && now - entry.lastPosAt < MIN_POS_INTERVAL_MS) return false;

  entry.lastPosAt = now;
  entry.name = data.name;
  entry.lat = data.lat; entry.lon = data.lon;
  entry.accuracy = data.accuracy; entry.speed = data.speed; entry.heading = data.heading; entry.ts = data.ts;
  if (ownerWs) entry.ws = ownerWs;
  room.senders.set(data.deviceId, entry);

  broadcast(room, {
    type: 'pos', deviceId: data.deviceId, name: data.name,
    lat: data.lat, lon: data.lon, accuracy: data.accuracy, speed: data.speed, heading: data.heading, ts: data.ts
  }, ownerWs || null);
  return true;
}

function parsePosFields(src) {
  const lat = Number(src.lat);
  const lon = Number(src.lon);
  if (!Number.isFinite(lat) || !Number.isFinite(lon)) return null;
  return {
    lat: lat,
    lon: lon,
    accuracy: Number.isFinite(Number(src.accuracy)) ? Number(src.accuracy) : null,
    speed: Number.isFinite(Number(src.speed)) ? Number(src.speed) : null,
    heading: Number.isFinite(Number(src.heading)) ? Number(src.heading) : null,
    ts: Number.isFinite(Number(src.ts)) ? Number(src.ts) : Date.now()
  };
}

function handlePos(ws, msg) {
  if (ws.role !== 'sender') return;
  const fields = parsePosFields(msg);
  if (!fields) return;
  const room = rooms.get(ws.room);
  if (!room) return;
  applyPos(room, Object.assign({ deviceId: ws.deviceId, name: ws.name }, fields), ws);
}

// ---------- HTTP API: /api/pos - cho thiet bi khong co trinh duyet ----------
// Chap nhan GET (query string, tuong thich toi da voi module/mach don gian
// chi lam duoc HTTP GET) hoac POST (JSON body). Bao mat cung 1 co che ma
// phong nhu WebSocket - khong can dang ky rieng.
function readJsonBody(req, maxBytes, cb) {
  let size = 0;
  const chunks = [];
  let failed = false;
  req.on('data', (chunk) => {
    if (failed) return;
    size += chunk.length;
    if (size > maxBytes) { failed = true; cb(new Error('body qua lon')); req.destroy(); return; }
    chunks.push(chunk);
  });
  req.on('end', () => {
    if (failed) return;
    const text = Buffer.concat(chunks).toString('utf8').trim();
    if (!text) return cb(null, {});
    try { cb(null, JSON.parse(text)); } catch (e) { cb(new Error('JSON khong hop le')); }
  });
  req.on('error', (e) => { if (!failed) { failed = true; cb(e); } });
}

function handleApiPos(req, res, parsedUrl) {
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');

  function respond(status, obj) {
    res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8' });
    res.end(JSON.stringify(obj));
  }

  if (req.method === 'OPTIONS') { res.writeHead(204); res.end(); return; }
  if (req.method !== 'GET' && req.method !== 'POST') { respond(405, { ok: false, error: 'chi ho tro GET hoac POST' }); return; }

  function handleParams(src) {
    const room = cleanString(src.room, 64);
    const deviceId = cleanString(src.deviceId, 100);
    let name = cleanString(src.name, 60);
    if (!room || !deviceId) { respond(400, { ok: false, error: 'thieu room hoac deviceId' }); return; }

    // sig/ts bat buoc tu ban HMAC nay tro di - chan gia mao/replay tren
    // duong HTTP (thiet bi khong trinh duyet). Dung dung gia tri CHUOI lat/
    // lon nhu ESP32 da ky, khong dung so da parse (tranh lech dinh dang).
    if (!verifySig(room, deviceId, src.ts, src.lat, src.lon, src.sig)) {
      respond(401, { ok: false, error: 'thieu hoac sai sig/ts (xem API_SECRET)' });
      return;
    }

    const fields = parsePosFields(src);
    if (!fields) { respond(400, { ok: false, error: 'lat/lon khong hop le' }); return; }
    if (!name) name = 'Thiet bi ' + deviceId.slice(0, 6);

    const room_ = getRoom(room, true);
    const accepted = applyPos(room_, Object.assign({ deviceId, name }, fields), null);
    respond(200, { ok: true, throttled: !accepted });
  }

  if (req.method === 'GET') {
    const params = {};
    parsedUrl.searchParams.forEach((v, k) => { params[k] = v; });
    handleParams(params);
  } else {
    readJsonBody(req, MAX_PAYLOAD_BYTES, (err, data) => {
      if (err) { respond(400, { ok: false, error: err.message }); return; }
      handleParams(data || {});
    });
  }
}

function handleClose(ws) {
  if (!ws.joined) return;
  const room = rooms.get(ws.room);
  if (!room) return;
  room.clients.delete(ws);

  if (ws.role === 'sender') {
    const entry = room.senders.get(ws.deviceId);
    // chi xoa neu entry con dang thuoc ve chinh ket noi nay - tranh truong hop
    // reconnect nhanh (ket noi moi da join) roi ket noi cu moi ban su kien close
    if (entry && entry.ws === ws) {
      room.senders.delete(ws.deviceId);
      broadcast(room, { type: 'leave', deviceId: ws.deviceId }, ws);
    }
  }
  dropRoomIfEmpty(ws.room);
}

wss.on('connection', (ws) => {
  ws.isAlive = true;
  ws.joined = false;

  const joinTimer = setTimeout(() => {
    if (!ws.joined) ws.close(1008, 'join timeout');
  }, JOIN_TIMEOUT_MS);

  ws.on('pong', () => { ws.isAlive = true; });

  ws.on('message', (raw) => {
    let msg;
    try { msg = JSON.parse(raw); } catch (e) { return; }
    if (!msg || typeof msg.type !== 'string') return;

    if (msg.type === 'join') { handleJoin(ws, msg); clearTimeout(joinTimer); return; }
    if (!ws.joined) return; // bo qua moi thu truoc khi join hop le

    if (msg.type === 'pos') { handlePos(ws, msg); return; }
    if (msg.type === 'bye') { ws.close(1000, 'bye'); return; }
  });

  ws.on('close', () => { clearTimeout(joinTimer); handleClose(ws); });
  ws.on('error', () => {}); // 'close' se tu ban sau loi
});

const heartbeat = setInterval(() => {
  wss.clients.forEach((ws) => {
    if (ws.isAlive === false) return ws.terminate();
    ws.isAlive = false;
    ws.ping();
  });
}, HEARTBEAT_MS);
wss.on('close', () => clearInterval(heartbeat));

function listLanAddresses() {
  const nets = os.networkInterfaces();
  const out = [];
  Object.keys(nets).forEach((name) => {
    (nets[name] || []).forEach((net) => {
      if (net.family === 'IPv4' && !net.internal) out.push(net.address);
    });
  });
  return out;
}

server.listen(PORT, () => {
  console.log('Vi tri server dang chay tai cong ' + PORT);
  console.log('  Trang thu (dashboard):  http://localhost:' + PORT + '/thu');
  console.log('  Trang phat (beacon):    http://localhost:' + PORT + '/phat');
  console.log('  API nhan vi tri tho:    GET/POST http://localhost:' + PORT + '/api/pos (cho mach/thiet bi khong co trinh duyet)');
  const lan = listLanAddresses();
  if (lan.length) {
    console.log('Cac thiet bi cung mang LAN/WiFi co the mo:');
    lan.forEach((ip) => console.log('  http://' + ip + ':' + PORT + '/phat  (va /thu)'));
  }
  console.log('Luu y: GPS tren dien thoai can HTTPS (tru localhost). Xem HUONG_DAN.txt de trien khai qua HTTPS/WSS that su.');
  console.log('De lay view key cho 1 phong (bat buoc de mo /thu): node compute-view-key.js <ma_phong>');
});
