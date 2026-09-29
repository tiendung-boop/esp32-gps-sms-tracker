'use strict';

// Doc toa do tu bo SmsReceiverUSB (ESP32+A7680C, xem
// SmsReceiverUSB/SmsReceiverUSB.ino) cam truc tiep vao may tinh nay qua day
// USB - KHONG can internet/WiFi. Doc tung dong tu cong COM, tim dong dinh
// dang "TOADO|deviceId|ten|lat|lon|secondsAgo" ma sketch do in ra, roi day
// len nhan-sms-tu-dong-server.js (server nay PHAI dang chay truoc - xem
// HUONG_DAN.txt muc "NHAN SMS TU DONG"). secondsAgo (them 2026-09-08) la so
// giay truoc luc gui SMS ma diem do THAT SU duoc GPS lay mau tren xe (1 SMS
// co the gop nhieu diem lay mau cach nhau vai chuc giay) - dung de tinh ts
// gan dung hon la gan chung "bay gio" cho ca 6 diem trong 1 SMS.
//
// Chay:
//   node doc-sms-qua-esp32-usb.js COM5
// (khong truyen cong COM -> script tu liet ke cac cong dang co de ban chon)
//
// Luu y: ESP32 in ra ca cac dong debug khac (AT command, phan hoi module SIM)
// ngoai dong "TOADO|..." - script nay bo qua het, chi xu ly dung dong khop
// dinh dang do.

const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');

const BAUD_RATE = 115200;
const SERVER_URL = process.env.SERVER_URL || 'http://127.0.0.1:8899';
const portArg = process.argv[2];

async function listPortsAndExit() {
  const ports = await SerialPort.list();
  console.log('Chua chi ro cong COM. Cach dung: node doc-sms-qua-esp32-usb.js <COM_x>\n');
  if (ports.length === 0) {
    console.log('Khong tim thay cong COM nao dang cam. Kiem tra day USB / driver CP210x-CH340.');
  } else {
    console.log('Cac cong COM hien co tren may:');
    ports.forEach((p) => console.log('  ' + p.path + (p.manufacturer ? '  (' + p.manufacturer + ')' : '')));
    console.log('\nChon dung cong ESP32 dang cam (thuong la cong moi xuat hien khi vua cam day USB vao).');
  }
  process.exit(1);
}

async function pushToServer(deviceId, name, lat, lon, ts) {
  try {
    const res = await fetch(SERVER_URL + '/push', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ deviceId, name, lat, lon, ts, source: 'esp32-usb' })
    });
    const data = await res.json();
    if (!data.ok) console.error('Server tu choi:', data.error);
  } catch (e) {
    console.error('Khong goi duoc server local (' + SERVER_URL + ') - da chay nhan-sms-tu-dong-server.js chua? Loi:', e.message);
  }
}

function handleLine(line) {
  line = line.trim();
  if (!line) return;
  if (!line.startsWith('TOADO|')) {
    // dong debug khac tu ESP32 (AT command, phan hoi module SIM...) - bo qua.
    return;
  }
  const parts = line.split('|');
  // (2026-09-08) Them phan thu 6 secondsAgo - xem giai thich dinh dang dong
  // TOADO| o dau SmsReceiverUSB38.ino/SmsReceiverUSB.ino: 1 SMS co the gop
  // nhieu diem lay mau cach nhau vai chuc giay tren xe, secondsAgo cho biet
  // diem nay THAT SU duoc lay mau bao lau truoc luc gui SMS, de tinh ts
  // chinh xac hon la gan chung "bay gio" cho ca 6 diem.
  if (parts.length !== 6) {
    console.error('Dong TOADO khong dung dinh dang (can 6 phan, cach nhau "|"):', line);
    return;
  }
  const [, deviceId, name, latStr, lonStr, secondsAgoStr] = parts;
  const lat = Number(latStr), lon = Number(lonStr);
  const secondsAgo = Number(secondsAgoStr);
  if (!Number.isFinite(lat) || !Number.isFinite(lon)) {
    console.error('Toa do khong hop le trong dong:', line);
    return;
  }
  const ts = Number.isFinite(secondsAgo) ? Date.now() - secondsAgo * 1000 : Date.now();
  console.log('Nhan toa do: ' + name + ' (' + deviceId + ') -> ' + lat.toFixed(6) + ', ' + lon.toFixed(6)
    + (Number.isFinite(secondsAgo) && secondsAgo > 0 ? ' (' + secondsAgo + 's truoc)' : ''));
  pushToServer(deviceId, name, lat, lon, ts);
}

async function main() {
  if (!portArg) { await listPortsAndExit(); return; }

  const port = new SerialPort({ path: portArg, baudRate: BAUD_RATE }, (err) => {
    if (err) {
      console.error('Khong mo duoc cong ' + portArg + ':', err.message);
      console.error('Kiem tra: dung ten cong (vd COM5), khong co chuong trinh nao khac (Arduino IDE Serial Monitor...) dang giu cong nay.');
      process.exit(1);
    }
  });
  port.on('open', () => {
    console.log('Da mo cong ' + portArg + ' (' + BAUD_RATE + ' baud) - dang doc SMS tu dong...');
    console.log('Day len server: ' + SERVER_URL + ' (dam bao nhan-sms-tu-dong-server.js dang chay).');
    console.log('Dong cua so nay de dung.');
  });
  const parser = port.pipe(new ReadlineParser({ delimiter: '\n' }));
  parser.on('data', handleLine);
}

main().catch((e) => { console.error('Loi khong mong doi:', e); process.exit(1); });
