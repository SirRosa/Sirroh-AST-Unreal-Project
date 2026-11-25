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
$root=Join-Path $env:TEMP ('ASTFireBurnDuration_'+$stamp)
$source=Join-Path $root 'Source'
$built=Join-Path $root 'Built'
$report=Join-Path $ProjectRoot 'Saved\ChatGPT\NiagaraFire\FireV4'
$backup=Join-Path $report ('Backup_BurnDuration_'+$stamp)
New-Item -ItemType Directory -Force -Path $root,$report | Out-Null
& git clone --quiet --depth 1 --branch work/niagara-fire-stage2 'https://github.com/SirRosa/Sirroh-AST-Unreal-Project.git' $source
if($LASTEXITCODE -ne 0){throw 'Clone failed'}
$actual=(& git -C $source rev-parse HEAD).Trim()
if($actual -ne $Revision){throw ('Revision mismatch '+$actual)}
Write-Output ('FIRE_V4_REVISION='+$actual)
$plugin='Plugins\ASTFireRuntime'
$files=@(
 "$plugin\Source\ASTFireRuntime\Public\ASTFireReactiveComponent.h",
 "$plugin\Source\ASTFireRuntime\Private\ASTFireReactiveComponent.cpp",
 'Automation\ASTFire\audit_burn_duration.py'
)
$sourcePlugin=Join-Path $source $plugin
$livePlugin=Join-Path $ProjectRoot $plugin
$baseline=@{
 "$plugin\Source\ASTFireRuntime\Private\ASTFireReactiveComponent.cpp"=@('beab74efdaed7e894cf6d42722d08b3237d61b8f')
 "$plugin\Source\ASTFireRuntime\Public\ASTFireReactiveComponent.h"=@('7efc9aa0c4a336a41d11cd2a1095805d3e476984')
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
$result=Join-Path $ProjectRoot 'Saved\ChatGPT\NiagaraFire\FireV4\burn_duration_result.json'
if(Test-Path -LiteralPath $result){Move-Item -LiteralPath $result -Destination (Join-Path $backup 'previous_burn_duration_result.json') -Force}
try {
 Assert-EditorClosed
 foreach($rel in $files){Copy-Item -LiteralPath (Join-Path $source $rel) -Destination (Join-Path $ProjectRoot $rel) -Force}
 foreach($f in Get-ChildItem -LiteralPath $bin -Recurse -File){
  $rel=$f.FullName.Substring($bin.Length).TrimStart('\')
  $dst=Join-Path $liveBin $rel
  New-Item -ItemType Directory -Force -Path (Split-Path $dst -Parent)|Out-Null
  Copy-Item -LiteralPath $f.FullName -Destination $dst -Force
 }
 $script=Join-Path $ProjectRoot 'Automation\ASTFire\audit_burn_duration.py'
 $project=Join-Path $ProjectRoot 'AST_V1_18.uproject'
 $auditLog=Join-Path $report ('Audit_'+$stamp+'.log')
 & $ue $project '-run=pythonscript' ('-script='+$script.Replace('\','/')) '-unattended' '-nop4' '-nosplash' '-nullrhi' *> $auditLog
 $exit=$LASTEXITCODE
 if(!(Test-Path -LiteralPath $result)){Get-Content -LiteralPath $auditLog -Tail 40;throw ('No audit result, UE exit '+$exit)}
 $data=Get-Content -LiteralPath $result -Raw|ConvertFrom-Json
 Write-Output ('BURN_DURATION_AUDIT='+($data|ConvertTo-Json -Depth 6 -Compress))
 if($exit -ne 0 -or $data.status -ne 'PASS'){throw 'Burn duration audit failed'}
 Write-Output 'BURN_DURATION_INSTALL_SUCCESS'
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
