param(
    [string]$PackageDir = "",
    [string]$PluginDir = "",
    [string]$PluginCacheDir = "",
    [string]$DistroName = "",
    [string]$InstallDir = "",
    [switch]$ReplaceExisting,
    [switch]$SkipCopyToPluginDir
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

function Convert-FileToLf([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or !(Test-Path $Path)) {
        return
    }

    $content = [System.IO.File]::ReadAllText($Path)
    $content = $content.Replace("`r`n", "`n").Replace("`r", "`n")
    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($Path, $content, $utf8NoBom)
}

function Copy-IfExists([string]$Source, [string]$Destination) {
    if (Test-Path $Source) {
        $srcFull = [System.IO.Path]::GetFullPath($Source)
        $dstFull = [System.IO.Path]::GetFullPath($Destination)
        if ($srcFull -ieq $dstFull) {
            return
        }
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
        Copy-Item -Force $Source $Destination
    }
}

function Sync-Directory([string]$SourceDir, [string]$DestinationDir) {
    if (!(Test-Path $SourceDir)) {
        return
    }
    if (Test-Path $DestinationDir) {
        Remove-Item -Recurse -Force $DestinationDir
    }
    New-Item -ItemType Directory -Force -Path $DestinationDir | Out-Null
    Copy-Item -Recurse -Force (Join-Path $SourceDir '*') $DestinationDir
}

function Resolve-DistroName([string]$Dir, [string]$Current) {
    if (-not [string]::IsNullOrWhiteSpace($Current)) {
        return $Current
    }

    $distroFile = Join-Path $Dir 'pjarczak_wsl_distro.txt'
    if (Test-Path $distroFile) {
        $value = (Get-Content $distroFile -Raw).Trim()
        if (-not [string]::IsNullOrWhiteSpace($value)) {
            return $value
        }
    }

    if ($env:PJARCZAK_WSL_DISTRO) {
        return $env:PJARCZAK_WSL_DISTRO.Trim()
    }

    return 'PJARCZAK-BAMBU'
}

function Get-FileSha256([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -Path $Path).Hash.ToLowerInvariant()
}

function Get-RootFsHashMarkerPath([string]$Dir) {
    return (Join-Path $Dir 'pjarczak-rootfs-sha256.txt')
}

function Write-RootFsHashMarker([string]$Dir, [string]$Hash) {
    if ([string]::IsNullOrWhiteSpace($Dir) -or [string]::IsNullOrWhiteSpace($Hash)) {
        return
    }
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    Set-Content -Path (Get-RootFsHashMarkerPath $Dir) -Value ($Hash.Trim().ToLowerInvariant()) -NoNewline
}

function Read-RootFsHashMarker([string]$Dir) {
    $path = Get-RootFsHashMarkerPath $Dir
    if (!(Test-Path $path)) {
        return ''
    }
    return ((Get-Content $path -Raw).Trim().ToLowerInvariant())
}


function Resolve-PluginCacheDir([string]$Dir, [string]$Current) {
    if (-not [string]::IsNullOrWhiteSpace($Current)) {
        return [System.IO.Path]::GetFullPath($Current.Trim())
    }

    if ($env:PJARCZAK_BAMBU_WINDOWS_PLUGIN_CACHE_DIR) {
        return [System.IO.Path]::GetFullPath($env:PJARCZAK_BAMBU_WINDOWS_PLUGIN_CACHE_DIR.Trim())
    }

    $subdirFile = Join-Path $Dir 'pjarczak_plugin_cache_subdir.txt'
    if (Test-Path $subdirFile) {
        $subdir = (Get-Content $subdirFile -Raw).Trim()
        if (-not [string]::IsNullOrWhiteSpace($subdir)) {
            if (-not $env:APPDATA) { throw 'APPDATA is not available' }
            return [System.IO.Path]::GetFullPath((Join-Path $env:APPDATA $subdir))
        }
    }

    if (-not $env:APPDATA) { throw 'APPDATA is not available' }
    return [System.IO.Path]::GetFullPath((Join-Path $env:APPDATA 'OrcaSlicer\ota\plugins'))
}

function Normalize-PluginCacheDir([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $Path
    }
    $full = [System.IO.Path]::GetFullPath($Path)
    $pluginsChild = Join-Path $full 'plugins'
    if ((Split-Path -Leaf $full) -ieq 'ota' -and (Test-Path $pluginsChild)) {
        return [System.IO.Path]::GetFullPath($pluginsChild)
    }
    if ((Test-Path $pluginsChild) -and !(Test-Path (Join-Path $full 'libbambu_networking.so')) -and !(Test-Path (Join-Path $full 'libBambuSource.so'))) {
        return [System.IO.Path]::GetFullPath($pluginsChild)
    }
    return $full
}

function Read-TextAuto([string]$Path) {
    if (!(Test-Path $Path)) {
        return ''
    }

    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -eq 0) {
        return ''
    }

    if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) {
        return ([System.Text.Encoding]::Unicode.GetString($bytes, 2, $bytes.Length - 2) -replace "`0", '')
    }
    if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFE -and $bytes[1] -eq 0xFF) {
        return ([System.Text.Encoding]::BigEndianUnicode.GetString($bytes, 2, $bytes.Length - 2) -replace "`0", '')
    }
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        return ([System.Text.Encoding]::UTF8.GetString($bytes, 3, $bytes.Length - 3) -replace "`0", '')
    }

    for ($i = 1; $i -lt [Math]::Min($bytes.Length, 64); $i += 2) {
        if ($bytes[$i] -eq 0) {
            return ([System.Text.Encoding]::Unicode.GetString($bytes) -replace "`0", '')
        }
    }

    return ([System.Text.Encoding]::UTF8.GetString($bytes) -replace "`0", '')
}

