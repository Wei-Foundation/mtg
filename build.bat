@echo off
rem Build the Wei compiler (MTG) from src\main.cpp.
setlocal
g++ -std=c++26 -O3 -o "%~dp0mtg.exe" "%~dp0src\main.cpp"
exit /b %ERRORLEVEL%
