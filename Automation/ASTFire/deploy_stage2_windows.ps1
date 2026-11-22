#Requires -Version 5.1
<#
AST Fire Stage 2 deployment for Windows + Unreal Engine 5.8.1.
Does not modify BP_Torch_U, project maps or existing placeholder materials.
Backs up every source/config file before overwriting and stops on any failure.
#>
[CmdletBinding()]
param(
    [string]$ProjectRoot = 'D:\0 Sirro\Unreal Projects\AST_V1_18',
    [string]$EngineRoot = 'D:\0 Sirro\UE 5.8.1\UE_5.8',
    [string]$Revision = 'cdfe542e54b74601a9b67d1a51b2c302e3c4cd96'
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$reportDir = Join-Path $ProjectRoot 'Saved\ChatGPT\NiagaraFire\Stage2'
$backupDir = Join-Path $reportDir ('SourceBackup_' + $stamp)
# BuildPlugin deletes HostProject directories while packaging. Google Drive file
# sync can temporarily lock them, so build completely outside the synced tree.
$packageDir = Join-Path ([System.IO.Path]::GetTempPath()) ('ASTFirePluginBuild_' + $stamp)
$log = Join-Path $reportDir ('BuildPlugin_' + $stamp + '.log')
$finalLog = Join-Path $reportDir ('Finalize_' + $stamp + '.log')

New-Item -ItemType Directory -Force -Path $reportDir | Out-Null
$project = Join-Path $ProjectRoot 'AST_V1_18.uproject'
$uat = Join-Path $EngineRoot 'Engine\Build\BatchFiles\RunUAT.bat'
$uecmd = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'

foreach($required in @($project,$uat,$uecmd)){
    if(!(Test-Path -LiteralPath $required)){throw 'Required file missing: ' + $required}
}
if(Get-Process -Name UnrealEditor -ErrorAction SilentlyContinue){
    throw 'UnrealEditor is running. Save and close the editor before replacing the compiled ASTFire plugin.'
}

# Deliberately small allowlist: no binary assets, no full project synchronization.
$files = @(
    'Plugins/ASTFireRuntime/ASTFireRuntime.uplugin',
    'Plugins/ASTFireRuntime/Source/ASTFireRuntime/ASTFireRuntime.Build.cs',
    'Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireRuntimeModule.cpp',
    'Plugins/ASTFireRuntime/Source/ASTFireRuntime/Public/ASTFireReactiveComponent.h',
    'Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireReactiveComponent.cpp',
    'Plugins/ASTFireRuntime/Source/ASTFireEditor/ASTFireEditor.Build.cs',
    'Plugins/ASTFireRuntime/Source/ASTFireEditor/Private/ASTFireEditorModule.cpp',
    'Plugins/ASTFireRuntime/Source/ASTFireEditor/Public/ASTFireEditorLibrary.h',
    'Plugins/ASTFireRuntime/Source/ASTFireEditor/Private/ASTFireEditorLibrary.cpp',
    'Automation/ASTFire/fix_stage2_materials.py',
    'Automation/ASTFire/finalize_stage2.py'
)
$rawBase = 'https://raw.githubusercontent.com/SirRosa/Sirroh-AST-Unreal-Project/' + $Revision

# Stage all downloads first. Existing project files remain untouched if HTTPS fails.
$stage = Join-Path $reportDir ('Downloads_' + $stamp)
New-Item -ItemType Directory -Force -Path $stage | Out-Null
foreach($relative in $files){
    $dest = Join-Path $stage ($relative.Replace('/','\'))
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
    $uri = $rawBase + '/' + $relative
    Invoke-WebRequest -Uri $uri -OutFile $dest -UseBasicParsing -TimeoutSec 40
    if(!(Test-Path -LiteralPath $dest) -or (Get-Item -LiteralPath $dest).Length -eq 0){
        throw 'Download empty/missing: ' + $relative
    }
}
Write-Output ('AST_STAGE2_DOWNLOADED=' + $files.Count)

# Back up only files we replace, not generated files or the full 16GB project.
foreach($relative in $files){
    $path = $relative.Replace('/','\')
    $dest = Join-Path $ProjectRoot $path
    $saved = Join-Path $backupDir $path
    if(Test-Path -LiteralPath $dest){
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $saved) | Out-Null
        Copy-Item -LiteralPath $dest -Destination $saved -Force
    }
}
New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
Copy-Item -LiteralPath $project -Destination (Join-Path $backupDir 'AST_V1_18.uproject') -Force

foreach($relative in $files){
    $path = $relative.Replace('/','\')
    $src = Join-Path $stage $path
    $dest = Join-Path $ProjectRoot $path
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
    Copy-Item -LiteralPath $src -Destination $dest -Force
}
Write-Output ('AST_STAGE2_SOURCE_BACKUP=' + $backupDir)

$metadata = Get-Content -LiteralPath $project -Raw -Encoding UTF8 | ConvertFrom-Json
$record = @($metadata.Plugins | Where-Object {$_.Name -eq 'ASTFireRuntime'})
if($record.Count -eq 0){
    $metadata.Plugins = @($metadata.Plugins) + [pscustomobject]@{
        Name = 'ASTFireRuntime'; Enabled = $true
    }
    $metadata | ConvertTo-Json -Depth 80 | Set-Content -LiteralPath $project -Encoding UTF8
}elseif(!$record[0].Enabled){
    $record[0].Enabled = $true
    $metadata | ConvertTo-Json -Depth 80 | Set-Content -LiteralPath $project -Encoding UTF8
}

$plugin = Join-Path $ProjectRoot 'Plugins\ASTFireRuntime\ASTFireRuntime.uplugin'
Write-Output 'AST_STAGE2_BUILDPLUGIN_STARTED'
& $uat 'BuildPlugin' ('-Plugin='+$plugin) ('-Package='+$packageDir) '-TargetPlatforms=Win64' '-Rocket' '-NoXGE' '-NoUBA' *> $log
$exit = $LASTEXITCODE
Write-Output ('AST_STAGE2_BUILDPLUGIN_EXIT=' + $exit)
if($exit -ne 0){
    Write-Output ('AST_STAGE2_BUILD_LOG=' + $log)
    Get-Content -LiteralPath $log -Tail 30
    throw 'ASTFireRuntime BuildPlugin failed; commandlet skipped.'
}

$published = Join-Path $packageDir 'Binaries'
if(!(Test-Path -LiteralPath $published)){
    throw 'BuildPlugin returned success but packaged Binaries are missing.'
}
$builtDlls = @(Get-ChildItem -LiteralPath $published -Recurse -File -Filter 'UnrealEditor-ASTFire*.dll')
if($builtDlls.Count -lt 2){
    throw 'Expected ASTFireRuntime and ASTFireEditor editor DLLs not found in package.'
}

$destination = Join-Path $ProjectRoot 'Plugins\ASTFireRuntime\Binaries'
foreach($f in Get-ChildItem -LiteralPath $published -Recurse -File){
    $relative = $f.FullName.Substring($published.Length).TrimStart('\')
    $out = Join-Path $destination $relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $out) | Out-Null
    Copy-Item -LiteralPath $f.FullName -Destination $out -Force
}
Write-Output ('AST_STAGE2_DEPLOYED_DLLS=' + $builtDlls.Count)

$script = Join-Path $ProjectRoot 'Automation\ASTFire\finalize_stage2.py'
$scriptArg = '-script=' + $script.Replace('\','/')
Write-Output 'AST_STAGE2_FINALIZE_STARTED'
& $uecmd $project '-run=pythonscript' $scriptArg '-unattended' '-nop4' '-nosplash' '-nullrhi' *> $finalLog
Write-Output ('AST_STAGE2_UNREAL_EXIT=' + $LASTEXITCODE)

$resultPath = Join-Path $reportDir 'stage2_complete_result.json'
if(Test-Path -LiteralPath $resultPath){
    $r = Get-Content -LiteralPath $resultPath -Raw -Encoding UTF8 | ConvertFrom-Json
    Write-Output ('AST_STAGE2_RESULT_STATUS=' + $r.status)
    Write-Output ('AST_STAGE2_RESULT_FILE=' + $resultPath)
    if($r.status -ne 'ASSETS_ASSEMBLED_REQUIRES_VISUAL_VALIDATION'){
        Write-Output ('AST_STAGE2_FINALIZE_LOG=' + $finalLog)
        exit 2
    }
    exit 0
}
Write-Output ('AST_STAGE2_RESULT_MISSING_LOG=' + $finalLog)
exit 3
