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
Assert-EditorClosed
$stamp=Get-Date -Format 'yyyyMMdd_HHmmss'
$root=Join-Path $env:TEMP ('ASTFireV4_'+$stamp)
$source=Join-Path $root 'Source'
$built=Join-Path $root 'Built'
$report=Join-Path $ProjectRoot 'Saved\ChatGPT\NiagaraFire\FireV4'
$backup=Join-Path $report ('Backup_'+$stamp)
New-Item -ItemType Directory -Force -Path $root,$report | Out-Null
& git clone --quiet --depth 1 --branch work/niagara-fire-stage2 'https://github.com/SirRosa/Sirroh-AST-Unreal-Project.git' $source
if($LASTEXITCODE -ne 0){throw 'Clone failed'}
$actual=(& git -C $source rev-parse HEAD).Trim()
if($actual -ne $Revision){throw ('Revision mismatch '+$actual)}
Write-Output ('FIRE_V4_REVISION='+$actual)
$plugin='Plugins\ASTFireRuntime'
$files=@(
 "$plugin\Source\ASTFireRuntime\Public\ASTFireFreeFlameComponent.h",
 "$plugin\Source\ASTFireRuntime\Private\ASTFireFreeFlameComponent.cpp",
 'Automation\ASTFire\setup_fire_v4.py'
)
$material='Content\VFX\Fire\Materials\M_AST_FireTongue.uasset'
$sourcePlugin=Join-Path $source $plugin
$livePlugin=Join-Path $ProjectRoot $plugin
# Compare the exact locally deployed source to the accepted revision.
$baseline=@{
 "$plugin\Source\ASTFireRuntime\Private\ASTFireFreeFlameComponent.cpp"=@('16af2c8653c72bba66508ff598fb157ff85d4a2d','0189c213c6d69a49beb72337dc01581b77f4e7f0','5050224e71c7a5f75a4f77ef46fdc189e89c706c','9520ada15f9b4816b3b3ba67457979e12e34832e','3c74313569b73fd65da07ab5c0baee5f751f0486','4724878aa11c80acfeabb7aa24daec5d956850aa')
 "$plugin\Source\ASTFireRuntime\Public\ASTFireFreeFlameComponent.h"=@('e4d980e5940a773065e7277300a35f0ea8190d6a','237adae01b04d70857c2029242e72299deaa1114','d74a3c735313c62524edc6d4ec735ea6bd57e1d6','0853677c1bd187d0e7fcda4ee3acd0e2d000786c','7e974c61ccd5ec2f5d7ba2840ed2587152212ccd')
}
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
$restore=@($files)+@($material,'Content\VFX\Fire\Materials\M_AST_FireSpark.uasset')
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
$result=Join-Path $ProjectRoot 'Saved\ChatGPT\NiagaraFire\FireV4\material_result.json'
if(Test-Path -LiteralPath $result){Move-Item -LiteralPath $result -Destination (Join-Path $backup 'previous_material_result.json') -Force}
try {
 Assert-EditorClosed
 foreach($rel in $files){Copy-Item -LiteralPath (Join-Path $source $rel) -Destination (Join-Path $ProjectRoot $rel) -Force}
 foreach($f in Get-ChildItem -LiteralPath $bin -Recurse -File){
  $rel=$f.FullName.Substring($bin.Length).TrimStart('\')
  $dst=Join-Path $liveBin $rel
  New-Item -ItemType Directory -Force -Path (Split-Path $dst -Parent)|Out-Null
  Copy-Item -LiteralPath $f.FullName -Destination $dst -Force
 }
 $script=Join-Path $ProjectRoot 'Automation\ASTFire\setup_fire_v4.py'
 $project=Join-Path $ProjectRoot 'AST_V1_18.uproject'
 $auditLog=Join-Path $report ('Audit_'+$stamp+'.log')
 & $ue $project '-run=pythonscript' ('-script='+$script.Replace('\','/')) '-unattended' '-nop4' '-nosplash' '-nullrhi' *> $auditLog
 $exit=$LASTEXITCODE
 if(!(Test-Path -LiteralPath $result)){Get-Content -LiteralPath $auditLog -Tail 40;throw ('No audit result, UE exit '+$exit)}
 $data=Get-Content -LiteralPath $result -Raw|ConvertFrom-Json
 Write-Output ('FIRE_V4_COMMANDLET_EXIT='+$exit+';STATUS='+$data.status+';FIRE='+$data.fire_audit+';SMOKE='+$data.smoke_audit+';SURFACE='+$data.surface_audit)
 if($exit -ne 0 -or $data.status -ne 'FIRE_V4_MATERIAL_READY_VISUAL_PENDING' -or $data.fire_audit -notlike 'PASS:*' -or $data.smoke_audit -notlike 'PASS:*' -or $data.surface_audit -notlike 'PASS:*'){
  $data.errors|ConvertTo-Json -Depth 5 -Compress|Write-Output
  throw 'Fire or smoke audit failed'
 }
 Write-Output 'FIRE_V4_INSTALL_SUCCESS_VISUAL_PENDING'
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
