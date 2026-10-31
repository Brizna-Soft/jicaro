#pragma once

#include <string>

#include "extractor.hpp"

namespace jicaro
{
    // Se guardan en %LOCALAPPDATA%\Jicaro\ajustes.json
    struct Ajustes
    {
        bool menuContextual = false;          // «Extraer aquí» en el clic derecho (solo si la persona lo marca)
        SiExiste siExiste = SiExiste::Preguntar;
        bool abrirCarpeta = true;             // abrir la carpeta al terminar (en la ventana)

        // Ventana
        int x = -1, y = -1, ancho = 0, alto = 0;
        bool maximizada = false;

        void Cargar();
        void Guardar() const;
    };

    // Textos para elegir qué hacer si un archivo ya existe (en el mismo orden que SiExiste)
    const wchar_t* const* TextosSiExiste();
    int CantidadSiExiste();
}
