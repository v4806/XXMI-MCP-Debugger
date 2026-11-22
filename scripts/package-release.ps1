param(
    [string]$Version = "0.1.6"
)

$ErrorActionPreference = "Stop"

$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$testScript = Join-Path $repo "standalone\MCPStandalone\Tests\run_tests.ps1"
$dll = Join-Path $repo "standalone\MCPStandalone\x64\Zip Release\mcp.dll"
$dist = Join-Path $repo "dist"
$stage = Join-Path $dist "XXMI-MCP-Debugger-$Version"
$archive = Join-Path $dist "XXMI-MCP-Debugger-$Version-win64.zip"

& $testScript
if ($LASTEXITCODE -ne 0) {
    throw "Verification failed with exit code $LASTEXITCODE"
}
if (-not (Test-Path -LiteralPath $dll)) {
    throw "Release DLL was not found: $dll"
}

New-Item -ItemType Directory -Path $dist -Force | Out-Null
$resolvedDist = (Resolve-Path $dist).Path
if (-not $stage.StartsWith(
        $resolvedDist + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "Release staging path escaped the dist directory"
}
if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Path $stage | Out-Null

Copy-Item -LiteralPath $dll -Destination $stage
Copy-Item -LiteralPath (Join-Path $repo "README.md") -Destination $stage
Copy-Item -LiteralPath (Join-Path $repo "LICENSE.GPL.txt") -Destination $stage
Copy-Item -LiteralPath (Join-Path $repo "COPYING.txt") -Destination $stage
Copy-Item -LiteralPath (Join-Path $repo "AUTHORS.txt") -Destination $stage
Copy-Item -LiteralPath (Join-Path $repo "THIRD_PARTY_NOTICES.md") -Destination $stage
Copy-Item -LiteralPath (Join-Path $repo "plugins") -Destination $stage -Recurse

$hashLines = Get-ChildItem -LiteralPath $stage -Recurse -File |
    Sort-Object FullName |
    ForEach-Object {
        $relative = [IO.Path]::GetRelativePath($stage, $_.FullName)
        $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        "$hash  $($relative.Replace('\', '/'))"
    }
$hashLines | Set-Content -LiteralPath (Join-Path $stage "SHA256SUMS.txt") `
    -Encoding utf8

if (Test-Path -LiteralPath $archive) {
    Remove-Item -LiteralPath $archive -Force
}
Compress-Archive -LiteralPath $stage -DestinationPath $archive `
    -CompressionLevel Optimal

$archiveHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
Write-Host "Archive: $archive"
Write-Host "SHA256:  $archiveHash"
