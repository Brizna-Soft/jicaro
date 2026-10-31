#pragma once

// Menú «Ayuda», igual en todos los programas de Brizna Soft: reportar un problema, buscar actualización,
// donar, página del estudio y «Acerca de».

#include <wx/frame.h>
#include <wx/menu.h>
#include <wx/string.h>

#include <string>

namespace ayuda
{
    // Crea el menú y conecta sus opciones a la ventana. «descripcion» va en «Acerca de».
    wxMenu* CrearMenu(wxFrame* ventana, const wxString& descripcion);

    // Como wxMessageBox, pero con los botones en español: wx los deja en inglés
    int Aviso(const wxString& texto, const wxString& titulo, long estilo = wxOK | wxICON_INFORMATION, wxWindow* padre = nullptr);

    void AbrirEnlace(const wxString& url);
    void ReportarProblema();
    void BuscarActualizacion(wxWindow* padre);
    void MostrarAcercaDe(wxWindow* padre, const wxString& descripcion);

    // Compara versiones «1.2.3»: negativo si a < b, 0 si son iguales, positivo si a > b.
    int CompararVersiones(const std::string& a, const std::string& b);
}
