# Trae 7z.dll de 7-Zip (versión fija) de su sitio oficial y comprueba la huella SHA-256 de todo lo que baja.
#
# Uso: powershell -ExecutionPolicy Bypass -File traer-7zip.ps1 [-Destino <carpeta>]
#
# Deja en build\7zip (o en -Destino):
#   7z.dll            el motor de 7-Zip para RAR y 7z (sale del instalador oficial, sin instalar nada)
#   License.txt       la licencia de 7-Zip que va con ese 7z.dll (LGPL, partes BSD y la restricción de unRAR)
#   copying.txt       el texto completo de la GNU LGPL, del código fuente de 7-Zip
#   unRarLicense.txt  la licencia de unRAR, del código fuente de 7-Zip
#   7z2605-src.7z     el código fuente de esa misma versión (la LGPL pide ofrecerlo junto a la DLL)
#
# Las descargas son las que enlaza https://www.7-zip.org/download.html (el sitio oficial; desde 2026 sus archivos están
# en el GitHub oficial de 7-Zip, github.com/ip7z/7zip, y www.7-zip.org/a/ lleva ahí). Las huellas coinciden con las que
# publica esa versión. Si una no coincide, se detiene y no usa nada.
param([string]$Destino = (Join-Path $PSScriptRoot 'build\7zip'))
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$version = '26.05'
$archivos = @(
    # El instalador para Windows de 64 bits: de aquí sale 7z.dll (no se ejecuta, solo se abre como archivo comprimido)
    @{ Nombre = '7z2605-x64.exe'; Url = @('https://www.7-zip.org/a/7z2605-x64.exe', 'https://github.com/ip7z/7zip/releases/download/26.05/7z2605-x64.exe');
       Sha256 = 'fd99e86f57d8f429ef5ed045bf98315af962e2309b0f1ba87a147a44c71647d0' },
    # 7-Zip de consola (7zr.exe), solo para sacar 7z.dll del instalador y los textos del código fuente
    @{ Nombre = '7zr-2605.exe'; Url = @('https://github.com/ip7z/7zip/releases/download/26.05/7zr.exe');
       Sha256 = '9bf582b894dfa665f852d7e9112b02290a1c303c2d95a21fcdc41ffcb03143b7' },
    # El código fuente de la misma versión
    @{ Nombre = '7z2605-src.7z'; Url = @('https://github.com/ip7z/7zip/releases/download/26.05/7z2605-src.7z');
       Sha256 = 'fa081a1a33186158d71e9e24f494ed852ef8d0c9d06e21a86fb4713d94b55ae9' }
)
$huellaDll = '78bf6b668d75801e3aa180b4ccb16e6d8395fba80dec8a065055bb6f8dfabb82'   # 7z.dll 26.05 de 64 bits

function Huella($ruta) { (Get-FileHash -LiteralPath $ruta -Algorithm SHA256).Hash.ToLower() }

# Lo bajado se guarda para la próxima vez (con su huella revisada cada vez)
$cache = if ($env:JICARO_DESCARGAS) { $env:JICARO_DESCARGAS } elseif ($env:VCPKG_DOWNLOADS) { $env:VCPKG_DOWNLOADS } else { Join-Path $PSScriptRoot 'vendor\descargas' }
New-Item -ItemType Directory -Force $cache | Out-Null
$rutas = @{}
foreach ($a in $archivos) {
    $ruta = Join-Path $cache $a.Nombre
    if (-not (Test-Path -LiteralPath $ruta) -or (Huella $ruta) -ne $a.Sha256) {
        # Cada dirección oficial, con un segundo intento si la red falla un momento
        $parcial = "$ruta.parcial"
        $bajo = $false
        foreach ($url in $a.Url) {
            foreach ($intento in 1, 2) {
                try {
                    Write-Host "Bajando $($a.Nombre) de $url"
                    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $parcial
                    $bajo = $true
                    break
                }
                catch {
                    Write-Host "  no se pudo ($($_.Exception.Message))"
                    Start-Sleep -Seconds 3
                }
            }
            if ($bajo) { break }
        }
        if (-not $bajo) { throw "No se pudo bajar $($a.Nombre) de ninguna dirección oficial" }
        $h = Huella $parcial
        if ($h -ne $a.Sha256) {
            Remove-Item -LiteralPath $parcial -Force
            throw "La huella de $($a.Nombre) no coincide (esperaba $($a.Sha256), bajó $h). No se usa."
        }
        Move-Item -LiteralPath $parcial -Destination $ruta -Force
    }
    $rutas[$a.Nombre] = $ruta
}

$temporal = Join-Path ([System.IO.Path]::GetTempPath()) ('jicaro-7zip-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $temporal | Out-Null
try {
    $siete = $rutas['7zr-2605.exe']
    & $siete e -y "-o$temporal" $rutas['7z2605-x64.exe'] '7z.dll' 'License.txt' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "No se pudo sacar 7z.dll del instalador (código $LASTEXITCODE)" }
    & $siete e -y "-o$temporal" $rutas['7z2605-src.7z'] 'DOC\copying.txt' 'DOC\unRarLicense.txt' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "No se pudieron sacar las licencias del código fuente (código $LASTEXITCODE)" }
    foreach ($n in '7z.dll', 'License.txt', 'copying.txt', 'unRarLicense.txt') {
        if (-not (Test-Path -LiteralPath (Join-Path $temporal $n))) { throw "Falta $n en lo que bajó" }
    }
    $h = Huella (Join-Path $temporal '7z.dll')
    if ($h -ne $huellaDll) { throw "La huella de 7z.dll no coincide (esperaba $huellaDll, salió $h). No se usa." }

    New-Item -ItemType Directory -Force $Destino | Out-Null
    foreach ($n in '7z.dll', 'License.txt', 'copying.txt', 'unRarLicense.txt') {
        Copy-Item -LiteralPath (Join-Path $temporal $n) -Destination (Join-Path $Destino $n) -Force
    }
    Copy-Item -LiteralPath $rutas['7z2605-src.7z'] -Destination (Join-Path $Destino '7z2605-src.7z') -Force
    Set-Content -LiteralPath (Join-Path $Destino 'version.txt') -Value $version -Encoding ascii
}
finally {
    Remove-Item -LiteralPath $temporal -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "7-Zip $version listo en $Destino (7z.dll con su huella comprobada)"
