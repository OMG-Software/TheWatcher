[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [switch]$SkipWindowsInstall,
    [switch]$InstallWsl,
    [switch]$SkipWsl,
    [string]$LinuxDistro = "Ubuntu",
    [switch]$SkipNode,
    [switch]$NoVerify
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

function Write-Step {
    param([string]$Message)
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Test-Command {
    param([string]$Name)
    return $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Invoke-LoggedCommand {
    param(
        [string]$FilePath,
        [string[]]$Arguments,
        [string]$Description
    )

    $rendered = "$FilePath $($Arguments -join ' ')"
    if ($PSCmdlet.ShouldProcess($Description, $rendered)) {
        & $FilePath @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "$Description failed with exit code $LASTEXITCODE"
        }
    } else {
        Write-Host "What if: $rendered"
    }
}

function Install-WingetPackage {
    param(
        [string]$Id,
        [string]$Name,
        [string[]]$ExtraArgs = @()
    )

    $args = @(
        "install",
        "--id", $Id,
        "--exact",
        "--accept-package-agreements",
        "--accept-source-agreements"
    ) + $ExtraArgs

    Invoke-LoggedCommand -FilePath "winget" -Arguments $args -Description "Install $Name"
}

function Get-VsWhere {
    $candidate = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $candidate) {
        return $candidate
    }

    return $null
}

function Test-VisualCppTools {
    $vswhere = Get-VsWhere
    if ($null -eq $vswhere) {
        return $false
    }

    $install = & $vswhere -latest -products * -version "[17.0,)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    return -not [string]::IsNullOrWhiteSpace($install)
}

function Install-WindowsDependencies {
    if (-not (Test-Command "winget")) {
        throw "winget is required. Install App Installer from Microsoft Store, then rerun this script."
    }

    if (-not (Test-Command "git")) {
        Install-WingetPackage -Id "Git.Git" -Name "Git for Windows"
    } else {
        Write-Host "Git is already available."
    }

    if (-not (Test-Command "python")) {
        Install-WingetPackage -Id "Python.Python.3.12" -Name "Python 3.12"
    } else {
        Write-Host "Python is already available."
    }

    if (-not (Test-Command "cmake")) {
        Install-WingetPackage -Id "Kitware.CMake" -Name "CMake"
    } else {
        Write-Host "CMake is already available."
    }

    if (-not $SkipNode -and -not (Test-Command "node")) {
        Install-WingetPackage -Id "OpenJS.NodeJS.LTS" -Name "Node.js LTS"
    }

    if (-not (Test-VisualCppTools)) {
        Install-WingetPackage `
            -Id "Microsoft.VisualStudio.2022.BuildTools" `
            -Name "Visual Studio 2022 Build Tools" `
            -ExtraArgs @(
                "--override",
                "--wait --quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --add Microsoft.VisualStudio.Component.Windows11SDK.26100"
            )
    } else {
        Write-Host "Visual C++ Build Tools are already installed."
    }
}

function Install-MesonAndNinja {
    if (-not (Test-Command "python")) {
        throw "python is not on PATH. Open a new PowerShell window after installation and rerun this script."
    }

    Invoke-LoggedCommand -FilePath "python" -Arguments @("-m", "pip", "install", "--upgrade", "meson>=1.4", "ninja") `
        -Description "Install Meson and Ninja via pip"
}

function Install-WslDependencies {
    if ($SkipWsl) {
        Write-Host "Skipping WSL setup."
        return
    }

    if (-not $InstallWsl) {
        Write-Host "Skipping WSL setup. Pass -InstallWsl to install a Linux build environment."
        return
    }

    if (-not (Test-IsAdmin)) {
        throw "WSL installation requires an elevated PowerShell session."
    }

    $distros = @()
    if (Test-Command "wsl.exe") {
        $distros = @(wsl.exe --list --quiet 2>$null | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    }

    if ($distros.Count -eq 0) {
        Invoke-LoggedCommand -FilePath "wsl.exe" -Arguments @("--install", "-d", $LinuxDistro) -Description "Install WSL distro $LinuxDistro"
        Write-Warning "If WSL requested a reboot or first-user setup, complete that first and rerun this script."
        return
    }

    Write-Host "WSL distro detected: $($distros -join ', ')"
    $packages = "build-essential clang lld git pkg-config autoconf automake libtool make cmake ninja-build unzip zip curl ca-certificates python3 python3-pip"
    Invoke-LoggedCommand `
        -FilePath "wsl.exe" `
        -Arguments @("-d", $LinuxDistro, "--", "bash", "-lc", "sudo apt-get update && sudo apt-get install -y $packages && pip3 install --user --upgrade 'meson>=1.4' ninja") `
        -Description "Install Linux build packages in WSL"
}

function Test-Environment {
    Write-Step "Environment verification"

    if (Test-Command "git") {
        Write-Host "Git: $((git --version) 2>&1 | Select-Object -First 1)"
    } else {
        throw "Git was not found on PATH."
    }

    if (Test-Command "python") {
        Write-Host "Python: $((python --version) 2>&1 | Select-Object -First 1)"
    } else {
        throw "Python was not found on PATH."
    }

    if (Test-Command "meson") {
        Write-Host "Meson: $((meson --version) 2>&1 | Select-Object -First 1)"
    } else {
        throw "Meson was not found on PATH. Open a new PowerShell window so pip's Scripts dir is on PATH."
    }

    if (Test-Command "ninja") {
        Write-Host "Ninja: $((ninja --version) 2>&1 | Select-Object -First 1)"
    } else {
        throw "Ninja was not found on PATH."
    }

    if (Test-Command "cmake") {
        Write-Host "CMake: $((cmake --version) 2>&1 | Select-Object -First 1)"
    } else {
        throw "CMake was not found on PATH."
    }

    if (-not (Test-VisualCppTools)) {
        throw "Visual C++ Build Tools were not detected by vswhere."
    }
    Write-Host "Visual C++ Build Tools: detected"

    if (-not $SkipNode) {
        if (Test-Command "node") {
            Write-Host "Node.js: $((node --version) 2>&1 | Select-Object -First 1)"
        } else {
            Write-Warning "Node.js was not found on PATH; the dashboard build will not work."
        }
    }
}

if ($InstallWsl -and $SkipWsl) {
    throw "Use either -InstallWsl or -SkipWsl, not both."
}

Write-Step "TheWatcher build environment setup"
Write-Host "Repository: $RepoRoot"

if (-not $SkipWindowsInstall) {
    Write-Step "Windows build dependencies"
    Install-WindowsDependencies

    Write-Step "Meson and Ninja"
    Install-MesonAndNinja
} else {
    Write-Host "Skipping Windows dependency installation."
}

Write-Step "Optional WSL/Linux build dependencies"
Install-WslDependencies

if (-not $NoVerify) {
    Test-Environment
}

Write-Host ""
Write-Host "Setup complete. Open a new PowerShell window if this script installed new tools." -ForegroundColor Green
Write-Host "Build with: .\meson-build.cmd  (or from a VS developer prompt: meson setup builddir-release; meson compile -C builddir-release)" -ForegroundColor Green
