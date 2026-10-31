#include "progreso.hpp"

#include <windows.h>

#include <wx/button.h>
#include <wx/gauge.h>
#include <wx/icon.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <functional>

#include "ayuda.hpp"
#include "comun.hpp"
#include "dialogos.hpp"
#include "rutas.hpp"
#include "util.hpp"

namespace
{
    enum { RELOJ = 1, CIERRE = 2 };

    // Con «Extraer en una carpeta con su nombre» el destino es una carpeta nueva al lado del archivo
    // (si ya hay un archivo, un enlace o una unión con ese nombre, se usa otro)
    std::wstring CarpetaNueva(const jicaro::volumenes::Juego& j)
    {
        return jicaro::rutas::CarpetaNuevaSegura(j.carpeta, j.nombreBase);
    }
}

VentanaProgreso::VentanaProgreso(const std::wstring& archivo, Modo modo, const std::wstring& destino, jicaro::SiExiste siExiste)
    : wxFrame(nullptr, wxID_ANY, L"Jícaro: extrayendo", wxDefaultPosition, wxDefaultSize, wxDEFAULT_FRAME_STYLE & ~wxMAXIMIZE_BOX),
      m_reloj(this, RELOJ), m_cierre(this, CIERRE), m_archivo(archivo), m_destino(destino), m_modo(modo), m_siExiste(siExiste)
{
    SetIcon(wxICON(AAAA_APP));
    m_panel = new wxPanel(this);
    auto* caja = new wxBoxSizer(wxVERTICAL);
    const int margen = FromDIP(14);
    m_titulo = new wxStaticText(m_panel, wxID_ANY, L"", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_MIDDLE);
    m_titulo->SetFont(m_titulo->GetFont().Bold());
    m_donde = new wxStaticText(m_panel, wxID_ANY, L"", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_MIDDLE);
    m_barra = new wxGauge(m_panel, wxID_ANY, 1000, wxDefaultPosition, wxSize(FromDIP(480), -1));
    m_estado = new wxStaticText(m_panel, wxID_ANY, L"Preparando...", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_MIDDLE);
    m_resumen = new wxStaticText(m_panel, wxID_ANY, L"");
    m_resumen->Hide();
    m_lista = new wxTextCtrl(m_panel, wxID_ANY, L"", wxDefaultPosition, wxSize(-1, FromDIP(130)),
                             wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
    m_lista->Hide();
    // Cada control con el mismo margen a los lados; la separación de arriba va aparte
    caja->AddSpacer(margen);
    caja->Add(m_titulo, 0, wxEXPAND | wxLEFT | wxRIGHT, margen);
    caja->AddSpacer(FromDIP(4));
    caja->Add(m_donde, 0, wxEXPAND | wxLEFT | wxRIGHT, margen);
    caja->AddSpacer(margen);
    caja->Add(m_barra, 0, wxEXPAND | wxLEFT | wxRIGHT, margen);
    caja->AddSpacer(FromDIP(8));
    caja->Add(m_estado, 0, wxEXPAND | wxLEFT | wxRIGHT, margen);
    caja->Add(m_resumen, 0, wxEXPAND | wxLEFT | wxRIGHT, margen);
    caja->AddSpacer(FromDIP(8));
    caja->Add(m_lista, 1, wxEXPAND | wxLEFT | wxRIGHT, margen);
    auto* botones = new wxBoxSizer(wxHORIZONTAL);
    botones->AddStretchSpacer();
    m_abrir = new wxButton(m_panel, wxID_OPEN, L"Abrir la carpeta");
    m_boton = new wxButton(m_panel, wxID_CANCEL, L"Cancelar");
    m_abrir->Hide();
    botones->Add(m_abrir, 0, wxRIGHT, FromDIP(6));
    botones->Add(m_boton, 0);
    caja->Add(botones, 0, wxEXPAND | wxALL, margen);
    m_panel->SetSizer(caja);
    caja->SetSizeHints(this);
    SetSize(FromDIP(wxSize(540, -1)).GetWidth(), GetSize().GetHeight());
    Centre();

    m_abrir->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        jicaro::AbrirCarpeta(m_destino);
        Close();
    });
    m_boton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
    Bind(wxEVT_TIMER, &VentanaProgreso::AlTick, this, RELOJ);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { Close(); }, CIERRE);
    Bind(wxEVT_CLOSE_WINDOW, &VentanaProgreso::AlCerrar, this);
}

