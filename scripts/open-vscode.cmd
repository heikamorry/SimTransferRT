@echo off
cd /d "%~dp0.."
call scripts\env.cmd
if errorlevel 1 exit /b 1
echo Save and close existing VS Code instances first to avoid inheriting an old compiler environment.
code .
