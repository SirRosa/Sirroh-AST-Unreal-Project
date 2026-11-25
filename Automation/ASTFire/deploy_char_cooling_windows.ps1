#Requires -Version 5.1
[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-fA-F]{40}$')][string]$Revision,
 [string]$ProjectRoot='D:\0 Sirro\Unreal Projects\AST_V1_18',
 [string]$EngineRoot='D:\0 Sirro\UE 5.8.1\UE_5.8',
 [string]$Dumpbin='D:\Sirro\Programy\VS\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\dumpbin.exe'
)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
function Assert-EditorClosed {
 if(Get-Process UnrealEditor,UnrealEditor-Cmd -ErrorAction SilentlyContinue){throw 'Editor is open; no files may be replaced'}
}
# Building in an isolated clone is safe while the editor is open.
# The existing Assert-EditorClosed after ABI checks still blocks all live writes.
$stamp=Get-Date -Format 'yyyyMMdd_HHmmss'
$root=Join-Path $env:TEMP ('ASTFireCharCooling_'+$stamp)
$source=Join-Path $root 'Source'
$built=Join-Path $root 'Built'
$report=Join-Path $ProjectRoot 'Saved\ChatGPT\NiagaraFire\FireV4'
$backup=Join-Path $report ('Backup_CharCooling_'+$stamp)
New-Item -ItemType Directory -Force -Path $root,$report | Out-Null
& git clone --quiet --depth 1 --branch work/fire-char-cooling-fix 'https://github.com/SirRosa/Sirroh-AST-Unreal-Project.git' $source
if($LASTEXITCODE -ne 0){throw 'Clone failed'}
$actual=(& git -C $source rev-parse HEAD).Trim()
if($actual -ne $Revision){throw ('Revision mismatch '+$actual)}
Write-Output ('FIRE_V4_REVISION='+$actual)
$plugin='Plugins\ASTFireRuntime'
$files=@(
 "$plugin\Source\ASTFireRuntime\Private\ASTFireReactiveComponent.cpp",
 "$plugin\Source\ASTFireRuntime\Public\ASTFireReactiveComponent.h"
)
$sourcePlugin=Join-Path $source $plugin
$livePlugin=Join-Path $ProjectRoot $plugin
$baseline=@{
 "$plugin\Source\ASTFireRuntime\Private\ASTFireReactiveComponent.cpp"=@('f265460c2f3eba17e1f6ec094181cf2ab749f267')
 "$plugin\Source\ASTFireRuntime\Public\ASTFireReactiveComponent.h"=@('cacced9b064461bfde4e3007c8c039858a785bb6')
 "$plugin\Source\ASTFireRuntime\Private\ASTFireFreeFlameComponent.cpp"=@('a4ebe83ffb67a9a125c6eb8bb9fac7b2a16ec04e')
 "$plugin\Source\ASTFireRuntime\Public\ASTFireFreeFlameComponent.h"=@('fb7817a1d7dff3a25fc1284935a6399d43d619ef')
 "$plugin\Source\ASTFireRuntime\Private\ASTFireIgnitionZoneComponent.cpp"=@('624604f71641b9dd0434ae9fb25a4d8ff8da704e')
 "$plugin\Source\ASTFireRuntime\Public\ASTFireIgnitionZoneComponent.h"=@('c09427c35fbd79770569d0f02bad2ec39683eb97')
 "$plugin\Source\ASTFireRuntime\Private\ASTFireBlueprintLibrary.cpp"=@('5527f2eace1733f2a1a5fd47b3cb98daa78161c8')
}
$material=Join-Path $ProjectRoot 'Content\VFX\Fire\Materials\M_AST_FireCharProp.uasset'
if(!(Test-Path $material)){throw 'Generated prop material missing'}
$caveMaterial=Join-Path $ProjectRoot 'Content\VFX\Fire\Materials\M_AST_FireCharCaveProp.uasset'
if(!(Test-Path $caveMaterial)){throw 'Generated cave prop material missing'}
foreach($rel in $baseline.Keys){
 $current=(& git -C $ProjectRoot hash-object ('--path='+$rel.Replace('\','/')) (Join-Path $ProjectRoot $rel)).Trim()
 if($LASTEXITCODE -ne 0 -or $baseline[$rel] -notcontains $current){throw ('Local source changed '+$rel+' '+$current)}
}
# Fail closed if unrelated live C++ differs; never overwrite someone else's work.
foreach($f in Get-ChildItem -LiteralPath (Join-Path $sourcePlugin 'Source') -File -Recurse){
 $rel=$plugin+$f.FullName.Substring($sourcePlugin.Length)
 if($files -contains $rel){continue}
 $live=Join-Path $ProjectRoot $rel
 if(!(Test-Path -LiteralPath $live)){throw ('Missing unrelated source '+$rel)}
 $a=(Get-Content -LiteralPath $f.FullName -Raw).Replace("`r`n","`n").TrimEnd()
 $b=(Get-Content -LiteralPath $live -Raw).Replace("`r`n","`n").TrimEnd()
 if($a -ne $b){throw ('Unrelated local source differs '+$rel)}
}
Push-Location $source
try {
 & python Automation/ASTFire/test_fire_v4.py
 if($LASTEXITCODE -ne 0){throw 'Static fire regression failed'}
 & python Automation/ASTFire/test_stage7_mesh_motion.py
 if($LASTEXITCODE -ne 0){throw 'Static mesh regression failed'}
} finally { Pop-Location }
$uat=Join-Path $EngineRoot 'Engine\Build\BatchFiles\RunUAT.bat'
$ue=Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$log=Join-Path $report ('Build_'+$stamp+'.log')
& $uat BuildPlugin ('-Plugin='+(Join-Path $sourcePlugin 'ASTFireRuntime.uplugin')) ('-Package='+$built) '-TargetPlatforms=Win64' '-Rocket' '-NoXGE' '-NoUBA' *> $log
$code=$LASTEXITCODE
Write-Output ('FIRE_V4_BUILD_EXIT='+$code)
if($code -ne 0){Get-Content -LiteralPath $log -Tail 50;throw 'Build failed'}
$bin=Join-Path $built 'Binaries'
$runtime=Join-Path $bin 'Win64\UnrealEditor-ASTFireRuntime.dll'
if(!(Test-Path -LiteralPath $runtime)){throw 'Runtime DLL missing'}
# Validate every imported ASTFireRuntime symbol of project modules BEFORE replacement.
if(!(Test-Path -LiteralPath $Dumpbin)){throw 'dumpbin unavailable'}
$exports=& $Dumpbin /exports $runtime
$exportNames=[Collections.Generic.HashSet[string]]::new()
foreach($line in $exports){if($line -match '^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)'){[void]$exportNames.Add($Matches[1])}}
$checked=0
foreach($dll in Get-ChildItem (Join-Path $ProjectRoot 'Binaries\Win64') -Filter 'UnrealEditor-*.dll' -File){
 $imports=& $Dumpbin /imports:UnrealEditor-ASTFireRuntime.dll $dll.FullName 2>$null
 foreach($line in $imports){
  if($line -match '^\s*[0-9A-Fa-f]+\s+(\?\S+)'){
   $name=$Matches[1];$checked++
   if(!$exportNames.Contains($name)){throw ('Missing import '+$dll.Name+': '+$name)}
  }
 }
}
if($checked -eq 0){throw 'ABI preflight found no ASTFire imports; cannot certify compatibility'}
Write-Output ('FIRE_V4_ABI_IMPORTS_CHECKED='+$checked)
Assert-EditorClosed
New-Item -ItemType Directory -Force -Path $backup|Out-Null
Set-Content (Join-Path $backup 'REVISION.txt') $actual
$restore=@($files)
$liveBin=Join-Path $livePlugin 'Binaries'
foreach($f in Get-ChildItem -LiteralPath $bin -Recurse -File){
 $rel=$f.FullName.Substring($bin.Length).TrimStart('\')
 $restore+=("$plugin\Binaries\"+$rel)
}
$restore=$restore|Select-Object -Unique
$had=@{}
foreach($rel in $restore){
 $live=Join-Path $ProjectRoot $rel
 $had[$rel]=Test-Path -LiteralPath $live
 if($had[$rel]){
  $dst=Join-Path $backup $rel
  New-Item -ItemType Directory -Force -Path (Split-Path $dst -Parent)|Out-Null
  Copy-Item -LiteralPath $live -Destination $dst -Force
 }
}
Write-Output ('FIRE_V4_BACKUP='+$backup)
$result=Join-Path $ProjectRoot 'Saved\ChatGPT\NiagaraFire\FireV4\fire_light_result.json'
if(Test-Path -LiteralPath $result){Move-Item -LiteralPath $result -Destination (Join-Path $backup 'previous_fire_light_result.json') -Force}
try {
 Assert-EditorClosed
 foreach($rel in $files){Copy-Item -LiteralPath (Join-Path $source $rel) -Destination (Join-Path $ProjectRoot $rel) -Force}
 foreach($f in Get-ChildItem -LiteralPath $bin -Recurse -File){
  $rel=$f.FullName.Substring($bin.Length).TrimStart('\')
  $dst=Join-Path $liveBin $rel
  New-Item -ItemType Directory -Force -Path (Split-Path $dst -Parent)|Out-Null
  Copy-Item -LiteralPath $f.FullName -Destination $dst -Force
 }
 $script=Join-Path $source 'Automation\ASTFire\audit_fire_light.py'
 $project=Join-Path $ProjectRoot 'AST_V1_18.uproject'
 $auditLog=Join-Path $report ('Audit_'+$stamp+'.log')
 & $ue $project '-run=pythonscript' ('-script='+$script.Replace('\','/')) '-unattended' '-nop4' '-nosplash' '-nullrhi' *> $auditLog
 $exit=$LASTEXITCODE
 if(!(Test-Path -LiteralPath $result)){Get-Content -LiteralPath $auditLog -Tail 40;throw ('No audit result, UE exit '+$exit)}
 $data=Get-Content -LiteralPath $result -Raw|ConvertFrom-Json
 Write-Output ('FIRE_LIGHT_AUDIT='+($data|ConvertTo-Json -Depth 6 -Compress))
 if($exit -ne 0 -or $data.status -ne 'PASS'){throw 'Fire lighting audit failed'}
 # GPU/POI_Light visual test is left to the user; headless regression must pass.
 Write-Output 'FIRE_CHAR_COOLING_INSTALL_SUCCESS'
} catch {
 $why=$_.Exception.Message
 Assert-EditorClosed
 foreach($rel in $restore){
  $live=Join-Path $ProjectRoot $rel
  if($had[$rel]){Copy-Item -LiteralPath (Join-Path $backup $rel) -Destination $live -Force}
  elseif(Test-Path -LiteralPath $live){Remove-Item -LiteralPath $live -Force}
 }
 Write-Output ('FIRE_V4_ROLLBACK_COMPLETE='+$why)
 throw
}
