param([string]$Destination = 'C:\RenderDocCustom')
$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot
# Runtime copies keep qrenderdoc, global hook helpers, games and UE from locking build outputs.
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Destination 'x86') | Out-Null
$copies = @()
foreach ($platform in @('x64', 'Win32')) {
    $source = Join-Path $repo "$platform\Development"
    $target = if ($platform -eq 'x64') { $Destination } else { Join-Path $Destination 'x86' }
    foreach ($file in Get-ChildItem -LiteralPath $source -File) {
        if ($file.Extension -in @('.dll', '.pyd', '.zip', '.json') -or
            $file.Name -in @('qrenderdoc.exe', 'renderdoccmd.exe', 'renderdocui.exe')) {
            $copies += [pscustomobject]@{ Source = $file.FullName; Target = Join-Path $target $file.Name }
        }
    }
    if (Test-Path -LiteralPath (Join-Path $source 'qtplugins')) {
        Get-ChildItem -LiteralPath (Join-Path $source 'qtplugins') -File -Recurse | ForEach-Object {
            $relative = $_.FullName.Substring($source.Length + 1)
            $copies += [pscustomobject]@{ Source = $_.FullName; Target = Join-Path $target $relative }
        }
    }
}
# Check all destinations before copying, so an active capture does not leave a mixed deployment.
foreach ($copy in $copies) {
    if (Test-Path -LiteralPath $copy.Target) {
        try {
            $stream = [IO.File]::Open($copy.Target, [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]::None)
            $stream.Dispose()
        } catch {
            throw "Runtime file is in use: $($copy.Target). Disable Global Hook and close RenderDoc, captured games, and UE before deploying. Building does not require closing them."
        }
    }
}
foreach ($copy in $copies) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $copy.Target) | Out-Null
    Copy-Item -LiteralPath $copy.Source -Destination $copy.Target -Force
}
Write-Output "Deployed custom RenderDoc to $Destination"
