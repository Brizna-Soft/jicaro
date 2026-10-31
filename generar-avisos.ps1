# Junta las licencias de las bibliotecas que van dentro de Jícaro en AVISOS-DE-TERCEROS.txt.
# Las toma de lo que instaló vcpkg (build\vcpkg_installed\<triplet>\share\<biblioteca>\copyright). Las que tienen el
# mismo texto van una sola vez, con todos sus nombres.
param([string]$Triplet = 'x64-windows-static-rel')
$ErrorActionPreference = 'Stop'
$share = Join-Path $PSScriptRoot "build\vcpkg_installed\$Triplet\share"
if (-not (Test-Path $share)) { throw "Primero compila: no existe $share" }

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
[void]$sb.AppendLine('La principal es libarchive (licencia BSD), que lee los archivos comprimidos. Jícaro no trae 7-Zip ni unRAR.')
[void]$sb.AppendLine('Abajo va el aviso de licencia de cada una, tal como lo publican. Nombrarlas no significa que sus')
[void]$sb.AppendLine('autores respalden Jícaro ni a Brizna Soft.')
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
Write-Host "Listo: $salida ($($grupos.Count) licencias distintas)"
