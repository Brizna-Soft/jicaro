#pragma once

// «Extraer aquí» y «Extraer en una carpeta con su nombre» en el menú clásico del clic derecho (en Windows 11 está en
// «Mostrar más opciones»). Se escribe solo en la cuenta de la persona (HKEY_CURRENT_USER\Software\Classes), sin ser
// administrador, y solo cuando la persona lo marca en Ajustes. Al desmarcarlo se quita todo lo que se puso.

#include <string>

namespace jicaro::menu
{
    bool Registrar(const std::wstring& exe);
    bool Quitar();
    bool EstaRegistrado();
    // ¿El menú registrado abre este mismo .exe? (si la persona movió la carpeta del programa, hay que registrarlo de nuevo)
    bool ApuntaA(const std::wstring& exe);
}
