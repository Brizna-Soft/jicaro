# Arma publicar\Jicaro-<versión>-x64.zip (programa, 7z.dll y licencias) y su .sha256.
# Antes hay que compilar con compilar.ps1.
$ErrorActionPreference = 'Stop'
$raiz = $PSScriptRoot
$version = (Get-Content (Join-Path $raiz 'version.txt') -TotalCount 1).Trim()
$programa = Join-Path $raiz 'programa'
$publicar = Join-Path $raiz 'publicar'
$nombre = "Jicaro-$version-x64"
$temporal = Join-Path $publicar $nombre

if (-not (Test-Path (Join-Path $programa 'Jicaro.exe'))) { throw "Falta Jicaro.exe en ${programa}. Compila primero." }
if (-not (Test-Path (Join-Path $programa '7z.dll'))) { throw "Falta 7z.dll en ${programa}. Compila primero." }
& (Join-Path $raiz 'generar-avisos.ps1')

if (Test-Path $temporal) { Remove-Item -Recurse -Force $temporal }
New-Item -ItemType Directory -Force $temporal | Out-Null
Copy-Item (Join-Path $programa 'Jicaro.exe') $temporal
Copy-Item (Join-Path $programa '7z.dll') $temporal
Copy-Item (Join-Path $raiz 'LICENSE') (Join-Path $temporal 'LICENCIA.txt')
Copy-Item (Join-Path $raiz 'AVISOS-DE-TERCEROS.txt') $temporal
$leeme = @"
Jícaro $version - descompresor gratis de Brizna Soft
====================================================

1. Abre Jicaro.exe (no hace falta instalar nada). Deja 7z.dll en la misma carpeta: con ese archivo Jícaro abre
   los RAR y los 7z.
2. Si Windows dice "Windows protegió su PC": pulsa "Más información" y luego "Ejecutar de todas formas".
   Sale porque el programa es nuevo y todavía no tiene firma digital.
3. Arrastra un archivo comprimido a la ventana, o pulsa "Abrir...". Con una parte de un RAR en varias partes
   (por ejemplo juego.part03.rar) abre el juego completo y te avisa si falta alguna parte.
4. Para tener "Extraer aquí" en el clic derecho: pulsa "Ajustes" y marca la casilla. En Windows 11 sale en
   "Mostrar más opciones". Antes de borrar Jícaro, desmarca esa casilla o abre "Quitar-clic-derecho.bat".
5. Jícaro solo extrae. Para comprimir, usa el de Windows: clic derecho > Enviar a > Carpeta comprimida.

Abre: ZIP, RAR, RAR 5 y 7z (también con contraseña y en varias partes), TAR, GZ, BZ2, XZ, ZST, ISO y CAB.
Si un archivo tiene contraseña, Jícaro te la pide; si no sirve, te avisa y te la pide otra vez. Jícaro solo extrae:
no crea RAR.

7z.dll es de 7-Zip (de Igor Pavlov, licencia GNU LGPL; su código fuente está en https://www.7-zip.org).
Las licencias de todo lo que usa Jícaro están en AVISOS-DE-TERCEROS.txt.

Ayuda y reportes de problemas: https://github.com/Brizna-Soft/jicaro
Sin publicidad, sin virus y sin recoger tus datos. Licencia MIT.
"@
[System.IO.File]::WriteAllText((Join-Path $temporal 'LEEME.txt'), $leeme, (New-Object System.Text.UTF8Encoding($true)))
# Para quien borra Jícaro sin desmarcar el clic derecho: quita «Extraer aquí» y lo demás que puso Jícaro.
# Solo letras sin acentos: cmd lee los .bat con la página de códigos de la consola.
$bat = "@echo off`r`nrem Quita del clic derecho lo que puso Jicaro (Extraer aqui). No borra nada mas.`r`n" +
       "`"%~dp0Jicaro.exe`" --quitar-menu`r`n"
[System.IO.File]::WriteAllText((Join-Path $temporal 'Quitar-clic-derecho.bat'), $bat, (New-Object System.Text.ASCIIEncoding))

$zip = Join-Path $publicar "$nombre.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $temporal '*') -DestinationPath $zip
Remove-Item -Recurse -Force $temporal
$huella = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
[System.IO.File]::WriteAllText("$zip.sha256", "$huella  $nombre.zip`n", (New-Object System.Text.UTF8Encoding($false)))
Write-Host "Listo: $zip"
Write-Host "SHA256: $huella"
