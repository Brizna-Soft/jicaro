#include "ventana.hpp"

#include <windows.h>

#include <wx/button.h>
#include <wx/display.h>
#include <wx/dnd.h>
#include <wx/filedlg.h>
#include <wx/gauge.h>
#include <wx/icon.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <algorithm>

#include "ayuda.hpp"
#include "comun.hpp"
#include "dialogos.hpp"
#include "menucontextual.hpp"
#include "rutas.hpp"
#include "util.hpp"

namespace
{
    const wchar_t* Descripcion = L"Jícaro abre y extrae archivos comprimidos: ZIP (también con contraseña), RAR, RAR 5 y "
                                 L"RAR en varias partes, 7z, TAR, GZ, BZ2, XZ, ZST, ISO y CAB.";

    // «12/10/2026 14:05» a partir de segundos desde 1970 (hora local)
    wxString TextoFecha(int64_t segundos)
    {
        if (segundos <= 0) return L"";
        ULARGE_INTEGER u;
        u.QuadPart = ((uint64_t)segundos + 11644473600ULL) * 10000000ULL;
        FILETIME utc{ u.LowPart, u.HighPart }, local;
        SYSTEMTIME t;
        if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &t)) return L"";
        return wxString::Format(L"%02d/%02d/%04d %02d:%02d", t.wDay, t.wMonth, t.wYear, t.wHour, t.wMinute);
    }

    // Arrastrar un archivo a la ventana lo abre
    class Soltar : public wxFileDropTarget
    {
    public:
        explicit Soltar(VentanaPrincipal* v) : m_v(v) {}
        bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& archivos) override
        {
            if (archivos.empty()) return false;
            std::wstring ruta = archivos[0].ToStdWstring();
            m_v->CallAfter([v = m_v, ruta]() { v->AbrirArchivo(ruta); });
            return true;
        }

    private:
        VentanaPrincipal* m_v;
    };

    bool EsZip(const jicaro::Contenido& c) { return c.formato.rfind(L"ZIP", 0) == 0; }
}

// ---------------------------------------------------------------------------------------------------------------------

ListaContenido::ListaContenido(wxWindow* padre)
    : wxListCtrl(padre, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_VIRTUAL)
{
    AppendColumn(L"Nombre", wxLIST_FORMAT_LEFT, FromDIP(430));
    AppendColumn(L"Tamaño", wxLIST_FORMAT_RIGHT, FromDIP(90));
    AppendColumn(L"Modificado", wxLIST_FORMAT_LEFT, FromDIP(130));
    AppendColumn(L"Notas", wxLIST_FORMAT_LEFT, FromDIP(170));
}

void ListaContenido::Poner(const std::vector<jicaro::Entrada>* entradas)
{
    m_entradas = entradas;
    SetItemCount(entradas ? (long)entradas->size() : 0);
    Refresh();
}

wxString ListaContenido::OnGetItemText(long fila, long columna) const
{
    if (!m_entradas || fila < 0 || fila >= (long)m_entradas->size()) return L"";
    const auto& e = (*m_entradas)[fila];
    switch (columna)
    {
    case 0: return e.ruta;
    case 1: return (e.carpeta || e.tamano < 0) ? wxString() : wxString(jicaro::TextoTamano(e.tamano));
    case 2: return TextoFecha(e.fecha);
    case 3:
        if (e.enlace) return L"Enlace (no se extrae)";
        if (e.carpeta) return L"Carpeta";
        if (e.cifrada) return L"Con contraseña";
        return L"";
    default: return L"";
    }
}

// ---------------------------------------------------------------------------------------------------------------------

