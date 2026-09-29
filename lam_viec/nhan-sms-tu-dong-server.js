'use strict';

// Server local cho "nhan SMS tu dong" - KHONG can internet, KHONG can
// API_SECRET (khac vi-tri-server/server.js) - chi chay tren chinh may nay,
// lam cau noi giua 1 trong 2 script doc SMS (doc-sms-qua-esp32-usb.js hoac
// doc-sms-qua-dien-thoai-adb.js) va trang nhan-sms-tu-dong-tool.html. Xem
// HUONG_DAN.txt muc "NHAN SMS TU DONG (khong can internet)".
//
// Luong du lieu: script doc SMS -> POST /push (luu vao bo nho RAM, KHONG ghi
// xuong dia) -> trang web GET /latest (poll moi vai giay) -> ve len ban do.
//
// KHONG bind 0.0.0.0 - chi 127.0.0.1, day la tool ca nhan tren 1 may.
//
// Cua so nay CAN GIU MO trong luc cho nhan SMS tu dong (khac
// chay-doc-toa-do-sms-tool.bat - do la tool xem 1 lan, tool nay la dich vu
// chay lien tuc). Dong cua so nay la tat.

const http = require('http');
const fs = require('fs');
const path = require('path');
const { exec } = require('child_process');

const PORT = process.env.PORT || 8899;
const DEFAULT_PAGE = 'nhan-sms-tu-dong-tool.html';
const DEVICES_PAGE = 'thong-tin-thiet-bi-tool.html';
const DEVICES_FILE = path.join(__dirname, 'device-directory.json');
const MAX_HISTORY = 500;

const STATIC_FILES = {
  '/': path.join(__dirname, DEFAULT_PAGE),
  ['/' + DEFAULT_PAGE]: path.join(__dirname, DEFAULT_PAGE),
  ['/' + DEVICES_PAGE]: path.join(__dirname, DEVICES_PAGE),
  '/da-nang-pmtiles-data.js': path.join(__dirname, 'da-nang-pmtiles-data.js'),
  '/da-nang-places-data.js': path.join(__dirname, 'da-nang-places-data.js'),
  '/da-nang-roads-data.js': path.join(__dirname, 'da-nang-roads-data.js')
};
const STATIC_DIRS = {
  '/vendor/': path.join(__dirname, 'vendor')
};
const MIME_TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.png': 'image/png',
  '.map': 'application/json; charset=utf-8'
};

// history: mang cac {id, deviceId, name, lat, lon, source, ts} - chi giu
// trong RAM (giong tinh than "khong luu gi xuong dia" cua vi-tri-server).
let history = [];
let nextId = 1;

// Danh ba "so dien thoai -> ten thiet bi" nhap tu thong-tin-thiet-bi-tool.html
// - KHAC voi history o tren: day la du lieu cau hinh nen CO ghi xuong dia
// (device-directory.json, cung thu muc voi server nay) de khong mat khi may
// khoi dong lai (khac tinh than RAM-only cua history vi trung du lieu vi tri
// theo doi la tam thoi, con danh ba ten thiet bi la cau hinh lau dai). Dang
// { "<so dien thoai khong dau '+'>": { name, speedKmh } } - speedKmh la toc
// do trung binh NGUOI DUNG TU NHAP rieng cho thiet bi do (tuy chon, dung de
// tinh ETA khi chi duong trong nhan-sms-tu-dong-tool.html, uu tien hon uoc
// luong chung theo loai duong - xem resolvePersonalSpeedKmh() trong file
// do). (2026-09-03) Truoc do gia tri chi la 1 chuoi ten thang, khong phai
// object - normalizeDirectoryEntry() chuyen doi nguoc (van doc duoc file
// device-directory.json cu) thanh dang object nay ngay luc nap, de moi noi
// khac trong file chi can lam viec voi 1 dang duy nhat.
function normalizeDirectoryEntry(v) {
  if (typeof v === 'string') return { name: v };
  if (v && typeof v === 'object' && !Array.isArray(v)) return { name: String(v.name || ''), speedKmh: v.speedKmh };
  return { name: '' };
}
let deviceDirectory = {};
try {
  const raw = JSON.parse(fs.readFileSync(DEVICES_FILE, 'utf8'));
  if (raw && typeof raw === 'object' && !Array.isArray(raw)) {
    for (const phone in raw) deviceDirectory[phone] = normalizeDirectoryEntry(raw[phone]);
  }
} catch (e) { deviceDirectory = {}; }

