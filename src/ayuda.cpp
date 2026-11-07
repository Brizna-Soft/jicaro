// Menú «Ayuda» común a todos los programas de Brizna Soft.
// ESTE ARCHIVO ES IGUAL EN TODOS LOS PROGRAMAS: la copia maestra está en Documents\Brizna-Soft\programas\comun.

#include "ayuda.hpp"

#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <shlobj.h>

#include <wx/app.h>
#include <wx/button.h>
#include <wx/clipbrd.h>
#include <wx/dialog.h>
#include <wx/hyperlink.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
#include <tuple>

#include "version.h"

#ifndef APP_NOMBRE_ASCII
#define APP_NOMBRE_ASCII APP_NOMBRE
#endif

const wchar_t* const ayuda::SiVuelveAPasar = L"Si vuelve a pasar, avísanos en Ayuda > Reportar un problema.";

namespace
{
    const wchar_t* PaginaEstudio = L"https://brizna-soft.github.io";
    const wchar_t* PaginaDonar = L"https://paypal.me/BriznaSoft";

    wxString Repo() { return wxString::FromUTF8(APP_REPO); }
    wxString Nombre() { return wxString::FromUTF8(APP_NOMBRE); }
    wxString Version() { return wxString::FromUTF8(APP_VERSION); }

    // «Brizna-Soft/atarraya» -> «atarraya»: así se llama también su página en brizna-soft.github.io/programas/
    wxString NombreDelRepo() { return Repo().AfterLast(L'/'); }

    // Pasar trabajo del hilo de «Buscar actualización» al de la ventana, sin riesgos al cerrar el programa:
    // después de Cerrar() no se manda nada más.
    std::mutex candado;
    bool abierta = true;

    void Enviar(std::function<void()> f)
    {
        std::lock_guard<std::mutex> g(candado);
        if (!abierta || !wxTheApp) return;
        wxTheApp->CallAfter(std::move(f));
    }

    std::wstring CarpetaDelPrograma()
    {
        std::wstring ruta(MAX_PATH, L'\0');
        for (;;)
        {
            DWORD n = GetModuleFileNameW(nullptr, ruta.data(), (DWORD)ruta.size());
            if (n == 0) return {};
            if (n < ruta.size())
            {
                ruta.resize(n);
                break;
            }
            if (ruta.size() >= 32768) return {};
            ruta.resize(ruta.size() * 2);
        }
        size_t barra = ruta.find_last_of(L"\\/");
        return barra == std::wstring::npos ? std::wstring() : ruta.substr(0, barra);
    }

    bool EsArchivo(const std::wstring& ruta)
    {
        DWORD a = GetFileAttributesW(ruta.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
    }

    bool EsCarpeta(const std::wstring& ruta)
    {
        DWORD a = GetFileAttributesW(ruta.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT);
    }

    // Para las direcciones: deja letras, números y «-._~»; lo demás va como %XX (en UTF-8)
    wxString Codificar(const wxString& texto)
    {
        wxString r;
        wxScopedCharBuffer utf8 = texto.utf8_str();
        for (size_t i = 0; i < utf8.length(); i++)
        {
            unsigned char c = (unsigned char)utf8.data()[i];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.'
                || c == '_' || c == '~')
                r += (wchar_t)c;
            else
                r += wxString::Format(L"%%%02X", c);
        }
        return r;
    }