VentanaPrincipal::VentanaPrincipal(jicaro::Ajustes& ajustes)
    : wxFrame(nullptr, wxID_ANY, wxString::FromUTF8("Jícaro")), m_ajustes(ajustes), m_reloj(this)
{
    SetIcon(wxICON(AAAA_APP));
    CrearControles();

    // Tamaño y posición de la última vez, si siguen dentro de una pantalla
    if (m_ajustes.ancho > 200 && m_ajustes.alto > 150
        && wxDisplay::GetFromPoint(wxPoint(m_ajustes.x + 40, m_ajustes.y + 20)) != wxNOT_FOUND)
        SetSize(m_ajustes.x, m_ajustes.y, m_ajustes.ancho, m_ajustes.alto);
    else
    {
        SetSize(FromDIP(wxSize(860, 580)));
        Centre();
    }
    if (m_ajustes.maximizada) Maximize();

    Bind(wxEVT_CLOSE_WINDOW, &VentanaPrincipal::AlCerrar, this);
    Bind(wxEVT_TIMER, &VentanaPrincipal::AlTick, this);
    ActualizarBotones();
}

void VentanaPrincipal::CrearControles()
{
    // Menú
    auto* archivo = new wxMenu();
    archivo->Append(wxID_OPEN, L"&Abrir...\tCtrl+O");
    archivo->Append(ID_EXTRAER_TODO, L"&Extraer todo...\tCtrl+E");
    archivo->Append(ID_EXTRAER_ELEGIDO, L"Extraer &lo elegido...");
    archivo->AppendSeparator();
    archivo->Append(wxID_PREFERENCES, L"A&justes...");
    archivo->AppendSeparator();
    archivo->Append(wxID_EXIT, L"&Salir");
    auto* barra = new wxMenuBar();
    barra->Append(archivo, L"&Archivo");
    barra->Append(ayuda::CrearMenu(this, Descripcion), L"A&yuda");
    SetMenuBar(barra);

    m_panel = new wxPanel(this);
    m_caja = new wxBoxSizer(wxVERTICAL);
    const int margen = FromDIP(10);

    auto* botones = new wxBoxSizer(wxHORIZONTAL);
    m_botonAbrir = new wxButton(m_panel, wxID_OPEN, L"Abrir...");
    m_botonTodo = new wxButton(m_panel, ID_EXTRAER_TODO, L"Extraer todo...");
    m_botonElegido = new wxButton(m_panel, ID_EXTRAER_ELEGIDO, L"Extraer lo elegido...");
    m_botonAjustes = new wxButton(m_panel, wxID_PREFERENCES, L"Ajustes");
    botones->Add(m_botonAbrir, 0, wxRIGHT, FromDIP(6));
    botones->Add(m_botonTodo, 0, wxRIGHT, FromDIP(6));
    botones->Add(m_botonElegido, 0, wxRIGHT, FromDIP(6));
    botones->AddStretchSpacer();
    botones->Add(m_botonAjustes, 0);
    m_caja->Add(botones, 0, wxEXPAND | wxALL, margen);

    m_titulo = new wxStaticText(m_panel, wxID_ANY, L"", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_MIDDLE);
    m_titulo->SetFont(m_titulo->GetFont().Scaled(1.25f).Bold());
    m_detalle = new wxStaticText(m_panel, wxID_ANY, L"");
    m_aviso = new wxStaticText(m_panel, wxID_ANY, L"");
    m_caja->Add(m_titulo, 0, wxEXPAND | wxLEFT | wxRIGHT, margen);
    m_caja->AddSpacer(FromDIP(2));
    m_caja->Add(m_detalle, 0, wxEXPAND | wxLEFT | wxRIGHT, margen);
    m_caja->Add(m_aviso, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margen);
    m_aviso->Hide();

    m_lista = new ListaContenido(m_panel);
    m_vacio = new wxStaticText(m_panel, wxID_ANY,
                               L"Arrastra aquí un archivo comprimido\n(ZIP, RAR, 7z, TAR, ISO, CAB...)\no pulsa «Abrir...».",
                               wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    m_vacio->SetFont(m_vacio->GetFont().Scaled(1.3f));
    m_vacio->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    m_caja->Add(m_lista, 1, wxEXPAND | wxALL, margen);
    m_caja->Add(m_vacio, 1, wxALIGN_CENTER | wxALL, FromDIP(40));
    m_lista->Hide();

    auto* abajo = new wxBoxSizer(wxHORIZONTAL);
    m_estado = new wxStaticText(m_panel, wxID_ANY, L"", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_MIDDLE);
    m_barra = new wxGauge(m_panel, wxID_ANY, 1000, wxDefaultPosition, wxSize(FromDIP(200), -1));
    m_botonCancelar = new wxButton(m_panel, ID_CANCELAR_TRABAJO, L"Cancelar");
    abajo->Add(m_estado, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, margen);
    abajo->Add(m_barra, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    abajo->Add(m_botonCancelar, 0, wxALIGN_CENTER_VERTICAL);
    m_caja->Add(abajo, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, margen);
    m_barra->Hide();
    m_botonCancelar->Hide();
    m_panel->SetSizer(m_caja);

    m_panel->SetDropTarget(new Soltar(this));
    m_lista->SetDropTarget(new Soltar(this));

    // Comandos (los mismos ids para el menú y los botones)
    auto abrir = [this](wxCommandEvent&) {
        if (m_extrayendo) return;
        wxFileDialog d(this, L"Abrir un archivo comprimido", L"", L"",
                       L"Archivos comprimidos|*.zip;*.rar;*.7z;*.tar;*.gz;*.tgz;*.bz2;*.tbz2;*.tbz;*.xz;*.txz;*.zst;*.tzst;"
                       L"*.iso;*.cab;*.001;*.r00|Todos los archivos|*.*",
                       wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (d.ShowModal() == wxID_OK) AbrirArchivo(d.GetPath().ToStdWstring());
    };
    Bind(wxEVT_MENU, abrir, wxID_OPEN);
    Bind(wxEVT_BUTTON, abrir, wxID_OPEN);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Extraer(false); }, ID_EXTRAER_TODO);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Extraer(false); }, ID_EXTRAER_TODO);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Extraer(true); }, ID_EXTRAER_ELEGIDO);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Extraer(true); }, ID_EXTRAER_ELEGIDO);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { MostrarAjustes(); }, wxID_PREFERENCES);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { MostrarAjustes(); }, wxID_PREFERENCES);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(); }, wxID_EXIT);
    auto cancelar = [this](wxCommandEvent&) {
        if (!m_trabajo.Ocupado()) return;
        m_trabajo.Cancelar();
        m_estado->SetLabel(L"Cancelando...");
        m_botonCancelar->Disable();
    };
    Bind(wxEVT_BUTTON, cancelar, ID_CANCELAR_TRABAJO);
    Bind(wxEVT_MENU, cancelar, ID_CANCELAR_TRABAJO);
    m_lista->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& e) { ActualizarBotones(); e.Skip(); });
    m_lista->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent& e) { ActualizarBotones(); e.Skip(); });
}

