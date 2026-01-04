@echo off
setlocal
chcp 65001 >nul

echo ========================================================
echo   OBS SEI Stamper 插件构建与安装脚本
echo ========================================================

:: 设置OBS安装路径 (如果不同请修改此处)
set OBS_PATH=C:\Program Files\obs-studio

:: 创建构建目录
if not exist build mkdir build
cd build

echo.
echo [1/3] 配置工程...
cmake .. -A x64
if %ERRORLEVEL% NEQ 0 (
    echo [错误] CMake配置失败。请检查是否安装了CMake和Visual Studio。
    pause
    exit /b 1
)

echo.
echo [2/3] 正在编译...
cmake --build . --config Release
if %ERRORLEVEL% NEQ 0 (
    echo [错误] 编译失败。
    pause
    exit /b 1
)

echo.
echo [3/3] 正在安装...
:: 复制插件dll到当前目录下的 out 文件夹（模拟安装，避免直接写入Program Files需要管理员权限）
cmake --install . --prefix "../out/obs-studio" --config Release

echo.
echo ========================================================
echo   构建成功！
echo ========================================================
echo.
echo 插件文件已生成在: %~dp0out\obs-studio
echo.
echo 如需安装到OBS，请手动将 out\obs-studio 下的文件夹
echo 复制到你的OBS安装目录 (例如 C:\Program Files\obs-studio)
echo.
pause
