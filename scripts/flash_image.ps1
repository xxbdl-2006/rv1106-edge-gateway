[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)]
    [string]$ImagePath,

    [ValidateSet("boot", "uboot", "idblock", "env")]
    [string]$Partition = "boot",

    [ValidatePattern('^/userdata/[A-Za-z0-9._-]+$')]
    [string]$RemoteImage = "/userdata/adb-flash.img",

    [string]$Serial,

    [switch]$SkipHashCheck,

    [switch]$Reboot,

    [switch]$Yes
)

$ErrorActionPreference = "Stop"
$script:AdbPath = (Get-Command adb.exe -ErrorAction Stop).Source
$script:AdbPrefix = @()

function Invoke-Adb {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$AdbArguments
    )

    & $script:AdbPath @script:AdbPrefix @AdbArguments
    if ($LASTEXITCODE -ne 0) {
        throw "adb command failed: adb $($AdbArguments -join ' ')"
    }
}

function Invoke-AdbCapture {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$AdbArguments
    )

    $output = & $script:AdbPath @script:AdbPrefix @AdbArguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "adb command failed: adb $($AdbArguments -join ' ')"
    }

    return (($output | Out-String).Trim())
}

$resolvedImage = Resolve-Path -LiteralPath $ImagePath -ErrorAction Stop
$image = Get-Item -LiteralPath $resolvedImage.Path

if (-not $image.PSIsContainer -and $image.Length -gt 0) {
    $extension = $image.Extension.ToLowerInvariant()
    if ($extension -ne ".img" -and $extension -ne ".bin") {
        throw "Expected an .img or .bin file, got: $($image.Name)"
    }
} else {
    throw "Image path is not a non-empty file: $($image.FullName)"
}

if ([string]::IsNullOrWhiteSpace($Serial)) {
    $deviceOutput = (& $script:AdbPath devices 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        throw "Could not enumerate ADB devices"
    }

    $devices = @(
        $deviceOutput -split "`r?`n" |
            Where-Object { $_ -match "\tdevice$" } |
            ForEach-Object { ($_ -split "\s+")[0] }
    )

    if ($devices.Count -ne 1) {
        throw "Expected exactly one ADB device, found $($devices.Count). Use -Serial."
    }

    $Serial = $devices[0]
} else {
    $deviceOutput = (& $script:AdbPath devices 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0 -or $deviceOutput -notmatch "(?m)^$([regex]::Escape($Serial))\s+device\r?$") {
        throw "ADB device is not online: $Serial"
    }
}

$script:AdbPrefix = @("-s", $Serial)

$description = "$Partition on $Serial from $($image.FullName)"
if (-not $PSCmdlet.ShouldProcess($description, "Flash image")) {
    return
}

if (-not $Yes) {
    Write-Host ""
    Write-Host "ADB device : $Serial"
    Write-Host "Image      : $($image.FullName)"
    Write-Host "Image size : $($image.Length) bytes"
    Write-Host "Partition  : $Partition"
    Write-Host ""
    $answer = Read-Host "Type FLASH to continue"
    if ($answer -cne "FLASH") {
        Write-Host "Cancelled."
        return
    }
}

Write-Host "Pushing image to $RemoteImage ..."
Invoke-Adb @("push", $image.FullName, $RemoteImage)

Write-Host "Pushing flash helper ..."
Invoke-Adb @("push", (Join-Path $PSScriptRoot "flash_partition.sh"), "/userdata/flash_partition.sh")
Invoke-Adb @("shell", "chmod +x /userdata/flash_partition.sh")

$hashArguments = @()

if (-not $SkipHashCheck) {
    $hashToolOutput = Invoke-AdbCapture @(
        "shell",
        "command -v sha256sum || command -v md5sum"
    )

    if ($hashToolOutput -match "sha256sum") {
        $hashTool = "sha256sum"
        $hostHash = (Get-FileHash -LiteralPath $image.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    } elseif ($hashToolOutput -match "md5sum") {
        $hashTool = "md5sum"
        $hostHash = (Get-FileHash -LiteralPath $image.FullName -Algorithm MD5).Hash.ToLowerInvariant()
    } else {
        throw "The board has neither sha256sum nor md5sum. Use -SkipHashCheck only for controlled development."
    }

    $hashArguments = @($hashTool, $hostHash)
    Write-Host "Board hash check: $hashTool $hostHash"
}

$flashCommand = "sh /userdata/flash_partition.sh $Partition $RemoteImage"
if ($hashArguments.Count -eq 2) {
    $flashCommand += " $($hashArguments[0]) $($hashArguments[1])"
}

Write-Host "Flashing partition $Partition ..."
Invoke-Adb @("shell", $flashCommand)

if ($Reboot) {
    Write-Host "Rebooting $Serial ..."
    Invoke-Adb @("shell", "sync")
    Invoke-Adb @("reboot")
}

Write-Host "Done."
