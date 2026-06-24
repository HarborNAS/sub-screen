[CmdletBinding()]
param(
    [string]$MetadataPath = "",
    [string]$DlibPath = "",
    [string]$SignToolPath = "",
    [string]$DistDir = "",
    [string]$Description = "Harbor Subscreen Windows",
    [string]$ProductUrl = "https://github.com/HarborNAS/sub-screen"
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Resolve-Path (Join-Path $ScriptDir "..")
$ReleaseDir = Join-Path $ProjectRoot "build\Release"
$SubscreenExe = Join-Path $ReleaseDir "subscreen.exe"
$SetupExe = Join-Path $ReleaseDir "HarborSubscreenSetup.exe"

if ([string]::IsNullOrWhiteSpace($MetadataPath)) {
    $MetadataPath = Join-Path $ProjectRoot "signing\metadata.json"
} elseif (-not [System.IO.Path]::IsPathRooted($MetadataPath)) {
    $MetadataPath = Join-Path $ProjectRoot $MetadataPath
}

if ([string]::IsNullOrWhiteSpace($DistDir)) {
    $DistDir = Join-Path $ProjectRoot "dist"
} elseif (-not [System.IO.Path]::IsPathRooted($DistDir)) {
    $DistDir = Join-Path $ProjectRoot $DistDir
}

function Resolve-ToolFile {
    param(
        [string]$Path,
        [string]$FriendlyName
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $null
    }

    if (-not [System.IO.Path]::IsPathRooted($Path)) {
        $Path = Join-Path $ProjectRoot $Path
    }

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$FriendlyName was not found: $Path"
    }

    return (Resolve-Path -LiteralPath $Path).Path
}

function Find-SignTool {
    param([string]$RequestedPath)

    $resolved = Resolve-ToolFile -Path $RequestedPath -FriendlyName "signtool.exe"
    if ($resolved) {
        return $resolved
    }

    $cmd = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    $kitsRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    if (Test-Path -LiteralPath $kitsRoot) {
        $candidate = Get-ChildItem -LiteralPath $kitsRoot -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match "\\x64\\signtool\.exe$" } |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($candidate) {
            return $candidate.FullName
        }
    }

    throw "signtool.exe was not found. Install the Windows SDK or pass -SignToolPath."
}

function Find-ArtifactSigningDlib {
    param([string]$RequestedPath)

    $resolved = Resolve-ToolFile -Path $RequestedPath -FriendlyName "Azure.CodeSigning.Dlib.dll"
    if ($resolved) {
        return $resolved
    }

    if ($env:ARTIFACT_SIGNING_DLIB) {
        $resolved = Resolve-ToolFile -Path $env:ARTIFACT_SIGNING_DLIB -FriendlyName "Azure.CodeSigning.Dlib.dll"
        if ($resolved) {
            return $resolved
        }
    }

    $roots = @(
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft"),
        (Join-Path $env:ProgramFiles "Microsoft"),
        (Join-Path $env:USERPROFILE ".nuget\packages")
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }

    foreach ($root in $roots) {
        $candidate = Get-ChildItem -LiteralPath $root -Recurse -Filter Azure.CodeSigning.Dlib.dll -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match "\\x64\\Azure\.CodeSigning\.Dlib\.dll$" } |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($candidate) {
            return $candidate.FullName
        }
    }

    throw "Azure.CodeSigning.Dlib.dll was not found. Install Microsoft Artifact Signing Client Tools or pass -DlibPath."
}

function Invoke-Native {
    param(
        [string]$FilePath,
        [string[]]$Arguments,
        [string]$FailureMessage
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FailureMessage Exit code: $LASTEXITCODE"
    }
}

function Invoke-Build {
    param([string[]]$Arguments)

    Push-Location $ProjectRoot
    try {
        Invoke-Native -FilePath "cmd.exe" -Arguments (@("/c", "build.bat") + $Arguments) -FailureMessage "Build failed."
    } finally {
        Pop-Location
    }
}

function Invoke-Sign {
    param(
        [string]$Path,
        [string]$FileDescription
    )

    Invoke-Native -FilePath $script:SignTool -Arguments @(
        "sign",
        "/v",
        "/debug",
        "/fd", "SHA256",
        "/tr", "http://timestamp.acs.microsoft.com",
        "/td", "SHA256",
        "/d", $FileDescription,
        "/du", $ProductUrl,
        "/dlib", $script:Dlib,
        "/dmdf", $script:Metadata,
        $Path
    ) -FailureMessage "Signing failed for $Path."
}

function Invoke-Verify {
    param([string]$Path)

    Invoke-Native -FilePath $script:SignTool -Arguments @(
        "verify",
        "/pa",
        "/v",
        $Path
    ) -FailureMessage "Signature verification failed for $Path."
}

$script:SignTool = Find-SignTool -RequestedPath $SignToolPath
$script:Dlib = Find-ArtifactSigningDlib -RequestedPath $DlibPath
$script:Metadata = Resolve-ToolFile -Path $MetadataPath -FriendlyName "Artifact Signing metadata.json"

Write-Host "Using SignTool: $script:SignTool"
Write-Host "Using Artifact Signing Dlib: $script:Dlib"
Write-Host "Using metadata: $script:Metadata"

Invoke-Build -Arguments @("Release")

if (-not (Test-Path -LiteralPath $SubscreenExe -PathType Leaf)) {
    throw "Release subscreen.exe was not produced: $SubscreenExe"
}

Invoke-Sign -Path $SubscreenExe -FileDescription "Harbor Subscreen service executable"

# Rebuild only the setup bootstrapper so the resource compiler embeds the signed subscreen.exe.
Invoke-Build -Arguments @("Release", "setup-only")

if (-not (Test-Path -LiteralPath $SetupExe -PathType Leaf)) {
    throw "Release installer was not produced: $SetupExe"
}

Invoke-Sign -Path $SetupExe -FileDescription "Harbor Subscreen Windows installer"

Invoke-Verify -Path $SubscreenExe
Invoke-Verify -Path $SetupExe

New-Item -ItemType Directory -Force -Path $DistDir | Out-Null
$DistSetup = Join-Path $DistDir "HarborSubscreenSetup.exe"
$DistZip = Join-Path $DistDir "HarborSubscreen-Windows-OneClick.zip"
$DistHashes = Join-Path $DistDir "SHA256SUMS.txt"

Copy-Item -LiteralPath $SetupExe -Destination $DistSetup -Force
if (Test-Path -LiteralPath $DistZip) {
    Remove-Item -LiteralPath $DistZip -Force
}
Compress-Archive -LiteralPath $DistSetup -DestinationPath $DistZip -Force

$hashLines = @(
    "$(Get-FileHash -Algorithm SHA256 -LiteralPath $DistSetup | ForEach-Object Hash)  HarborSubscreenSetup.exe",
    "$(Get-FileHash -Algorithm SHA256 -LiteralPath $DistZip | ForEach-Object Hash)  HarborSubscreen-Windows-OneClick.zip"
)
$hashLines | Set-Content -LiteralPath $DistHashes -Encoding ASCII

Write-Host ""
Write-Host "Signed release artifacts:"
Write-Host "  $DistSetup"
Write-Host "  $DistZip"
Write-Host "  $DistHashes"