    // «Windows 11 (compilación 26300)». Se le pregunta a ntdll porque GetVersionEx miente desde Windows 8.1.
    wxString VersionDeWindows()
    {
        using Funcion = LONG(WINAPI*)(OSVERSIONINFOW*);
        OSVERSIONINFOW v{};
        v.dwOSVersionInfoSize = sizeof(v);
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        auto f = ntdll ? reinterpret_cast<Funcion>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))) : nullptr;
        if (!f || f(&v) != 0) return L"Windows";
        wxString nombre;
        if (v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000) nombre = L"Windows 11";
        else if (v.dwMajorVersion == 10) nombre = L"Windows 10";
        else nombre = wxString::Format(L"Windows %lu.%lu", v.dwMajorVersion, v.dwMinorVersion);
        return wxString::Format(L"%s (compilación %lu)", nombre, v.dwBuildNumber);
    }

    // GET sencillo con WinHTTP. Devuelve el cuerpo, o vacío si algo falla.
    std::string Descargar(const std::wstring& host, const std::wstring& ruta)
    {
        std::string cuerpo;
        std::wstring agente = std::wstring(L"" APP_NOMBRE_ASCII L"/") + std::wstring(Version().wc_str());
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

void ayuda::Cerrar()
{
    std::lock_guard<std::mutex> g(candado);
    abierta = false;
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
    // Solo direcciones web seguras: nunca se abre otra cosa por aquí
    if (!url.StartsWith(L"https://")) return;
    ShellExecuteW(nullptr, L"open", url.wc_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ayuda::ComoSeUsa()
{
    AbrirEnlace(wxString(PaginaEstudio) + L"/programas/" + NombreDelRepo() + L"/");
}

void ayuda::ReportarProblema()
{
    // El formulario «Algo no funciona» de GitHub, con la versión del programa y la de Windows ya escritas.
    // Cuando exista el formulario de la página (sin cuenta de GitHub), se cambia solo aquí.
    AbrirEnlace(L"https://github.com/" + Repo() + L"/issues/new?template=falla.yml&version="
                + Codificar(wxString::FromUTF8(APP_VERSION_TEXTO)) + L"&windows=" + Codificar(VersionDeWindows()));
}

void ayuda::AbrirCarpetaDeRegistros(const std::wstring& carpeta, wxWindow* padre)
{
    if (carpeta.empty() || !EsCarpeta(carpeta))
    {
        Aviso(Nombre() + L" todavía no tiene registros: los crea cuando hay algo que anotar.",
              L"Carpeta de registros", wxOK | wxICON_INFORMATION, padre);
        return;
    }
    // Se abre la carpeta con registro.txt ya marcado, para que se vea cuál pegar en el reporte
    std::wstring registro = carpeta + L"\\registro.txt";
    if (EsArchivo(registro))
    {
        if (PIDLIST_ABSOLUTE elemento = ILCreateFromPathW(registro.c_str()))
        {
            HRESULT hr = SHOpenFolderAndSelectItems(elemento, 0, nullptr, 0);
            ILFree(elemento);
            if (SUCCEEDED(hr)) return;
        }
    }
    ShellExecuteW(nullptr, L"explore", carpeta.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

wxString ayuda::DatosParaReporte()
{
    return Nombre() + L" " + wxString::FromUTF8(APP_VERSION_TEXTO) + L"\n" + VersionDeWindows() + L", 64 bits";
}

void ayuda::BuscarActualizacion(wxWindow* padre)
{
    // El hilo de trabajo no guarda ninguna referencia de wx: solo el número de la ventana. Ya de vuelta en el hilo de
    // la ventana se la busca por ese número (si se cerró, el aviso sale sin ventana madre).
    int idVentana = padre ? padre->GetId() : wxID_ANY;
    std::wstring repo(Repo().wc_str());
    std::wstring ruta = L"/repos/" + repo + L"/releases/latest";

    std::thread([idVentana, ruta, repo]() {
        std::string cuerpo = Descargar(L"api.github.com", ruta);
        std::string etiqueta, pagina, zip;
        try
        {
            auto j = nlohmann::json::parse(cuerpo);
            etiqueta = j.value("tag_name", "");
            pagina = j.value("html_url", "");
            // El .zip del programa: «Nombre-versión-x64.zip», bajado de las releases de su propio repositorio
            std::string inicio = std::string(APP_NOMBRE_ASCII) + "-";
            std::string base = "https://github.com/" + std::string(APP_REPO) + "/releases/download/";
            for (const auto& a : j.value("assets", nlohmann::json::array()))
            {
                std::string nombre = a.value("name", ""), url = a.value("browser_download_url", "");
                bool esZip = nombre.size() > inicio.size() + 8 && nombre.compare(0, inicio.size(), inicio) == 0
                             && nombre.compare(nombre.size() - 8, 8, "-x64.zip") == 0;
                if (esZip && url.compare(0, base.size(), base) == 0)
                {
                    zip = url;
                    break;
                }
            }
        }
        catch (...)
        {
        }

        Enviar([idVentana, etiqueta, pagina, zip, repo]() {
            wxWindow* padre = idVentana != wxID_ANY ? wxWindow::FindWindowById(idVentana) : nullptr;
            if (etiqueta.empty())
            {
                Aviso(L"No pude buscar la actualización. Revisa tu internet y vuelve a intentarlo.",
                      L"Buscar actualización", wxOK | wxICON_WARNING, padre);
                return;
            }
            if (CompararVersiones(etiqueta, APP_VERSION) <= 0)
            {
                Aviso(wxString::Format(L"Ya tienes la versión más nueva (%s).", Version()), L"Buscar actualización",
                      wxOK | wxICON_INFORMATION, padre);
                return;
            }

            wxString nueva = wxString::FromUTF8(etiqueta);
            if (nueva.StartsWith(L"v") || nueva.StartsWith(L"V")) nueva = nueva.Mid(1);
            wxString paginaUrl = wxString::FromUTF8(pagina);
            if (!paginaUrl.StartsWith(L"https://github.com/" + wxString(repo) + L"/releases/"))
                paginaUrl = L"https://github.com/" + wxString(repo) + L"/releases/latest";

            wxString texto = wxString::Format(L"Hay una versión nueva: %s (tienes la %s).", nueva, Version());
            if (!zip.empty())
            {
                texto += L"\n\nPara ponerla:\n"
                         L"1. Pulsa «Bajar la versión nueva». Se baja con tu navegador.\n"
                         L"2. Cierra " + Nombre() + L".\n"
                         L"3. Descomprime el .zip en la carpeta donde está ahora y reemplaza los archivos:\n"
                         + wxString(CarpetaDelPrograma()) + L"\n\n"
                         L"Tus ajustes no se pierden.";
                wxMessageDialog d(padre, texto, L"Buscar actualización", wxYES_NO | wxCANCEL | wxICON_INFORMATION);
                d.SetYesNoCancelLabels(L"Bajar la versión nueva", L"Ver qué trae", L"Ahora no");
                int r = d.ShowModal();
                if (r == wxID_YES) AbrirEnlace(wxString::FromUTF8(zip));
                else if (r == wxID_NO) AbrirEnlace(paginaUrl);
                return;
            }
            wxMessageDialog d(padre, texto + L"\n\n¿Abrir la página para bajarla?", L"Buscar actualización",
                              wxYES_NO | wxICON_INFORMATION);
            d.SetYesNoLabels(L"Abrir la página", L"Ahora no");
            if (d.ShowModal() == wxID_YES) AbrirEnlace(paginaUrl);
        });
    }).detach();
}

void ayuda::MostrarAcercaDe(wxWindow* padre, const Opciones& opciones)
{
    wxDialog d(padre, wxID_ANY, L"Acerca de " + Nombre());
    auto* caja = new wxBoxSizer(wxVERTICAL);

    auto* titulo = new wxStaticText(&d, wxID_ANY, Nombre() + L" " + wxString::FromUTF8(APP_VERSION_TEXTO));
    titulo->SetFont(titulo->GetFont().Scaled(1.4f).Bold());
    caja->Add(titulo, 0, wxALL, d.FromDIP(16));

    wxString texto = opciones.descripcion + L"\n\n"
        L"Hecho por Brizna Soft. Gratis y de código abierto (licencia MIT).\n"
        L"Sin publicidad, sin virus y sin recoger tus datos.\n"
        L"Hecho con ayuda de IA.";
    if (!opciones.creditos.empty()) texto += L"\n\n" + opciones.creditos;
    auto* cuerpo = new wxStaticText(&d, wxID_ANY, texto);
    cuerpo->Wrap(d.FromDIP(440));
    caja->Add(cuerpo, 0, wxLEFT | wxRIGHT, d.FromDIP(16));

    auto enlace = [&](const wxString& etiqueta, const wxString& url) {
        caja->Add(new wxHyperlinkCtrl(&d, wxID_ANY, etiqueta, url), 0, wxLEFT | wxRIGHT | wxTOP, d.FromDIP(12));
    };
    // Los avisos de terceros que vienen junto al programa se abren con el Bloc de notas
    std::wstring avisos = CarpetaDelPrograma() + L"\\AVISOS-DE-TERCEROS.txt";
    if (EsArchivo(avisos))
    {
        auto* local = new wxHyperlinkCtrl(&d, wxID_ANY, L"Ver los avisos de terceros", L"https://github.com/" + Repo());
        local->Bind(wxEVT_HYPERLINK, [avisos](wxHyperlinkEvent&) {
            ShellExecuteW(nullptr, L"open", avisos.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        });
        caja->Add(local, 0, wxLEFT | wxRIGHT | wxTOP, d.FromDIP(12));
    }
    enlace(L"Página de Brizna Soft", PaginaEstudio);
    enlace(L"Código en GitHub", L"https://github.com/" + Repo());
    enlace(L"Donar (PayPal)", PaginaDonar);

    // «Copiar datos para el reporte»: solo la versión del programa y la de Windows
    auto* botones = new wxBoxSizer(wxHORIZONTAL);
    auto* copiar = new wxButton(&d, wxID_ANY, L"&Copiar datos para el reporte");
    copiar->SetToolTip(L"Copia la versión de " + Nombre() + L" y la de Windows, para pegarlas en el reporte. "
                       L"No lleva tu nombre ni el de tu PC.");
    copiar->Bind(wxEVT_BUTTON, [copiar](wxCommandEvent&) {
        if (wxTheClipboard->Open())
        {
            wxTheClipboard->SetData(new wxTextDataObject(DatosParaReporte()));
            wxTheClipboard->Close();
            copiar->SetLabel(L"Datos copiados");
        }
        else
        {
            copiar->SetLabel(L"No se pudo copiar");
        }
    });
    auto* cerrar = new wxButton(&d, wxID_OK, L"Cerrar");
    botones->Add(copiar, 0, wxRIGHT, d.FromDIP(8));
    botones->AddStretchSpacer();
    botones->Add(cerrar, 0);
    caja->Add(botones, 0, wxEXPAND | wxALL, d.FromDIP(16));
    d.SetAffirmativeId(wxID_OK);
    d.SetEscapeId(wxID_OK);
    cerrar->SetDefault();
    d.SetSizerAndFit(caja);
    d.CentreOnParent();
    d.ShowModal();
}

void ayuda::AgregarMenu(wxMenuBar* barra, wxFrame* ventana, const Opciones& opciones)
{
    auto* menu = new wxMenu();
    int idComo = wxWindow::NewControlId(), idReportar = wxWindow::NewControlId(), idRegistros = wxWindow::NewControlId(),
        idActualizar = wxWindow::NewControlId(), idDonar = wxWindow::NewControlId(), idPagina = wxWindow::NewControlId();
    menu->Append(idComo, L"&Cómo se usa\tF1");
    menu->Append(idReportar, L"&Reportar un problema...");
    if (!opciones.carpetaDeRegistros.empty()) menu->Append(idRegistros, L"Abrir la carpeta de re&gistros");
    menu->AppendSeparator();
    menu->Append(idActualizar, L"&Buscar actualización");
    menu->AppendSeparator();
    menu->Append(idDonar, L"&Donar...");
    menu->Append(idPagina, L"&Página de Brizna Soft");
    menu->AppendSeparator();
    menu->Append(wxID_ABOUT, L"&Acerca de " + Nombre());

    std::wstring carpeta = opciones.carpetaDeRegistros;
    ventana->Bind(wxEVT_MENU, [](wxCommandEvent&) { ComoSeUsa(); }, idComo);
    ventana->Bind(wxEVT_MENU, [](wxCommandEvent&) { ReportarProblema(); }, idReportar);
    ventana->Bind(wxEVT_MENU, [ventana, carpeta](wxCommandEvent&) { AbrirCarpetaDeRegistros(carpeta, ventana); }, idRegistros);
    ventana->Bind(wxEVT_MENU, [ventana](wxCommandEvent&) { BuscarActualizacion(ventana); }, idActualizar);
    ventana->Bind(wxEVT_MENU, [](wxCommandEvent&) { AbrirEnlace(PaginaDonar); }, idDonar);
    ventana->Bind(wxEVT_MENU, [](wxCommandEvent&) { AbrirEnlace(PaginaEstudio); }, idPagina);
    ventana->Bind(wxEVT_MENU, [ventana, opciones](wxCommandEvent&) { MostrarAcercaDe(ventana, opciones); }, wxID_ABOUT);
    barra->Append(menu, L"A&yuda");
}
