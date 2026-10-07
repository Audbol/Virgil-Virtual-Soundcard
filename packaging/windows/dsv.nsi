; NSIS installer for the DSV virtual soundcard (Windows x64).
; Built by packaging/windows/build-installer.sh (or makensis directly):
;   makensis -DVERSION=0.1.0 -DBIN_DIR=<dir with dsvd.exe, DSVAsio.dll, ...>
;            -DSRC_DIR=<repo root> -DOUT_FILE=<setup.exe> dsv.nsi

Target amd64-unicode   ; native 64-bit installer (DSV is x64-only)
SetCompressor /SOLID lzma

!ifndef VERSION
  !error "pass -DVERSION=x.y.z"
!endif
!ifndef BIN_DIR
  !error "pass -DBIN_DIR=<build output>"
!endif
!ifndef SRC_DIR
  !define SRC_DIR "..\.."
!endif
!ifndef OUT_FILE
  !define OUT_FILE "DSV-${VERSION}-win64-setup.exe"
!endif

!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"

!define PRODUCT "DSV Virtual Soundcard"
!define SERVICE "DSV"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\DSV"
!define FIREWALL_RULE "DSV Virtual Soundcard (AES67)"

Name "${PRODUCT}"
OutFile "${OUT_FILE}"
InstallDir "$PROGRAMFILES64\DSV"
InstallDirRegKey HKLM "${UNINST_KEY}" "InstallLocation"
RequestExecutionLevel admin
BrandingText "${PRODUCT} ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${PRODUCT}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${PRODUCT} installer"
VIAddVersionKey "LegalCopyright" "DSV Project"

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TEXT "This installs the DSV virtual soundcard:$\r$\n$\r$\n\
  - dsvd, a background service that sends and receives AES67 network audio \
(compatible with Dante devices in AES67 mode)$\r$\n\
  - an ASIO driver named $\"${PRODUCT}$\" for your DAW$\r$\n$\r$\n\
After installing, set the network interface in the configuration file \
(Start menu > DSV > Edit configuration)."
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\README.md"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Open the README"
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Edit the configuration now"
!define MUI_FINISHPAGE_RUN_FUNCTION EditConfig

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Var ConfDir

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "${PRODUCT} requires 64-bit Windows."
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Function un.onInit
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Function EditConfig
  Exec '"$WINDIR\notepad.exe" "$APPDATA\DSV\dsv.conf"'
FunctionEnd

;; Stop the service and wait (up to ~20 s) until it has really exited, so its
;; files can be replaced or deleted. Falls back to killing the process.
!macro StopService UN
Function ${UN}StopService
  nsExec::Exec '"$SYSDIR\sc.exe" query ${SERVICE}'
  Pop $0
  ${If} $0 != 0
    Return  ; not installed
  ${EndIf}
  nsExec::Exec '"$SYSDIR\sc.exe" stop ${SERVICE}'
  Pop $0
  StrCpy $1 0
  ${Do}
    nsExec::Exec '"$SYSDIR\cmd.exe" /c ""$SYSDIR\sc.exe" query ${SERVICE} | "$SYSDIR\find.exe" "STOPPED""'
    Pop $0
    ${If} $0 == 0
      ${Break}
    ${EndIf}
    Sleep 500
    IntOp $1 $1 + 1
  ${LoopUntil} $1 >= 40
  nsExec::Exec '"$SYSDIR\taskkill.exe" /F /IM dsvd.exe'
  Pop $0
  Sleep 500
FunctionEnd
!macroend
!insertmacro StopService ""
!insertmacro StopService "un."

