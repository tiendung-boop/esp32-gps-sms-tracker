'use strict';

// Doc SMS toa do truc tiep tu 1 dien thoai Android noi USB vao may tinh nay
// (KHONG can internet/WiFi, KHONG can module SIM rieng) - dung ADB (Android
// Debug Bridge) de doc noi dung SMS trong may. Tim tin nhan khop dinh dang
// "Toa do: lat,lon" (giong GpsSmsSender.ino gui) roi day len
// nhan-sms-tu-dong-server.js (server nay PHAI dang chay truoc).
//
// CAN CHUAN BI TRUOC (mot lan):
//   1. Cai "Android Platform Tools" (co adb.exe) - tai tu trang chinh thuc
//      Google, giai nen, dam bao adb.exe nam trong PATH (hoac dat
//      ADB_PATH ben duoi tro dung file adb.exe).
//   2. Tren dien thoai: Cai dat > Gioi thieu > bam lien tiep "So build" 7
//      lan de mo "Tuy chon nha phat trien" > bat "Go loi USB" (USB
//      debugging). Noi day USB vao PC, tren dien thoai se hien hop thoai
//      "Cho phep go loi USB tu may tinh nay?" - bam Cho phep (co the tick
//      "Luon cho phep tu may tinh nay" cho lan sau khoi phai bam lai).
//   3. Chay "adb devices" (co san sau buoc 1) de kiem tra dien thoai da
//      hien ra dung trang thai "device" (khong phai "unauthorized").
//
// (2026-09-07) DA KIEM CHUNG TREN THIET BI THAT (Android, model "zeus" /
// 2201122G) - quyen doc content://sms/inbox qua "adb shell" (khong can
// root) hoat dong binh thuong, KHONG bi chan. Tim thay 1 loi that su luc
// kiem chung: "--sort date DESC" (co khoang trang) khien adb bao
// "[ERROR] Unsupported argument: DESC" tren MOI lan poll (adb shell khong
// giu duoc dau ngoac kep khi gui lenh qua dien thoai, nen "DESC" bi tach
// thanh 1 tham so rieng) - da sua ben duoi thanh "--sort date". Neu sau
// nay van gap loi "Permission denial" hoac tra ve rong du dien thoai co
// SMS moi (mot so ROM/Android khac co the chan khac voi may da test), coi
// day la gioi han cua may/ROM do - dung ban doc-toa-do-sms-tool.html (dan
// tay) hoac cach ESP32 USB (doc-sms-qua-esp32-usb.js) thay the.
//
// Chay: node doc-sms-qua-dien-thoai-adb.js

const { exec } = require('child_process');
const fs = require('fs');
const path = require('path');

const ADB_PATH = process.env.ADB_PATH || 'adb';
const SERVER_URL = process.env.SERVER_URL || 'http://127.0.0.1:8899';
const POLL_MS = 10 * 1000; // 10 giay - doc tin nhan da co san tren may, khong ton chi phi mang nhu AT+CMGL nen poll nhanh hon SmsReceiver cung duoc
const STATE_FILE = path.join(__dirname, '.adb-sms-last-seen.json');

// Ten hien thi (tuy chon) cho tung so gui toi - dien so KHONG dau '+'. Giong
// KNOWN_SENDERS trong SmsReceiver.ino/SmsReceiverUSB.ino.
const KNOWN_SENDERS = {
  // '84987654321': 'Xe may 1',
};

function runAdb(args) {
  return new Promise((resolve, reject) => {
    exec('"' + ADB_PATH + '" ' + args, { maxBuffer: 10 * 1024 * 1024 }, (err, stdout, stderr) => {
      if (err) { reject(new Error(stderr || err.message)); return; }
      resolve(stdout);
    });
  });
}

function loadLastSeen() {
  try { return JSON.parse(fs.readFileSync(STATE_FILE, 'utf8')).lastDate || 0; }
  catch (e) { return 0; }
}
function saveLastSeen(ts) {
  try { fs.writeFileSync(STATE_FILE, JSON.stringify({ lastDate: ts })); } catch (e) {}
}

function digitsOnly(s) {
  return String(s || '').replace(/\D/g, '');
}
function nameForPhone(digits) {
  return KNOWN_SENDERS[digits] || digits;
}