void VentanaPrincipal::PonerAviso(const wxString& texto, bool grave)
{
    m_aviso->SetLabel(texto);
    m_aviso->SetForegroundColour(grave ? wxColour(205, 90, 20) : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
    m_aviso->Wrap(-1);
    m_aviso->Wrap(std::max(FromDIP(300), m_panel->GetClientSize().GetWidth() - FromDIP(20)));
    m_aviso->Show(!texto.empty());
    m_panel->Layout();
}

void VentanaPrincipal::ActualizarBotones()
{
    bool ocupado = m_trabajo.Ocupado();
    bool sePuede = m_hayArchivo && m_juego.faltan.empty() && !m_extrayendo
        && (m_listando || !m_contenido.entradas.empty() || m_contenido.estado == jicaro::Estado::Bien);
    m_botonAbrir->Enable(!m_extrayendo);
    m_botonTodo->Enable(sePuede && (!m_contenido.entradas.empty() || m_listando));
    m_botonElegido->Enable(sePuede && !m_listando && m_lista->GetSelectedItemCount() > 0);
    m_botonAjustes->Enable(!ocupado);
    if (auto* barra = GetMenuBar())
    {
        barra->Enable(wxID_OPEN, !m_extrayendo);
        barra->Enable(ID_EXTRAER_TODO, m_botonTodo->IsEnabled());
        barra->Enable(ID_EXTRAER_ELEGIDO, m_botonElegido->IsEnabled());
        barra->Enable(wxID_PREFERENCES, !ocupado);
    }
}

void VentanaPrincipal::Ocupar(const wxString& texto)
{
    m_estado->SetLabel(texto);
    m_barra->SetValue(0);
    m_barra->Show();
    m_botonCancelar->Enable();
    m_botonCancelar->Show();
    m_panel->Layout();
    m_reloj.Start(150);
    ActualizarBotones();
}

void VentanaPrincipal::Liberar(const wxString& texto)
{
    m_reloj.Stop();
    m_estado->SetLabel(texto);
    m_barra->Hide();
    m_botonCancelar->Hide();
    m_panel->Layout();
    ActualizarBotones();
}

void VentanaPrincipal::AbrirArchivo(const std::wstring& ruta)
{
    if (m_extrayendo)
    {
        ayuda::Aviso(L"Espera a que termine la extracción para abrir otro archivo.", L"Jícaro", wxOK | wxICON_INFORMATION, this);
        return;
    }
    if (!jicaro::ExisteArchivo(ruta))
    {
        ayuda::Aviso(L"No encuentro ese archivo. ¿Se movió o se borró?", L"Jícaro", wxOK | wxICON_WARNING, this);
        return;
    }
    if (m_listando)
    {
        // Se deja de leer el anterior y se abre este cuando termine de cancelarse
        m_trabajo.Cancelar();
        m_despuesDeListar = [this, ruta]() { AbrirArchivo(ruta); };
        return;
    }
    m_juego = jicaro::volumenes::Buscar(ruta);
    m_contenido = jicaro::Contenido();
    m_lista->Poner(nullptr);
    m_hayArchivo = true;
    m_lista->Show();
    m_vacio->Hide();

    std::wstring nombre = jicaro::NombreDe(m_juego.partes.empty() ? ruta : m_juego.partes.front());
    m_titulo->SetLabel(nombre);
    SetTitle(wxString(nombre) + wxString::FromUTF8(" - Jícaro"));
    m_detalle->SetLabel(m_juego.Varias() ? wxString::Format(L"%zu partes encontradas", m_juego.partes.size()) : wxString());
    PonerAviso(L"", false);
    if (!m_juego.faltan.empty())
    {
        wxString faltan;
        for (size_t i = 0; i < m_juego.faltan.size() && i < 8; i++) faltan += (i ? L", " : L"") + wxString(m_juego.faltan[i]);
        if (m_juego.faltan.size() > 8) faltan += wxString::Format(L" y %zu más", m_juego.faltan.size() - 8);
        PonerAviso(L"Faltan partes: " + faltan + L". Bájalas y ponlas en la misma carpeta que las demás; mientras tanto no se "
                   L"puede extraer.", true);
    }
    // Se lee hasta donde las partes van seguidas desde la primera
    std::vector<std::wstring> leer(m_juego.partes.begin(), m_juego.partes.begin() + std::min(m_juego.seguidas, m_juego.partes.size()));
    if (leer.empty())
    {
        m_contenido.estado = jicaro::Estado::FaltaParte;
        ActualizarBotones();
        return;
    }
    m_listando = true;
    Ocupar(L"Leyendo el contenido...");
    m_trabajo.Listar(leer, m_juego.nombreSuelto, [this](jicaro::Contenido c) { AlTerminarLista(std::move(c)); });
    ActualizarBotones();
}

void VentanaPrincipal::AlTerminarLista(jicaro::Contenido c)
{
    m_listando = false;
    m_contenido = std::move(c);
    m_lista->Poner(&m_contenido.entradas);

    int64_t archivos = 0, comprimido = 0;
    for (auto& e : m_contenido.entradas)
        if (!e.carpeta) archivos++;
    for (auto& p : m_juego.partes) comprimido += std::max<int64_t>(0, jicaro::TamanoArchivo(p));
    wxString detalle = m_contenido.formato.empty() ? wxString(L"") : wxString(m_contenido.formato) + L" · ";
    if (m_juego.Varias()) detalle += wxString::Format(L"%zu partes · ", m_juego.partes.size());
    detalle += (archivos == 1 ? wxString(L"1 archivo") : wxString(jicaro::TextoNumero(archivos)) + L" archivos");
    if (m_contenido.total > 0) detalle += L" · " + wxString(jicaro::TextoTamano(m_contenido.total)) + L" al extraer";
    detalle += L" · " + wxString(jicaro::TextoTamano(comprimido)) + L" comprimido";
    if (!m_contenido.completo && m_contenido.estado == jicaro::Estado::Bien) detalle += L" (la lista no está completa)";
    m_detalle->SetLabel(detalle);

    bool seguir = (bool)m_despuesDeListar;
    if (!m_juego.faltan.empty())
    {
        // el aviso de las partes que faltan ya está puesto
    }
    else if (seguir)
        PonerAviso(L"", false);
    else if (m_contenido.estado != jicaro::Estado::Bien && !m_contenido.mensaje.empty())
        PonerAviso(m_contenido.mensaje, true);
    else if (m_contenido.cifradas > 0 && !EsZip(m_contenido))
        PonerAviso(wxString::Format(L"%s archivos tienen una contraseña de un tipo que Jícaro todavía no puede abrir. Para esos "
                                    L"usa otro programa, como 7-Zip o WinRAR; los demás sí se pueden extraer.",
                                    jicaro::TextoNumero(m_contenido.cifradas)),
                   true);
    else if (m_contenido.cifradas > 0)
        PonerAviso(L"Este ZIP tiene contraseña: Jícaro te la va a pedir al extraer.", false);
    else if (!m_contenido.mensaje.empty())
        PonerAviso(m_contenido.mensaje, false);

    if (!m_juego.faltan.empty()) Liberar(L"No se puede extraer hasta que estén todas las partes.");
    else Liberar(m_contenido.estado == jicaro::Estado::Bien ? L"Listo para extraer." : L"");
    if (seguir)
    {
        auto f = std::move(m_despuesDeListar);
        m_despuesDeListar = nullptr;
        f();
    }
}

void VentanaPrincipal::Extraer(bool soloElegido)
{
    if (!m_hayArchivo || m_extrayendo) return;
    if (!m_juego.faltan.empty())
    {
        ayuda::Aviso(L"Faltan partes de este archivo. Bájalas y ponlas en la misma carpeta que las demás antes de extraer.",
                     L"Faltan partes", wxOK | wxICON_WARNING, this);
        return;
    }
    if (m_listando)
    {
        // Todavía está leyendo la lista (con archivos grandes puede tardar): se deja de leer y se extrae directo
        if (soloElegido) return;
        m_trabajo.Cancelar();
        m_despuesDeListar = [this]() { Extraer(false); };
        return;
    }

    std::vector<std::wstring> elegidas;
    int64_t total = m_contenido.total;
    if (soloElegido)
    {
        total = 0;
        for (long i = m_lista->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED); i != -1;
             i = m_lista->GetNextItem(i, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED))
        {
            const auto& e = m_contenido.entradas[(size_t)i];
            elegidas.push_back(e.ruta);
            if (e.tamano > 0) total += e.tamano;
        }
        if (elegidas.empty())
        {
            ayuda::Aviso(L"Primero elige en la lista lo que quieres extraer (con Ctrl o Mayús puedes elegir varios).",
                         L"Extraer lo elegido", wxOK | wxICON_INFORMATION, this);
            return;
        }
    }

    // RAR o 7z con contraseña: libarchive no los abre
    if (m_contenido.completo && m_contenido.cifradas > 0 && !EsZip(m_contenido))
    {
        int64_t archivos = 0;
        for (auto& e : m_contenido.entradas)
            if (!e.carpeta && !e.enlace) archivos++;
        if (m_contenido.cifradas >= archivos)
        {
            ayuda::Aviso(L"Todos los archivos de aquí tienen una contraseña de un tipo que Jícaro todavía no puede abrir. "
                         L"Para este archivo usa otro programa, como 7-Zip o WinRAR.",
                         L"Con contraseña", wxOK | wxICON_WARNING, this);
            return;
        }
    }

    std::wstring carpeta = jicaro::rutas::CarpetaNuevaSegura(m_juego.carpeta, m_juego.nombreBase);
    jicaro::SiExiste siExiste = m_ajustes.siExiste;
    bool abrir = m_ajustes.abrirCarpeta;
    if (!dialogos::ElegirDestino(this, soloElegido ? L"Extraer lo elegido" : L"Extraer todo", carpeta, siExiste, abrir)) return;
    if (abrir != m_ajustes.abrirCarpeta)
    {
        m_ajustes.abrirCarpeta = abrir;
        m_ajustes.Guardar();
    }

    // ¿Alcanza el espacio? ¿Crece demasiado?
    jicaro::Opciones op;
    std::wstring existente = carpeta;
    while (!existente.empty() && !jicaro::ExisteCarpeta(existente)) existente = jicaro::CarpetaDe(existente);
    int64_t libre = jicaro::EspacioLibre(existente);
    if (total > 0 && libre >= 0 && total > libre)
    {
        ayuda::Aviso(wxString::Format(L"No alcanza el espacio: hace falta %s y en ese disco quedan %s libres.",
                                      jicaro::TextoTamano(total), jicaro::TextoTamano(libre)),
                     L"Sin espacio", wxOK | wxICON_WARNING, this);
        return;
    }
    int64_t comprimido = 0;
    for (auto& p : m_juego.partes) comprimido += std::max<int64_t>(0, jicaro::TamanoArchivo(p));
    if (total >= op.limites.bytesMinimos && (double)total > op.limites.proporcion * (double)std::max<int64_t>(comprimido, 1))
    {
        std::wstring motivo = L"Lo que trae este archivo ocuparía " + jicaro::TextoTamano(total) + L", más de " +
            jicaro::TextoNumero(total / std::max<int64_t>(comprimido, 1)) + L" veces lo que pesa comprimido.";
        if (!dialogos::PreguntarBomba(this, motivo)) return;
        op.limites.proporcion = 1e18;   // ya se preguntó
    }

    op.partes = m_juego.partes;
    op.destino = carpeta;
    op.siExiste = siExiste;
    op.elegidas = elegidas;
    op.nombreSuelto = m_juego.nombreSuelto;
    op.totalEsperado = m_contenido.completo ? total : 0;
    m_extrayendo = true;
    Ocupar(L"Extrayendo...");
    m_trabajo.Extraer(op, [this, carpeta, abrir](jicaro::Resumen r) { AlTerminarExtraccion(r, carpeta, abrir); });
}

