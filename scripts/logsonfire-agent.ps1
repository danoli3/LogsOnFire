$ErrorActionPreference = 'Stop'
$endpoint = Join-Path $PSScriptRoot '../python/logsonfire.py'
if (!(Test-Path $endpoint)) {
    $endpoint = Join-Path $PSScriptRoot '../share/logsonfire/logsonfire.py'
}
if ($env:PYTHON) {
    & $env:PYTHON $endpoint @args
} elseif (Get-Command python -ErrorAction SilentlyContinue) {
    & python $endpoint @args
} else {
    & py -3 $endpoint @args
}
exit $LASTEXITCODE
