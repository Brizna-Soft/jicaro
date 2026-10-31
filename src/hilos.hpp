#pragma once

// Pasar trabajo de un hilo cualquiera al hilo de la ventana, sin riesgos al cerrar el programa.
//
// Los hilos de trabajo no deben llamar a wxTheApp->CallAfter mientras la aplicación se destruye. En su lugar,
// Enviar(f) pone f en la cola de la ventana solo si la aplicación sigue viva, y dice si lo hizo.

#include <functional>

namespace jicaro::hilos
{
    bool Enviar(std::function<void()> f);
    // Al cerrar: desde aquí no se manda nada más al hilo de la ventana
    void Cerrar();
}
