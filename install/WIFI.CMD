@echo off
rem WIFI.CMD - start the MT7921 WiFi: wpa_supplicant (minimised window) and DHCP on lan1
rem Call it from the startup.cmd in the Startup folder:   call d:\wpa\wifi.cmd
rem The firmware copy line stays in C:\STARTUP.CMD.

rem directory with wpa_supplicant.exe and wpa.conf
set WIFIDIR=d:\wpa
set WIFILAN=lan1

d:
cd %WIFIDIR%

rem wpa_supplicant in its own minimised window "WiFi" (closing it ends the connection)
start "WiFi" /min %WIFIDIR%\wpa_supplicant.exe -i %WIFILAN% -c %WIFIDIR%\wpa.conf

rem DHCP: waits up to 60 seconds, long enough for wpa_supplicant to connect
dhcpstrt -i %WIFILAN% -d 60

rem show the result
ifconfig %WIFILAN%
