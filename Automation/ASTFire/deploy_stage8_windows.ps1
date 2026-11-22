#Requires -Version 5.1
[CmdletBinding()]
param(
  [string]$ProjectRoot = 'D:\0 Sirro\Unreal Projects\AST_V1_18',
  [string]$EngineRoot = 'D:\0 Sirro\UE 5.8.1\UE_5.8',
  [string]$Revision = ''
)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
if([string]::IsNullOrWhiteSpace($Revision)){
  $branch=Invoke-RestMethod -Uri 'https://api.github.com/repos/SirRosa/Sirroh-AST-Unreal-Project/branches/work/niagara-fire-stage2' -TimeoutSec 30
  $Revision=$branch.commit.sha
}
if($Revision -notmatch '^[0-9a-fA-F]{40}$'){throw 'Revision must be an exact full commit SHA'}
$stamp=Get-Date -Format 'yyyyMMdd_HHmmss'
$source=Join-Path $env:TEMP ('ASTFireStage8_Source_'+$stamp)
$built=Join-Path $env:TEMP ('ASTFireStage8_Package_'+$stamp)
$report=Join-Path $ProjectRoot 'Saved\ChatGPT\NiagaraFire\Stage8'
$backup=Join-Path $report ('Backup_'+$stamp)
$project=Join-Path $ProjectRoot 'AST_V1_18.uproject'
$uat=Join-Path $EngineRoot 'Engine\Build\BatchFiles\RunUAT.bat'
$ue=Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
foreach($required in @($project,$uat,$ue)){if(!(Test-Path -LiteralPath $required)){throw ('Missing '+$required)}}
New-Item -ItemType Directory -Path $report -Force | Out-Null
& git clone --quiet --depth 1 --branch 'work/niagara-fire-stage2' 'https://github.com/SirRosa/Sirroh-AST-Unreal-Project.git' $source
if($LASTEXITCODE -ne 0){throw 'Git clone failed'}
$actual=(& git -C $source rev-parse HEAD).Trim()
if($actual -ne $Revision){throw ('GitHub head changed: expected '+$Revision+' actual '+$actual)}
Write-Output ('STAGE8_REVISION='+$actual)
$plugin=Join-Path $source 'Plugins\ASTFireRuntime\ASTFireRuntime.uplugin'
$buildLog=Join-Path $report ('BuildPlugin_'+$stamp+'.log')
& $uat BuildPlugin ('-Plugin='+$plugin) ('-Package='+$built) '-TargetPlatforms=Win64' '-Rocket' '-NoXGE' '-NoUBA' *> $buildLog
$code=$LASTEXITCODE
Write-Output ('STAGE8_BUILD_EXIT='+$code)
if($code -ne 0){Get-Content -LiteralPath $buildLog -Tail 60;throw 'BuildPlugin failed'}
$binarySource=Join-Path $built 'Binaries'
if(!(Test-Path -LiteralPath $binarySource)){throw 'BuildPlugin Binaries folder missing'}
$dlls=@(Get-ChildItem -LiteralPath $binarySource -Recurse -Filter 'UnrealEditor-ASTFire*.dll' -File)
if($dlls.Count -lt 2){throw 'Expected runtime and editor DLLs'}
if(Get-Process -Name UnrealEditor -ErrorAction SilentlyContinue){
  Write-Output 'STAGE8_BUILT_ONLY_EDITOR_OPEN_NO_FILES_REPLACED'
  exit 4
}
$projectConfig=Get-Content -LiteralPath $project -Raw -Encoding UTF8 | ConvertFrom-Json
if(-not @($projectConfig.Plugins | Where-Object {$_.Name -eq 'ASTFireRuntime' -and $_.Enabled}).Count){
  throw 'ASTFireRuntime must be enabled in the existing project'
}
$paths=@(
  'Plugins/ASTFireRuntime/ASTFireRuntime.uplugin',
  'Plugins/ASTFireRuntime/Source/ASTFireRuntime/ASTFireRuntime.Build.cs',
  'Plugins/ASTFireRuntime/Source/ASTFireRuntime/Public/ASTFireReactiveComponent.h',
  'Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireReactiveComponent.cpp',
  'Plugins/ASTFireRuntime/Source/ASTFireRuntime/Public/ASTFireBlueprintLibrary.h',
  'Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireBlueprintLibrary.cpp',
  'Plugins/ASTFireRuntime/Source/ASTFireRuntime/Public/ASTFireIgnitionZoneComponent.h',
  'Plugins/ASTFireRuntime/Source/ASTFireRuntime/Private/ASTFireIgnitionZoneComponent.cpp',
  'Automation/ASTFire/setup_stage4_volume.py',
  'Automation/ASTFire/setup_stage6_igniter.py',
  'Automation/ASTFire/setup_stage8_surface.py'
)
foreach($path in $paths){if(!(Test-Path -LiteralPath (Join-Path $source ($path.Replace('/','\'))))){throw ('Missing '+$path)}}
New-Item -ItemType Directory -Path $backup -Force | Out-Null
Set-Content -LiteralPath (Join-Path $backup 'REVISION.txt') -Value $Revision -Encoding ASCII
$ownedAssets=@(
  'Content/VFX/Fire/Materials/M_AST_FirePixel_Volume.uasset',
  'Content/VFX/Fire/Materials/M_AST_FireSurface.uasset',
  'Content/VFX/Fire/Blueprints/BP_AST_FireSource.uasset',
  'Content/VFX/Fire/Blueprints/BP_AST_FireSource_Igniter.uasset'
)
foreach($path in ($paths + $ownedAssets)){
  $rel=$path.Replace('/','\')
  $file=Join-Path $ProjectRoot $rel
  if(Test-Path -LiteralPath $file){
    $out=Join-Path $backup $rel
    New-Item -ItemType Directory -Force -Path (Split-Path $out -Parent) | Out-Null
    Copy-Item -LiteralPath $file -Destination $out -Force
  }
}
$binaryDest=Join-Path $ProjectRoot 'Plugins\ASTFireRuntime\Binaries'
if(Test-Path -LiteralPath $binaryDest){
  $out=Join-Path $backup 'Plugins\ASTFireRuntime\Binaries'
  New-Item -ItemType Directory -Force -Path (Split-Path $out -Parent) | Out-Null
  Copy-Item -LiteralPath $binaryDest -Destination $out -Recurse -Force
}
if(Get-Process -Name UnrealEditor -ErrorAction SilentlyContinue){throw 'Editor opened during backup: no writes'}
foreach($path in $paths){
  $rel=$path.Replace('/','\')
  $target=Join-Path $ProjectRoot $rel
  New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
  Copy-Item -LiteralPath (Join-Path $source $rel) -Destination $target -Force
}
foreach($file in Get-ChildItem -LiteralPath $binarySource -Recurse -File){
  $rel=$file.FullName.Substring($binarySource.Length).TrimStart('\')
  $target=Join-Path $binaryDest $rel
  New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
  Copy-Item -LiteralPath $file.FullName -Destination $target -Force
}
Write-Output ('STAGE8_DEPLOYED_DLLS='+$dlls.Count)
Write-Output ('STAGE8_BACKUP='+$backup)
$steps=@(
  @{Name='setup_stage4_volume.py'; Result='Stage4\stage4_result.json'; Status='VOLUME_ASSET_CONFIGURED_REQUIRES_USER_VIEW'},
  @{Name='setup_stage6_igniter.py'; Result='Stage6\stage6_result.json'; Status='IGNITER_CREATED_REQUIRES_CONTACT_TEST'},
  @{Name='setup_stage8_surface.py'; Result='Stage8\stage8_result.json'; Status='SURFACE_FIRE_CONFIGURED_REQUIRES_VISUAL_TEST'}
)
foreach($step in $steps){
  $script=Join-Path $ProjectRoot ('Automation\ASTFire\'+$step.Name)
  $result=Join-Path $ProjectRoot ('Saved\ChatGPT\NiagaraFire\'+$step.Result)
  if(Test-Path -LiteralPath $result){
    Move-Item -LiteralPath $result -Destination (Join-Path $backup ($step.Name+'.previous.json')) -Force
  }
  $log=Join-Path $report ($step.Name+'_'+$stamp+'.log')
  & $ue $project '-run=pythonscript' ('-script='+$script.Replace('\','/')) '-unattended' '-nop4' '-nosplash' '-nullrhi' *> $log
  $ueExit=$LASTEXITCODE
  if(!(Test-Path -LiteralPath $result)){Get-Content -LiteralPath $log -Tail 45;throw ('Missing commandlet result '+$step.Name)}
  $json=Get-Content -LiteralPath $result -Raw -Encoding UTF8 | ConvertFrom-Json
  Write-Output ('STAGE8_COMMANDLET='+$step.Name+';EXIT='+$ueExit+';STATUS='+$json.status)
  if($ueExit -ne 0 -or $json.status -ne $step.Status){
    $json.errors | ConvertTo-Json -Depth 8 -Compress | Write-Output
    Get-Content -LiteralPath $log -Tail 45
    throw ('Commandlet failed '+$step.Name)
  }
}
Write-Output 'STAGE8_INSTALL_SUCCESS_VISUAL_AND_CONTACT_TEST_PENDING'
