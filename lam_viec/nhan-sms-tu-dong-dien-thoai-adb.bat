@echo off
rem Bam 2 lan (hoac "Run") file nay de khoi dong TOAN BO: server ban do local
rem (127.0.0.1 - KHONG dung internet) + trinh duyet + doc SMS truc tiep tu
rem dien thoai Android noi USB (qua ADB). Can chuan bi truoc: cai Android
rem Platform Tools (adb.exe) + bat "Go loi USB" tren dien thoai - xem
rem HUONG_DAN.txt muc "NHAN SMS TU DONG" de biet chi tiet cac buoc nay.

cd /d "%~dp0"
title Nhan SMS tu dong - Dien thoai qua ADB

echo Dang khoi dong server ban do (mo cua so rieng)...
start "Nhan SMS - Server ban do (dong cua so nay la tat)" node nhan-sms-tu-dong-server.js
ping -n 3 127.0.0.1 >nul

node doc-sms-qua-dien-thoai-adb.js
pause
