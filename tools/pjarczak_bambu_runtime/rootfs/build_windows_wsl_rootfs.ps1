param(
    [string]$OutputTar = "",
    [string]$BaseImage = "ubuntu:24.04",
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

function Get-ScriptDir {
    if (-not [string]::IsNullOrWhiteSpace($PSScriptRoot)) {
        return $PSScriptRoot
    }
    if (-not [string]::IsNullOrWhiteSpace($PSCommandPath)) {
        return (Split-Path -Parent $PSCommandPath)
    }
    if ($MyInvocation.MyCommand -and -not [string]::IsNullOrWhiteSpace($MyInvocation.MyCommand.Path)) {
        return (Split-Path -Parent $MyInvocation.MyCommand.Path)
    }
    return (Get-Location).Path
}

$scriptDir = Get-ScriptDir
if ([string]::IsNullOrWhiteSpace($OutputTar)) {
    $OutputTar = Join-Path $scriptDir 'windows-wsl2-rootfs.tar'
}
$OutputTar = [System.IO.Path]::GetFullPath($OutputTar)

if ((Test-Path $OutputTar) -and -not $Force) {
    Write-Host "Rootfs already exists: $OutputTar"
    exit 0
}

$docker = Get-Command docker -ErrorAction SilentlyContinue
if (-not $docker) {
    throw 'docker not found. Install Docker Desktop or set PJARCZAK_WSL_ROOTFS_TAR to an existing rootfs tar.'
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutputTar) | Out-Null

$containerName = "pjarczak-bambu-rootfs-" + [guid]::NewGuid().ToString('N')
try {
    & docker pull --platform linux/amd64 $BaseImage
    if ($LASTEXITCODE -ne 0) {
        throw "docker pull failed for image: $BaseImage"
    }

    & docker create --platform linux/amd64 --name $containerName $BaseImage /bin/sh -lc 'exit 0'
    if ($LASTEXITCODE -ne 0) {
        throw "docker create failed for image: $BaseImage"
    }

    if (Test-Path $OutputTar) {
        Remove-Item -Force $OutputTar
    }

    $exportProcess = Start-Process -FilePath docker -ArgumentList @('export', $containerName, '-o', $OutputTar) -NoNewWindow -Wait -PassThru
    if ($exportProcess.ExitCode -ne 0) {
        throw 'docker export failed'
    }

    if (!(Test-Path $OutputTar)) {
        throw "rootfs tar was not created: $OutputTar"
    }

    $size = (Get-Item $OutputTar).Length
    if ($size -le 0) {
        throw "rootfs tar is empty: $OutputTar"
    }

    Write-Host "WSL rootfs created:"
    Write-Host $OutputTar
}
finally {
    & docker rm -f $containerName 2>$null | Out-Null
}
