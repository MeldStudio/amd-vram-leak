# Load the Visual Studio dev environment and run the provided command
# Usage examples:
#
# .\with-devenv.ps1 cmake -Bout/dev -S. -GNinja
# .\with-devenv.ps1 ninja -C out/dev
#

if (-not $args -or -not $args[0]) {
  Write-Error 'Usage: .\with-devenv.ps1 <command> [args...]'
  exit 1
}

# Capture all arguments to pass through
$CmdArgs = $args

# Find Visual Studio installation
$DefaultVsPath = 'C:\Program Files\Microsoft Visual Studio\18\Community'
$DevShellDll = 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'

if (Test-Path (Join-Path $DefaultVsPath $DevShellDll)) {
  $VsInstallPath = $DefaultVsPath
}
else {
  # Try to find VS using vswhere
  $VsWherePath = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  
  if (-not (Test-Path $VsWherePath)) {
    # Download vswhere if not found
    $VsWhereUrl = 'https://github.com/microsoft/vswhere/releases/latest/download/vswhere.exe'
    $VsWherePath = Join-Path $env:TEMP 'vswhere.exe'
    
    Write-Host 'Downloading vswhere.exe...' -ForegroundColor Yellow
    try {
      Invoke-WebRequest -Uri $VsWhereUrl -OutFile $VsWherePath -UseBasicParsing
    }
    catch {
      Write-Error "Failed to download vswhere.exe: $_"
      exit 1
    }
  }
  
  # Use vswhere to find the latest VS installation
  $VsInstallPath = & $VsWherePath -latest -property installationPath -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
  
  if (-not $VsInstallPath) {
    # Fallback: try without component requirement
    $VsInstallPath = & $VsWherePath -latest -property installationPath
  }
  
  if (-not $VsInstallPath -or -not (Test-Path (Join-Path $VsInstallPath $DevShellDll))) {
    Write-Error 'Could not find Visual Studio installation with DevShell support'
    exit 1
  }
  
  Write-Host "Found Visual Studio at: $VsInstallPath" -ForegroundColor Green
}

# Run in a child process to isolate environment variables
& pwsh -NoProfile -Command {
  param(
    [string] $VsInstallPath,
    [string[]] $CmdArgs
  )

  Import-Module (Join-Path $VsInstallPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')

  $null = Enter-VsDevShell `
    -VsInstallPath $VsInstallPath `
    -Arch amd64 `
    -HostArch amd64 `
    -SkipAutomaticLocation 6>&1

  # Display what we're about to run (for logging only)
  Write-Host "Running: $($CmdArgs -join ' ')" -ForegroundColor Cyan

  # Split into executable and its arguments
  $exe = $CmdArgs[0]
  $exeArgs =
  if ($CmdArgs.Count -gt 1) {
    $CmdArgs[1..($CmdArgs.Count - 1)]
  }
  else {
    @()
  }

  # Run the command with proper argument passing
  & $exe @exeArgs

  exit $LASTEXITCODE
} -Args $VsInstallPath, $CmdArgs

# Propagate the exit code from the child pwsh
exit $LASTEXITCODE