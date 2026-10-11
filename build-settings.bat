@echo off
rem Builds Def Jam Vendetta Recompiled Settings, the settings program (settings-app\, Avalonia on .NET 10; needs the .NET 10 SDK):
rem settings-app\bin\Release\net10.0\Def Jam Vendetta Recompiled Settings.exe. It finds the game's folder by looking up from where it is.
cd /d "%~dp0settings-app"
dotnet build -c Release -nologo -v q || exit /b 1
echo built: %~dp0settings-app\bin\Release\net10.0\Def Jam Vendetta Recompiled Settings.exe
