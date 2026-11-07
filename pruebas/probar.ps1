# Corre las pruebas sin ventana: prueba-rutas (comprobación de rutas y partes) y prueba-nucleo (extrae archivos de
# verdad, propios y de la suite de pruebas de libarchive, que es BSD). Los RAR y 7z se leen con 7z.dll, que tiene que
# estar junto a prueba-nucleo.exe (la pone compilar.ps1). Los RAR 4 cifrados en 6 partes son de las pruebas de
# SharpCompress (licencia MIT, https://github.com/adamhathcock/sharpcompress): se bajan de una versión fija y se
# comprueba su huella. Los ZIP en partes (.z01) son de las pruebas de Apache Commons Compress (licencia Apache-2.0).
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
# ZIP en partes (.z01, .z02 y .zip) hechos con WinRAR y con Info-ZIP, y sus ZIP enteros para comparar, de las pruebas de
# Apache Commons Compress (licencia Apache-2.0, https://github.com/apache/commons-compress)
$commons = Join-Path $locales 'commons-compress'
New-Item -ItemType Directory -Force $commons | Out-Null
$commitCommons = '69a29b75922f7e525412dc6b7454b70d1f2be2bd'
$partesCommons = [ordered]@{
    'split_zip_created_by_winrar/split_zip_created_by_winrar.z01' = '0bb2b1a3f49d01e0fd5b407c7e34dc7af984969c0bbe47367912dad01a016559'
    'split_zip_created_by_winrar/split_zip_created_by_winrar.z02' = 'e54f70cc37f5771602c17c10091a6126c15a0c8895b7e4ca6ea35312477946ab'
    'split_zip_created_by_winrar/split_zip_created_by_winrar.zip' = 'ea2d067a99a38f10288b2eed4543337719a41b0e6b8585a87f8728e5e317410a'
    'split_zip_created_by_winrar/zip_to_compare_created_by_winrar.zip' = '33169cd55b80be1c1b6174d3bf8432b7c239bc5372e2c86e8b7bef95f88e6d81'
    'split_zip_created_by_zip/split_zip_created_by_zip.z01' = '976bb2c4c8a9093be32824e0b2d112335581aba733d6103e65fcc63bc1560878'
    'split_zip_created_by_zip/split_zip_created_by_zip.z02' = 'e6cdf0faf9016807160197d017cfd8a59815b5e55c3360f4c98f105ac609dc40'
    'split_zip_created_by_zip/split_zip_created_by_zip.zip' = 'fb86d35f40656889434d6e69b1055d11dfb44d3d68555a74891f187e6dc3333c'
    'split_zip_created_by_zip/zip_to_compare_created_by_zip.zip' = '91ea5329c0edd6a9ef7e2ce93cfca92501fc6e4f083dedf39037ae8659dd676e'
    'split_zip_created_by_zip/split_zip_created_by_zip_zip64.z01' = '38a2673a82a76deb82bf1897a87b06938fa02119a8382eb80c500e6ca7a67a17'
    'split_zip_created_by_zip/split_zip_created_by_zip_zip64.z02' = '3e6812211e4f5fb3e636e4a700bf8dfd385bac26b36e989a2c97dacf1d25748e'
    'split_zip_created_by_zip/split_zip_created_by_zip_zip64.zip' = 'b647a24b712f7f873a20e38e21ca898bf32ab5297a28dc7a7b69ad9e9bb986f2'
    'split_zip_created_by_zip/zip_to_compare_created_by_zip_zip64.zip' = 'd963653f8c6e22a9e2ec276909aac4d98e00f6951f9b175e7848a135b0920e2f'
}
foreach ($a in $partesCommons.Keys) {
    $ruta = Join-Path $commons (Split-Path $a -Leaf)
    if ((Test-Path $ruta) -and (Get-FileHash $ruta -Algorithm SHA256).Hash.ToLower() -eq $partesCommons[$a]) { continue }
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/apache/commons-compress/$commitCommons/src/test/resources/COMPRESS-477/$a" -OutFile $ruta
    if ((Get-FileHash $ruta -Algorithm SHA256).Hash.ToLower() -ne $partesCommons[$a]) { Remove-Item -Force $ruta; throw "La huella de $a no coincide" }
}
if (-not (Test-Path (Join-Path $Build '7z.dll'))) { throw "Falta 7z.dll junto a las pruebas ($Build). Compila con compilar.ps1." }

& (Join-Path $Build 'prueba-rutas.exe')
if ($LASTEXITCODE -ne 0) { throw "prueba-rutas: $LASTEXITCODE fallas" }
$trabajo = Join-Path $locales ('trabajo-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
& (Join-Path $Build 'prueba-nucleo.exe') $suite $trabajo $sharp $commons
$fallas = $LASTEXITCODE
if ($fallas -ne 0) { throw "prueba-nucleo: $fallas fallas (los archivos quedaron en $trabajo)" }
# Las pruebas dejan rutas más largas que 260 letras: si Remove-Item no puede, se borra con la forma larga (\\?\)
try { Remove-Item -Recurse -Force $trabajo -ErrorAction Stop }
catch {
    & cmd.exe /c "rmdir /s /q `"\\?\$trabajo`""
    if (Test-Path $trabajo) { Write-Warning "No se pudo borrar $trabajo (las pruebas sí pasaron)" }
}
Write-Host 'Pruebas sin ventana: todo bien.' -ForegroundColor Green
