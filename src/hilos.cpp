#include "hilos.hpp"

#include <wx/app.h>

#include <mutex>

namespace
{
    std::mutex m;
    bool abierta = true;
}

bool jicaro::hilos::Enviar(std::function<void()> f)
{
    std::lock_guard g(m);
    if (!abierta || !wxTheApp) return false;
    wxTheApp->CallAfter(std::move(f));
    return true;
}

void jicaro::hilos::Cerrar()
{
    std::lock_guard g(m);
    abierta = false;
}
