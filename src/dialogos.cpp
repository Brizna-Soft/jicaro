#include "dialogos.hpp"

#include <windows.h>

#include <wx/artprov.h>
#include <wx/settings.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/dirdlg.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>

#include "ayuda.hpp"
#include "comun.hpp"
#include "util.hpp"

namespace
{
    // «12/10/2026 14:05» (hora local), o vacío si no se sabe
    wxString FechaDeArchivo(const std::wstring& ruta)
    {
        WIN32_FILE_ATTRIBUTE_DATA d;
        if (!GetFileAttributesExW(ruta.c_str(), GetFileExInfoStandard, &d)) return L"";
        FILETIME local;
        SYSTEMTIME t;
        if (!FileTimeToLocalFileTime(&d.ftLastWriteTime, &local) || !FileTimeToSystemTime(&local, &t)) return L"";
        return wxString::Format(L"%02d/%02d/%04d %02d:%02d", t.wDay, t.wMonth, t.wYear, t.wHour, t.wMinute);
    }

    wxStaticText* Texto(wxWindow* padre, const wxString& texto, int ancho)
    {
        auto* t = new wxStaticText(padre, wxID_ANY, texto);
        t->Wrap(padre->FromDIP(ancho));
        return t;
    }

    wxStaticText* Nota(wxWindow* padre, const wxString& texto, int ancho)
    {
        auto* t = Texto(padre, texto, ancho);
        t->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        return t;
    }

    void BotonesEnEspanol(wxDialog& d, const wxString& aceptar, const wxString& cancelar)
    {
        if (auto* b = d.FindWindow(wxID_OK)) b->SetLabel(aceptar);
        if (auto* b = d.FindWindow(wxID_CANCEL)) b->SetLabel(cancelar);
    }
}

std::optional<std::wstring> dialogos::PedirContrasena(wxWindow* padre, const std::wstring& archivo, const std::wstring& entrada,
                                                      bool otraVez)
{
    wxDialog d(padre, wxID_ANY, L"Contraseña");
    auto* caja = new wxBoxSizer(wxVERTICAL);
    const int margen = d.FromDIP(14);
    // «entrada» vacía: la contraseña es para abrir el archivo entero (también la lista de lo que trae)
    wxString texto;
    if (otraVez)
        texto = entrada.empty() ? wxString(L"La contraseña no sirvió. Prueba otra:")
                                : wxString::Format(L"La contraseña no sirvió para «%s». Prueba otra:", jicaro::NombreDe(entrada));
    else
        texto = entrada.empty()
            ? wxString::Format(L"«%s» está protegido con contraseña, también la lista de lo que trae. Escríbela para abrirlo:",
                               jicaro::NombreDe(archivo))
            : wxString::Format(L"«%s» está protegido con contraseña. Escríbela para extraerlo:", jicaro::NombreDe(archivo));
    caja->Add(Texto(&d, texto, 380), 0, wxALL, margen);
    auto* oculta = new wxTextCtrl(&d, wxID_ANY, L"", wxDefaultPosition, wxSize(d.FromDIP(380), -1), wxTE_PASSWORD | wxTE_PROCESS_ENTER);
    auto* visible = new wxTextCtrl(&d, wxID_ANY, L"", wxDefaultPosition, wxSize(d.FromDIP(380), -1), wxTE_PROCESS_ENTER);
    visible->Hide();
    caja->Add(oculta, 0, wxLEFT | wxRIGHT | wxEXPAND, margen);
    caja->Add(visible, 0, wxLEFT | wxRIGHT | wxEXPAND, margen);
    auto* mostrar = new wxCheckBox(&d, wxID_ANY, L"Mostrar la contraseña");
    caja->Add(mostrar, 0, wxALL, margen);
    caja->Add(d.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, margen);
    BotonesEnEspanol(d, L"Aceptar", L"Cancelar");
    wxWindow* aceptar = d.FindWindow(wxID_OK);

    auto actual = [&]() { return mostrar->IsChecked() ? visible : oculta; };
    mostrar->Bind(wxEVT_CHECKBOX, [&](wxCommandEvent&) {
        bool ver = mostrar->IsChecked();
        if (ver) visible->ChangeValue(oculta->GetValue());
        else oculta->ChangeValue(visible->GetValue());
        visible->Show(ver);
        oculta->Show(!ver);
        actual()->SetFocus();
        actual()->SetInsertionPointEnd();
        d.Layout();
    });
    auto alEscribir = [&](wxCommandEvent&) {
        if (aceptar) aceptar->Enable(!actual()->GetValue().empty());
    };
    auto alPulsarEnter = [&](wxCommandEvent&) {
        if (!actual()->GetValue().empty()) d.EndModal(wxID_OK);
    };
    oculta->Bind(wxEVT_TEXT, alEscribir);
    visible->Bind(wxEVT_TEXT, alEscribir);
    oculta->Bind(wxEVT_TEXT_ENTER, alPulsarEnter);
    visible->Bind(wxEVT_TEXT_ENTER, alPulsarEnter);
    if (aceptar) aceptar->Enable(false);

    d.SetSizerAndFit(caja);
    d.CentreOnParent();
    oculta->SetFocus();
    if (d.ShowModal() != wxID_OK) return std::nullopt;
    wxString valor = actual()->GetValue();
    if (valor.empty()) return std::nullopt;
    return std::wstring(valor.wc_str());
}