function Normalize-NativeText([string]$Text) {
    if ([string]::IsNullOrEmpty($Text)) {
        return ''
    }
    $value = $Text -replace "`0", ''
    $value = $value -replace "`r`n", "`n"
    $value = $value -replace "`r", "`n"
    return $value
}

function Invoke-NativeCapture([string]$FilePath, [string[]]$ArgumentList) {
    $stdoutPath = [System.IO.Path]::GetTempFileName()
    $stderrPath = [System.IO.Path]::GetTempFileName()
    try {
        $proc = Start-Process -FilePath $FilePath -ArgumentList $ArgumentList -Wait -PassThru -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -WindowStyle Hidden
        $stdoutText = if (Test-Path $stdoutPath) { Normalize-NativeText (Read-TextAuto $stdoutPath) } else { '' }
        $stderrText = if (Test-Path $stderrPath) { Normalize-NativeText (Read-TextAuto $stderrPath) } else { '' }
        $combined = (($stdoutText + "`n" + $stderrText).Trim())
        return @{
            ExitCode = $proc.ExitCode
            StdOut = $stdoutText
            StdErr = $stderrText
            Combined = $combined
        }
    } finally {
        Remove-Item -Force -ErrorAction SilentlyContinue $stdoutPath, $stderrPath
    }
}

function Test-WslDistroExists([string]$WslPath, [string]$Name, [ref]$Reason) {
    $list = Invoke-NativeCapture $WslPath @('--list', '--quiet')
    if ($list.ExitCode -ne 0) {
        $text = $list.Combined
        if ([string]::IsNullOrWhiteSpace($text)) {
            throw 'Failed to query WSL distributions'
        }
        throw ("Failed to query WSL distributions: {0}" -f $text)
    }

    $exists = $false
    foreach ($line in ($list.StdOut -split "`n")) {
        $item = $line.Trim()
        if ([string]::IsNullOrWhiteSpace($item)) {
            continue
        }
        if ($item -ieq $Name) {
            $exists = $true
            break
        }
    }

    if (-not $exists) {
        $Reason.Value = "WSL distro '$Name' is not installed"
        return $false
    }

    $probe = Invoke-NativeCapture $WslPath @('-d', $Name, '--user', 'root', '--', 'sh', '-lc', 'true')
    if ($probe.ExitCode -eq 0) {
        $Reason.Value = ''
        return $true
    }

    $text = $probe.Combined
    if ([string]::IsNullOrWhiteSpace($text)) {
        throw "Failed to start WSL distro '$Name'"
    }

    throw ("Failed to start WSL distro '{0}': {1}" -f $Name, $text)
}