function saveDeviceDirectory() {
  fs.writeFile(DEVICES_FILE, JSON.stringify(deviceDirectory), (err) => {
    if (err) console.error('Loi ghi ' + DEVICES_FILE + ':', err.message);
  });
}

function serveStaticFile(req, res, baseDir, relPath) {
  const filePath = path.normalize(path.join(baseDir, relPath));
  if (filePath !== baseDir && !filePath.startsWith(baseDir + path.sep)) {
    res.writeHead(400); res.end('Bad request'); return;
  }
  fs.readFile(filePath, (err, data) => {
    if (err) { res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' }); res.end('Khong tim thay file.'); return; }
    const ext = path.extname(filePath).toLowerCase();
    res.writeHead(200, { 'Content-Type': MIME_TYPES[ext] || 'application/octet-stream' });
    res.end(data);
  });
}

function readJsonBody(req, cb, maxBytes) {
  maxBytes = maxBytes || 8192;
  let body = '';
  let failed = false;
  req.on('data', (chunk) => {
    body += chunk;
    if (body.length > maxBytes) { failed = true; cb(new Error('body qua lon')); req.destroy(); }
  });
  req.on('end', () => {
    if (failed) return;
    try { cb(null, body ? JSON.parse(body) : {}); } catch (e) { cb(new Error('JSON khong hop le')); }
  });
}

function isValidLatLon(lat, lon) {
  return Number.isFinite(lat) && Number.isFinite(lon) && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180;
}

const server = http.createServer((req, res) => {
  let parsedUrl;
  try { parsedUrl = new URL(req.url, 'http://localhost'); }
  catch (e) { res.writeHead(400); res.end('Bad request'); return; }
  const pathname = parsedUrl.pathname;

  if (pathname === '/push' && req.method === 'POST') {
    readJsonBody(req, (err, data) => {
      res.setHeader('Content-Type', 'application/json; charset=utf-8');
      if (err) { res.writeHead(400); res.end(JSON.stringify({ ok: false, error: err.message })); return; }
      const lat = Number(data.lat), lon = Number(data.lon);
      if (!isValidLatLon(lat, lon)) { res.writeHead(400); res.end(JSON.stringify({ ok: false, error: 'lat/lon khong hop le' })); return; }
      const deviceId = String(data.deviceId || 'unknown').slice(0, 100);
      const name = String(data.name || deviceId).slice(0, 100);
      const source = String(data.source || '').slice(0, 40);
      // (2026-09-08) Cho phep client tu goi ts (vd doc-sms-qua-esp32-usb.js
      // tinh ts lui ve qua khu vai chuc giay tu secondsAgo cua tung diem
      // trong 1 SMS gop nhieu diem - xem SmsReceiverUSB38.ino) thay vi luon
      // ep Date.now() (luc server nhan duoc request, co the sai lech ca phut
      // so voi luc GPS thuc su lay mau). Sanity-check: phai la so hop le,
      // khong qua xa trong tuong lai (5s du cho lech dong ho nho) - client
      // gui ts loi/khong gui thi lui ve Date.now() nhu truoc.
      const tsRaw = Number(data.ts);
      const ts = (Number.isFinite(tsRaw) && tsRaw > 0 && tsRaw <= Date.now() + 5000) ? tsRaw : Date.now();
      const entry = { id: nextId++, deviceId, name, lat, lon, source, ts };
      history.push(entry);
      if (history.length > MAX_HISTORY) history.shift();
      console.log('[push] ' + entry.name + ' (' + entry.source + ') -> ' + lat.toFixed(6) + ',' + lon.toFixed(6));
      res.writeHead(200); res.end(JSON.stringify({ ok: true, id: entry.id }));
    });
    return;
  }

  if (pathname === '/latest' && req.method === 'GET') {
    const afterId = Number(parsedUrl.searchParams.get('afterId')) || 0;
    const items = history.filter((h) => h.id > afterId);
    res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
    res.end(JSON.stringify({ ok: true, items }));
    return;
  }

  if (pathname === '/devices' && req.method === 'GET') {
    res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
    res.end(JSON.stringify({ ok: true, devices: deviceDirectory }));
    return;
  }

  if (pathname === '/devices' && req.method === 'POST') {
    readJsonBody(req, (err, data) => {
      res.setHeader('Content-Type', 'application/json; charset=utf-8');
      if (err) { res.writeHead(400); res.end(JSON.stringify({ ok: false, error: err.message })); return; }
      const input = data.devices;
      if (!input || typeof input !== 'object' || Array.isArray(input)) { res.writeHead(400); res.end(JSON.stringify({ ok: false, error: 'devices phai la object { sdt: {name, speedKmh} }' })); return; }
      const cleaned = {};
      let count = 0;
      for (const phone in input) {
        if (!/^\d{1,20}$/.test(phone)) continue;
        if (++count > 2000) break; // gioi han hop ly, tranh file phinh to bat thuong
        const entry = normalizeDirectoryEntry(input[phone]);
        const out = { name: entry.name.slice(0, 200) };
        const speed = Number(entry.speedKmh);
        if (Number.isFinite(speed) && speed > 0 && speed <= 300) out.speedKmh = speed; // bo neu khong hop le/de trong, thay vi luu gia tri rac
        cleaned[phone] = out;
      }
      deviceDirectory = cleaned;
      saveDeviceDirectory();
      res.writeHead(200); res.end(JSON.stringify({ ok: true, count: Object.keys(cleaned).length }));
    }, 1024 * 1024); // danh ba co the toi hang nghin thiet bi - 8KB mac dinh cua readJsonBody qua nho
    return;
  }

  for (const prefix in STATIC_DIRS) {
    if (pathname.startsWith(prefix)) { serveStaticFile(req, res, STATIC_DIRS[prefix], pathname.slice(prefix.length)); return; }
  }
  const file = STATIC_FILES[pathname];
  if (!file) { res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' }); res.end('Khong tim thay trang.'); return; }
  fs.readFile(file, (err, data) => {
    if (err) { res.writeHead(500, { 'Content-Type': 'text/plain; charset=utf-8' }); res.end('Loi doc file: ' + err.message); return; }
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    res.end(data);
  });
});

function openBrowser() {
  exec('start "" "http://127.0.0.1:' + PORT + '/' + DEFAULT_PAGE + '"');
}

server.on('error', (err) => {
  if (err.code === 'EADDRINUSE') {
    console.log('Server da chay san tai cong ' + PORT + ' - mo lai trinh duyet.');
    openBrowser();
    process.exit(0);
  }
  console.error('Loi server:', err.message);
  process.exit(1);
});

server.listen(PORT, '127.0.0.1', () => {
  console.log('Server nhan SMS tu dong dang chay tai http://127.0.0.1:' + PORT);
  console.log('Chay THEM 1 trong 2 script sau (tuy phan cung ban dung):');
  console.log('  node doc-sms-qua-esp32-usb.js <COM_port>');
  console.log('  node doc-sms-qua-dien-thoai-adb.js');
  console.log('Giu cua so nay mo trong luc cho nhan SMS. Dong cua so nay la tat.');
  openBrowser();
});
