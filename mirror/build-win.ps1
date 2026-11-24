$ErrorActionPreference = 'Stop'
Push-Location -LiteralPath $PSScriptRoot
try {
    $packagerDir = 'node_modules/@electron/packager'
    $packagerManifest = Join-Path $packagerDir 'package.json'
    if (!(Test-Path -LiteralPath $packagerManifest)) {
        throw 'Run npm install in mirror first.'
    }
    $packager = Get-Content -LiteralPath $packagerManifest -Raw | ConvertFrom-Json
    $electron = Get-Content -LiteralPath 'node_modules/electron/package.json' -Raw | ConvertFrom-Json
    $packagerArgs = @(
        '.', 'NaomiPhone Mirror', '--platform=win32', '--arch=x64', "--electron-version=$($electron.version)",
        '--icon=icon.ico', '--out=dist', '--overwrite', '--asar',
        '--ignore=^/(dist|node_modules|tests)(/|$)',
        '--ignore=^/(build\.sh|build-win\.ps1|make_icon\.py|icon\.icns|home\.json|package-lock\.json)$'
    )
    & node (Join-Path $packagerDir $packager.bin.'electron-packager') @packagerArgs
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