$scriptDir = Get-ScriptDir
$defaultPackageDir = $scriptDir
if ([string]::IsNullOrWhiteSpace($PackageDir)) {
    $PackageDir = $defaultPackageDir
}
$PackageDir = [System.IO.Path]::GetFullPath($PackageDir)

$DistroName = Resolve-DistroName $PackageDir $DistroName
$PluginCacheDir = Normalize-PluginCacheDir (Resolve-PluginCacheDir $PackageDir $PluginCacheDir)

if ([string]::IsNullOrWhiteSpace($PluginDir)) {
    if (-not $env:APPDATA) {
        throw 'APPDATA is not available'
    }
    $PluginDir = Join-Path $env:APPDATA 'OrcaSlicer\plugins'
}
$PluginDir = [System.IO.Path]::GetFullPath($PluginDir)

if ([string]::IsNullOrWhiteSpace($InstallDir)) {
    if (-not $env:LOCALAPPDATA) {
        throw 'LOCALAPPDATA is not available'
    }
    $InstallDir = Join-Path $env:LOCALAPPDATA $DistroName
}
$InstallDir = [System.IO.Path]::GetFullPath($InstallDir)

$wsl = Join-Path $env:WINDIR 'System32\wsl.exe'
if (!(Test-Path $wsl)) {
    throw 'wsl.exe not found'
}

if (-not $SkipCopyToPluginDir) {
    New-Item -ItemType Directory -Force -Path $PluginDir | Out-Null

    $fileNames = @(
        'pjarczak_bambu_networking_bridge.dll',
        'pjarczak_bambu_linux_host',
        'pjarczak_bambu_linux_host_abi1',
        'pjarczak_bambu_linux_host_abi0',
        'pjarczak_wsl_distro.txt',
        'pjarczak_plugin_cache_subdir.txt',
        'pjarczak_wsl_run_host.sh',
        'pjarczak-wsl-run-host.sh',
        'install_runtime.ps1',
        'install_runtime.cmd',
        'verify_runtime.ps1',
        'windows-wsl2-rootfs.tar',
        'README_runtime_bridge.txt',
        'assemble_windows_runtime_bundle.ps1',
        'linux_payload_manifest.json',
        'libbambu_networking.so',
        'libBambuSource.so',
        'liblive555.so',
        'libagora_rtc_sdk.so',
        'libagora-fdkaac.so',
        'ca-certificates.crt',
        'slicer_base64.cer'
    )

    foreach ($name in $fileNames) {
        Copy-IfExists (Join-Path $PackageDir $name) (Join-Path $PluginDir $name)
    }

    Get-ChildItem -Path $PackageDir -File -ErrorAction SilentlyContinue | ForEach-Object {
        $name = $_.Name
        if ($name -match '^lib.+\.so(\..+)?$') {
            Copy-IfExists $_.FullName (Join-Path $PluginDir $name)
        }
    }

    $legacyRuntimeDir = Join-Path $PluginDir 'pjarczak_bambu_linux_host.runtime'
    if (Test-Path $legacyRuntimeDir) {
        Remove-Item -Recurse -Force $legacyRuntimeDir
    }
    $PackageDir = $PluginDir

    Write-Host "Bridge package dir: $PackageDir"
    Write-Host "Plugin dir: $PluginDir"
    Write-Host "Plugin cache dir: $PluginCacheDir"
    Write-Host "WSL distro: $DistroName"
}

$requiredFiles = @(
    'pjarczak_bambu_networking_bridge.dll',
    'pjarczak_bambu_linux_host',
    'pjarczak_bambu_linux_host_abi1',
    'pjarczak_bambu_linux_host_abi0',
    'pjarczak_wsl_distro.txt',
    'install_runtime.ps1',
    'verify_runtime.ps1',
    'windows-wsl2-rootfs.tar',
    'ca-certificates.crt',
    'slicer_base64.cer'
)

