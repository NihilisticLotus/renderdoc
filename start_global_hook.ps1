# Legacy entry point. Global Hook is managed by qrenderdoc so its normal cleanup restores AppInit.
$ErrorActionPreference = 'Stop'
$runtime = 'C:\RenderDocCustom\qrenderdoc.exe'
if (-not (Test-Path -LiteralPath $runtime)) {
    throw 'Deploy the custom runtime with deploy_custom_runtime.ps1 first.'
}
Write-Output 'Use Enable Global Hook in the RenderDoc Launch Application tab.'
Start-Process -FilePath $runtime
