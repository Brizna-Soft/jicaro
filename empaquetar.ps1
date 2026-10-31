# Arma publicar\Jicaro-<versión>-x64.zip (programa y licencias) y su .sha256.
# Antes hay que compilar con compilar.ps1.
$ErrorActionPreference = 'Stop'
$raiz = $PSScriptRoot
$version = (Get-Content (Join-Path $raiz 'version.txt') -TotalCount 1).Trim()
$programa = Join-Path $raiz 'programa'
$publicar = Join-Path $raiz 'publicar'
$nombre = "Jicaro-$version-x64"
$temporal = Join-Path $publicar $nombre

if (-not (Test-Path (Join-Path $programa 'Jicaro.exe'))) { throw "Falta Jicaro.exe en ${programa}. Compila primero." }
& (Join-Path $raiz 'generar-avisos.ps1')

if (Test-Path $temporal) { Remove-Item -Recurse -Force $temporal }
New-Item -ItemType Directory -Force $temporal | Out-Null
Copy-Item (Join-Path $programa 'Jicaro.exe') $temporal
Copy-Item (Join-Path $raiz 'LICENSE') (Join-Path $temporal 'LICENCIA.txt')
Copy-Item (Join-Path $raiz 'AVISOS-DE-TERCEROS.txt') $temporal
$leeme = @"
Jícaro $version - descompresor gratis de Brizna Soft
====================================================

1. Abre Jicaro.exe (no hace falta instalar nada).
2. Si Windows dice "Windows protegió su PC": pulsa "Más información" y luego "Ejecutar de todas formas".
   Sale porque el programa es nuevo y todavía no tiene firma digital.
3. Arrastra un archivo comprimido a la ventana, o pulsa "Abrir...". Con una parte de un RAR en varias partes
   (por ejemplo juego.part03.rar) abre el juego completo y te avisa si falta alguna parte.
4. Para tener "Extraer aquí" en el clic derecho: pulsa "Ajustes" y marca la casilla. En Windows 11 sale en
   "Mostrar más opciones". Antes de borrar Jícaro, desmarca esa casilla.

Abre: ZIP (también con contraseña), RAR, RAR 5 y RAR en varias partes, 7z, TAR, GZ, BZ2, XZ, ZST, ISO y CAB.
No abre los RAR ni los 7z con contraseña: para esos usa 7-Zip o WinRAR. Jícaro solo extrae: no crea RAR.

Ayuda y reportes de problemas: https://github.com/Brizna-Soft/jicaro
Sin publicidad, sin virus y sin recoger tus datos. Licencia MIT.
"@
[System.IO.File]::WriteAllText((Join-Path $temporal 'LEEME.txt'), $leeme, (New-Object System.Text.UTF8Encoding($true)))

$zip = Join-Path $publicar "$nombre.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $temporal '*') -DestinationPath $zip
Remove-Item -Recurse -Force $temporal
$huella = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
[System.IO.File]::WriteAllText("$zip.sha256", "$huella  $nombre.zip`n", (New-Object System.Text.UTF8Encoding($false)))
Write-Host "Listo: $zip"
Write-Host "SHA256: $huella"
