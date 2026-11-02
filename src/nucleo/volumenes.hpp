#pragma once

// Archivos en varias partes: «nombre.part01.rar» ... «nombre.part16.rar», el estilo viejo «nombre.rar» + «nombre.r00»,
// «nombre.r01»..., los ZIP en partes «nombre.z01», «nombre.z02»... + «nombre.zip», y los cortados en trozos
// «nombre.7z.001», «nombre.7z.002»...

#include <string>
#include <vector>

namespace jicaro::volumenes
{
    enum class Tipo { Uno, RarNuevo, RarViejo, Numerado, ZipPartes };

    struct Juego
    {
        Tipo tipo = Tipo::Uno;
        std::vector<std::wstring> partes;   // rutas completas en orden; la primera abre el juego
        std::vector<std::wstring> faltan;   // nombres de las partes que faltan antes de la última encontrada
        size_t seguidas = 0;                // cuántas partes hay desde la primera sin ningún hueco
        std::wstring carpeta;               // donde están las partes
        std::wstring nombreBase;            // para «Extraer en una carpeta con su nombre»
        std::wstring nombreSuelto;          // para un .gz, .bz2, .xz o .zst de un solo archivo: «datos.txt.gz» → «datos.txt»

        bool Varias() const { return tipo != Tipo::Uno; }
        bool Completo() const { return faltan.empty() && !partes.empty(); }
    };

    // Busca las demás partes del juego al que pertenece «ruta» (cualquiera de sus partes)
    Juego Buscar(const std::wstring& ruta);

    // «película.tar.gz» → «película», «datos.zip» → «datos»
    std::wstring NombreSinExtension(const std::wstring& nombre);
}
