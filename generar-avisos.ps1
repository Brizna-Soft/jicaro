# Junta las licencias de las bibliotecas que van dentro de Jícaro en AVISOS-DE-TERCEROS.txt.
# Las toma de lo que instaló vcpkg (build\vcpkg_installed\<triplet>\share\<biblioteca>\copyright). Las que tienen el
# mismo texto van una sola vez, con todos sus nombres.
param([string]$Triplet = 'x64-windows-static-rel')
$ErrorActionPreference = 'Stop'
$share = Join-Path $PSScriptRoot "build\vcpkg_installed\$Triplet\share"
if (-not (Test-Path $share)) { throw "Primero compila: no existe $share" }
# 7-Zip: lo deja traer-7zip.ps1 (lo llama compilar.ps1)
$sieteZip = Join-Path $PSScriptRoot 'build\7zip'
foreach ($n in 'License.txt', 'copying.txt', 'unRarLicense.txt', 'version.txt') {
    if (-not (Test-Path (Join-Path $sieteZip $n))) { throw "Primero compila: falta $sieteZip\$n" }
}
$versionSieteZip = (Get-Content (Join-Path $sieteZip 'version.txt') -TotalCount 1).Trim()
$fuenteSieteZip = '7z' + $versionSieteZip.Replace('.', '') + '-src.7z'

$grupos = [ordered]@{}   # texto de la licencia -> nombres de las bibliotecas
foreach ($d in Get-ChildItem $share -Directory | Sort-Object Name) {
    if ($d.Name -like 'vcpkg-*') { continue }   # herramientas de compilación, no van en el programa
    $copyright = Join-Path $d.FullName 'copyright'
    if (-not (Test-Path $copyright)) { continue }
    $texto = (Get-Content $copyright -Raw -Encoding UTF8).TrimEnd()
    # Lo que vcpkg no copia entero (el aviso de PCRE2 solo dice «mira el archivo LICENCE», el de libjpeg-turbo remite
    # a README.ijg y el COPYING de libarchive solo nombra la licencia de UC Regents): avisos-extra\<biblioteca>.txt
    # trae el texto que falta, sacado del repositorio oficial, con su fuente
    $extra = Join-Path $PSScriptRoot "avisos-extra\$($d.Name).txt"
    if (Test-Path $extra) { $texto += "`n`n" + (Get-Content $extra -Raw -Encoding UTF8).TrimEnd() }
    if (-not $grupos.Contains($texto)) { $grupos[$texto] = New-Object System.Collections.Generic.List[string] }
    $grupos[$texto].Add($d.Name)
}

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('AVISOS DE TERCEROS')
[void]$sb.AppendLine('==================')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('Jícaro usa estas bibliotecas de código abierto. Gracias a sus autores.')
[void]$sb.AppendLine('Lee los archivos comprimidos con libarchive (licencia BSD) y, los RAR y los 7z, con 7z.dll de 7-Zip')
[void]$sb.AppendLine('(licencia GNU LGPL, con partes BSD y la restricción de unRAR).')
[void]$sb.AppendLine('Abajo va el aviso de licencia de cada una, tal como lo publican. Nombrarlas no significa que sus')
[void]$sb.AppendLine('autores respalden Jícaro ni a Brizna Soft.')
[void]$sb.AppendLine('')

# Primero 7-Zip: es el único que va como archivo aparte (7z.dll) y su licencia pide más
[void]$sb.AppendLine('------------------------------------------------------------------------')
[void]$sb.AppendLine("7-Zip $versionSieteZip (7z.dll)")
[void]$sb.AppendLine('------------------------------------------------------------------------')
[void]$sb.AppendLine('Jícaro usa 7z.dll, de 7-Zip, para abrir los RAR y los 7z (también con contraseña y en partes) y los ZIP en partes.')
[void]$sb.AppendLine('7z.dll va como archivo aparte, sin ningún cambio, junto a Jicaro.exe: se puede cambiar por otra versión')
[void]$sb.AppendLine('compatible de 7-Zip. Jícaro solo extrae: no crea RAR ni 7z.')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('7-Zip: Copyright (C) 1999-2026 Igor Pavlov. https://www.7-zip.org')
[void]$sb.AppendLine("Código fuente de esta misma versión (7-Zip $versionSieteZip), del sitio oficial:")
[void]$sb.AppendLine('  https://www.7-zip.org/download.html')
[void]$sb.AppendLine("  https://github.com/ip7z/7zip/releases/download/$versionSieteZip/$fuenteSieteZip")
[void]$sb.AppendLine("También va ($fuenteSieteZip) junto a cada versión de Jícaro: https://github.com/Brizna-Soft/jicaro/releases")
[void]$sb.AppendLine('')
[void]$sb.AppendLine('Texto de License.txt de 7-Zip, tal como viene con 7z.dll:')
[void]$sb.AppendLine('')
[void]$sb.AppendLine((Get-Content (Join-Path $sieteZip 'License.txt') -Raw -Encoding UTF8).TrimEnd())
[void]$sb.AppendLine('')
[void]$sb.AppendLine('Texto completo de la GNU LGPL (copying.txt, del código fuente de 7-Zip):')
[void]$sb.AppendLine('')
[void]$sb.AppendLine((Get-Content (Join-Path $sieteZip 'copying.txt') -Raw -Encoding UTF8).TrimEnd())
[void]$sb.AppendLine('')
[void]$sb.AppendLine('Licencia de unRAR (unRarLicense.txt, del código fuente de 7-Zip):')
[void]$sb.AppendLine('')
[void]$sb.AppendLine((Get-Content (Join-Path $sieteZip 'unRarLicense.txt') -Raw -Encoding UTF8).TrimEnd())
[void]$sb.AppendLine('')
foreach ($texto in $grupos.Keys) {
    [void]$sb.AppendLine('------------------------------------------------------------------------')
    [void]$sb.AppendLine(($grupos[$texto] -join ', '))
    [void]$sb.AppendLine('------------------------------------------------------------------------')
    [void]$sb.AppendLine($texto)
    [void]$sb.AppendLine('')
}
$salida = Join-Path $PSScriptRoot 'AVISOS-DE-TERCEROS.txt'
[System.IO.File]::WriteAllText($salida, $sb.ToString(), (New-Object System.Text.UTF8Encoding($true)))
Write-Host "Listo: $salida (7-Zip y $($grupos.Count) licencias distintas más)"
