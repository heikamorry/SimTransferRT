@echo off
setlocal
call "%~dp0package-v2.cmd" %*
exit /b %errorlevel%
