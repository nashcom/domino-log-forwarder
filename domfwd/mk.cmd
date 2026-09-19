@echo off

nmake /f mswin%NOTESAPI_BITNESS%.mak  %1 %2

if ERRORLEVEL 1 exit /b %errorlevel%

set TARGET_BIN_DIR=d:\lotus\domino

del vc*.pdb > nul 2>&1

mkdir %TARGET_BIN_DIR% > nul 2>&1
copy /y *.exe  %TARGET_BIN_DIR% > nul 2>&1
copy /y *.dll %TARGET_BIN_DIR% > nul 2>&1
copy /y *.pdb %TARGET_BIN_DIR% > nul 2>&1
copy /y *.sym %TARGET_BIN_DIR% > nul 2>&1

IF "%NOTESAPI_BITNESS%" == "32" GOTO W32

:W64

set NOTES_BIN=d:\lotus\notes

copy /Y *.dll %NOTES_BIN% > nul 2>&1
copy /Y *.exe %NOTES_BIN% > nul 2>&1
copy /Y *.sym %NOTES_BIN% > nul 2>&1

echo:
echo [W64 compiled + copied]
echo:

del *.obj *.exe *.dll *.pdb *.dym *.res > nul 2>&1

goto FINISH

:W32

set NOTES_BIN=d:\lotus\notes12

copy /Y *.dll %NOTES_BIN% > nul 2>&1
copy /Y *.exe %NOTES_BIN% > nul 2>&1
copy /Y *.sym %NOTES_BIN% > nul 2>&1

echo:
echo [W32 compiled + copied]
echo:

goto FINISH

:FINISH

del *.obj *.exe *.dll *.pdb *.sym *.res *.map *.ilk *.exp > nul 2>&1
exit /b 0