VentanaProgreso::~VentanaProgreso()
{
    if (m_candado) CloseHandle((HANDLE)m_candado);
}

bool VentanaProgreso::Empezar()
{
    m_inicio = std::chrono::steady_clock::now();
    m_juego = jicaro::volumenes::Buscar(m_archivo);
    std::wstring primera = m_juego.partes.empty() ? m_archivo : m_juego.partes.front();

    // Si se eligen varias partes del mismo archivo y se pulsa «Extraer aquí», Windows abre una ventana por cada parte:
    // solo la primera extrae
    std::wstring clave = jicaro::Minusculas(jicaro::RutaCompleta(m_juego.partes.empty() ? m_archivo : m_juego.partes.front()));
    if (m_juego.Varias() && !m_juego.faltan.empty()) clave = jicaro::Minusculas(jicaro::RutaCompleta(m_archivo));
    wchar_t nombre[64];
    swprintf(nombre, 64, L"Local\\Jicaro-Extraer-%016llx", (unsigned long long)std::hash<std::wstring>{}(clave));
    HANDLE candado = CreateMutexW(nullptr, FALSE, nombre);
    if (candado && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(candado);
        PonerCodigoDeSalida(SALIDA_YA_EXTRAYENDO);
        return false;
    }
    m_candado = candado;

    wxString titulo = L"«" + wxString(jicaro::NombreDe(primera)) + L"»";
    if (m_juego.Varias()) titulo += wxString::Format(L" (%zu partes)", m_juego.partes.size());
    m_nombre = titulo;
    m_titulo->SetLabel(L"Extrayendo " + titulo);
    Show();

    if (!m_juego.faltan.empty() || m_juego.seguidas == 0 || m_juego.partes.empty())
    {
        wxString faltan;
        for (size_t i = 0; i < m_juego.faltan.size() && i < 8; i++) faltan += (i ? L", " : L"") + wxString(m_juego.faltan[i]);
        if (m_juego.faltan.size() > 8) faltan += wxString::Format(L" y %zu más", m_juego.faltan.size() - 8);
        m_titulo->SetLabel(L"No se puede extraer " + titulo);
        Terminar(SALIDA_FALTA_PARTE,
                 L"Faltan partes: " + faltan + L".\nBájalas y ponlas en la misma carpeta que las demás; después vuelve a intentarlo.",
                 L"", false);
        return true;
    }

    switch (m_modo)
    {
    case Modo::Aqui: m_destino = m_juego.carpeta; break;
    case Modo::Carpeta: m_destino = CarpetaNueva(m_juego); break;
    case Modo::Destino: m_destino = jicaro::RutaCompleta(m_destino); break;
    }
    m_donde->SetLabel(L"En: " + wxString(m_destino));

    jicaro::Opciones op;
    op.partes = m_juego.partes;
    op.destino = m_destino;
    op.siExiste = m_siExiste;
    op.nombreSuelto = m_juego.nombreSuelto;
    m_reloj.Start(150);
    m_trabajo.Extraer(op, [this](jicaro::Resumen r) { AlTerminar(r); });
    Raise();
    return true;
}

void VentanaProgreso::AlTick(wxTimerEvent&)
{
    m_trabajo.RecogerPendiente();   // por si el aviso de «terminé» no pudo llegar solo
    if (m_terminado) return;
    auto& p = m_trabajo.Progreso();
    int64_t total = p.totalComprimido, leidos = p.leidos;
    int valor = total > 0 ? (int)std::min<int64_t>(1000, leidos * 1000 / total) : 0;
    m_barra->SetValue(valor);
    if (m_trabajo.Cancelando()) return;
    m_estado->SetLabel(wxString::Format(L"%d %% · %s archivos · %s · %s", valor / 10, jicaro::TextoNumero(p.archivos),
                                        jicaro::TextoTamano(p.escritos), p.Actual()));
}

