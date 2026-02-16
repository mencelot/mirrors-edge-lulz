@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
cd /d "E:\Benchmarks\mirrorsedge_camera_proxy"
echo Building...
cl /LD /EHsc /O2 d3d9_proxy.cpp /link /DEF:d3d9.def /OUT:d3d9.dll
echo Done. Exit code: %ERRORLEVEL%
dir d3d9.dll 2>nul || echo d3d9.dll NOT found
