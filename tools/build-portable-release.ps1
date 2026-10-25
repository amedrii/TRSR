param(
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [string]$BuildDirectory = "build\Desktop_Qt_6_11_2_MinGW_64_bit_Release"
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $projectRoot $BuildDirectory
$clientExe = Join-Path $buildDirectory "TRASR.exe"
$releaseDirectory = Join-Path $projectRoot "TRSR-$Version-Windows"
$releaseZip = "$releaseDirectory.zip"
$portableBuildDirectory = Join-Path $buildDirectory "portable-tools"
$portableDistDirectory = Join-Path $portableBuildDirectory "dist"
$qtBin = "D:\Qt\6.11.2\mingw_64\bin"
$tesseractSource = "C:\Program Files\Tesseract-OCR"

if (-not (Test-Path -LiteralPath $clientExe)) {
    throw "Release client not found: $clientExe. Build the Release target in Qt Creator first."
}
if (-not (Test-Path -LiteralPath (Join-Path $qtBin "windeployqt.exe"))) {
    throw "Qt deployment tool not found at $qtBin. Update `$qtBin in this script for your Qt installation."
}
if (-not (Test-Path -LiteralPath (Join-Path $tesseractSource "tesseract.exe"))) {
    throw "Tesseract was not found at $tesseractSource. Install it on the release-building PC first."
}
if (Test-Path -LiteralPath $releaseDirectory) {
    throw "Release directory already exists: $releaseDirectory. Rename or remove it before packaging."
}
if (Test-Path -LiteralPath $releaseZip) {
    throw "Release ZIP already exists: $releaseZip. Rename or remove it before packaging."
}

# PyInstaller is a build-time-only dependency. Testers never need Python or pip.
& py -3.13 -m PyInstaller --version *> $null
if ($LASTEXITCODE -ne 0) {
    & py -3.13 -m pip install --upgrade pyinstaller
    if ($LASTEXITCODE -ne 0) {
        throw "Could not install PyInstaller."
    }
}

Remove-Item -LiteralPath $portableBuildDirectory -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $portableBuildDirectory | Out-Null

function Build-Helper([string]$Name, [string]$Script) {
    & py -3.13 -m PyInstaller --noconfirm --clean --onedir --name $Name `
        --distpath $portableDistDirectory `
        --workpath (Join-Path $portableBuildDirectory "work-$Name") `
        --specpath $portableBuildDirectory `
        --collect-all mss --collect-all pytesseract --collect-all PIL `
        (Join-Path $projectRoot $Script)
    if ($LASTEXITCODE -ne 0) {
        throw "PyInstaller failed while building $Name."
    }
}

Build-Helper "level_end_detector" "tools\level_end_detector.py"
Build-Helper "menu_navigator" "tools\menu_navigator.py"

New-Item -ItemType Directory -Path $releaseDirectory | Out-Null
Copy-Item -LiteralPath $clientExe -Destination (Join-Path $releaseDirectory "TRASR.exe")

& (Join-Path $qtBin "windeployqt.exe") --release --compiler-runtime `
    (Join-Path $releaseDirectory "TRASR.exe")
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed."
}

$toolsDirectory = Join-Path $releaseDirectory "tools"
New-Item -ItemType Directory -Path $toolsDirectory | Out-Null
Copy-Item -LiteralPath (Join-Path $portableDistDirectory "level_end_detector") `
    -Destination $toolsDirectory -Recurse
Copy-Item -LiteralPath (Join-Path $portableDistDirectory "menu_navigator") `
    -Destination $toolsDirectory -Recurse

$tesseractDestination = Join-Path $toolsDirectory "tesseract"
New-Item -ItemType Directory -Path $tesseractDestination | Out-Null
Copy-Item -LiteralPath (Join-Path $tesseractSource "tesseract.exe") -Destination $tesseractDestination
Copy-Item -Path (Join-Path $tesseractSource "*.dll") -Destination $tesseractDestination
Copy-Item -LiteralPath (Join-Path $tesseractSource "tessdata") -Destination $tesseractDestination -Recurse

Copy-Item -LiteralPath (Join-Path $projectRoot "README.md") -Destination $releaseDirectory
Copy-Item -LiteralPath (Join-Path $projectRoot "THIRD_PARTY_NOTICES.txt") -Destination $releaseDirectory
Copy-Item -LiteralPath (Join-Path $tesseractSource "doc\LICENSE") `
    -Destination (Join-Path $releaseDirectory "TESSERACT_LICENSE.txt")
if (Test-Path -LiteralPath "D:\Qt\6.11.2\mingw_64\LICENSES") {
    Copy-Item -LiteralPath "D:\Qt\6.11.2\mingw_64\LICENSES" `
        -Destination (Join-Path $releaseDirectory "LICENSES") -Recurse
}

Compress-Archive -Path (Join-Path $releaseDirectory "*") -DestinationPath $releaseZip
Write-Host "Portable release created: $releaseZip"