function isValidLatLon(lat, lon) {
  return Number.isFinite(lat) && Number.isFinite(lon) && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180;
}
function findCoordinates(text) {
  const re = /(-?\d{1,3}\.\d+)\s*,\s*(-?\d{1,3}\.\d+)/g;
  let m;
  while ((m = re.exec(text)) !== null) {
    const lat = parseFloat(m[1]), lon = parseFloat(m[2]);
    if (isValidLatLon(lat, lon)) return { lat, lon };
  }
  return null;
}

// "adb shell content query" in moi dong dang:
//   Row: 0 address=+84987654321, body=Toa do: 16.047238,108.219700, date=1691740800000
// body co the chua dau ',' (chinh la giua lat/lon!) nen KHONG the tach bang
// split(', ') don gian - dung regex neo vao ten truong, ".*" tham lam cho
// body se tu dung dung truoc ", date=<so>" cuoi dong.
const ROW_RE = /^Row:\s*\d+\s+address=(.*?),\s*body=(.*),\s*date=(\d+)\s*$/;

function parseRows(stdout) {
  const rows = [];
  stdout.split('\n').forEach((line) => {
    const m = ROW_RE.exec(line.trim());
    if (!m) return;
    rows.push({ address: m[1], body: m[2], date: Number(m[3]) });
  });
  return rows;
}

async function pushToServer(deviceId, name, lat, lon) {
  try {
    const res = await fetch(SERVER_URL + '/push', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ deviceId, name, lat, lon, source: 'phone-adb' })
    });
    const data = await res.json();
    if (!data.ok) console.error('Server tu choi:', data.error);
  } catch (e) {
    console.error('Khong goi duoc server local (' + SERVER_URL + ') - da chay nhan-sms-tu-dong-server.js chua? Loi:', e.message);
  }
}

let lastSeen = loadLastSeen();

async function pollOnce() {
  let stdout;
  try {
    // "--sort date" (khong dung khoang trang) - "date DESC" tung bi loi vi
    // adb shell KHONG giu duoc dau ngoac kep khi gui lenh sang dien thoai,
    // nen "DESC" bi tach thanh 1 tham so rieng, dien thoai bao loi
    // "[ERROR] Unsupported argument: DESC" va script tuong nhu khong doc
    // duoc SMS nao (that ra la moi lan poll deu that bai). Khong sao vi
    // ket qua da duoc sap xep lai theo thoi gian ngay ben duoi (dong
    // ".sort((a, b) => a.date - b.date)").
    stdout = await runAdb('shell content query --uri content://sms/inbox --projection address:body:date --sort date');
  } catch (e) {
    console.error('Loi doc SMS qua adb:', e.message.trim());
    console.error('Kiem tra: dien thoai da noi USB + bat "Go loi USB" + bam "Cho phep" tren dien thoai chua? Chay "adb devices" de kiem tra.');
    return;
  }
  const rows = parseRows(stdout).filter((r) => r.date > lastSeen).sort((a, b) => a.date - b.date);
  if (rows.length === 0) return;

  for (const row of rows) {
    const coords = findCoordinates(row.body);
    const digits = digitsOnly(row.address);
    if (coords) {
      const deviceId = 'sms-' + digits;
      const name = nameForPhone(digits);
      console.log('Nhan toa do: ' + name + ' (' + deviceId + ') -> ' + coords.lat.toFixed(6) + ', ' + coords.lon.toFixed(6));
      await pushToServer(deviceId, name, coords.lat, coords.lon);
    }
    lastSeen = Math.max(lastSeen, row.date);
  }
  saveLastSeen(lastSeen);
}

async function main() {
  try {
    const devices = await runAdb('devices');
    console.log(devices.trim());
    if (!/\tdevice\s*$/m.test(devices)) {
      console.log('Khong thay dien thoai o trang thai "device" san sang. Kiem tra day USB + "Cho phep go loi USB" tren man hinh dien thoai.');
    }
  } catch (e) {
    console.error('Khong chay duoc adb - da cai Android Platform Tools va them vao PATH chua?', e.message.trim());
    process.exit(1);
  }

  console.log('Dang doc SMS tu dong tu dien thoai qua ADB (kiem tra moi ' + (POLL_MS / 1000) + 's)...');
  console.log('Day len server: ' + SERVER_URL + ' (dam bao nhan-sms-tu-dong-server.js dang chay).');
  console.log('Dong cua so nay de dung.');

  pollOnce();
  setInterval(pollOnce, POLL_MS);
}

main();