void VentanaPrincipal::AlTerminarExtraccion(const jicaro::Resumen& r, const std::wstring& destino, bool abrir)
{
    m_extrayendo = false;
    if (m_cerrarAlTerminar)
    {
        if (!r.aMedias.empty()) dialogos::MostrarResumen(this, r, destino);
        Close();
        return;
    }
    bool limpio = r.Limpio();
    if (limpio)
    {
        Liberar(dialogos::TextoResumen(r, destino).BeforeFirst(L'\n'));
        if (abrir && r.archivos > 0) jicaro::AbrirCarpeta(destino);
        return;
    }
    Liberar(jicaro::TextoEstado(r.estado));
    if (r.estado != jicaro::Estado::Cancelado || r.archivos > 0 || !r.aMedias.empty()) dialogos::MostrarResumen(this, r, destino);
}

void VentanaPrincipal::MostrarAjustes()
{
    jicaro::Ajustes nuevos = m_ajustes;
    if (!dialogos::MostrarAjustes(this, nuevos)) return;
    if (nuevos.menuContextual != m_ajustes.menuContextual)
    {
        std::wstring exe = jicaro::CarpetaDelPrograma() + L"\\Jicaro.exe";
        wchar_t propio[MAX_PATH * 4] = {};
        if (GetModuleFileNameW(nullptr, propio, MAX_PATH * 4)) exe = propio;
        bool ok = nuevos.menuContextual ? jicaro::menu::Registrar(exe) : jicaro::menu::Quitar();
        if (!ok)
        {
            ayuda::Aviso(nuevos.menuContextual ? L"No se pudo agregar al clic derecho." : L"No se pudo quitar del clic derecho.",
                         L"Ajustes", wxOK | wxICON_WARNING, this);
            nuevos.menuContextual = jicaro::menu::EstaRegistrado();
        }
        else if (nuevos.menuContextual)
        {
            ayuda::Aviso(L"Listo. Haz clic derecho en un archivo comprimido: en Windows 11 están en «Mostrar más opciones».",
                         L"Ajustes", wxOK | wxICON_INFORMATION, this);
        }
    }
    m_ajustes = nuevos;
    m_ajustes.Guardar();
}

