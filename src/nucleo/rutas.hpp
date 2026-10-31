#pragma once

// Comprobación de las rutas que traen los archivos comprimidos. Es la defensa propia de Jícaro, aparte de las banderas
// SECURE de libarchive: nada se escribe fuera de la carpeta de destino.

#include <string>
#include <string_view>

namespace jicaro::rutas
{
    enum class Motivo
    {
        Ninguno,
        Vacia,          // sin nombre
        Absoluta,       // C:\..., \..., \\servidor\..., /...
        SubeDeCarpeta,  // tiene «..»
        MuyLarga,
    };

    struct Limpia
    {
        bool ok = false;
        std::wstring relativa;   // con «\», sin «.» ni «..» y sin caracteres que Windows no acepta
        Motivo motivo = Motivo::Ninguno;
    };

    // Convierte la ruta de una entrada del archivo en una ruta relativa segura, o la rechaza (rutas absolutas,
    // con «..» en cualquier parte o vacías). Cambia por «_» los caracteres que Windows no acepta (por ejemplo «:»,
    // que en NTFS escribiría en un flujo oculto de otro archivo) y pone «_» delante de nombres reservados como CON.
    Limpia Limpiar(std::wstring_view ruta);

    // carpeta + «\» + relativa
    std::wstring Unir(const std::wstring& carpeta, const std::wstring& relativa);

    // ¿«ruta» queda dentro de «carpeta»? Compara las rutas completas ya normalizadas por Windows (sin tocar el disco).
    bool DentroDe(const std::wstring& carpeta, const std::wstring& ruta);

    // Revisa en el disco lo que ya existe entre «carpeta» y «carpeta\relativa»: ningún tramo puede ser un enlace
    // simbólico ni una unión (junction), porque desviaría lo extraído a otro lugar. «donde» dice cuál falló.
    bool SinDesvios(const std::wstring& carpeta, const std::wstring& relativa, std::wstring* donde = nullptr);

    // ¿La ruta misma (su último tramo) es un enlace simbólico o una unión?
    bool EsDesvio(const std::wstring& ruta);

    // Carpeta nueva «carpeta\nombre» para «Extraer en una carpeta con su nombre»: si ya hay un archivo, un enlace o una
    // unión con ese nombre, busca otro nombre libre («nombre (2)»)
    std::wstring CarpetaNuevaSegura(const std::wstring& carpeta, const std::wstring& nombre);

    // La ruta con «\\?\» delante cuando hace falta para pasar de 260 caracteres
    std::wstring Larga(const std::wstring& ruta);

    // «nombre (2).ext», «nombre (3).ext»... el primero que no exista
    std::wstring NombreLibre(const std::wstring& ruta);

    // Para la persona: por qué se saltó una entrada
    std::wstring TextoMotivo(Motivo motivo);
}
