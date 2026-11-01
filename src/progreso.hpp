#pragma once

// Ventana chica de progreso para «Jicaro.exe --extraer»: la usan el clic derecho («Extraer aquí», «Extraer en una
// carpeta con su nombre») y Atarraya. Si todo sale bien se cierra sola; si algo falló o se saltó algo, se queda abierta
// con el resumen.

#include <wx/frame.h>
#include <wx/timer.h>

#include <chrono>
#include <optional>
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
    SALIDA_CONTRASENA = 4,      // hace falta una contraseña y no se dio, o es un cifrado que Jícaro no abre
    SALIDA_YA_EXTRAYENDO = 5,   // otra ventana de Jícaro ya está extrayendo ese mismo archivo
    SALIDA_PARCIAL = 6,         // salió solo una parte: algo se saltó por seguridad o no se pudo extraer
    SALIDA_CONTRASENA_MALA = 7, // se dio una contraseña y no sirvió (para todo o para una parte)
};

// La pone la ventana de progreso; la devuelve el programa al terminar
void PonerCodigoDeSalida(int codigo);

class VentanaProgreso : public wxFrame
{
public:
    enum class Modo { Carpeta, Aqui, Destino };

    // «clave»: la que llegó por --contrasena-stdin. Con ella no se muestran preguntas de contraseña: si no sirve, sale
    // con SALIDA_CONTRASENA_MALA.
    VentanaProgreso(const std::wstring& archivo, Modo modo, const std::wstring& destino, jicaro::SiExiste siExiste,
                    std::optional<std::wstring> clave = std::nullopt, bool claveDeStdin = false);
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
    std::optional<std::wstring> m_clave;
    bool m_claveDeStdin = false;

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
