#pragma once

// Ventanas de diálogo de Jícaro, todas con los botones en español.

#include <wx/window.h>

#include <optional>
#include <string>

#include "ajustes.hpp"
#include "extractor.hpp"

namespace dialogos
{
    // La contraseña de un ZIP. «otraVez»: la anterior no sirvió. Vacío si la persona cancela.
    std::optional<std::wstring> PedirContrasena(wxWindow* padre, const std::wstring& archivo, const std::wstring& entrada,
                                                bool otraVez);

    // Ya existe un archivo con ese nombre en la carpeta de destino
    jicaro::Decision PreguntarSiExiste(wxWindow* padre, const std::wstring& ruta, bool& paraTodos);

    // Posible bomba de descompresión. true: seguir.
    bool PreguntarBomba(wxWindow* padre, const std::wstring& motivo);

    // Dónde extraer y qué hacer si algo ya existe. false si la persona cancela.
    bool ElegirDestino(wxWindow* padre, const wxString& titulo, std::wstring& carpeta, jicaro::SiExiste& siExiste,
                       bool& abrirAlTerminar);

    // Ajustes del programa. true si la persona pulsó «Guardar».
    bool MostrarAjustes(wxWindow* padre, jicaro::Ajustes& ajustes);

    // El resultado de una extracción, para la persona
    wxString TextoResumen(const jicaro::Resumen& r, const std::wstring& destino);
    wxString TextoOmitidas(const jicaro::Resumen& r);
    void MostrarResumen(wxWindow* padre, const jicaro::Resumen& r, const std::wstring& destino);
}
