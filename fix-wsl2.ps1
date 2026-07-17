# fix-wsl2.ps1 - one-click repair for WSL2 error HCS_E_HYPERV_NOT_INSTALLED
# Run as Administrator:
#   cd E:\WorkBuddy\GDKmini
#   powershell -ExecutionPolicy Bypass -File .\fix-wsl2.ps1

$ErrorActionPreference = 'Continue'

function Section($t) { Write-Host "`n===== $t =====" -ForegroundColor Cyan }
function OK($t)      { Write-Host "  [OK]   $t" -ForegroundColor Green }
function WARN($t)    { Write-Host "  [NOTE] $t" -ForegroundColor Yellow }
function STEP($t)    { Write-Host "  [RUN]  $t" -ForegroundColor White }

# 0. administrator check
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "Please run PowerShell AS ADMINISTRATOR." -ForegroundColor Red
    Write-Host "Start menu -> search PowerShell -> right-click -> Run as Administrator." -ForegroundColor Red
    exit 1
}
OK "Running with administrator privileges"

$needRestart = $false
$biosNeeded  = $false

# 1. hardware capability check
Section "1. Hardware check (CPU virtualization / SLAT)"
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
Write-Host ("  CPU: {0}" -f $cpu.Name)
$fwVirt = $cpu.VirtualizationFirmwareEnabled
$slat   = $cpu.SecondLevelAddressTranslationExtensions
Write-Host ("  VirtualizationFirmwareEnabled: {0}" -f $fwVirt)
Write-Host ("  Second Level Address Translation (SLAT): {0}" -f $slat)
if ($slat -eq $false) { WARN "CPU may not support SLAT; WSL2 might be unsupported on this hardware." }
if ($fwVirt -eq $false) { WARN "BIOS virtualization is OFF -> you must enable VT-x / AMD-V / SVM in BIOS."; $biosNeeded = $true }
else { OK "BIOS virtualization appears enabled" }

try {
    $si = & systeminfo 2>$null | Select-String -Pattern "Hyper-V","Virtualization"
    if ($si) { Write-Host "  systeminfo virtualization lines:"; $si | ForEach-Object { Write-Host ("    " + $_.ToString().Trim()) } }
} catch { WARN ("systeminfo check skipped: " + $_.ToString()) }

# 2. hypervisor launch type
Section "2. Hypervisor launch type (bcdedit)"
$hlt = (& bcdedit /enum | Select-String -Pattern "hypervisorlaunchtype")
if ($hlt) { Write-Host ("  Current: " + $hlt.ToString().Trim()) }
if ($hlt -and $hlt.ToString() -match "Off") {
    STEP "hypervisorlaunchtype=Off, setting to Auto ..."
    & bcdedit /set hypervisorlaunchtype Auto | Out-Null
    OK "Set to Auto (takes effect after reboot)"
    $needRestart = $true
} elseif (-not $hlt) {
    STEP "hypervisorlaunchtype not found, setting to Auto ..."
    & bcdedit /set hypervisorlaunchtype Auto | Out-Null
    OK "Set to Auto (takes effect after reboot)"
    $needRestart = $true
} else { OK "hypervisorlaunchtype already Auto" }

# 3. key services
Section "3. Key services (vmcompute / HvHost / LxssManager)"
foreach ($svc in @("vmcompute","HvHost","LxssManager")) {
    $s = Get-Service -Name $svc -ErrorAction SilentlyContinue
    if (-not $s) { WARN ("$svc not present (feature may not be installed yet, see step 4)"); continue }
    $cfg = & sc.exe qc $svc 2>$null | Select-String "START_TYPE"
    Write-Host ("  {0}: status={1} {2}" -f $svc, $s.Status, ($cfg -replace '\s+',' '))
    if ($cfg -and $cfg.ToString() -match "DISABLED") {
        STEP ("$svc is disabled, setting to demand ...")
        & sc.exe config $svc start= demand | Out-Null
        OK "$svc start type set to demand"
    }
    if ($s.Status -ne 'Running' -and $svc -eq 'vmcompute') {
        STEP "Starting vmcompute ..."
        Start-Service vmcompute -ErrorAction SilentlyContinue
        Write-Host ("    vmcompute now: " + (Get-Service vmcompute).Status)
    }
}

# 4. windows optional features
Section "4. Enable Windows optional features"
$features = @("VirtualMachinePlatform","Microsoft-Windows-Subsystem-Linux","HypervisorPlatform","Microsoft-Hyper-V-All")
foreach ($f in $features) {
    $feat = Get-WindowsOptionalFeature -Online -FeatureName $f -ErrorAction SilentlyContinue
    if ($null -eq $feat) { WARN ("${f}: not available on this Windows edition (Home has no Hyper-V; ignore)"); continue }
    if ($feat.State -eq "Enabled") { OK ("$f already enabled") }
    else {
        STEP ("Enabling $f ...")
        $r = Enable-WindowsOptionalFeature -Online -FeatureName $f -All -NoRestart -ErrorAction SilentlyContinue
        if ($r.RestartNeeded) { $needRestart = $true }
        OK "$f enabled (reboot may be required)"
    }
}

# 5. update wsl
Section "5. Update WSL"
try {
    STEP "wsl --update ..."
    & wsl.exe --update 2>&1 | ForEach-Object { Write-Host ("    " + $_.ToString().Trim()) }
} catch { WARN ("wsl --update failed: " + $_.ToString()) }

# 6. summary
Section "Result and next steps"
if ($biosNeeded) {
    Write-Host "  >>> MUST enter BIOS and enable virtualization, otherwise fixes above will not help:" -ForegroundColor Red
    Write-Host "     Reboot -> press Del/F2/F10 (depends on mainboard) -> find Intel VT-x / AMD-V / SVM Mode -> Enable -> Save & Exit"
}
if ($needRestart) {
    Write-Host "  >>> Need to REBOOT for changes to take effect. After reboot run: wsl -d Ubuntu" -ForegroundColor Yellow
    $ans = Read-Host "  Reboot now? (y/N)"
    if ($ans -eq 'y' -or $ans -eq 'Y') { Restart-Computer -Force }
} else {
    Write-Host "  >>> Try now: wsl -d Ubuntu" -ForegroundColor Green
}
Write-Host "`nDone. If it still fails after reboot, see the checklist: Hyper-V off-then-on trick." -ForegroundColor Cyan
