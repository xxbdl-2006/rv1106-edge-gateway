# Flash an image through Windows ADB

This workflow is suitable for raw partition images such as `boot.img` and,
with extra care, `uboot.img` or `idblock.img`. It does not replace the
Rockchip MaskROM/upgrade tool for full-device recovery.

## Safety boundary

Use this workflow for:

- `boot`
- `uboot`
- `idblock`
- `env`

Do not use live ADB `dd` for:

- `rootfs`, because the Linux root filesystem is mounted
- `userdata`, because the image is normally staged there and the partition
  may be mounted
- `update.img`, because it is a packaged multi-partition image, not a raw
  image for one partition

The scripts reject `rootfs`, `userdata`, and unknown partitions.

## Files

```text
scripts/flash_image.ps1
scripts/flash_partition.sh
```

`flash_image.ps1` runs on Windows and controls ADB. It checks the image,
pushes both helper files, computes a SHA-256 or MD5 hash, and invokes the
board-side helper.

`flash_partition.sh` runs on the board. It verifies the partition name,
image size, and hash before calling `dd`.

## Verify the board connection

```powershell
adb devices -l
adb shell "cat /proc/cmdline"
adb shell "ls -l /dev/block/by-name/"
```

The target must show as `device`, not `offline` or `unauthorized`.

## Dry run

PowerShell `-WhatIf` performs all host-side validation and prints what would
happen without pushing or writing:

```powershell
.\scripts\flash_image.ps1 `
  -ImagePath F:\luckfox_share\boot.img `
  -Partition boot `
  -WhatIf
```

## Flash boot.img

```powershell
.\scripts\flash_image.ps1 `
  -ImagePath F:\luckfox_share\boot.img `
  -Partition boot `
  -RemoteImage /userdata/boot.img `
  -Reboot
```

The script asks for the literal confirmation word `FLASH`. For automation,
`-Yes` skips only this interactive prompt. It does not skip image size or
hash validation unless `-SkipHashCheck` is explicitly supplied.

## Multiple ADB devices

Select the board by serial number:

```powershell
adb devices -l
.\scripts\flash_image.ps1 `
  -Serial 0123456789ABCDEF `
  -ImagePath F:\luckfox_share\boot.img `
  -Partition boot `
  -RemoteImage /userdata/boot.img `
  -Reboot
```

## Manual equivalent

The scripts perform the equivalent of:

```powershell
adb push F:\luckfox_share\boot.img /userdata/boot.img
adb push .\scripts\flash_partition.sh /userdata/flash_partition.sh
adb shell "chmod +x /userdata/flash_partition.sh"
adb shell "sh /userdata/flash_partition.sh boot /userdata/boot.img"
adb shell "sync"
adb reboot
```

Do not omit the partition-size and hash checks when doing this manually.

## Post-flash validation

After the board boots:

```powershell
adb wait-for-device
adb shell "uname -a"
adb shell "cat /proc/device-tree/model"
adb shell "ls -l /dev/spidev* /dev/i2c-4"
```

For a device-tree-only change, `uname -a` timestamp does not prove the new
DTB was loaded. Verify the actual hardware node or a property under
`/proc/device-tree/`.
