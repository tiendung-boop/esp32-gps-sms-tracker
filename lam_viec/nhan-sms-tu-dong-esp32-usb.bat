@echo off
rem Bam 2 lan (hoac "Run") file nay de khoi dong TOAN BO: server ban do local
rem (127.0.0.1 - KHONG dung internet) + trinh duyet + doc SMS tu bo
rem SmsReceiverUSB cam qua day USB. Chi can go 1 thu duy nhat: cong COM.
rem Xem HUONG_DAN.txt muc "NHAN SMS TU DONG" neu can chi tiet/xu ly loi.

cd /d "%~dp0"
title Nhan SMS tu dong - ESP32 qua USB

echo Dang khoi dong server ban do (mo cua so rieng)...
start "Nhan SMS - Server ban do (dong cua so nay la tat)" node nhan-sms-tu-dong-server.js
ping -n 3 127.0.0.1 >nul

echo.
set /p COMPORT="Nhap cong COM cua ESP32 (vd COM5) - de trong roi Enter de xem danh sach cong dang co: "
if "%COMPORT%"=="" (
  node doc-sms-qua-esp32-usb.js
  echo.
  echo Chay lai file nay, lan nay go dung ten cong COM o tren.
  pause
) else (
  node doc-sms-qua-esp32-usb.js %COMPORT%
)
