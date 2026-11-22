#Requires -Version 5.1
# Stage5 prebuilt deployment. Runs only after an exact-revision BuildPlugin succeeds.
# Scope: ASTFireRuntime sources/binaries and /Game/VFX/Fire/ own assets. NO maps/torches.
[CmdletBinding()]
param(
 [string]$ProjectRoot = 'D:\0 Sirro\Unreal Projects\AST_V1_18',
 [string]$SourceFolder = '',
 [string]$PackageFolder = ''
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$revision = 'f5d90365ea25718daf11847191549f2bf4f243b3'
if([string]::IsNullOrWhiteSpace($SourceFolder)){
 $SourceFolder=Join-Path $env:TEMP 'ASTFireStage5FinalSource_f5d9036'
}
if([string]::IsNullOrWhiteSpace($PackageFolder)){
 $PackageFolder=Join-Path $env:TEMP 'ASTFireStage5FinalBuild_f5d9036'
}
if(Get-Process -Name UnrealEditor -ErrorAction SilentlyContinue){
 throw 'Unreal Editor is open; refusing to deploy DLLs'
}
$project=Join-Path $ProjectRoot 'AST_V1_18.uproject'
$ue='D:\0 Sirro\UE 5.8.1\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
if(!(Test-Path -LiteralPath $project) -or !(Test-Path -LiteralPath $ue)){
 throw 'Project or UnrealEditor-Cmd.exe missing'
}
$actual=(& git -C $SourceFolder rev-parse HEAD).Trim()
if($actual -ne $revision){throw ('Expected exact verified SHA '+$revision+', got '+$actual)}
$bin=Join-Path $PackageFolder 'Binaries'
if(!(Test-Path -LiteralPath $bin)){throw 'BuildPlugin binaries directory missing'}
$editorDlls=@(Get-ChildItem -LiteralPath $bin -Recurse -File -Filter 'UnrealEditor-ASTFire*.dll')
if($editorDlls.Count -lt 2){throw 'Expected at least 2 editor DLLs in verified BuildPlugin package'}
$projectMeta=Get-Content -LiteralPath $project -Raw -Encoding UTF8 | ConvertFrom-Json
if(-not @($projectMeta.Plugins | Where-Object {$_.Name -eq 'ASTFireRuntime' -and $_.Enabled}).Count){
 throw 'ASTFireRuntime plugin must already be enabled in project'
}
$sourceList=@(
 'Plugins/ASTFireRuntime/ASTFireRuntime.uplugin',
 'Plugins/ASTFireRuntime/Source/ASTFireRuntime/ASTFireRuntime.Build.cs',
 'Plugins/ASTFireRuntime/Source/ASTFireRuntime/Public/ASTFireReactiveComponent.h',
 'Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireReactiveComponent.cpp',
 'Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireBlueprintLibrary.cpp',
 'Automation/ASTFire/setup_stage4_volume.py'
)
foreach($rel in $sourceList){
 $path=Join-Path $SourceFolder ($rel.Replace('/','\'))
 if(!(Test-Path -LiteralPath $path)){throw ('Missing file in verified git checkout: '+$rel)}
}
$timestamp=Get-Date -Format 'yyyyMMdd_HHmmss'
$report=Join-Path $ProjectRoot 'Saved\ChatGPT\NiagaraFire\Stage5'
$backup=Join-Path $report ('Backup_'+$timestamp)
New-Item -ItemType Directory -Path $backup -Force | Out-Null
Set-Content -LiteralPath (Join-Path $backup 'SOURCE_REVISION.txt') -Value $revision -Encoding ASCII
foreach($rel in $sourceList){
 $relative=$rel.Replace('/','\')
 $dest=Join-Path $ProjectRoot $relative
 if(Test-Path -LiteralPath $dest){
  $saved=Join-Path $backup $relative
  New-Item -ItemType Directory -Path (Split-Path -Parent $saved) -Force | Out-Null
  Copy-Item -LiteralPath $dest -Destination $saved -Force
 }
}
# Back up only the two own assets the material commandlet can change.
foreach($relative in @(
 'Content\VFX\Fire\Materials\M_AST_FirePixel_Volume.uasset',
 'Content\VFX\Fire\Blueprints\BP_AST_FireSource.uasset'
)){
 $dest=Join-Path $ProjectRoot $relative
 if(Test-Path -LiteralPath $dest){
  $saved=Join-Path $backup $relative
  New-Item -ItemType Directory -Path (Split-Path -Parent $saved) -Force | Out-Null
  Copy-Item -LiteralPath $dest -Destination $saved -Force
 }
}
$binaryDest=Join-Path $ProjectRoot 'Plugins\ASTFireRuntime\Binaries'
if(Test-Path -LiteralPath $binaryDest){
 $binaryBackup=Join-Path $backup 'Plugins\ASTFireRuntime\Binaries'
 New-Item -ItemType Directory -Path (Split-Path -Parent $binaryBackup) -Force | Out-Null
 Copy-Item -LiteralPath $binaryDest -Destination $binaryBackup -Recurse -Force
}
if(Get-Process -Name UnrealEditor -ErrorAction SilentlyContinue){
 throw 'Editor opened during backup; refusing to replace DLLs'
}
foreach($rel in $sourceList){
 $relative=$rel.Replace('/','\')
 $source=Join-Path $SourceFolder $relative
 $dest=Join-Path $ProjectRoot $relative
 New-Item -ItemType Directory -Path (Split-Path -Parent $dest) -Force | Out-Null
 Copy-Item -LiteralPath $source -Destination $dest -Force
}
foreach($file in Get-ChildItem -LiteralPath $bin -Recurse -File){
 $relative=$file.FullName.Substring($bin.Length).TrimStart('\')
 $dest=Join-Path $binaryDest $relative
 New-Item -ItemType Directory -Path (Split-Path -Parent $dest) -Force | Out-Null
 Copy-Item -LiteralPath $file.FullName -Destination $dest -Force
}
Write-Output ('STAGE5_DEPLOY_SHA='+$revision)
Write-Output ('STAGE5_DLL_COUNT='+$editorDlls.Count)
Write-Output ('STAGE5_BACKUP='+$backup)
$oldResult=Join-Path $ProjectRoot 'Saved\ChatGPT\NiagaraFire\Stage4\stage4_result.json'
if(Test-Path -LiteralPath $oldResult){
 Move-Item -LiteralPath $oldResult -Destination (Join-Path $report ('previous_result_'+$timestamp+'.json')) -Force
}
$script=Join-Path $ProjectRoot 'Automation\ASTFire\setup_stage4_volume.py'
$log=Join-Path $report ('Stage5_Commandlet_'+$timestamp+'.log')
& $ue $project '-run=pythonscript' ('-script='+$script.Replace('\','/')) '-unattended' '-nop4' '-nosplash' '-nullrhi' *> $log
$exitCode=$LASTEXITCODE
Write-Output ('STAGE5_COMMANDLET_EXIT='+$exitCode)
Write-Output ('STAGE5_COMMANDLET_LOG='+$log)
if(!(Test-Path -LiteralPath $oldResult)){
 Get-Content -LiteralPath $log -Tail 40
 throw 'Stage5 commandlet did not produce result JSON'
}
$j=Get-Content -LiteralPath $oldResult -Raw -Encoding UTF8 | ConvertFrom-Json
Write-Output ('STAGE5_REPORT_STATUS='+$j.status)
Write-Output ('STAGE5_REPORT_ERRORS='+$j.errors.Count)
Write-Output ('STAGE5_MODIFIED_COMPONENTS='+$j.modified_components)
Write-Output ('STAGE5_DISABLED_LEGACY_FX='+$j.disabled_legacy_niagara)
Write-Output ('STAGE5_ADJUSTED_LIGHTS='+$j.adjusted_lights)
if($exitCode -ne 0 -or $j.status -ne 'VOLUME_ASSET_CONFIGURED_REQUIRES_USER_VIEW'){
 $j.errors | ConvertTo-Json -Depth 5 -Compress | Write-Output
 Get-Content -LiteralPath $log -Tail 40
 throw 'Stage5 setup reported failure. Backups remain available.'
}
Write-Output 'STAGE5_INSTALL_SUCCESS'
