# SPDX-License-Identifier: GPL-3.0-or-later
#
# Downloads the Xbox Wireless Adapter firmware from Microsoft's own driver packages
# (Windows Update catalog) and saves it in Apps\xadapter\ next to this script:
# xone_dongle_02fe.bin for the slim adapter and xone_dongle_02e6.bin for the original one.
# Same sources and checksums as the Linux xone driver uses.
#
# The firmware is subject to Microsoft's Terms of Use:
# https://www.microsoft.com/en-us/legal/terms-of-use
#
# Run:  double-click get-firmware.cmd, or
#       powershell -ExecutionPolicy Bypass -File get-firmware.ps1
$ErrorActionPreference = 'Stop'

$firmware = @(
    @{ name = 'xone_dongle_02fe.bin'; adapter = 'slim adapter (045e:02fe)'; file = 'FW_ACC_00U.bin'
       hash = '48084D9FA53B9BB04358F3BB127B7495DC8F7BB0B3CA1437BD24EF2B6EABDF66'
       url  = 'https://catalog.s.download.windowsupdate.com/c/msdownload/update/driver/drvs/2017/07/1cd6a87c-623f-4407-a52d-c31be49e925c_e19f60808bdcbfbd3c3df6be3e71ffc52e43261e.cab' },
    @{ name = 'xone_dongle_02e6.bin'; adapter = 'original adapter (045e:02e6)'; file = 'FW_ACC_00U.bin'
       hash = '080CE4091E53A4EF3E5FE29939F51FD91F46D6A88BE6D67EB6E99A5723B3A223'
       url  = 'https://catalog.s.download.windowsupdate.com/d/msdownload/update/driver/drvs/2017/03/2ea9591b-f751-442c-80ce-8f4692cdc67b_6b555a3a288153cf04aec6e03cba360afe2fce34.cab' }
)

$dir = Join-Path $PSScriptRoot 'Apps\xadapter'
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('xadapter-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $tmp | Out-Null
$ok = $false
try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    foreach ($fw in $firmware) {
        Write-Host "Downloading the firmware for the $($fw.adapter) from Microsoft..."
        $cab = Join-Path $tmp ($fw.name + '.cab')
        $out = Join-Path $tmp $fw.name
        New-Item -ItemType Directory -Path $out | Out-Null
        Invoke-WebRequest -Uri $fw.url -OutFile $cab -UseBasicParsing

        # tar.exe ships with Windows 10 1803 and later and reads CAB files
        & "$env:SystemRoot\System32\tar.exe" -xf $cab -C $out $fw.file
        if ($LASTEXITCODE -ne 0) { throw "could not extract $($fw.file) from the driver package" }

        $got = (Get-FileHash (Join-Path $out $fw.file) -Algorithm SHA256).Hash
        if ($got -ne $fw.hash) { throw "checksum mismatch for $($fw.name): $got" }

        $dest = Join-Path $dir $fw.name
        Move-Item -Force (Join-Path $out $fw.file) $dest
        Write-Host "Saved $dest"
    }
    $ok = $true
} catch {
    Write-Host "Failed: $_" -ForegroundColor Red
} finally {
    Remove-Item -Recurse -Force $tmp
}

# keep the window open when started by double-click
if ($ok) {
    Write-Host ''
    Write-Host 'Done. Copy the Apps folder to your console.' -ForegroundColor Green
}
Read-Host 'Press Enter to close' | Out-Null
if (-not $ok) { exit 1 }
