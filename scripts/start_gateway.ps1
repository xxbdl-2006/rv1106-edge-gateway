#
# One shot startup for the RV1106 gateway over ADB.
#
#   .\scripts\start_gateway.ps1                       # push, stop rkipc, start 3A, stream
#   .\scripts\start_gateway.ps1 -NoPush               # keep the binary already on the board
#   .\scripts\start_gateway.ps1 -Sink file -Frames 300
#
# Why this script exists:
#
#   - rkipc starts on boot and owns /dev/video11. If it is not gone before the
#     3A script runs, start_rkaiq.sh exits silently and the capture node stays
#     busy, so the gateway dies with "VIDIOC_S_FMT: Device or resource busy".
#   - adbd kills the whole process group when a shell session ends, so a plain
#     "cmd &" does not survive. The launch below keeps the session alive for
#     two seconds so the child can escape through setsid.
#   - The binary must be cross compiled in the Ubuntu VM. A Windows build is an
#     x86 program the board cannot run, and the failure looks like a shell
#     "not found" rather than something obvious. The ELF magic check below
#     catches it before it wastes an afternoon.
#

param(
    [string]$Binary = "F:\luckfox_share\rv1103\v4l2_mpp_encode",
    [string]$Device = "/dev/video11",
    [int]$Width = 1280,
    [int]$Height = 720,
    [int]$Warmup = 30,
    [int]$Frames = 0,
    [string]$Sink = "rtsp",
    [int]$Port = 8554,
    [string]$Out = "/userdata/live.h264",
    [switch]$NoPush
)

$ErrorActionPreference = "Stop"

#
# adb reports progress on stderr, and Windows PowerShell 5.1 wraps native
# stderr into error records that abort the script when the error action is
# strict, even though the command succeeded. Run every native call through
# here so stderr becomes plain text and the exit code is the only verdict.
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

Step "stopping a previous gateway instance"
Board -Command "killall -9 v4l2_mpp_encode" -AllowFailure | Out-Null

Step "stopping rkipc"
Board -Command "killall -9 rkipc" -AllowFailure | Out-Null
Start-Sleep -Seconds 2
$rkipc = Board -Command "pidof rkipc" -AllowFailure
if ($rkipc) {
    throw "rkipc is still running (pid $rkipc); it owns the capture node"
}

if (-not $NoPush) {
    Step "pushing $Binary"
    $result = Invoke-Native -FilePath "adb" -ArgumentList @("push", $Binary, "/userdata/")
    if ($result.Code -ne 0) {
        throw "adb push failed`n$($result.Text)"
    }
    Board -Command "chmod +x /userdata/v4l2_mpp_encode" | Out-Null

    #
    # An x86 build pushed by accident starts with "MZ" instead of the ELF
    # magic, and the board refuses it with a shell "not found". Fail here with
    # a message that says what to do about it.
    #
    Step "checking the binary is an ARM build"
    $magic = Board -Command "head -c 4 /userdata/v4l2_mpp_encode | od -An -tx1" -AllowFailure
    if ($magic -notmatch "7f\s+45\s+4c\s+46") {
        throw ("the pushed file is not an ELF binary (magic: " + ($magic.Trim()) + ").`n" +
               "Build it in the Ubuntu VM:  make clean && make CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf-")
    }
    Write-Host "    ELF binary ok"
}

Step "starting the 3A server"
$threeA = Board -Command "/userdata/start_rkaiq.sh" -AllowFailure
Write-Host "    $threeA"
$server = Board -Command "pidof rkaiq_3A_server" -AllowFailure
if (-not $server) {
    throw "rkaiq_3A_server did not start"
}
Write-Host "    rkaiq_3A_server pid $server"

Step "launching the gateway in the background"
$arguments = "-d $Device -w $Width -H $Height --warmup $Warmup --sink $Sink"
if ($Sink -eq "rtsp") {
    $arguments += " --rtsp-port $Port"
} else {
    $arguments += " -o $Out"
}
if ($Frames -gt 0) {
    $arguments += " -n $Frames"
}

$launch = "setsid nohup /userdata/v4l2_mpp_encode $arguments > /userdata/rtsp.log 2>&1 < /dev/null & sleep 2; echo launched"
Board -Command $launch | Out-Null

Start-Sleep -Seconds 2
$gatewayPid = Board -Command "pidof v4l2_mpp_encode" -AllowFailure
if (-not $gatewayPid) {
    throw "the gateway did not stay up; read /userdata/rtsp.log on the board"
}
Write-Host "    gateway pid $gatewayPid"

Step "board log"
Write-Host (Board -Command "tail -n 5 /userdata/rtsp.log" -AllowFailure)

if ($Sink -eq "rtsp") {
    $address = Board -Command "ip -4 addr show usb0" -AllowFailure
    $ip = "<board-ip>"
    if ($address -match "inet (\d+\.\d+\.\d+\.\d+)") {
        $ip = $Matches[1]
    }

    $url = "rtsp://" + $ip + ":" + $Port + "/live/0"
    Write-Host ""
    Write-Host "Stream: $url"
    Write-Host "Stop:   adb shell `"killall v4l2_mpp_encode`""
}

Write-Host ""
Write-Host "done."
