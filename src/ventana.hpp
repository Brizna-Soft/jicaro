#pragma once

// Ventana principal: abrir un archivo comprimido (también arrastrándolo), ver lo que trae y extraer todo o lo elegido.

#include <wx/frame.h>
#include <wx/listctrl.h>
#include <wx/timer.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "ajustes.hpp"
#include "extractor.hpp"
#include "trabajo.hpp"
#include "volumenes.hpp"

class wxBoxSizer;
class wxButton;
class wxGauge;
class wxPanel;
class wxStaticText;

// Ids de los comandos (sirven también para las pruebas, que mandan WM_COMMAND)
enum
{
    ID_EXTRAER_TODO = wxID_HIGHEST + 100,
    ID_EXTRAER_ELEGIDO,
    ID_CANCELAR_TRABAJO,
};

// Lista del contenido. Es «virtual»: aguanta cientos de miles de archivos sin trabarse.
class ListaContenido : public wxListCtrl
{
public:
    explicit ListaContenido(wxWindow* padre);
    void Poner(const std::vector<jicaro::Entrada>* entradas);
    wxString OnGetItemText(long fila, long columna) const override;

private:
    const std::vector<jicaro::Entrada>* m_entradas = nullptr;
};

class VentanaPrincipal : public wxFrame
{
public:
    explicit VentanaPrincipal(jicaro::Ajustes& ajustes);
    void AbrirArchivo(const std::wstring& ruta);

private:
    void CrearControles();
    void Extraer(bool soloElegido);
    void AlTerminarLista(jicaro::Contenido c);
    void AlTerminarExtraccion(const jicaro::Resumen& r, const std::wstring& destino, bool abrir);
    void MostrarAjustes();
    void ActualizarBotones();
    void Ocupar(const wxString& texto);
    void Liberar(const wxString& texto);
    void PonerAviso(const wxString& texto, bool grave);
    void AlTick(wxTimerEvent&);
    void AlCerrar(wxCloseEvent& e);
    void GuardarPosicion();

    jicaro::Ajustes& m_ajustes;
    Trabajo m_trabajo{ this };
    jicaro::volumenes::Juego m_juego;
    jicaro::Contenido m_contenido;
    bool m_hayArchivo = false, m_listando = false, m_extrayendo = false, m_cerrarAlTerminar = false;
    std::function<void()> m_despuesDeListar;
    wxTimer m_reloj;
    std::optional<std::wstring> m_clave;   // la contraseña que ya sirvió con este archivo (se borra al abrir otro)
    void BorrarClave();

    wxPanel* m_panel = nullptr;
    wxBoxSizer* m_caja = nullptr;
    wxButton* m_botonAbrir = nullptr;
    wxButton* m_botonTodo = nullptr;
    wxButton* m_botonElegido = nullptr;
    wxButton* m_botonAjustes = nullptr;
    wxButton* m_botonCancelar = nullptr;
    wxStaticText* m_titulo = nullptr;
    wxStaticText* m_detalle = nullptr;
    wxStaticText* m_aviso = nullptr;
    wxStaticText* m_vacio = nullptr;
    wxStaticText* m_estado = nullptr;
    ListaContenido* m_lista = nullptr;
    wxGauge* m_barra = nullptr;
};
