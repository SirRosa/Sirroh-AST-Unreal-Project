#Requires -Version 5.1
<#
AST Fire Stage 4: small source-only deployment and headless UE material setup.
Never modifies original AST maps, Blueprint torches or placeholder assets.
Close UnrealEditor before running; build is outside Google Drive sync tree.
#>
[CmdletBinding()]
param(
    [string]$ProjectRoot = 'D:\0 Sirro\Unreal Projects\AST_V1_18',
    [string]$EngineRoot = 'D:\0 Sirro\UE 5.8.1\UE_5.8',
    [string]$Revision = ''
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
if ([string]::IsNullOrWhiteSpace($Revision)) {
    # Resolve HEAD once: all files in this deployment come from one commit.
    $branch = Invoke-RestMethod -Uri 'https://api.github.com/repos/SirRosa/Sirroh-AST-Unreal-Project/branches/work/niagara-fire-stage2' -TimeoutSec 30
    $Revision = $branch.commit.sha
}
if ($Revision -notmatch '^[0-9a-fA-F]{40}$') {
    throw 'Revision must be a full commit SHA (or omitted for current stage2 branch HEAD).'
}
$timestamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$report = Join-Path $ProjectRoot 'Saved\ChatGPT\NiagaraFire\Stage4'
$backup = Join-Path $report ("Backup_" + $timestamp)
$stage = Join-Path ([IO.Path]::GetTempPath()) ("ASTFireStage4Sources_" + $timestamp)
$package = Join-Path ([IO.Path]::GetTempPath()) ("ASTFireStage4Build_" + $timestamp)
$project = Join-Path $ProjectRoot 'AST_V1_18.uproject'
$uat = Join-Path $EngineRoot 'Engine\Build\BatchFiles\RunUAT.bat'
$ue = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$files = @(
    'Plugins/ASTFireRuntime/ASTFireRuntime.uplugin',
    'Plugins/ASTFireRuntime/Source/ASTFireRuntime/ASTFireRuntime.Build.cs',
    'Plugins/ASTFireRuntime/Source/ASTFireRuntime/Public/ASTFireReactiveComponent.h',
    'Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireReactiveComponent.cpp',
    'Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireBlueprintLibrary.cpp',
    'Automation/ASTFire/setup_stage4_volume.py'
)
foreach ($required in @($project, $uat, $ue)) {
    if (!(Test-Path -LiteralPath $required)) { throw "Missing: $required" }
}
if (Get-Process -Name UnrealEditor -ErrorAction SilentlyContinue) {
    throw 'Close Unreal Editor before replacing plugin DLLs.'
}
New-Item -ItemType Directory -Force -Path $stage,$report,$backup | Out-Null
Write-Output "AST_FIRE_STAGE4_REVISION=$Revision"
foreach ($relative in $files) {
    $target = Join-Path $stage ($relative.Replace('/', '\'))
    New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
    $url = "https://raw.githubusercontent.com/SirRosa/Sirroh-AST-Unreal-Project/$Revision/$relative"
    Invoke-WebRequest -Uri $url -OutFile $target -UseBasicParsing -TimeoutSec 45
    if ((Get-Item -LiteralPath $target).Length -eq 0) { throw "Empty download: $relative" }
}
# Stage fully before touching the project, and back up only the small allowlist.
foreach ($relative in $files) {
    $dest = Join-Path $ProjectRoot ($relative.Replace('/', '\'))
    if (Test-Path -LiteralPath $dest) {
        $old = Join-Path $backup ($relative.Replace('/', '\'))
        New-Item -ItemType Directory -Force -Path (Split-Path $old -Parent) | Out-Null
        Copy-Item -LiteralPath $dest -Destination $old
    }
}
foreach ($relative in $files) {
    $dest = Join-Path $ProjectRoot ($relative.Replace('/', '\'))
    New-Item -ItemType Directory -Force -Path (Split-Path $dest -Parent) | Out-Null
    Copy-Item -LiteralPath (Join-Path $stage ($relative.Replace('/', '\'))) -Destination $dest -Force
}
$metadata = Get-Content -LiteralPath $project -Raw -Encoding UTF8 | ConvertFrom-Json
if (-not @($metadata.Plugins | Where-Object { $_.Name -eq 'ASTFireRuntime' -and $_.Enabled }).Count) {
    throw 'ASTFireRuntime plugin is not enabled in the project. Stage3 must be installed first.'
}
$plugin = Join-Path $ProjectRoot 'Plugins\ASTFireRuntime\ASTFireRuntime.uplugin'
$buildLog = Join-Path $report ("BuildPlugin_" + $timestamp + ".log")
& $uat 'BuildPlugin' ("-Plugin=$plugin") ("-Package=$package") '-TargetPlatforms=Win64' '-Rocket' '-NoXGE' '-NoUBA' *> $buildLog
if ($LASTEXITCODE -ne 0) {
    Get-Content -LiteralPath $buildLog -Tail 45
    throw "Stage4 BuildPlugin failed; see $buildLog"
}
$binarySource = Join-Path $package 'Binaries'
$editorDlls = @(Get-ChildItem -LiteralPath $binarySource -Recurse -File -Filter 'UnrealEditor-ASTFire*.dll')
if ($editorDlls.Count -lt 2) { throw 'Both plugin editor DLLs are required.' }
foreach ($binary in Get-ChildItem -LiteralPath $binarySource -Recurse -File) {
    $relative = $binary.FullName.Substring($binarySource.Length).TrimStart('\')
    $dest = Join-Path $ProjectRoot ('Plugins\ASTFireRuntime\Binaries\' + $relative)
    New-Item -ItemType Directory -Force -Path (Split-Path $dest -Parent) | Out-Null
    Copy-Item -LiteralPath $binary.FullName -Destination $dest -Force
}
$setup = Join-Path $ProjectRoot 'Automation\ASTFire\setup_stage4_volume.py'
$commandletLog = Join-Path $report ("MaterialSetup_" + $timestamp + ".log")
& $ue $project '-run=pythonscript' ("-script=" + $setup.Replace('\','/')) '-unattended' '-nop4' '-nosplash' '-nullrhi' *> $commandletLog
$commandletExit = $LASTEXITCODE
$resultPath = Join-Path $report 'stage4_result.json'
if ($commandletExit -ne 0 -or !(Test-Path $resultPath)) {
    Get-Content -LiteralPath $commandletLog -Tail 50
    throw "Stage4 commandlet failed (exit $commandletExit)"
}
$result = Get-Content -LiteralPath $resultPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($result.status -ne 'VOLUME_ASSET_CONFIGURED_REQUIRES_USER_VIEW') {
    $result.errors | ConvertTo-Json -Depth 6 | Write-Output
    throw "Stage4 setup failed: $($result.status)"
}
Write-Output "AST_FIRE_STAGE4_SUCCESS=$Revision"
Write-Output "AST_FIRE_STAGE4_RESULT=$resultPath"