jicaro::Decision dialogos::PreguntarSiExiste(wxWindow* padre, const std::wstring& ruta, bool& paraTodos)
{
    wxDialog d(padre, wxID_ANY, L"El archivo ya existe");
    auto* caja = new wxBoxSizer(wxVERTICAL);
    const int margen = d.FromDIP(14);
    caja->Add(Texto(&d, wxString::Format(L"Ya hay un archivo llamado «%s» en «%s».", jicaro::NombreDe(ruta), jicaro::CarpetaDe(ruta)), 460),
              0, wxALL, margen);
    wxString fecha = FechaDeArchivo(ruta);
    wxString detalle = L"El que ya está pesa " + jicaro::TextoTamano(jicaro::TamanoArchivo(ruta));
    if (!fecha.empty()) detalle += L" y es del " + fecha;
    caja->Add(Texto(&d, detalle + L".", 460), 0, wxLEFT | wxRIGHT, margen);
    caja->Add(Nota(&d, L"«Reemplazar» manda el que ya está a la Papelera.", 460), 0, wxLEFT | wxRIGHT | wxTOP, margen);
    auto* todos = new wxCheckBox(&d, wxID_ANY, L"Hacer lo mismo con los demás que ya existan");
    caja->Add(todos, 0, wxALL, margen);

    auto* fila = new wxBoxSizer(wxHORIZONTAL);
    struct { int id; const wchar_t* texto; } botones[] = {
        { wxID_YES, L"Reemplazar" }, { wxID_NO, L"Guardar los dos" }, { wxID_IGNORE, L"Saltar" }, { wxID_CANCEL, L"Cancelar todo" },
    };
    for (auto& b : botones)
    {
        auto* boton = new wxButton(&d, b.id, b.texto);
        boton->Bind(wxEVT_BUTTON, [&d, id = b.id](wxCommandEvent&) { d.EndModal(id); });
        fila->Add(boton, 0, wxLEFT, d.FromDIP(6));
    }
    caja->Add(fila, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM, margen);
    d.SetSizerAndFit(caja);
    d.SetEscapeId(wxID_CANCEL);
    if (auto* b = d.FindWindow(wxID_IGNORE)) b->SetFocus();
    d.CentreOnParent();
    int r = d.ShowModal();
    paraTodos = todos->IsChecked();
    switch (r)
    {
    case wxID_YES: return jicaro::Decision::Reemplazar;
    case wxID_NO: return jicaro::Decision::Renombrar;
    case wxID_IGNORE: return jicaro::Decision::Saltar;
    default: return jicaro::Decision::Cancelar;
    }
}

