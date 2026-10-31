# Corre las pruebas sin ventana: prueba-rutas (comprobación de rutas y partes) y prueba-nucleo (extrae archivos de
# verdad, propios y de la suite de pruebas de libarchive, que es BSD).
#
# Uso: powershell -ExecutionPolicy Bypass -File pruebas\probar.ps1
#
# La suite de libarchive sale del mismo código que bajó vcpkg (la versión del port). Si no está en la caché de vcpkg,
# se baja de GitHub. Todo queda en pruebas-locales\ (no se sube al repositorio).
param([string]$Build = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build\Release'))
$ErrorActionPreference = 'Stop'
$raiz = Split-Path $PSScriptRoot -Parent
$locales = Join-Path $raiz 'pruebas-locales'
$suite = Join-Path $locales 'libarchive-test'
New-Item -ItemType Directory -Force $locales | Out-Null

if (-not (Test-Path (Join-Path $suite 'test_read_format_rar.rar.uu'))) {
    if (-not $env:VCPKG_ROOT) { throw 'Falta la variable VCPKG_ROOT (la pone compilar.ps1)' }
    $version = (Get-Content (Join-Path $env:VCPKG_ROOT 'ports\libarchive\vcpkg.json') -Raw | ConvertFrom-Json).version
    $nombre = "libarchive-libarchive-v$version.tar.gz"
    $tar = @($env:VCPKG_DOWNLOADS, (Join-Path $env:VCPKG_ROOT 'downloads')) | Where-Object { $_ } |
        ForEach-Object { Join-Path $_ $nombre } | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $tar) {
        $tar = Join-Path $locales $nombre
        Invoke-WebRequest "https://github.com/libarchive/libarchive/archive/refs/tags/v$version.tar.gz" -OutFile $tar -UseBasicParsing
    }
    $fuente = Join-Path $locales 'libarchive-fuente'
    if (Test-Path $fuente) { Remove-Item -Recurse -Force $fuente }
    New-Item -ItemType Directory -Force $fuente | Out-Null
    & "$env:SystemRoot\System32\tar.exe" -xf $tar -C $fuente "libarchive-$version/libarchive/test"
    if ($LASTEXITCODE -ne 0) { throw 'No se pudo sacar la suite de pruebas de libarchive' }
    Move-Item (Join-Path $fuente "libarchive-$version\libarchive\test") $suite
    Remove-Item -Recurse -Force $fuente
}

& (Join-Path $Build 'prueba-rutas.exe')
if ($LASTEXITCODE -ne 0) { throw "prueba-rutas: $LASTEXITCODE fallas" }
$trabajo = Join-Path $locales ('trabajo-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
& (Join-Path $Build 'prueba-nucleo.exe') $suite $trabajo
$fallas = $LASTEXITCODE
if ($fallas -ne 0) { throw "prueba-nucleo: $fallas fallas (los archivos quedaron en $trabajo)" }
Remove-Item -Recurse -Force $trabajo
Write-Host 'Pruebas sin ventana: todo bien.' -ForegroundColor Green
