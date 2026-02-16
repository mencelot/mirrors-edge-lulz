@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" > nul 2>&1
cd /d "C:\Program Files (x86)\Steam\steamapps\common\mirrors edge\mirrorsedge_camera_proxy"
echo Building d3d9_proxy.cpp... > build_log.txt 2>&1
cl /LD /EHsc /O2 d3d9_proxy.cpp /link /DEF:d3d9.def /OUT:d3d9.dll >> build_log.txt 2>&1
echo Exit code: %ERRORLEVEL% >> build_log.txt 2>&1
dir d3d9.dll >> build_log.txt 2>&1 || echo d3d9.dll NOT found >> build_log.txt 2>&1
