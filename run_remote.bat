@echo off
setlocal
cd /d "%~dp0"
py -B pytool\run_remote.py %*
set ERR=%ERRORLEVEL%
if %ERR% neq 0 (
  echo.
  echo run_remote failed with exit code %ERR%
  pause
)
endlocal & exit /b %ERR%
