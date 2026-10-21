Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

Name "Oh My 3DO Library"
OutFile "${OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\OhMy3DOLibrary"
RequestExecutionLevel user
SetCompressor /SOLID lzma
ShowInstDetails show
ShowUninstDetails show
Icon "${ICON}"
UninstallIcon "${ICON}"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "Oh My 3DO Library"
VIAddVersionKey "FileDescription" "Oh My 3DO Library Setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "Oh My 3DO contributors"

!define APPKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\OhMy3DOLibrary"
!define WEBVIEWKEY "Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TEXT "Install Oh My 3DO Library for your Windows account.$\r$\n$\r$\nSetup adds a Start menu shortcut and an uninstaller. Games, save backups and settings stay in their existing locations.$\r$\n$\r$\nClose the app before continuing. An internet connection is needed only if Microsoft WebView2 must be installed."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${PAYLOAD}/LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\OhMy3DOLibrary.exe"
!define MUI_FINISHPAGE_RUN_NOTCHECKED
!insertmacro MUI_PAGE_FINISH
!define MUI_UNCONFIRMPAGE_TEXT_TOP "Remove the application and its shortcuts? Games, save backups, settings, metadata snapshots and Microsoft WebView2 will be kept."
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  SetShellVarContext current
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "This installer requires 64-bit Windows."
    Abort
  ${EndIf}
  ; A fixed per-user location remains writable by the app's verified updater.
  ; Do not adopt an unrelated or portable directory, even with /D=.
  StrCpy $INSTDIR "$LOCALAPPDATA\Programs\OhMy3DOLibrary"
  IfFileExists "$INSTDIR\*.*" 0 done
  IfFileExists "$INSTDIR\installer-owned.txt" done
  MessageBox MB_ICONSTOP "The install folder already contains files from another installation. Move that folder aside before running Setup:$\r$\n$INSTDIR"
  Abort
done:
FunctionEnd

Function DetectWebView
  ; EdgeUpdate's machine key is in the 32-bit registry view on x64 Windows.
  SetRegView 32
  ReadRegStr $0 HKLM "${WEBVIEWKEY}" "pv"
  ${If} $0 != ""
  ${AndIf} $0 != "0.0.0.0"
    Return
  ${EndIf}
  SetRegView 64
  ReadRegStr $0 HKCU "${WEBVIEWKEY}" "pv"
  ${If} $0 != ""
  ${AndIf} $0 != "0.0.0.0"
    Return
  ${EndIf}
  SetRegView 32
  ReadRegStr $0 HKCU "${WEBVIEWKEY}" "pv"
FunctionEnd

Section "Oh My 3DO Library (required)" Core
  SectionIn RO
  Call DetectWebView
  ${If} $0 == ""
  ${OrIf} $0 == "0.0.0.0"
    DetailPrint "Installing Microsoft WebView2. This may take a few minutes."
    InitPluginsDir
    SetOutPath "$PLUGINSDIR"
    File /oname=MicrosoftEdgeWebview2Setup.exe "${WEBVIEW}"
    ExecWait '"$PLUGINSDIR\MicrosoftEdgeWebview2Setup.exe" /silent /install' $1
    Call DetectWebView
    ${If} $0 == ""
    ${OrIf} $0 == "0.0.0.0"
      MessageBox MB_ICONSTOP "Microsoft WebView2 could not be installed (exit code $1). Check your connection or install the WebView2 Evergreen Runtime from Microsoft, then run Setup again. The app has not been installed."
      Abort
    ${EndIf}
  ${EndIf}
  SetRegView 64
  CreateDirectory "$INSTDIR"
  ClearErrors
  FileOpen $0 "$INSTDIR\installer-owned.txt" w
  ${If} ${Errors}
    MessageBox MB_ICONSTOP "The application folder is not writable: $INSTDIR"
    Abort
  ${EndIf}
  FileWrite $0 "OhMy3DO-User-Install-1$\r$\n"
  FileClose $0
  ; The app executable is copied first: close a running copy and retry if locked.
  !include "${INSTALL_FILES}"
  SetOutPath "$INSTDIR\licenses\installer"
  File /oname=NSIS.txt "${NSIS_LICENSE}"
  SetOutPath "$INSTDIR"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\Oh My 3DO"
  CreateShortcut "$SMPROGRAMS\Oh My 3DO\Oh My 3DO Library.lnk" "$INSTDIR\OhMy3DOLibrary.exe"
  CreateShortcut "$SMPROGRAMS\Oh My 3DO\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "${APPKEY}" "DisplayName" "Oh My 3DO Library"
  WriteRegStr HKCU "${APPKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${APPKEY}" "Publisher" "Oh My 3DO contributors"
  WriteRegStr HKCU "${APPKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${APPKEY}" "DisplayIcon" "$INSTDIR\OhMy3DOLibrary.exe"
  WriteRegStr HKCU "${APPKEY}" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
  WriteRegStr HKCU "${APPKEY}" "QuietUninstallString" '$\"$INSTDIR\Uninstall.exe$\" /S'
  WriteRegStr HKCU "${APPKEY}" "URLInfoAbout" "https://github.com/Scyllinice/oh-my-3do"
  WriteRegDWORD HKCU "${APPKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${APPKEY}" "NoRepair" 1
SectionEnd

Section /o "Desktop shortcut"
  CreateShortcut "$DESKTOP\Oh My 3DO Library.lnk" "$INSTDIR\OhMy3DOLibrary.exe"
SectionEnd

Function un.onInit
  SetShellVarContext current
  SetRegView 64
  IfFileExists "$INSTDIR\installer-owned.txt" owned
  MessageBox MB_ICONSTOP "This folder is not an installed copy of Oh My 3DO Library."
  Abort
owned:
FunctionEnd

Section "Uninstall"
  ; Refuse to proceed while Windows has the program open.
  ClearErrors
  Delete "$INSTDIR\OhMy3DOLibrary.exe"
  ${If} ${Errors}
    MessageBox MB_ICONSTOP "Close Oh My 3DO Library and run the uninstaller again."
    Abort
  ${EndIf}
  !include "${UNINSTALL_FILES}"
  ; This subtree contains only application dependency notices, including updates.
  RMDir /r "$INSTDIR\licenses"
  Delete "$INSTDIR\.ohmy3do-previous-*.exe"
  Delete "$INSTDIR\.ohmy3do-update-*.exe"
  Delete "$INSTDIR\installer-owned.txt"
  Delete "$INSTDIR\Uninstall.exe"
  Delete "$SMPROGRAMS\Oh My 3DO\Oh My 3DO Library.lnk"
  Delete "$SMPROGRAMS\Oh My 3DO\Uninstall.lnk"
  RMDir "$SMPROGRAMS\Oh My 3DO"
  Delete "$DESKTOP\Oh My 3DO Library.lnk"
  DeleteRegKey HKCU "${APPKEY}"
  ; Preserve any files the user placed in this folder.
  RMDir "$INSTDIR"
SectionEnd
