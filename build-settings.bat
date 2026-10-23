@echo off
rem Builds HCTP Recompiled Settings (Avalonia on .NET 10; needs the .NET 10 SDK).
rem Output: settings-app\bin\Release\net10.0\HCTP Recompiled Settings.exe.
cd /d "%~dp0settings-app"
dotnet build -c Release -nologo -v q || exit /b 1
echo built: %~dp0settings-app\bin\Release\net10.0\Def Jam Vendetta Recompiled Settings.exe
