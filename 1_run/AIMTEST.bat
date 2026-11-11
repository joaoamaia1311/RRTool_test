@echo off
setlocal
title RRTool - Aim Sensitivity Calibrator
color 0B
echo ========================================================
echo   RRTool Aim Calibration Helper (Decimal Support)
echo ========================================================
echo.
set /p "dpi=Informe o DPI atual do seu rato (ex: 800 ou 1600): "
set /p "sens=Informe a sensibilidade do jogo (ex: 0.37): "

for /f %%i in ('powershell -Command "[math]::Round(%dpi% / %sens% + 35)"') do set "calibrated_value=%%i"

echo.
echo ========================================================
echo [SUCESSO] Valor calculado para o CoreRR.cfg: %calibrated_value%
echo ========================================================
echo.
pause