void VentanaProgreso::AlTerminar(const jicaro::Resumen& r)
{
    m_reloj.Stop();
    int codigo = SALIDA_ERROR;
    switch (r.estado)
    {
    case jicaro::Estado::Bien:
        if (r.cifradas > 0) codigo = SALIDA_CONTRASENA;
        else if (r.rechazados > 0 || r.fallidos > 0 || r.quitadosPorWindows > 0 || !r.aMedias.empty()) codigo = SALIDA_PARCIAL;
        else codigo = SALIDA_BIEN;
        break;
    case jicaro::Estado::Cancelado:
    case jicaro::Estado::Detenido: codigo = SALIDA_CANCELADO; break;
    case jicaro::Estado::FaltaParte: codigo = SALIDA_FALTA_PARTE; break;
    case jicaro::Estado::CifradoNoSoportado:
    case jicaro::Estado::SinContrasena: codigo = SALIDA_CONTRASENA; break;
    default: codigo = SALIDA_ERROR;
    }
    if (r.aMedias.empty() && (m_cerrarAlTerminar || (r.estado == jicaro::Estado::Cancelado && r.archivos == 0)))
    {
        PonerCodigoDeSalida(codigo);
        m_terminado = true;
        Close();
        return;
    }
    bool limpio = r.Limpio();
    m_barra->SetValue(limpio ? 1000 : m_barra->GetValue());
    if (codigo == SALIDA_CANCELADO || codigo == SALIDA_ERROR) m_barra->Hide();   // la barra llena confundiría
    m_abrir->Show(r.archivos > 0 && jicaro::ExisteCarpeta(m_destino));
    Terminar(codigo, dialogos::TextoResumen(r, m_destino), dialogos::TextoOmitidas(r), limpio);
}

void VentanaProgreso::Terminar(int codigo, const wxString& texto, const wxString& lista, bool cerrarSola)
{
    m_reloj.Stop();
    m_terminado = true;
    PonerCodigoDeSalida(codigo);
    SetTitle(codigo == SALIDA_BIEN ? L"Jícaro: listo" : L"Jícaro");
    if (!m_nombre.empty() && codigo != SALIDA_FALTA_PARTE)
        m_titulo->SetLabel((codigo == SALIDA_BIEN ? L"Extraído " : (codigo == SALIDA_CANCELADO ? L"Detenido: " : L"Con problemas: ")) + m_nombre);
    m_estado->Hide();
    m_resumen->SetLabel(texto);
    m_resumen->Wrap(FromDIP(500));
    m_resumen->Show();
    m_boton->SetLabel(L"Cerrar");
    if (!lista.empty())
    {
        m_lista->ChangeValue(lista);
        m_lista->Show();
    }
    m_panel->GetSizer()->SetSizeHints(this);
    m_panel->Layout();
    Fit();
    if (cerrarSola)
    {
        // Si todo salió bien se cierra sola, después de mostrar «Listo» un momento
        auto pasado = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_inicio).count();
        m_cierre.StartOnce((int)std::max<long long>(1200, 2000 - pasado));
    }
    else
    {
        Raise();
        RequestUserAttention();
    }
}

void VentanaProgreso::AlCerrar(wxCloseEvent& e)
{
    if (m_trabajo.Ocupado())
    {
        if (e.CanVeto())
        {
            if (!m_trabajo.Cancelando()
                && ayuda::Aviso(L"¿Cancelar la extracción? Lo que ya salió se queda; el archivo que estaba a medias se quita.",
                                L"Jícaro", wxYES_NO | wxICON_QUESTION, this) != wxID_YES)
            {
                e.Veto();
                return;
            }
            m_trabajo.Cancelar();
            m_cerrarAlTerminar = true;
            m_estado->SetLabel(L"Cancelando...");
            m_boton->Disable();
            e.Veto();
            return;
        }
        m_trabajo.Cancelar();
        PonerCodigoDeSalida(SALIDA_CANCELADO);
    }
    m_reloj.Stop();
    m_cierre.Stop();
    Destroy();
}
