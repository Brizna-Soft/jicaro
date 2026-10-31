#pragma once

// Utilidades sin wxWidgets: textos, rutas y archivos. Las usan el programa y las pruebas.

#include <cstdint>
#include <string>
#include <string_view>

namespace jicaro
{
    std::wstring Ancho(std::string_view utf8);
    std::wstring AnchoDesde(unsigned pagina, std::string_view texto);
    std::string Utf8(std::wstring_view texto);

    std::wstring Minusculas(std::wstring texto);
    bool IgualesSinMayusculas(std::wstring_view a, std::wstring_view b);
    bool EmpiezaSinMayusculas(std::wstring_view texto, std::wstring_view inicio);
    bool TerminaSinMayusculas(std::wstring_view texto, std::wstring_view fin);

    // Textos para la persona: «1.50 GB», «1,234»
    std::wstring TextoTamano(int64_t bytes);
    std::wstring TextoNumero(int64_t n);

    std::wstring RutaCompleta(const std::wstring& ruta);   // sin «.» ni «..», como la ve Windows
    std::wstring CarpetaDe(const std::wstring& ruta);
    std::wstring NombreDe(const std::wstring& ruta);
    std::wstring CarpetaDelPrograma();

    bool ExisteArchivo(const std::wstring& ruta);
    bool ExisteCarpeta(const std::wstring& ruta);
    int64_t TamanoArchivo(const std::wstring& ruta);       // -1 si no existe
    int64_t EspacioLibre(const std::wstring& carpeta);     // -1 si no se sabe
    bool CrearCarpetas(const std::wstring& ruta);          // crea también las carpetas de arriba que falten

    // Manda un archivo a la Papelera. Si el disco no tiene Papelera, Windows pregunta antes de borrarlo.
    bool MandarAPapelera(const std::wstring& ruta);
}

#ifdef JICARO_PRUEBAS
// Solo en las pruebas: «la Papelera» puede ser una carpeta de prueba, para no llenar la Papelera de la persona
#include <functional>
namespace jicaro::pruebas
{
    extern std::function<bool(const std::wstring&)> papelera;
    extern std::string ultimoError;   // el texto de libarchive de la última falla (en inglés; no se muestra)
}
#endif
