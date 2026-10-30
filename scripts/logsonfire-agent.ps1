$ErrorActionPreference = 'Stop'
$endpoint = Join-Path $PSScriptRoot '../python/logsonfire.py'
if (!(Test-Path $endpoint)) {
    $endpoint = Join-Path $PSScriptRoot '../share/logsonfire/logsonfire.py'
}
$endpointArgs = @($args)
if ($endpointArgs.Count -gt 0 -and $endpointArgs[0] -eq 'savings') {
    $endpoint = Join-Path (Split-Path $endpoint) 'logsonfire_savings.py'
    $endpointArgs = @($endpointArgs | Select-Object -Skip 1)
}
if ($env:PYTHON) {
    & $env:PYTHON $endpoint @endpointArgs
} elseif (Get-Command python -ErrorAction SilentlyContinue) {
    & python $endpoint @endpointArgs
} else {
    & py -3 $endpoint @endpointArgs
}
exit $LASTEXITCODE
