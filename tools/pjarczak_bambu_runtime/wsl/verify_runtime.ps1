param(
    [string]$PackageDir = "",
    [string]$DistroName = "",
    [string]$PluginCacheDir = "",
    [switch]$AllowMissingLinuxPlugin,
    [switch]$SkipProbe
)

$ErrorActionPreference = 'Stop'
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $script:__pj_prev_native_pref = $PSNativeCommandUseErrorActionPreference
    $PSNativeCommandUseErrorActionPreference = $false
}

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

function To-WslPath([string]$Path) {
    $full = [System.IO.Path]::GetFullPath($Path)
    if ($full.Length -ge 2 -and $full[1] -eq ':') {
        $drive = $full.Substring(0, 1).ToLowerInvariant()
        $tail = ($full.Substring(2) -replace '\\', '/')
        if ($tail.StartsWith('/')) {
            $tail = $tail.Substring(1)
        }
        return "/mnt/$drive/$tail"
    }
    return ($full -replace '\\', '/')
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

function Test-WslDistroExists([string]$WslPath, [string]$Name) {
    $probe = Invoke-NativeCapture $WslPath @('-d', $Name, '--user', 'root', '--', 'sh', '-lc', 'true')
    if ($probe.ExitCode -eq 0) {
        return @{
            Exists = $true
            Reason = ''
        }
    }

    $text = $probe.Combined
    $lower = $text.ToLowerInvariant()

    if ($lower.Contains('there is no distribution with the supplied name') -or
        $lower.Contains('wsl_e_distribution_not_found') -or
        ($lower.Contains('distribution') -and $lower.Contains('not') -and $lower.Contains('found'))) {
        return @{
            Exists = $false
            Reason = "WSL distro '$Name' is not installed"
        }
    }

    if ([string]::IsNullOrWhiteSpace($text)) {
        throw "Failed to start WSL distro '$Name'"
    }

    throw ("Failed to start WSL distro '{0}': {1}" -f $Name, $text)
}

if ([string]::IsNullOrWhiteSpace($PackageDir)) {
    $PackageDir = Get-ScriptDir
}
$PackageDir = [System.IO.Path]::GetFullPath($PackageDir)

if ([string]::IsNullOrWhiteSpace($PluginCacheDir)) {
    if ($env:PJARCZAK_BAMBU_WINDOWS_PLUGIN_CACHE_DIR) {
        $PluginCacheDir = $env:PJARCZAK_BAMBU_WINDOWS_PLUGIN_CACHE_DIR
    } else {
        $subdirFile = Join-Path $PackageDir 'pjarczak_plugin_cache_subdir.txt'
        if ((-not [string]::IsNullOrWhiteSpace($env:APPDATA)) -and (Test-Path $subdirFile)) {
            $subdir = (Get-Content $subdirFile -Raw).Trim()
            if (-not [string]::IsNullOrWhiteSpace($subdir)) {
                $PluginCacheDir = Join-Path $env:APPDATA $subdir
            }
        }
        if ([string]::IsNullOrWhiteSpace($PluginCacheDir) -and $env:APPDATA) {
            $PluginCacheDir = Join-Path $env:APPDATA 'OrcaSlicer\ota\plugins'
        }
    }
}
if (-not [string]::IsNullOrWhiteSpace($PluginCacheDir)) {
    $PluginCacheDir = [System.IO.Path]::GetFullPath($PluginCacheDir)
    $pluginsChild = Join-Path $PluginCacheDir 'plugins'
    if ((Split-Path -Leaf $PluginCacheDir) -ieq 'ota' -and (Test-Path $pluginsChild)) {
        $PluginCacheDir = [System.IO.Path]::GetFullPath($pluginsChild)
    } elseif ((Test-Path $pluginsChild) -and !(Test-Path (Join-Path $PluginCacheDir 'libbambu_networking.so')) -and !(Test-Path (Join-Path $PluginCacheDir 'libBambuSource.so'))) {
        $PluginCacheDir = [System.IO.Path]::GetFullPath($pluginsChild)
    }
}

if ([string]::IsNullOrWhiteSpace($DistroName)) {
    $distroFile = Join-Path $PackageDir 'pjarczak_wsl_distro.txt'
    if (Test-Path $distroFile) {
        $DistroName = (Get-Content $distroFile -Raw).Trim()
    }
}
if ([string]::IsNullOrWhiteSpace($DistroName)) {
    throw 'Missing distro name. Set PJARCZAK_WSL_DISTRO or provide pjarczak_wsl_distro.txt.'
}

$requiredFiles = @(
    'pjarczak_bambu_networking_bridge.dll',
    'pjarczak_wsl_distro.txt',
    'install_runtime.ps1',
    'verify_runtime.ps1',
    'pjarczak_bambu_linux_host',
    'pjarczak_bambu_linux_host_abi1',
    'pjarczak_bambu_linux_host_abi0',
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
Convert-FileToLf $bootstrapPath

$wsl = Join-Path $env:WINDIR 'System32\wsl.exe'
if (!(Test-Path $wsl)) {
    throw 'wsl.exe not found'
}

$distroStatus = Test-WslDistroExists $wsl $DistroName
if (-not $distroStatus.Exists) {
    throw $distroStatus.Reason
}

$packageDirWsl = To-WslPath $PackageDir
$pluginCacheDirWsl = ""
if (-not [string]::IsNullOrWhiteSpace($PluginCacheDir)) {
    $pluginCacheDirWsl = To-WslPath $PluginCacheDir
}
$bootstrapWsl = "$packageDirWsl/$([System.IO.Path]::GetFileName($bootstrapPath))"

Write-Host "Bridge package dir: $PackageDir"
Write-Host "Plugin cache dir: $PluginCacheDir"
Write-Host "WSL distro: $DistroName"
Write-Host "Bootstrap script: $bootstrapPath"

if ($SkipProbe) {
    Write-Host 'WSL runtime core OK'
    exit 0
}

$probe = Invoke-NativeCapture $wsl @('-d', $DistroName, '--user', 'root', '--', 'sh', $bootstrapWsl, '--probe', $packageDirWsl, $pluginCacheDirWsl)
if ($probe.ExitCode -ne 0) {
    $probeText = $probe.Combined
    if ($AllowMissingLinuxPlugin -and $probeText -match 'plugin_not_downloaded') {
        Write-Host 'WSL runtime package OK, linux plugin not downloaded yet.'
        Write-Host $probeText
        exit 0
    }
    throw "WSL runtime probe failed: $probeText"
}

Write-Host 'WSL runtime probe OK'
Write-Host $probe.Combined
