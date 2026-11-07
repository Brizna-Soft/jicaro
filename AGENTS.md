# Instrucciones para agentes de IA (Codex, Claude y otros)

Este repositorio es de **Brizna Soft**, un estudio pequeño de Honduras que hace programas gratis, sin
publicidad y sin recoger datos. Estas reglas valen para cualquier agente que lea, revise o cambie el código.

## Reglas que no se rompen

- **Idioma:** todo lo que ve la persona va en español sencillo: ventanas, avisos, README y notas. Se trata
  de «tú». Sin jerga técnica en los avisos; si hace falta un código, va al final («Para el técnico: …»).
- **Privacidad:** el programa no se conecta a internet salvo para hacer su trabajo o cuando la persona lo
  pide (por ejemplo «Buscar actualización»). Sin telemetría, sin publicidad y sin datos personales en los
  registros ni en los reportes (nada de usuario, nombre de la PC, rutas completas ni direcciones completas).
- **Seguridad:**
  - Nada se borra sin una confirmación clara, y lo que se borra va a la Papelera siempre que se pueda.
  - Nada pide ser administrador ni cambia la configuración de Windows, salvo las excepciones de la guía,
    que son opcionales y solo pasan si la persona pulsa un botón.
  - Nada se baja ni se instala solo.
- **Autor:** los commits van como «Brizna Soft»
  (`349058083+Brizna-Soft@users.noreply.github.com`), sin líneas que nombren a una IA ni `Co-Authored-By`.
  Se puede decir «Hecho con ayuda de IA».
- **Publicar:** nadie publica una versión (etiqueta `v*`) ni sube a la rama principal sin que el dueño lo
  apruebe. Lo que borra archivos o toca datos personales pasa antes por una revisión de solo lectura.

## Menú Ayuda común

`src/ayuda.cpp` y `src/ayuda.hpp` son **iguales en todos los programas**. No se cambian aquí: la copia
maestra está en `Documents\Brizna-Soft\programas\comun` y se copia con `comun\copiar.ps1`.

## Revisiones de solo lectura

Cuando se pide una revisión sin cambiar archivos:
- No cambies nada, ni siquiera para probar.
- Responde en español sencillo y empieza con **APROBADO** o **FALTA**.
- Para cada falla: gravedad (alta, media o baja), archivo, línea y qué le pasaría a la persona.

## Más detalles

Las reglas completas, la estructura de cada programa y cómo compilar están en
`Documents\Brizna-Soft\programas\GUIA-PROGRAMAS.md` (en la PC del estudio) y en el README de este
repositorio.

## Este programa: Jícaro

Descompresor (libarchive y, para RAR y 7z, `7z.dll` de 7-Zip, LGPL).

- Compilar: `powershell -ExecutionPolicy Bypass -File compilar.ps1`.
- Probar: `ctest --test-dir build -C Release --output-on-failure` (llama a `pruebas/probar.ps1`).
- El clic derecho «Extraer aquí» es opcional (Ajustes) y se quita con `Jicaro.exe --quitar-menu`.
- Nunca crea enlaces ni flujos alternos, nunca sale de la carpeta de destino y lo que reemplaza va a la Papelera.

Antes de compilar en la PC del estudio, poner en la misma consola `VCPKG_ROOT`, `VCPKG_DEFAULT_BINARY_CACHE` y
`VCPKG_DOWNLOADS` como dice la guía. En GitHub Actions lo hace `compilar.yml`.