foreach ($name in $requiredFiles) {
    $path = Join-Path $PackageDir $name
    if (!(Test-Path $path)) {
        throw "Missing package file: $name"
    }
}

$bootstrapPath = Join-Path $PackageDir 'pjarczak_wsl_run_host.sh'
if (!(Test-Path $bootstrapPath)) { $bootstrapPath = Join-Path $PackageDir 'pjarczak-wsl-run-host.sh' }
if (!(Test-Path $bootstrapPath)) {
    throw 'Missing package file: pjarczak_wsl_run_host.sh'
}

try {
    & $wsl --status | Out-Null
} catch {
    throw 'WSL is not ready. Run as Administrator once and enable Microsoft-Windows-Subsystem-Linux and VirtualMachinePlatform, then reboot.'
}

Convert-FileToLf $bootstrapPath

$rootFsTar = Join-Path $PackageDir 'windows-wsl2-rootfs.tar'
$currentRootFsHash = Get-FileSha256 $rootFsTar
$storedRootFsHash = Read-RootFsHashMarker $InstallDir

$distroReason = ''
$alreadyInstalled = Test-WslDistroExists $wsl $DistroName ([ref]$distroReason)
if ($alreadyInstalled) {
    if (-not $ReplaceExisting) {
        if ([string]::IsNullOrWhiteSpace($storedRootFsHash) -or $storedRootFsHash -ne $currentRootFsHash) {
            Write-Host "WSL rootfs changed or marker missing - reinstalling distro $DistroName"
            $ReplaceExisting = $true
        }
    }

    if ($ReplaceExisting) {
        & $wsl --terminate $DistroName 2>$null | Out-Null
        & $wsl --unregister $DistroName
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to unregister existing distro '$DistroName'"
        }
        $alreadyInstalled = $null
    }
}

if (-not $alreadyInstalled) {
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

    & $wsl --import $DistroName $InstallDir $rootFsTar --version 2
    if ($LASTEXITCODE -ne 0) {
        throw "wsl --import failed for distro '$DistroName'"
    }

    $wslConf = @'
[automount]
enabled=true
root=/mnt/
mountFsTab=false

[interop]
enabled=true
appendWindowsPath=false
'@

    $setupCmd = @"
cat > /etc/wsl.conf <<'WSL_EOF'
$wslConf
WSL_EOF
mkdir -p /root/.pjarczak-bambu-runtime
"@

    & $wsl -d $DistroName --user root -- sh -lc $setupCmd
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to initialize distro '$DistroName'"
    }

    & $wsl --terminate $DistroName
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to terminate distro '$DistroName' after initialization"
    }

    Write-RootFsHashMarker $InstallDir $currentRootFsHash
} elseif ($storedRootFsHash -ne $currentRootFsHash) {
    Write-RootFsHashMarker $InstallDir $currentRootFsHash
}

$verifyArgs = @(
    '-NoProfile',
    '-ExecutionPolicy', 'Bypass',
    '-File', (Join-Path $PackageDir 'verify_runtime.ps1'),
    '-PackageDir', $PackageDir,
    '-DistroName', $DistroName,
    '-PluginCacheDir', $PluginCacheDir,
    '-AllowMissingLinuxPlugin',
    '-SkipProbe'
)

$verifyShell = $null
$pwshCmd = Get-Command pwsh -ErrorAction SilentlyContinue
if ($pwshCmd) {
    $verifyShell = $pwshCmd.Source
} else {
    $powershellCmd = Get-Command powershell -ErrorAction SilentlyContinue
    if ($powershellCmd) {
        $verifyShell = $powershellCmd.Source
    }
}
if ([string]::IsNullOrWhiteSpace($verifyShell)) {
    throw 'No PowerShell host found to run verify_runtime.ps1'
}

& $verifyShell @verifyArgs
if ($LASTEXITCODE -ne 0) {
    throw 'verify_runtime.ps1 failed'
}

Write-Host ''
Write-Host "WSL runtime installed to: $PackageDir"
Write-Host "WSL distro: $DistroName"
Write-Host "WSL install dir: $InstallDir"
Write-Host 'Now start OrcaSlicer.'
