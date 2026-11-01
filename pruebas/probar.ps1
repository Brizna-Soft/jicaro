# Corre las pruebas sin ventana: prueba-rutas (comprobación de rutas y partes) y prueba-nucleo (extrae archivos de
# verdad, propios y de la suite de pruebas de libarchive, que es BSD). Los RAR y 7z se leen con 7z.dll, que tiene que
# estar junto a prueba-nucleo.exe (la pone compilar.ps1). Los RAR 4 cifrados en 6 partes son de las pruebas de
# SharpCompress (licencia MIT, https://github.com/adamhathcock/sharpcompress): se bajan de una versión fija y se
# comprueba su huella.
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

# RAR 4 cifrado en 6 partes (contraseña «test»), de SharpCompress
$sharp = Join-Path $locales 'sharpcompress'
New-Item -ItemType Directory -Force $sharp | Out-Null
$commitSharp = '6128ec51eec4ca9f7b7b9f44b77bdb0cd8b3bd97'
$partesSharp = [ordered]@{
    'Rar.EncryptedParts.part01.rar' = 'd753717b1210d70b2835c6e2929f2a2d70170fed7bbb7430082a45d293149857'
    'Rar.EncryptedParts.part02.rar' = '56f164a11108a25c0c353825bce5169e160d3c2d2265ed2522c8596f9a8f6c48'
    'Rar.EncryptedParts.part03.rar' = 'fcf69b2c8775a3ca8d9b1c88f0e3d4fe5ce2c730ea83984e008b7c3f9e3cde5b'
    'Rar.EncryptedParts.part04.rar' = '3d8569689c726f9d3a8f6a0016bf135316f220f629c4e289f50cabca1ceb5402'
    'Rar.EncryptedParts.part05.rar' = '39c072812755922fae70e62027f9878eaaaf61fa93f0d9fa103042891445503f'
    'Rar.EncryptedParts.part06.rar' = 'a383f45967078ba441a99b90fc98f54d8130d34d856cd7b2922da51b53dc46d4'
}
foreach ($n in $partesSharp.Keys) {
    $ruta = Join-Path $sharp $n
    if ((Test-Path $ruta) -and (Get-FileHash $ruta -Algorithm SHA256).Hash.ToLower() -eq $partesSharp[$n]) { continue }
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/adamhathcock/sharpcompress/$commitSharp/tests/TestArchives/Archives/$n" -OutFile $ruta
    if ((Get-FileHash $ruta -Algorithm SHA256).Hash.ToLower() -ne $partesSharp[$n]) { Remove-Item -Force $ruta; throw "La huella de $n no coincide" }
}
if (-not (Test-Path (Join-Path $Build '7z.dll'))) { throw "Falta 7z.dll junto a las pruebas ($Build). Compila con compilar.ps1." }

& (Join-Path $Build 'prueba-rutas.exe')
if ($LASTEXITCODE -ne 0) { throw "prueba-rutas: $LASTEXITCODE fallas" }
$trabajo = Join-Path $locales ('trabajo-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
& (Join-Path $Build 'prueba-nucleo.exe') $suite $trabajo $sharp
$fallas = $LASTEXITCODE
if ($fallas -ne 0) { throw "prueba-nucleo: $fallas fallas (los archivos quedaron en $trabajo)" }
Remove-Item -Recurse -Force $trabajo
Write-Host 'Pruebas sin ventana: todo bien.' -ForegroundColor Green
