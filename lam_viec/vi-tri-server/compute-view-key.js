'use strict';

// Tinh "view key" cho 1 ma phong - key nay can de mo trang /thu (dashboard
// xem vi tri) tu khi server yeu cau xac thuc rieng cho nguoi xem, tach
// khoi ma phong ma cac thiet bi gui dung. Chi nguoi giu API_SECRET (nguoi
// van hanh server) moi tinh duoc view key, roi tu chia se link cho nguoi
// duoc phep xem.
//
// Dung:
//   $env:API_SECRET = "<giong het gia tri dang chay server.js>"
//   node compute-view-key.js <ma_phong>
//
// In ra 1 dong view key (hex), dan vao o "View key" tren trang /thu, hoac
// gan vao link: http://<server>/thu?room=<ma_phong>&vk=<view_key>

const crypto = require('crypto');

const API_SECRET = process.env.API_SECRET;
const room = process.argv[2];

if (!API_SECRET) {
  console.error('Thieu bien moi truong API_SECRET (phai giong voi gia tri dang chay server.js).');
  process.exit(1);
}
if (!room) {
  console.error('Dung: node compute-view-key.js <ma_phong>');
  process.exit(1);
}

console.log(crypto.createHmac('sha256', API_SECRET).update('view:' + room).digest('hex'));
