# Jícaro

**Descompresor gratis para Windows 10 y 11.** Abre y extrae ZIP, RAR, 7z, ISO y muchos más, también los que tienen
contraseña y los RAR cortados en varias partes que bajas de internet. Sin publicidad, sin virus y sin recoger tus datos.

El jícaro es el árbol hondureño cuyo fruto, seco, sirve de vasija. Hecho por [Brizna Soft](https://brizna-soft.github.io)
en Honduras.

![Jícaro mostrando lo que trae un ZIP, listo para extraer](docs/captura.png)

## Descargar

**[Baja la última versión](https://github.com/Brizna-Soft/jicaro/releases/latest)**: el archivo `Jicaro-x.y.z-x64.zip`.

1. Descomprime el .zip en una carpeta, por ejemplo `Documentos\Jícaro`.
2. Abre `Jicaro.exe`. No hace falta instalar nada. Deja `7z.dll` en la misma carpeta: con ese archivo Jícaro abre
   los RAR y los 7z.

> **¿Windows dice «Windows protegió su PC»?** Sale porque el programa es nuevo y todavía no tiene firma
> digital, que cuesta dinero. Pulsa **Más información** y luego **Ejecutar de todas formas**.
>
> Si quieres comprobar que el archivo es el original, compara su huella SHA-256 con el archivo `.sha256` de la
> misma página. Los .zip los arma GitHub a partir de este mismo código.

## Qué hace

- **Abre:** ZIP, RAR 4, RAR 5 y 7z (también con contraseña), TAR, GZ, BZ2, XZ, ZST, ISO y CAB. También los
  `.tar.gz`, `.tar.xz`, `.tar.zst` y parecidos, y los archivos sueltos como `notas.txt.gz`. Los RAR y los 7z los
  abre con el motor de 7-Zip (`7z.dll`); lo demás, con libarchive.
- **Contraseñas.** Si un archivo tiene contraseña, Jícaro te la pide (si hasta la lista de lo que trae tiene
  contraseña, te la pide al abrirlo). Si no sirve, te lo dice y te pide otra. Nunca prueba contraseñas por su cuenta.
  Sirve con ZIP (ZipCrypto y AES), RAR 4, RAR 5 y 7z, también en varias partes.
- **RAR en varias partes.** Abre cualquier parte (`juego.part03.rar`) y Jícaro junta el juego completo, desde la
  primera. Sirve con el estilo nuevo (`.part01.rar`, `.part02.rar`...) y con el viejo (`.rar`, `.r00`, `.r01`...).
  También los archivos cortados en trozos `.7z.001`, `.7z.002`... **Si falta una parte, te dice cuál.**
- **Ver antes de extraer:** arrastra el archivo a la ventana (o pulsa **Abrir...**) y ves lo que trae, con su tamaño
  y su fecha. Puedes extraer todo o solo lo que elijas, con progreso y un botón para cancelar.
- **Clic derecho:** si lo activas en **Ajustes**, aparecen «Extraer aquí» y «Extraer en una carpeta con su nombre».
  En Windows 11 están en **Mostrar más opciones**.
- **Si un archivo ya existe,** pregunta: reemplazar (lo que ya estaba va a la **Papelera**), guardar los dos o
  saltarlo. Se puede dejar elegido en Ajustes.

## Lo que no hace

- **No crea archivos RAR** (ni otros comprimidos): solo extrae.
- Sin `7z.dll` junto a `Jicaro.exe`, los RAR y los 7z se leen con libarchive, que no abre los que tienen
  contraseña: Jícaro te avisa que falta ese archivo.
- El nombre de la opción del clic derecho es fijo («Extraer en una carpeta con su nombre»): para que muestre el nombre
  de cada archivo haría falta un componente de Windows aparte, que esta versión no trae. Tampoco sale en el menú nuevo
  de Windows 11, solo en «Mostrar más opciones».

## Cuida tu seguridad

Los archivos comprimidos pueden venir con trampas. Jícaro las frena:

- **Nada sale de la carpeta de destino.** Se saltan las rutas con `..`, las rutas completas (`C:\...`, `\...`,
  `\\servidor\...`) y los nombres que Windows trataría como otra cosa (`CON`, `NUL`, `archivo:flujo`...). Jícaro hace
  su propia revisión además de usar las protecciones de libarchive: en las pruebas, libarchive sola dejaba pasar una
  ruta como `\Users\...\archivo.txt`. Con los RAR y los 7z pasa igual: 7z.dll solo lee, y cada archivo lo crea y lo
  revisa Jícaro, con las mismas reglas.
- **No crea flujos ocultos de NTFS** (`archivo:flujo`) que traiga un RAR o un 7z: así nadie puede cambiar la marca de
  la Web de lo que extraes.
- **No crea enlaces.** Los enlaces simbólicos y los enlaces duros que traiga el archivo no se crean, y no escribe a
  través de un enlace o una unión que ya estuviera en la carpeta: revisa todo el camino antes de escribir cada archivo.
  Un enlace duro a otro archivo del mismo comprimido se guarda como copia.
- **Bombas de descompresión** (archivos chiquitos hechos para llenar el disco): pregunta antes de seguir si lo
  extraído pasa de 512 MB y de 100 veces lo que pesa el comprimido, si ya ocupa más de la mitad del espacio libre que
  había o si trae más de 200,000 archivos. Cuando el archivo anuncia el tamaño, pregunta antes de crearlo; también
  cuentan los archivos «con huecos» y las copias de enlaces duros. Se detiene solo si el disco queda con menos de
  512 MB libres. Un RAR que pida más memoria de la que es razonable (hasta 4 GB, y nunca más de tres cuartos de la de
  la PC) no se extrae. Lo que quede a medias al detenerse se quita, pero solo si es el mismo archivo que creó Jícaro (lo
  comprueba con el identificador que le da Windows).
- **Marca de la Web.** Si el archivo comprimido se bajó de internet, lo que extraes queda marcado igual, como hace
  Windows con sus propios ZIP. Así Windows y tu antivirus siguen avisando antes de abrir programas que vienen de
  internet. Se hace como indica Microsoft: los programas y scripts pasan por los servicios de adjuntos de Windows
  (`IAttachmentExecute`, con el comprimido como origen), que además avisan al antivirus; lo demás recibe la misma
  marca con `IZoneIdentifier`.
- **Nunca reemplaza el archivo que está leyendo**, aunque la entrada use su nombre corto (como `JUEGO~1.RAR`), ni
  borra nada sin preguntar: lo que se reemplaza va a la Papelera.
- **7z.dll se carga solo de la carpeta de Jícaro**, con su ruta completa (nunca de la carpeta de trabajo ni del PATH).
  La compilación la baja del sitio oficial de 7-Zip, en una versión fija, y comprueba su huella SHA-256.
- **Las contraseñas no se guardan:** no van al registro de fallas ni a ningún archivo, y se borran de la memoria
  cuando ya no hacen falta.
- **«Extraer en una carpeta con su nombre»** no reutiliza un enlace o una unión que ya tenga ese nombre: en ese caso
  usa otro, como `juego (2)`.

## Privacidad

- Jícaro no tiene publicidad, no manda estadísticas y no se conecta a internet. Solo lo hace cuando pulsas
  **Ayuda → Buscar actualización**, que pregunta a GitHub por la versión más nueva.
- No pide ser administrador y no cambia la configuración de Windows. El clic derecho solo se escribe en tu cuenta
  (`HKEY_CURRENT_USER\Software\Classes`) cuando marcas la casilla, y se quita al desmarcarla.
  **Antes de borrar Jícaro, desmarca esa casilla.**
- Tus ajustes están en `%LOCALAPPDATA%\Jicaro`. El registro de fallas (`registro.txt`) no guarda nombres de archivos.

## Para otros programas (por ejemplo Atarraya)

```
Jicaro.exe --extraer "<archivo>" [--destino "<carpeta>" | --aqui] [--si-existe preguntar|reemplazar|saltar|renombrar]
           [--contrasena-stdin]
```

- Sin `--destino` ni `--aqui`, extrae en una carpeta nueva con el nombre del archivo, al lado de él.
- Muestra una ventana chica de progreso. Si todo sale bien se cierra sola; si algo falla, se queda con el resumen.
- Si se le pasa cualquier parte de un RAR en varias partes, extrae el juego completo. Si se abren varias ventanas
  para el mismo juego (por ejemplo, al elegir todas las partes y pulsar «Extraer aquí»), solo una extrae.
- Sin `--si-existe`, usa lo que la persona eligió en Ajustes.
- **Contraseña:** sin `--contrasena-stdin`, si hace falta, Jícaro la pide en una ventana (y otra vez si no sirve). Con
  `--contrasena-stdin`, la lee de la **entrada estándar**: la primera línea, en UTF-8 (sin el salto de línea, hasta
  4 KB, espera hasta 15 segundos). Así no se muestra ninguna pregunta de contraseña: si no sirve, termina con el
  código `7`. **La contraseña nunca va en los argumentos** (cualquier programa vería la lista de procesos): Jícaro no
  acepta `--contrasena=...` y termina con el código `1`.

  ```powershell
  # Ejemplo en PowerShell 7 (manda la contraseña en UTF-8)
  'mi contraseña' | & .\Jicaro.exe --extraer 'juego.part01.rar' --contrasena-stdin
  ```

Códigos de salida:

| Código | Qué pasó |
|---|---|
| `0` | Bien: salió todo. |
| `1` | Error (el archivo no existe o está dañado, la contraseña llegó en los argumentos o no en UTF-8...). |
| `2` | Cancelado, o detenido ante una posible bomba de descompresión. |
| `3` | Falta una parte. |
| `4` | **Hace falta una contraseña** y no se dio (o es un cifrado que Jícaro no abre). |
| `5` | Otra ventana de Jícaro ya está extrayendo ese archivo. |
| `6` | Salió solo una parte: algo se saltó por seguridad o no se pudo extraer. |
| `7` | **La contraseña no es correcta** (no sirvió para todo o para una parte). |

También: `Jicaro.exe --registrar-menu` y `Jicaro.exe --quitar-menu` ponen y quitan el clic derecho.

## ¿Algo falla?

[Reporta el problema aquí](https://github.com/Brizna-Soft/jicaro/issues/new/choose): cuéntanos qué pasó y qué tipo de
archivo era. Si puedes, pega el contenido de `%LOCALAPPDATA%\Jicaro\registro.txt`.

## Apoyar

Jícaro es y seguirá siendo gratis. Si te sirve, puedes apoyar a Brizna Soft con una
[donación por PayPal](https://paypal.me/BriznaSoft) o simplemente recomendarlo.

## Compilar desde el código

Necesitas Visual Studio 2022 (o sus Build Tools) con C++ y Git. Luego:

```
powershell -ExecutionPolicy Bypass -File compilar.ps1
powershell -ExecutionPolicy Bypass -File pruebas\probar.ps1
```

La primera vez baja vcpkg y compila wxWidgets y libarchive (con zlib, bzip2, liblzma y zstd). Después `traer-7zip.ps1`
baja 7-Zip 26.05 de su sitio oficial (el instalador, `7zr.exe` para abrirlo sin instalar nada, y el código fuente),
comprueba sus huellas SHA-256 y saca `7z.dll` y sus licencias. El programa queda en `programa\` (`Jicaro.exe` y
`7z.dll`). Las pruebas usan archivos de la suite de pruebas de libarchive (licencia BSD), unos RAR 4 cifrados en
6 partes de las pruebas de [SharpCompress](https://github.com/adamhathcock/sharpcompress) (licencia MIT) y otros que
arman ellas mismas, entre ellos archivos con trampas y una bomba chica.

Para probar una copia de Jícaro sin tocar los ajustes de la que tienes instalada, pon la variable
`JICARO_CARPETA_DATOS` con otra carpeta (una ruta completa): ahí guarda sus ajustes y su registro.

## Licencia y créditos

- Código de Jícaro: licencia MIT, © 2026 Brizna Soft. Hecho con ayuda de IA.
- Bibliotecas de código abierto que usa, con su licencia:
  - [libarchive](https://www.libarchive.org): BSD de 2 cláusulas (con partes bajo BSD de UC Regents, dominio público
    y CC0/OpenSSL/Apache 2.0, según su archivo COPYING).
  - [wxWidgets](https://www.wxwidgets.org): wxWindows Library Licence 3.1.
  - [zlib](https://zlib.net): licencia zlib. [bzip2](https://sourceware.org/bzip2/): licencia de bzip2.
    [XZ Utils / liblzma](https://tukaani.org/xz/): 0BSD. [Zstandard](https://github.com/facebook/zstd): BSD.
  - [nlohmann/json](https://github.com/nlohmann/json): MIT.
- [7-Zip](https://www.7-zip.org) de Igor Pavlov, solo su `7z.dll`, que va aparte y sin cambios junto a `Jicaro.exe`:
  GNU LGPL, con partes BSD de 3 y de 2 cláusulas, y la restricción de unRAR (su código no se puede usar para crear un
  programa que comprima en RAR; Jícaro no lo hace). Su código fuente está en
  [7-zip.org](https://www.7-zip.org/download.html) y va junto a cada versión de Jícaro (`7z2605-src.7z`).

  Los textos completos de esas licencias, y los de otras bibliotecas que trae la compilación, están en
  `AVISOS-DE-TERCEROS.txt`. Nombrarlas aquí no significa que sus autores respalden Jícaro.
- «WinRAR» y «RAR» se nombran solo para explicar con qué archivos trabaja Jícaro; no tiene relación con sus autores,
  ni con los de 7-Zip.
