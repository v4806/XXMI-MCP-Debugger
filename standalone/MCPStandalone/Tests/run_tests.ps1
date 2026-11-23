$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$env:PYTHONPATH = if ([string]::IsNullOrEmpty($env:PYTHONPATH)) {
    $root
} else {
    "$root;$env:PYTHONPATH"
}
$msbuild = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path -LiteralPath $vswhere) {
    $found = & $vswhere -latest -products * `
        -requires Microsoft.Component.MSBuild `
        -find 'MSBuild\**\Bin\MSBuild.exe'
    if ($found) {
        $msbuild = @($found)[0]
    }
}
if (-not $msbuild) {
    $fallback = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
    if (Test-Path -LiteralPath $fallback) {
        $msbuild = $fallback
    }
}
if (-not $msbuild) {
    throw 'MSBuild was not found via vswhere or the current fallback path.'
}

$testProject = Join-Path $root 'MCPStandalone\Tests\MCPStandaloneTests.vcxproj'
$testExe = Join-Path $root 'MCPStandalone\Tests\x64\Debug\MCPStandaloneTests.exe'
$dllProject = Join-Path $root 'MCPStandalone\MCPStandalone.vcxproj'
$debugDll = Join-Path $root 'MCPStandalone\x64\Debug\mcp.dll'
$releaseTestExe = Join-Path $root 'MCPStandalone\Tests\x64\Zip Release\MCPStandaloneTests.exe'
$releaseDll = Join-Path $root 'MCPStandalone\x64\Zip Release\mcp.dll'

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Command,
        [Parameter(Mandatory = $true)][string]$Description
    )
    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE"
    }
}

$analyzerTests = Join-Path $root 'MCPStandalone\Tools\test_analyze_migoto.py'
if (Test-Path -LiteralPath $analyzerTests) {
    Push-Location $root
    try {
        Invoke-Checked {
            & python -m unittest MCPStandalone.Tools.test_analyze_migoto -v
        } 'Analyzer unit tests'
    } finally {
        Pop-Location
    }
}

Invoke-Checked {
    & $msbuild $testProject /m /p:Configuration=Debug /p:Platform=x64 /v:minimal
} 'Debug native test build'
Invoke-Checked {
    & $testExe
} 'Debug native tests'

Invoke-Checked {
    & $msbuild $dllProject /m /p:Configuration=Debug /p:Platform=x64 /v:minimal
} 'Debug DLL build'

$env:XXMI_MCP_DLL = $debugDll
Invoke-Checked {
    & python (Join-Path $root 'tests\test_standalone_pipe.py') -v
} 'Debug pipe tests'

Invoke-Checked {
    & $msbuild $testProject /m /p:Configuration='Zip Release' /p:Platform=x64 /v:minimal
} 'Release native test build'
Invoke-Checked {
    & $releaseTestExe
} 'Release native tests'

Invoke-Checked {
    & $msbuild $dllProject /m /p:Configuration='Zip Release' /p:Platform=x64 /v:minimal
} 'Release DLL build'

$env:XXMI_MCP_DLL = $releaseDll
Invoke-Checked {
    & python (Join-Path $root 'tests\test_standalone_pipe.py') -v
} 'Release pipe tests'

$exportCheck = @'
import pefile
import sys

path = sys.argv[1]
pe = pefile.PE(path)
if not hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
    raise SystemExit(0)
names = {entry.name.decode() for entry in pe.DIRECTORY_ENTRY_EXPORT.symbols if entry.name}
forbidden = {"D3D11CreateDevice", "D3D11CreateDeviceAndSwapChain"}
bad = names & forbidden
if bad:
    raise SystemExit("unexpected proxy exports: " + ", ".join(sorted(bad)))
imports = {
    entry.dll.decode().lower()
    for entry in getattr(pe, "DIRECTORY_ENTRY_IMPORT", [])
}
forbidden_runtime = {
    "msvcp140.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll",
}
bad_runtime = imports & forbidden_runtime
if bad_runtime:
    raise SystemExit(
        "dynamic CRT imports found: " + ", ".join(sorted(bad_runtime))
    )
print(f"export check passed ({len(names)} named exports)")
print("static CRT import check passed")
'@
Invoke-Checked {
    & python -c $exportCheck $releaseDll
} 'DLL export check'

$official = $env:XXMI_OFFICIAL_DLL
if (-not $official) {
    $official = 'D:\Temp\XXMI-PACKAGE-v1.3.3\d3d11.dll'
}
if (Test-Path -LiteralPath $official) {
    $analysis = Join-Path $root 'MCPStandalone\analysis\official-v1.3.3-signatures.json'
    Invoke-Checked {
        & python -m MCPStandalone.Tools.analyze_migoto $official --json $analysis
    } 'Official DLL signature analysis'
} else {
    Write-Host "Official DLL fixture not found; analyzer check skipped."
}

Remove-Item Env:XXMI_MCP_DLL -ErrorAction SilentlyContinue
Write-Host "All standalone MCP DLL checks passed."