Section "DSV" SecMain
  SectionIn RO
  StrCpy $ConfDir "$APPDATA\DSV"

  Call StopService

  SetOutPath "$INSTDIR"
  File "${BIN_DIR}\dsvd.exe"
  File "${BIN_DIR}\DSVAsio.dll"
  File "${BIN_DIR}\dsv-latency-probe.exe"
  File "${SRC_DIR}\README.md"
  File "${SRC_DIR}\config\dsv.conf.example"
  File "${SRC_DIR}\packaging\windows\restart-dsv.cmd"

  ; Configuration: keep an existing (user-edited) file on upgrade.
  CreateDirectory "$ConfDir"
  ${IfNot} ${FileExists} "$ConfDir\dsv.conf"
    SetOutPath "$ConfDir"
    File "/oname=dsv.conf" "${SRC_DIR}\packaging\dsv.conf"
  ${EndIf}
  ; Let local users edit the configuration without elevation.
  nsExec::Exec '"$SYSDIR\icacls.exe" "$ConfDir\dsv.conf" /grant *S-1-5-32-545:M'
  Pop $0

  ; ASIO driver (64-bit regsvr32).
  ${DisableX64FSRedirection}
  ExecWait '"$SYSDIR\regsvr32.exe" /s "$INSTDIR\DSVAsio.dll"' $0
  ${EnableX64FSRedirection}
  ${If} $0 != 0
    DetailPrint "regsvr32 failed ($0): ASIO driver not registered"
  ${EndIf}

  ; Service: auto start, restart on failure, logs next to the config.
  nsExec::Exec '"$SYSDIR\sc.exe" query ${SERVICE}'
  Pop $0
  ${If} $0 == 0
    nsExec::ExecToLog '"$SYSDIR\sc.exe" config ${SERVICE} binPath= "\"$INSTDIR\dsvd.exe\" --service --log \"$ConfDir\dsvd.log\"" start= auto'
  ${Else}
    nsExec::ExecToLog '"$SYSDIR\sc.exe" create ${SERVICE} binPath= "\"$INSTDIR\dsvd.exe\" --service --log \"$ConfDir\dsvd.log\"" start= auto DisplayName= "${PRODUCT}"'
  ${EndIf}
  Pop $0
  nsExec::Exec '"$SYSDIR\sc.exe" description ${SERVICE} "AES67 network audio engine for the DSV virtual soundcard (ASIO)."'
  Pop $0
  nsExec::Exec '"$SYSDIR\sc.exe" failure ${SERVICE} reset= 86400 actions= restart/2000/restart/5000/restart/30000'
  Pop $0

  ; Firewall: RTP, PTP (319/320) and SAP arrive unsolicited.
  nsExec::Exec '"$SYSDIR\netsh.exe" advfirewall firewall delete rule name="${FIREWALL_RULE}"'
  Pop $0
  nsExec::Exec '"$SYSDIR\netsh.exe" advfirewall firewall add rule name="${FIREWALL_RULE}" dir=in action=allow program="$INSTDIR\dsvd.exe" protocol=udp enable=yes profile=any'
  Pop $0

  nsExec::ExecToLog '"$SYSDIR\sc.exe" start ${SERVICE}'
  Pop $0

  ; Start menu.
  CreateDirectory "$SMPROGRAMS\DSV"
  CreateShortcut "$SMPROGRAMS\DSV\Edit configuration.lnk" "$WINDIR\notepad.exe" '"$ConfDir\dsv.conf"'
  CreateShortcut "$SMPROGRAMS\DSV\Restart DSV service.lnk" "$INSTDIR\restart-dsv.cmd"
  CreateShortcut "$SMPROGRAMS\DSV\DSV status.lnk" "$SYSDIR\cmd.exe" '/k ""$INSTDIR\dsvd.exe" --status"'
  CreateShortcut "$SMPROGRAMS\DSV\Log file.lnk" "$WINDIR\notepad.exe" '"$ConfDir\dsvd.log"'
  CreateShortcut "$SMPROGRAMS\DSV\README.lnk" "$INSTDIR\README.md"
  CreateShortcut "$SMPROGRAMS\DSV\Uninstall.lnk" "$INSTDIR\uninstall.exe"

  ; Add/Remove Programs.
  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${PRODUCT}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "DSV Project"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\dsvd.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" 4096
SectionEnd

Section "Uninstall"
  Call un.StopService
  nsExec::Exec '"$SYSDIR\sc.exe" delete ${SERVICE}'
  Pop $0

  ${DisableX64FSRedirection}
  ExecWait '"$SYSDIR\regsvr32.exe" /u /s "$INSTDIR\DSVAsio.dll"'
  ${EnableX64FSRedirection}

  nsExec::Exec '"$SYSDIR\netsh.exe" advfirewall firewall delete rule name="${FIREWALL_RULE}"'
  Pop $0

  Delete /REBOOTOK "$INSTDIR\dsvd.exe"
  Delete /REBOOTOK "$INSTDIR\DSVAsio.dll"
  Delete /REBOOTOK "$INSTDIR\dsv-latency-probe.exe"
  Delete /REBOOTOK "$INSTDIR\README.md"
  Delete /REBOOTOK "$INSTDIR\dsv.conf.example"
  Delete /REBOOTOK "$INSTDIR\restart-dsv.cmd"
  Delete /REBOOTOK "$INSTDIR\uninstall.exe"
  RMDir /REBOOTOK "$INSTDIR"
  RMDir /r "$SMPROGRAMS\DSV"
  DeleteRegKey HKLM "${UNINST_KEY}"
  ; The configuration and log in ProgramData\DSV are kept on purpose.
SectionEnd
