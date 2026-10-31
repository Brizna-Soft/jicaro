#pragma once

// Ventana chica de progreso para «Jicaro.exe --extraer»: la usan el clic derecho («Extraer aquí», «Extraer en una
// carpeta con su nombre») y Atarraya. Si todo sale bien se cierra sola; si algo falló o se saltó algo, se queda abierta
// con el resumen.

#include <wx/frame.h>
#include <wx/timer.h>

#include <chrono>
#include <string>

#include "extractor.hpp"
#include "trabajo.hpp"
#include "volumenes.hpp"

class wxButton;
class wxGauge;
class wxPanel;
class wxStaticText;
class wxTextCtrl;

// Códigos de salida de «Jicaro.exe --extraer» (para Atarraya y otros programas)
enum CodigoSalida
{
    SALIDA_BIEN = 0,
    SALIDA_ERROR = 1,
    SALIDA_CANCELADO = 2,
    SALIDA_FALTA_PARTE = 3,
    SALIDA_CONTRASENA = 4,      // pide una contraseña que no se dio, o un cifrado que Jícaro no abre
    SALIDA_YA_EXTRAYENDO = 5,   // otra ventana de Jícaro ya está extrayendo ese mismo archivo
};

// La pone la ventana de progreso; la devuelve el programa al terminar
void PonerCodigoDeSalida(int codigo);

class VentanaProgreso : public wxFrame
{
public:
    enum class Modo { Carpeta, Aqui, Destino };

    VentanaProgreso(const std::wstring& archivo, Modo modo, const std::wstring& destino, jicaro::SiExiste siExiste);
    ~VentanaProgreso() override;

    // false si no hay nada que hacer (por ejemplo, otra ventana ya extrae ese archivo): no se muestra
    bool Empezar();

private:
    void AlTick(wxTimerEvent&);
    void AlTerminar(const jicaro::Resumen& r);
    void Terminar(int codigo, const wxString& texto, const wxString& lista, bool cerrarSola);
    void AlCerrar(wxCloseEvent& e);

    Trabajo m_trabajo{ this };
    wxTimer m_reloj, m_cierre;
    std::wstring m_archivo, m_destino;
    Modo m_modo;
    jicaro::SiExiste m_siExiste;
    jicaro::volumenes::Juego m_juego;
    wxString m_nombre;   // «archivo» (8 partes), para el título
    bool m_terminado = false, m_cerrarAlTerminar = false;
    std::chrono::steady_clock::time_point m_inicio;
    void* m_candado = nullptr;   // mutex con nombre: una sola ventana por archivo

    wxPanel* m_panel = nullptr;
    wxStaticText* m_titulo = nullptr;
    wxStaticText* m_donde = nullptr;
    wxStaticText* m_estado = nullptr;
    wxStaticText* m_resumen = nullptr;
    wxGauge* m_barra = nullptr;
    wxTextCtrl* m_lista = nullptr;
    wxButton* m_abrir = nullptr;
    wxButton* m_boton = nullptr;
};