void VentanaPrincipal::AlTick(wxTimerEvent&)
{
    auto& p = m_trabajo.Progreso();
    // Con la lista completa se sabe cuánto se va a escribir; si no, se mide por lo leído del archivo comprimido
    int64_t esperado = p.totalEsperado, total = p.totalComprimido, leidos = p.leidos;
    int valor = 0;
    if (!m_listando && esperado > 0) valor = (int)std::min<int64_t>(1000, p.escritos * 1000 / esperado);
    else if (total > 0) valor = (int)std::min<int64_t>(1000, leidos * 1000 / total);
    m_barra->SetValue(valor);
    if (m_trabajo.Cancelando()) return;
    wxString actual = p.Actual();
    if (m_listando)
        m_estado->SetLabel(wxString::Format(L"Leyendo el contenido... %d %% · %s archivos", valor / 10, jicaro::TextoNumero(p.archivos)));
    else
        m_estado->SetLabel(wxString::Format(L"%d %% · %s archivos · %s · %s", valor / 10, jicaro::TextoNumero(p.archivos),
                                            jicaro::TextoTamano(p.escritos), actual));
}

void VentanaPrincipal::GuardarPosicion()
{
    m_ajustes.maximizada = IsMaximized();
    if (!IsMaximized() && !IsIconized())
    {
        wxRect r = GetRect();
        m_ajustes.x = r.x;
        m_ajustes.y = r.y;
        m_ajustes.ancho = r.width;
        m_ajustes.alto = r.height;
    }
    m_ajustes.Guardar();
}

void VentanaPrincipal::AlCerrar(wxCloseEvent& e)
{
    if (m_trabajo.Ocupado())
    {
        if (m_extrayendo && e.CanVeto()
            && ayuda::Aviso(L"Se está extrayendo. ¿Cancelar y salir?", L"Jícaro", wxYES_NO | wxICON_QUESTION, this) != wxID_YES)
        {
            e.Veto();
            return;
        }
        // Se cancela y se cierra cuando el trabajo termine (lo que quedó a medias se quita)
        m_trabajo.Cancelar();
        m_cerrarAlTerminar = true;
        m_despuesDeListar = [this]() { Close(); };
        m_estado->SetLabel(L"Cancelando...");
        Disable();
        if (e.CanVeto())
        {
            e.Veto();
            return;
        }
    }
    GuardarPosicion();
    m_reloj.Stop();
    Destroy();
}
