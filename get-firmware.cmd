@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem Double-click to download the adapter firmware (runs get-firmware.ps1).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0get-firmware.ps1"
