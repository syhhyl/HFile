$ErrorActionPreference = 'Stop'

$RepoOwner = 'syhhyl'
$RepoName = 'HFile'
$BinName = 'hf.exe'
$ApiBase = "https://api.github.com/repos/$RepoOwner/$RepoName"
$ReleaseBase = "https://github.com/$RepoOwner/$RepoName/releases/download"

function Write-Log {
  param([string]$Message)
  Write-Host $Message
}

function Fail {
  param([string]$Message)
  throw $Message
}

function Resolve-Version {
  $release = Invoke-RestMethod -Uri "$ApiBase/releases/latest"
  if (-not $release.tag_name) {
    Fail 'failed to resolve release version'
  }

  return $release.tag_name
}

function Assert-SupportedTarget {
  $arch = $env:PROCESSOR_ARCHITECTURE
  if ($arch -ne 'AMD64') {
    Fail "unsupported platform: windows-$arch"
  }
}

function Ensure-InstallDir {
  param([string]$Path)

  if (-not (Test-Path -LiteralPath $Path)) {
    New-Item -ItemType Directory -Path $Path | Out-Null
  }

  if (-not (Test-Path -LiteralPath $Path)) {
    Fail "failed to create install directory: $Path"
  }
}

function Ensure-UserPath {
  param([string]$Path)

  $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
  $segments = @()
  if ($userPath) {
    $segments = $userPath.Split(';', [System.StringSplitOptions]::RemoveEmptyEntries)
  }

  if ($segments -contains $Path) {
    return
  }

  $newPath = if ($userPath) { "$userPath;$Path" } else { $Path }
  [Environment]::SetEnvironmentVariable('Path', $newPath, 'User')
  Write-Log "Added $Path to the user PATH"
  Write-Log 'Open a new terminal window before running hf'
}

Assert-SupportedTarget

$tag = Resolve-Version
$archiveName = 'hf-windows-amd64.zip'
$checksumsName = 'checksums.txt'
$archiveUrl = "$ReleaseBase/$tag/$archiveName"
$checksumsUrl = "$ReleaseBase/$tag/$checksumsName"
$baseDir = [Environment]::GetFolderPath('LocalApplicationData')
$installDir = Join-Path $baseDir 'Programs\HFile\bin'
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("hfile-install-" + [System.Guid]::NewGuid().ToString('N'))
$extractDir = Join-Path $tempRoot 'extract'
$archivePath = Join-Path $tempRoot $archiveName
$checksumsPath = Join-Path $tempRoot $checksumsName

try {
  New-Item -ItemType Directory -Path $tempRoot | Out-Null
  New-Item -ItemType Directory -Path $extractDir | Out-Null

  Write-Log "Installing hf $tag for windows-amd64"
  Write-Log "Download: $archiveUrl"

  $previousProgressPreference = $ProgressPreference
  $ProgressPreference = 'SilentlyContinue'
  try {
    Invoke-WebRequest -Uri $archiveUrl -OutFile $archivePath
    Invoke-WebRequest -Uri $checksumsUrl -OutFile $checksumsPath
  }
  finally {
    $ProgressPreference = $previousProgressPreference
  }

  $expected = Select-String -Path $checksumsPath -Pattern ([regex]::Escape($archiveName)) |
    ForEach-Object { ($_ -split '\s+')[0] } |
    Select-Object -First 1

  if (-not $expected) {
    Fail "checksum entry not found for $archiveName"
  }

  $actual = (Get-FileHash -Algorithm SHA256 -Path $archivePath).Hash.ToLowerInvariant()
  if ($expected.ToLowerInvariant() -ne $actual) {
    Fail "checksum mismatch for $archiveName"
  }

  Expand-Archive -Path $archivePath -DestinationPath $extractDir -Force

  $binPath = Join-Path $extractDir $BinName
  if (-not (Test-Path -LiteralPath $binPath)) {
    Fail "archive does not contain $BinName at the top level"
  }

  Ensure-InstallDir -Path $installDir
  Copy-Item -Path $binPath -Destination (Join-Path $installDir $BinName) -Force
  Ensure-UserPath -Path $installDir

  Write-Log "Installed to $(Join-Path $installDir $BinName)"
  Write-Log "Run 'hf -h' to get started"
}
finally {
  if (Test-Path -LiteralPath $tempRoot) {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force
  }
}
