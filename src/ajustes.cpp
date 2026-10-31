#include "ajustes.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "comun.hpp"
#include "util.hpp"

namespace
{
    std::wstring Archivo() { return jicaro::CarpetaDatos() + L"\\ajustes.json"; }

    const wchar_t* const textos[] = {
        L"Preguntar",
        L"Reemplazar (lo anterior va a la Papelera)",
        L"Saltarlo",
        L"Guardar los dos (el nuevo con otro nombre)",
    };
}

const wchar_t* const* jicaro::TextosSiExiste() { return textos; }
int jicaro::CantidadSiExiste() { return (int)(sizeof(textos) / sizeof(textos[0])); }

void jicaro::Ajustes::Cargar()
{
    std::string texto;
    if (!LeerArchivo(Archivo(), texto)) return;
    try
    {
        auto j = nlohmann::json::parse(texto);
        menuContextual = j.value("menuContextual", false);
        siExiste = (SiExiste)std::clamp(j.value("siExiste", 0), 0, CantidadSiExiste() - 1);
        abrirCarpeta = j.value("abrirCarpeta", true);
        x = j.value("x", -1);
        y = j.value("y", -1);
        ancho = j.value("ancho", 0);
        alto = j.value("alto", 0);
        maximizada = j.value("maximizada", false);
    }
    catch (...)
    {
    }
}

void jicaro::Ajustes::Guardar() const
{
    nlohmann::json j = {
        { "menuContextual", menuContextual }, { "siExiste", (int)siExiste }, { "abrirCarpeta", abrirCarpeta },
        { "x", x }, { "y", y }, { "ancho", ancho }, { "alto", alto }, { "maximizada", maximizada },
    };
    EscribirSeguro(Archivo(), j.dump(1));
}