bool dialogos::PreguntarBomba(wxWindow* padre, const std::wstring& motivo)
{
    wxMessageDialog d(padre,
                      motivo + L"\n\nEste archivo es mucho más grande de lo normal al abrirlo. Puede ser un archivo trampa "
                               L"hecho para llenar el disco. Si no esperabas algo tan grande, pulsa «Detener».\n\n"
                               L"¿Seguir extrayendo?",
                      L"¿Seguir extrayendo?", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
    d.SetYesNoLabels(L"Seguir", L"Detener");
    return d.ShowModal() == wxID_YES;
}

bool dialogos::ElegirDestino(wxWindow* padre, const wxString& titulo, std::wstring& carpeta, jicaro::SiExiste& siExiste,
                             bool& abrirAlTerminar)
{
    wxDialog d(padre, wxID_ANY, titulo);
    auto* caja = new wxBoxSizer(wxVERTICAL);
    const int margen = d.FromDIP(14);
    caja->Add(new wxStaticText(&d, wxID_ANY, L"Extraer en esta carpeta (si no existe, se crea):"), 0, wxLEFT | wxRIGHT | wxTOP, margen);
    auto* fila = new wxBoxSizer(wxHORIZONTAL);
    auto* ruta = new wxTextCtrl(&d, wxID_ANY, carpeta, wxDefaultPosition, wxSize(d.FromDIP(420), -1));
    auto* elegir = new wxButton(&d, wxID_ANY, L"Elegir...");
    fila->Add(ruta, 1, wxALIGN_CENTER_VERTICAL);
    fila->Add(elegir, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, d.FromDIP(6));
    caja->Add(fila, 0, wxEXPAND | wxALL, margen);

    auto* filaExiste = new wxBoxSizer(wxHORIZONTAL);
    filaExiste->Add(new wxStaticText(&d, wxID_ANY, L"Si un archivo ya existe:"), 0, wxALIGN_CENTER_VERTICAL);
    auto* existe = new wxChoice(&d, wxID_ANY);
    for (int i = 0; i < jicaro::CantidadSiExiste(); i++) existe->Append(jicaro::TextosSiExiste()[i]);
    existe->SetSelection((int)siExiste);
    filaExiste->Add(existe, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, d.FromDIP(8));
    caja->Add(filaExiste, 0, wxLEFT | wxRIGHT, margen);
    auto* abrir = new wxCheckBox(&d, wxID_ANY, L"Abrir la carpeta al terminar");
    abrir->SetValue(abrirAlTerminar);
    caja->Add(abrir, 0, wxALL, margen);
    caja->Add(d.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, margen);
    BotonesEnEspanol(d, L"Extraer", L"Cancelar");

    elegir->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        std::wstring inicio = ruta->GetValue().ToStdWstring();
        while (!inicio.empty() && !jicaro::ExisteCarpeta(inicio)) inicio = jicaro::CarpetaDe(inicio);
        wxDirDialog dd(&d, L"Elige dónde extraer", inicio, wxDD_DEFAULT_STYLE | wxDD_NEW_DIR_BUTTON);
        if (dd.ShowModal() == wxID_OK) ruta->ChangeValue(dd.GetPath());
    });
    d.SetSizerAndFit(caja);
    d.CentreOnParent();
    if (auto* b = d.FindWindow(wxID_OK)) b->SetFocus();
    for (;;)
    {
        if (d.ShowModal() != wxID_OK) return false;
        std::wstring elegida = ruta->GetValue().Strip(wxString::both).ToStdWstring();
        bool conUnidad = elegida.size() >= 3 && elegida[1] == L':' && (elegida[2] == L'\\' || elegida[2] == L'/');
        bool deRed = false;
        if (elegida.rfind(L"\\\\", 0) == 0 && elegida.size() > 2 && elegida[2] != L'?' && elegida[2] != L'.')
        {
            // \\servidor\compartida[\carpeta...]: servidor y carpeta compartida no vacíos
            size_t barra = elegida.find(L'\\', 2);
            deRed = barra != std::wstring::npos && barra > 2 && barra + 1 < elegida.size() && elegida[barra + 1] != L'\\';
        }
        if (conUnidad || deRed)
        {
            carpeta = jicaro::RutaCompleta(elegida);
            break;
        }
        ayuda::Aviso(L"Escribe la ruta completa de una carpeta, por ejemplo C:\\Users\\Nombre\\Descargas\\juego, o pulsa «Elegir...».",
                     titulo, wxOK | wxICON_WARNING, &d);
    }
    siExiste = (jicaro::SiExiste)existe->GetSelection();
    abrirAlTerminar = abrir->IsChecked();
    return true;
}

