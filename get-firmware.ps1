# SPDX-License-Identifier: GPL-3.0-or-later
#
# Downloads the Xbox Wireless Adapter firmware from Microsoft's own driver package
# (Windows Update catalog) and saves it as Apps\xadapter\xone_dongle_02fe.bin next to
# this script. Same source and checksum as the Linux xone driver uses.
#
# The firmware is subject to Microsoft's Terms of Use:
# https://www.microsoft.com/en-us/legal/terms-of-use
#
# Run:  double-click get-firmware.cmd, or
#       powershell -ExecutionPolicy Bypass -File get-firmware.ps1
$ErrorActionPreference = 'Stop'

$url  = 'https://catalog.s.download.windowsupdate.com/c/msdownload/update/driver/drvs/2017/07/1cd6a87c-623f-4407-a52d-c31be49e925c_e19f60808bdcbfbd3c3df6be3e71ffc52e43261e.cab'
$file = 'FW_ACC_00U.bin'
$hash = '48084D9FA53B9BB04358F3BB127B7495DC8F7BB0B3CA1437BD24EF2B6EABDF66'

$dest = Join-Path $PSScriptRoot 'Apps\xadapter\xone_dongle_02fe.bin'
$tmp  = Join-Path ([IO.Path]::GetTempPath()) ('xadapter-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    Write-Host 'Downloading the adapter driver package from Microsoft...'
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $url -OutFile (Join-Path $tmp 'driver.cab') -UseBasicParsing

    # tar.exe ships with Windows 10 1803 and later and reads CAB files
    & "$env:SystemRoot\System32\tar.exe" -xf (Join-Path $tmp 'driver.cab') -C $tmp $file
    if ($LASTEXITCODE -ne 0) { throw "could not extract $file from the driver package" }

    $got = (Get-FileHash (Join-Path $tmp $file) -Algorithm SHA256).Hash
    if ($got -ne $hash) { throw "checksum mismatch: $got" }

    New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
    Move-Item -Force (Join-Path $tmp $file) $dest
    Write-Host "Saved $dest"
} finally {
    Remove-Item -Recurse -Force $tmp
}
