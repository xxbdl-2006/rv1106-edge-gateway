#
# Install boot time autostart for the RV1106 gateway.
#
#   .\scripts\install_autostart.ps1            # install only
#   .\scripts\install_autostart.ps1 -Start     # install and start it right away
#   .\scripts\install_autostart.ps1 -Remove    # take it back out
#
# What it does:
#
#   1. pushes the supervisor and the env file into /userdata
#   2. disables the rkipc init script so the vendor app never grabs the camera
#   3. installs /etc/init.d/S99gateway
#
# The root filesystem is ext4 mounted read only, so it is remounted read write
# around the install and put back to read only afterwards. Nothing outside
# /etc/init.d and /userdata is touched.
#

param(
    [switch]$Start,
    [switch]$Remove
)

$ErrorActionPreference = "Stop"

$ScriptsDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$SupervisorLocal = Join-Path $ScriptsDir "gateway-supervise.sh"
$InitLocal = Join-Path $ScriptsDir "S99gateway"

$InitPath = "/etc/init.d/S99gateway"
$EnvPath = "/userdata/gateway.env"
$SupervisorPath = "/userdata/gateway-supervise.sh"

#
# See start_gateway.ps1: adb writes progress on stderr and Windows PowerShell
# 5.1 turns that into error records that abort a strict script.
#
function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$ArgumentList
    )

    $previous = $ErrorActionPreference
    $ErrorActionPreference = "SilentlyContinue"
    try {
        $output = & $FilePath @ArgumentList 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }

    $text = ($output | ForEach-Object {
        if ($_ -is [System.Management.Automation.ErrorRecord]) {
            $_.Exception.Message
        } else {
            $_
        }
    }) -join "`n"

    return @{ Code = $code; Text = $text }
}

function Board {
    param(
        [Parameter(Mandatory = $true)][string]$Command,
        [switch]$AllowFailure
    )

    $result = Invoke-Native -FilePath "adb" -ArgumentList @("shell", $Command)
    if (-not $AllowFailure -and $result.Code -ne 0) {
        throw "board command failed: $Command`n$($result.Text)"
    }

    return $result.Text.Trim()
}

function Step {
    param([string]$Message)

    Write-Host "==> $Message"
}

function Push-And-Check {
    param(
        [Parameter(Mandatory = $true)][string]$LocalPath,
        [Parameter(Mandatory = $true)][string]$RemotePath
    )

    $result = Invoke-Native -FilePath "adb" -ArgumentList @("push", $LocalPath, $RemotePath)
    if ($result.Code -ne 0) {
        throw "adb push failed for $LocalPath`n$($result.Text)"
    }

    # A CRLF shebang or line ending turns into "/bin/sh^M: not found" on the
    # board, and it is much easier to fix here than to debug there.
    Board -Command "sed -i 's/\r$//' $RemotePath" | Out-Null
}

Step "checking the board is reachable"
$devices = (Invoke-Native -FilePath "adb" -ArgumentList @("devices")).Text
if ($devices -notmatch "\tdevice") {
    throw "no board connected over adb"
}

if ($Remove) {
    Step "stopping the gateway"
    Board -Command "$InitPath stop" -AllowFailure | Out-Null

    Step "remounting the root filesystem read write"
    Board -Command "mount -o remount,rw /"

    Step "removing the init script"
    Board -Command "rm -f $InitPath"

    Write-Host ""
    Write-Host "autostart removed. /userdata scripts are left in place."
    exit 0
}

foreach ($file in @($SupervisorLocal, $InitLocal)) {
    if (-not (Test-Path $file)) {
        throw "missing $file"
    }
}

Step "pushing the supervisor"
Push-And-Check -LocalPath $SupervisorLocal -RemotePath $SupervisorPath
Board -Command "chmod +x $SupervisorPath" | Out-Null

Step "checking the shell scripts parse"
foreach ($remote in @($SupervisorPath)) {
    $syntax = Board -Command "sh -n $remote" -AllowFailure
    if ($syntax) {
        throw "sh -n reported a problem in ${remote}:`n$syntax"
    }
}
Write-Host "    ok"

Step "writing $EnvPath"
$quiet = ""
$help = Board -Command "/userdata/v4l2_mpp_encode --help 2>&1" -AllowFailure
if ($help -match "--quiet") {
    $quiet = " --quiet"
    Write-Host "    the binary supports --quiet, per frame trace stays off"
} else {
    Write-Host "    the binary does not support --quiet yet, log will be wordy"
}

$arguments = "-d /dev/video11 -w 1280 -H 720 --warmup 30 --sink rtsp --rtsp-port 8554" + $quiet
$envContent = "# Change the gateway arguments here, then: $InitPath restart`nGATEWAY_ARGS=`"$arguments`"`n"
$envContent | Out-File -FilePath (Join-Path $env:TEMP "gateway.env") -Encoding ascii
Push-And-Check -LocalPath (Join-Path $env:TEMP "gateway.env") -RemotePath $EnvPath

#
# /etc/inittab already remounts / read write at sysinit, so this is normally a
# no-op. It is here because the image could be changed to keep the root
# filesystem read only, and the install must not silently do nothing then.
#
Step "making sure the root filesystem is writable"
$remount = Board -Command "mount -o remount,rw /" -AllowFailure
if ($remount) {
    Write-Host "    $remount"
}

Step "disabling the rkipc init script"
$rkipcScripts = Board -Command "ls /etc/init.d/ 2>/dev/null | grep -i -E '^S.*(rkipc|rkpc)'" -AllowFailure
if ($rkipcScripts) {
    foreach ($name in ($rkipcScripts -split "`r?`n" | Where-Object { $_ })) {
        $disabled = "K" + $name.Substring(1)
        Board -Command "mv -f /etc/init.d/$name /etc/init.d/$disabled"
        Write-Host "    $name -> $disabled"
    }
} else {
    Write-Host "    no rkipc init script found (it may be started elsewhere)"
}

Step "installing $InitPath"
Push-And-Check -LocalPath $InitLocal -RemotePath $InitPath
Board -Command "chmod +x $InitPath" | Out-Null

$syntax = Board -Command "sh -n $InitPath" -AllowFailure
if ($syntax) {
    throw "sh -n reported a problem in ${InitPath}:`n$syntax"
}

#
# Deliberately no remount,ro here. The stock inittab mounts / read write at
# boot, and leaving it read only would change behaviour for everything else
# running on the board.
#

Step "installed"
Write-Host (Board -Command "ls -l $InitPath $SupervisorPath $EnvPath")

if ($Start) {
    Step "starting the service"
    Board -Command "$InitPath start" -AllowFailure | Out-Null

    #
    # start only kicks off a background worker, because taking the camera over
    # means waiting for rkipc and retrying the 3A server. Poll instead of
    # sleeping a fixed amount and hoping.
    #
    $deadline = (Get-Date).AddSeconds(60)
    $pidText = ""
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 3
        $pidText = Board -Command "cat /tmp/gateway-supervisor.pid 2>/dev/null" -AllowFailure
        if ($pidText) {
            break
        }
    }

    Write-Host (Board -Command "$InitPath status" -AllowFailure)
    Write-Host ""
    Write-Host "boot log:"
    Write-Host (Board -Command "cat /tmp/gateway-boot.log 2>/dev/null" -AllowFailure)
}

Write-Host ""
Write-Host "After a reboot the stream comes up on its own."
Write-Host "  start/stop/status: adb shell `"$InitPath start|stop|status`""
Write-Host "  live log:          adb shell `"tail -f /userdata/gateway.log`""