bool dialogos::MostrarAjustes(wxWindow* padre, jicaro::Ajustes& a)
{
    wxDialog d(padre, wxID_ANY, L"Ajustes");
    auto* caja = new wxBoxSizer(wxVERTICAL);
    const int margen = d.FromDIP(14);
    auto* menu = new wxCheckBox(&d, wxID_ANY, L"Agregar «Extraer aquí» y «Extraer en una carpeta con su nombre» al clic derecho");
    menu->SetValue(a.menuContextual);
    caja->Add(menu, 0, wxLEFT | wxRIGHT | wxTOP, margen);
    caja->Add(Nota(&d,
                   L"En Windows 11 salen en «Mostrar más opciones». Solo se guarda en tu cuenta de Windows (no hace falta ser "
                   L"administrador) y se quita al desmarcar esta casilla. Si mueves la carpeta de Jícaro, abre Jícaro una vez "
                   L"para que el clic derecho siga funcionando.",
                   470),
              0, wxLEFT | wxRIGHT | wxTOP, d.FromDIP(6) + margen / 2);

    auto* filaExiste = new wxBoxSizer(wxHORIZONTAL);
    filaExiste->Add(new wxStaticText(&d, wxID_ANY, L"Si un archivo ya existe:"), 0, wxALIGN_CENTER_VERTICAL);
    auto* existe = new wxChoice(&d, wxID_ANY);
    for (int i = 0; i < jicaro::CantidadSiExiste(); i++) existe->Append(jicaro::TextosSiExiste()[i]);
    existe->SetSelection((int)a.siExiste);
    filaExiste->Add(existe, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, d.FromDIP(8));
    caja->Add(filaExiste, 0, wxLEFT | wxRIGHT | wxTOP, margen);
    caja->Add(Nota(&d, L"Vale para la ventana, para el clic derecho y para cuando Atarraya le pide a Jícaro que extraiga.", 470),
              0, wxLEFT | wxRIGHT | wxTOP, d.FromDIP(6));

    auto* abrir = new wxCheckBox(&d, wxID_ANY, L"Abrir la carpeta al terminar de extraer (en la ventana de Jícaro)");
    abrir->SetValue(a.abrirCarpeta);
    caja->Add(abrir, 0, wxALL, margen);
    caja->Add(d.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, margen);
    BotonesEnEspanol(d, L"Guardar", L"Cancelar");
    d.SetSizerAndFit(caja);
    d.CentreOnParent();
    if (d.ShowModal() != wxID_OK) return false;
    a.menuContextual = menu->IsChecked();
    a.siExiste = (jicaro::SiExiste)existe->GetSelection();
    a.abrirCarpeta = abrir->IsChecked();
    return true;
}

wxString dialogos::TextoResumen(const jicaro::Resumen& r, const std::wstring& destino)
{
    using jicaro::TextoNumero;
    wxString t;
    if (r.estado == jicaro::Estado::Bien)
    {
        if (r.cifradas > 0 || r.clavesMalas > 0 || r.sinClave > 0) t = r.mensaje;
        else if (r.archivos == 0 && r.saltados > 0)
            t = wxString::Format(L"No se extrajo nada nuevo: los %s archivos ya estaban en «%s».", TextoNumero(r.saltados), destino);
        else if (r.archivos == 1) t = wxString::Format(L"Se extrajo 1 archivo en «%s».", destino);
        else t = wxString::Format(L"Se extrajeron %s archivos en «%s».", TextoNumero(r.archivos), destino);
        if (r.saltados > 0 && r.archivos > 0)
            t += wxString::Format(L"\n%s ya existían y se dejaron como estaban.", TextoNumero(r.saltados));
    }
    else
    {
        t = r.mensaje;
        if (r.archivos > 0) t += wxString::Format(L"\nAlcanzaron a salir %s archivos.", TextoNumero(r.archivos));
        if (r.estado == jicaro::Estado::Error) t += wxString(L"\n") + ayuda::SiVuelveAPasar;
    }
    if (r.zona && r.marcados > 0)
        t += L"\n\nLo extraído quedó marcado como bajado de internet, igual que el archivo comprimido: Windows te avisará "
             L"antes de abrir programas que vienen de ahí.";
    if (r.quitadosPorWindows > 0)
        t += wxString::Format(L"\nEl antivirus de Windows quitó %s archivos.", TextoNumero(r.quitadosPorWindows));
    if (r.sinMarca > 0)
        t += wxString::Format(L"\nA %s archivos no se les pudo poner la marca de internet (puede ser el disco, un permiso o "
                              L"algo de Windows). Ten cuidado al abrir programas de ahí.",
                              TextoNumero(r.sinMarca));
    if (r.quitoAMedias) t += L"\nSe quitó el archivo que quedó a medias.";
    if (!r.aMedias.empty())
        t += L"\nNo se pudo quitar un archivo que quedó a medias: «" + wxString(r.aMedias) + L"». Está incompleto; puedes borrarlo.";
    // Encabezado de la lista de lo que no salió: «Se saltaron 2 por seguridad, 1 no se pudo extraer y 3 no salieron...»
    std::vector<wxString> motivos;
    if (r.rechazados > 0) motivos.push_back(wxString::Format(L"se saltaron %s por seguridad", TextoNumero(r.rechazados)));
    if (r.fallidos > 0) motivos.push_back(wxString::Format(L"%s no se pudieron extraer", TextoNumero(r.fallidos)));
    if (r.clavesMalas > 0)
        motivos.push_back(wxString::Format(L"%s no salieron porque la contraseña no sirvió", TextoNumero(r.clavesMalas)));
    if (r.sinClave > 0) motivos.push_back(wxString::Format(L"%s tienen contraseña y no se dio", TextoNumero(r.sinClave)));
    if (!motivos.empty())
    {
        wxString lista;
        for (size_t i = 0; i < motivos.size(); i++)
            lista += (i == 0 ? L"" : (i + 1 == motivos.size() ? L" y " : L", ")) + motivos[i];
        t += L"\n\n" + lista.Left(1).Upper() + lista.Mid(1) + L":";
    }
    return t;
}

