#pragma once

// Menú «Ayuda» común a todos los programas de Brizna Soft.
//
// ESTE ARCHIVO ES IGUAL EN TODOS LOS PROGRAMAS. La copia maestra está en Documents\Brizna-Soft\programas\comun
// y se copia a cada programa con comun\copiar.ps1. Si hay que cambiar algo, se cambia allá y se copia a todos.
//
// Usa de version.h: APP_NOMBRE, APP_VERSION, APP_VERSION_TEXTO, APP_REPO y, si el nombre lleva acentos,
// APP_NOMBRE_ASCII.

#include <wx/frame.h>
#include <wx/menu.h>
#include <wx/string.h>

#include <string>

namespace ayuda
{
    struct Opciones
    {
        wxString descripcion;            // qué hace el programa, en una o dos frases (sale en «Acerca de»)
        wxString creditos;               // opcional: bibliotecas con aviso propio (sale en «Acerca de»)
        std::wstring carpetaDeRegistros; // opcional: carpeta donde el programa escribe registro.txt
        bool conF1 = true;               // false si el programa deja usar F1 para otra cosa (Chachalaca)
    };

    // Agrega el menú «Ayuda» al final de la barra y conecta sus opciones a la ventana:
    // Cómo se usa (F1), Reportar un problema, Abrir la carpeta de registros (si hay), Buscar actualización,
    // Donar, Página de Brizna Soft y Acerca de.
    void AgregarMenu(wxMenuBar* barra, wxFrame* ventana, const Opciones& opciones);

    // Como wxMessageBox, pero con los botones en español: wx los deja en inglés
    int Aviso(const wxString& texto, const wxString& titulo, long estilo = wxOK | wxICON_INFORMATION, wxWindow* padre = nullptr);

    // Frase para el final de los avisos de error
    extern const wchar_t* const SiVuelveAPasar;

    void AbrirEnlace(const wxString& url);     // solo direcciones https://
    void ComoSeUsa();                          // la guía corta del programa en la página de Brizna Soft
    void ReportarProblema();                   // el formulario de fallas, con la versión ya puesta
    void AbrirCarpetaDeRegistros(const std::wstring& carpeta, wxWindow* padre);
    void BuscarActualizacion(wxWindow* padre); // solo al pulsar
    void MostrarAcercaDe(wxWindow* padre, const Opciones& opciones);

    // Versión del programa y de Windows, para pegar en un reporte. Nada personal.
    wxString DatosParaReporte();

    // Compara versiones «1.2.3»: negativo si a < b, 0 si son iguales, positivo si a > b.
    int CompararVersiones(const std::string& a, const std::string& b);

    // Llamar en wxApp::OnExit: desde ahí «Buscar actualización» ya no manda nada a la ventana.
    void Cerrar();
}
