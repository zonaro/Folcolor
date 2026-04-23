@echo off
:: Verifica elevacao
fltmc >nul 2>&1
if %errorlevel% neq 0 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs -Wait"
    exit /b %errorlevel%
)

set MSBUILD=C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe
set VCXPROJ=D:\GIT\Folcolor\src\Controller\Controller.vcxproj
set EXE_SRC=D:\GIT\Folcolor\src\Controller\Win32\Release\Folcolor.exe
set EXE_DST=C:\Program Files (x86)\Folcolor\Folcolor.exe

echo [BUILD] Compilando Release Win32 (Main Application com DLL embutida)...
"%MSBUILD%" "%VCXPROJ%" /p:Configuration=Release /p:Platform=Win32 /t:Rebuild /verbosity:minimal
if %errorlevel% neq 0 (
    echo [ERRO] Build do main falhou!
    pause
    exit /b 1
)

echo [DEPLOY] Copiando para Program Files...
copy /Y "%EXE_SRC%" "%EXE_DST%"
if %errorlevel% neq 0 (
    echo [ERRO] Copia do EXE falhou!
    pause
    exit /b 1
)

echo [INSTALL] Reinstalando menu de contexto e registrando handler...
"%EXE_DST%" --reinstall-registry
echo [OK] Build e deploy concluidos com sucesso.