wxString dialogos::TextoOmitidas(const jicaro::Resumen& r)
{
    wxString t;
    size_t n = 0;
    for (auto& o : r.omitidas)
    {
        if (++n > 500)
        {
            t += wxString::Format(L"... y %s más.\n", jicaro::TextoNumero((int64_t)(r.omitidas.size() - 500)));
            break;
        }
        t += L"«" + o.ruta + L"»: " + o.motivo + L"\n";
    }
    return t;
}

void dialogos::MostrarResumen(wxWindow* padre, const jicaro::Resumen& r, const std::wstring& destino)
{
    bool bien = r.estado == jicaro::Estado::Bien;
    wxDialog d(padre, wxID_ANY, bien ? L"Listo" : L"No se pudo terminar", wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* caja = new wxBoxSizer(wxVERTICAL);
    const int margen = d.FromDIP(14);
    auto* fila = new wxBoxSizer(wxHORIZONTAL);
    auto icono = wxArtProvider::GetBitmapBundle(r.Limpio() ? wxART_INFORMATION : wxART_WARNING, wxART_MESSAGE_BOX);
    fila->Add(new wxStaticBitmap(&d, wxID_ANY, icono), 0, wxRIGHT, margen);
    fila->Add(Texto(&d, TextoResumen(r, destino), 460), 1);
    caja->Add(fila, 0, wxALL | wxEXPAND, margen);
    if (!r.omitidas.empty())
    {
        auto* lista = new wxTextCtrl(&d, wxID_ANY, TextoOmitidas(r), wxDefaultPosition, wxSize(d.FromDIP(520), d.FromDIP(160)),
                                     wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
        caja->Add(lista, 1, wxEXPAND | wxLEFT | wxRIGHT, margen);
    }
    auto* botones = new wxBoxSizer(wxHORIZONTAL);
    botones->AddStretchSpacer();
    if (r.archivos > 0 && jicaro::ExisteCarpeta(destino))
    {
        auto* abrir = new wxButton(&d, wxID_ANY, L"Abrir la carpeta");
        abrir->Bind(wxEVT_BUTTON, [&d, destino](wxCommandEvent&) {
            jicaro::AbrirCarpeta(destino);
            d.EndModal(wxID_OK);
        });
        botones->Add(abrir, 0, wxRIGHT, d.FromDIP(6));
    }
    auto* cerrar = new wxButton(&d, wxID_CANCEL, L"Cerrar");
    botones->Add(cerrar);
    caja->Add(botones, 0, wxEXPAND | wxALL, margen);
    d.SetSizerAndFit(caja);
    d.SetEscapeId(wxID_CANCEL);
    cerrar->SetFocus();
    d.CentreOnParent();
    d.ShowModal();
}
