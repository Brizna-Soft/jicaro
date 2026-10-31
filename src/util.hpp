#pragma once

// Utilidades del programa (con o sin ventana, pero no las usan las pruebas del núcleo).

#include <string>

namespace jicaro
{
    std::wstring CarpetaDatos();   // %LOCALAPPDATA%\Jicaro (la crea si no existe)

    // Escribe a un archivo temporal y lo cambia por el bueno: nunca deja el archivo a medias
    bool EscribirSeguro(const std::wstring& ruta, const std::string& contenido);
    bool LeerArchivo(const std::wstring& ruta, std::string& contenido);

    // Registro corto para ayudar a encontrar fallas (%LOCALAPPDATA%\Jicaro\registro.txt).
    // No guarda nombres de archivos ni carpetas: solo qué pasó.
    void Registrar(const std::string& texto);

    // Abre una carpeta en el Explorador
    void AbrirCarpeta(const std::wstring& carpeta);
}
