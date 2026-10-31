#include "ayuda.hpp"

#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

#include <wx/app.h>
#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/hyperlink.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <nlohmann/json.hpp>

#include <sstream>
#include <thread>
#include <tuple>

#include "comun.hpp"
#include "hilos.hpp"
#include "version.h"

namespace
{
    const wchar_t* PaginaEstudio = L"https://brizna-soft.github.io";
    const wchar_t* PaginaDonar = L"https://paypal.me/BriznaSoft";

    wxString Repo() { return wxString::FromUTF8(APP_REPO); }
    wxString Nombre() { return wxString::FromUTF8(APP_NOMBRE); }

    // GET sencillo con WinHTTP. Devuelve el cuerpo, o vacío si algo falla.
    std::string Descargar(const std::wstring& host, const std::wstring& ruta)
    {
        std::string cuerpo;
        std::wstring agente = std::wstring(L"" APP_NOMBRE_ASCII L"/") + jicaro::Ancho(APP_VERSION);
        HINTERNET sesion = WinHttpOpen(agente.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS, 0);
        if (!sesion) return cuerpo;
        WinHttpSetTimeouts(sesion, 10000, 10000, 15000, 15000);
        HINTERNET conexion = WinHttpConnect(sesion, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        HINTERNET pedido = conexion ? WinHttpOpenRequest(conexion, L"GET", ruta.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                    : nullptr;
        const wchar_t* cabeceras = L"Accept: application/vnd.github+json\r\n";
        if (pedido && WinHttpSendRequest(pedido, cabeceras, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            && WinHttpReceiveResponse(pedido, nullptr))
        {
            DWORD estado = 0, largo = sizeof(estado);
            WinHttpQueryHeaders(pedido, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &estado, &largo, WINHTTP_NO_HEADER_INDEX);
            if (estado == 200)
            {
                DWORD disponible = 0;
                while (WinHttpQueryDataAvailable(pedido, &disponible) && disponible > 0 && cuerpo.size() < 4 * 1024 * 1024)
                {
                    std::string trozo(disponible, '\0');
                    DWORD leidos = 0;
                    if (!WinHttpReadData(pedido, trozo.data(), disponible, &leidos) || leidos == 0) break;
                    cuerpo.append(trozo.data(), leidos);
                }
            }
        }
        if (pedido) WinHttpCloseHandle(pedido);
        if (conexion) WinHttpCloseHandle(conexion);
        WinHttpCloseHandle(sesion);
        return cuerpo;
    }
}

int ayuda::CompararVersiones(const std::string& a, const std::string& b)
{
    auto partes = [](const std::string& v) {
        int n[3] = { 0, 0, 0 };
        std::string limpio = (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? v.substr(1) : v;
        std::istringstream in(limpio);
        for (int i = 0; i < 3; i++)
        {
            if (!(in >> n[i])) break;
            if (in.peek() != '.') break;
            in.get();
        }
        return std::make_tuple(n[0], n[1], n[2]);
    };
    auto pa = partes(a), pb = partes(b);
    if (pa < pb) return -1;
    if (pb < pa) return 1;
    return 0;
}

int ayuda::Aviso(const wxString& texto, const wxString& titulo, long estilo, wxWindow* padre)
{
    wxMessageDialog d(padre, texto, titulo, estilo);
    if ((estilo & wxYES_NO) && (estilo & wxCANCEL)) d.SetYesNoCancelLabels(L"Sí", L"No", L"Cancelar");
    else if (estilo & wxYES_NO) d.SetYesNoLabels(L"Sí", L"No");
    else if ((estilo & wxOK) && (estilo & wxCANCEL)) d.SetOKCancelLabels(L"Aceptar", L"Cancelar");
    else if (estilo & wxOK) d.SetOKLabel(L"Aceptar");
    return d.ShowModal();
}

void ayuda::AbrirEnlace(const wxString& url)
{
    // Solo direcciones web: nunca se abre otra cosa por aquí
    if (!url.StartsWith(L"https://") && !url.StartsWith(L"http://")) return;
    ShellExecuteW(nullptr, L"open", url.wc_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ayuda::ReportarProblema()
{
    AbrirEnlace(L"https://github.com/" + Repo() + L"/issues/new/choose");
}

void ayuda::BuscarActualizacion(wxWindow* padre)
{
    // El hilo de trabajo no guarda ninguna referencia de wx: solo el número de la ventana. Ya de vuelta en el hilo de
    // la ventana se la busca por ese número (si se cerró, el aviso sale sin ventana madre).
    int idVentana = padre ? padre->GetId() : wxID_ANY;
    std::wstring ruta = L"/repos/" + std::wstring(Repo().wc_str()) + L"/releases/latest";

    std::thread([idVentana, ruta]() {
        std::string cuerpo = Descargar(L"api.github.com", ruta);
        std::string etiqueta, pagina;
        try
        {
            auto j = nlohmann::json::parse(cuerpo);
            etiqueta = j.value("tag_name", "");
            pagina = j.value("html_url", "");
        }
        catch (...)
        {
        }

        jicaro::hilos::Enviar([idVentana, etiqueta, pagina]() {
            wxWindow* padre = idVentana != wxID_ANY ? wxWindow::FindWindowById(idVentana) : nullptr;
            if (etiqueta.empty())
            {
                ayuda::Aviso(L"No pude consultar GitHub. Revisa tu conexión a internet e inténtalo de nuevo.",
                             L"Buscar actualización", wxOK | wxICON_WARNING, padre);
                return;
            }
            if (CompararVersiones(etiqueta, APP_VERSION) > 0)
            {
                wxMessageDialog d(padre,
                                  wxString::Format(L"Hay una versión nueva: %s (tienes la %s).\n\n¿Abrir la página para bajarla?",
                                                   wxString::FromUTF8(etiqueta), wxString::FromUTF8(APP_VERSION)),
                                  L"Buscar actualización", wxYES_NO | wxICON_INFORMATION);
                d.SetYesNoLabels(L"Abrir la página", L"Ahora no");
                if (d.ShowModal() == wxID_YES)
                {
                    wxString url = wxString::FromUTF8(pagina);
                    if (!url.StartsWith(L"https://github.com/")) url = L"https://github.com/" + Repo() + L"/releases/latest";
                    AbrirEnlace(url);
                }
            }
            else
            {
                ayuda::Aviso(wxString::Format(L"Ya tienes la versión más nueva (%s).", wxString::FromUTF8(APP_VERSION)),
                             L"Buscar actualización", wxOK | wxICON_INFORMATION, padre);
            }
        });
    }).detach();
}

void ayuda::MostrarAcercaDe(wxWindow* padre, const wxString& descripcion)
{
    wxDialog d(padre, wxID_ANY, L"Acerca de " + Nombre());
    auto* caja = new wxBoxSizer(wxVERTICAL);

    auto* titulo = new wxStaticText(&d, wxID_ANY, Nombre() + L" " + wxString::FromUTF8(APP_VERSION_TEXTO));
    titulo->SetFont(titulo->GetFont().Scaled(1.4f).Bold());
    caja->Add(titulo, 0, wxALL, d.FromDIP(16));

    wxString texto = descripcion + L"\n\n"
        L"Hecho por Brizna Soft. Gratis y de código abierto (licencia MIT).\n"
        L"Sin publicidad, sin virus y sin recoger tus datos.\n"
        L"Hecho con ayuda de IA.\n\n"
        L"Usa libarchive (licencia BSD), wxWidgets y otras bibliotecas libres. Sus avisos de licencia están en el "
        L"archivo AVISOS-DE-TERCEROS.txt, junto al programa.";
    auto* cuerpo = new wxStaticText(&d, wxID_ANY, texto);
    cuerpo->Wrap(d.FromDIP(440));
    caja->Add(cuerpo, 0, wxLEFT | wxRIGHT, d.FromDIP(16));

    auto enlace = [&](const wxString& etiqueta, const wxString& url) {
        caja->Add(new wxHyperlinkCtrl(&d, wxID_ANY, etiqueta, url), 0, wxLEFT | wxRIGHT | wxTOP, d.FromDIP(12));
    };
    std::wstring avisos = jicaro::CarpetaDelPrograma() + L"\\AVISOS-DE-TERCEROS.txt";
    if (jicaro::ExisteArchivo(avisos))
    {
        auto* local = new wxHyperlinkCtrl(&d, wxID_ANY, L"Ver los avisos de terceros", L"https://github.com/" + Repo());
        local->Bind(wxEVT_HYPERLINK, [avisos](wxHyperlinkEvent&) {
            ShellExecuteW(nullptr, L"open", avisos.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        });
        caja->Add(local, 0, wxLEFT | wxRIGHT | wxTOP, d.FromDIP(12));
    }
    enlace(L"Página de Brizna Soft", PaginaEstudio);
    enlace(L"Código y avisos de terceros en GitHub", L"https://github.com/" + Repo());
    enlace(L"Reportar un problema", L"https://github.com/" + Repo() + L"/issues/new/choose");
    enlace(L"Donar (PayPal)", PaginaDonar);

    caja->Add(d.CreateSeparatedButtonSizer(wxOK), 0, wxEXPAND | wxALL, d.FromDIP(16));
    if (auto* cerrar = d.FindWindow(wxID_OK)) cerrar->SetLabel(L"Cerrar");   // wx lo pondría en inglés
    d.SetSizerAndFit(caja);
    d.CentreOnParent();
    d.ShowModal();
}

wxMenu* ayuda::CrearMenu(wxFrame* ventana, const wxString& descripcion)
{
    auto* menu = new wxMenu();
    int idReportar = wxWindow::NewControlId(), idActualizar = wxWindow::NewControlId(), idDonar = wxWindow::NewControlId(),
        idPagina = wxWindow::NewControlId();
    menu->Append(idReportar, L"&Reportar un problema...");
    menu->Append(idActualizar, L"&Buscar actualización");
    menu->AppendSeparator();
    menu->Append(idDonar, L"&Donar...");
    menu->Append(idPagina, L"&Página de Brizna Soft");
    menu->AppendSeparator();
    menu->Append(wxID_ABOUT, L"&Acerca de " + Nombre());

    ventana->Bind(wxEVT_MENU, [](wxCommandEvent&) { ReportarProblema(); }, idReportar);
    ventana->Bind(wxEVT_MENU, [ventana](wxCommandEvent&) { BuscarActualizacion(ventana); }, idActualizar);
    ventana->Bind(wxEVT_MENU, [](wxCommandEvent&) { AbrirEnlace(PaginaDonar); }, idDonar);
    ventana->Bind(wxEVT_MENU, [](wxCommandEvent&) { AbrirEnlace(PaginaEstudio); }, idPagina);
    ventana->Bind(wxEVT_MENU, [ventana, descripcion](wxCommandEvent&) { MostrarAcercaDe(ventana, descripcion); }, wxID_ABOUT);
    return menu;
}
