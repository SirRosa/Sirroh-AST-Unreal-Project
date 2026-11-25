#Requires -Version 5.1
[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-fA-F]{40}$')][string]$Revision,
 [string]$ProjectRoot='D:\0 Sirro\Unreal Projects\AST_V1_18',
 [string]$EngineRoot='D:\0 Sirro\UE 5.8.1\UE_5.8'
)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$report=Join-Path $ProjectRoot 'Saved\ChatGPT\NiagaraFire\FireV4'
$here=$PSScriptRoot
$stamp=Get-Date -Format yyyyMMdd_HHmmss
New-Item -ItemType Directory -Force $report|Out-Null
foreach($name in @('actual_raster.txt','actual_raster_report.json','actual_point_contact.png','actual_scale_mesh_contact.png','gpu_result.json')){
 $file=Join-Path $report $name
 if(Test-Path $file){Move-Item $file ($file+'.previous_'+$stamp) -Force}
}
$gpuFolder=Join-Path $report 'GPU'
if(Test-Path $gpuFolder){Move-Item $gpuFolder ($gpuFolder+'_previous_'+$stamp)}
$installed=$false
try {
 & (Join-Path $here 'deploy_fire_v4_windows.ps1') -Revision $Revision -ProjectRoot $ProjectRoot -EngineRoot $EngineRoot
 $installed=$true
} catch {
 Write-Output ('INSTALL_ERROR='+$_.Exception.Message)
}
$raster=Join-Path $report 'actual_raster.txt'
if(Test-Path $raster){
 & python (Join-Path $here 'render_fire_v4_raster.py') $raster
 Write-Output ('RASTER_REPORT_EXIT='+$LASTEXITCODE)
}
if(!$installed){throw 'Deployment not accepted; original local version preserved'}
if(Get-Process UnrealEditor,UnrealEditor-Cmd -ErrorAction SilentlyContinue){throw 'Editor opened before GPU audit'}
$ue=Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$script=(Join-Path $here 'verify_fire_v4_gpu.py').Replace('\','/')
$log=Join-Path $report ('GPU_Art_'+$stamp+'.log')
Write-Output ('GPU_START='+$log)
# Avoid the broken XGE remote shader compile fallback; use local workers.
& $ue (Join-Path $ProjectRoot 'AST_V1_18.uproject') '-run=pythonscript' ('-script='+$script) '-unattended' '-nop4' '-nosplash' '-AllowCommandletRendering' '-RenderOffscreen' '-d3d12' '-NoRemoteShaderCompile' *> $log
$code=$LASTEXITCODE
Write-Output ('GPU_EXIT='+$code)
$result=Join-Path $report 'gpu_result.json'
if(Test-Path $result){Get-Content $result -Raw}
Select-String $log -Pattern 'Failed to compile Material|ShaderCompileWorker.*(crash|failed)|LogShaderCompilers: Error|LogMaterial: Error|Creating D3D12 RHI|M_AST_FireTongue'|Select-Object -Last 15|ForEach-Object {$_.Line}
if($code -ne 0 -or !(Test-Path $result)){throw 'GPU audit incomplete'}
$j=Get-Content $result -Raw|ConvertFrom-Json
if($j.status -ne 'FIRE_V4_GPU_GRAPH_CHECKED_SHADER_LOG_REQUIRED'){throw 'GPU capture failed'}
Write-Output 'FIRE_V4_COMBINED_CHECK_COMPLETE'
