#pragma once

// Un trabajo (leer la lista o extraer) en un hilo aparte, para que la ventana no se congele. Las preguntas a la persona
// (contraseña, archivo que ya existe, posible bomba) se muestran en el hilo de la ventana mientras el hilo de trabajo
// espera la respuesta. Si se cancela mientras espera, la pregunta se da por contestada con «cancelar».

#include <wx/window.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "extractor.hpp"

class Trabajo
{
public:
    explicit Trabajo(wxWindow* ventana) : m_ventana(ventana) {}
    ~Trabajo();
    Trabajo(const Trabajo&) = delete;
    Trabajo& operator=(const Trabajo&) = delete;

    bool Ocupado() const { return m_ocupado; }
    void Cancelar() { m_cancelar = true; }
    bool Cancelando() const { return m_cancelar; }
    jicaro::Progreso& Progreso() { return m_progreso; }

    // «alTerminar» corre en el hilo de la ventana, una sola vez y solo si este objeto sigue vivo. Si hace falta una
    // contraseña, se pregunta en una ventana (en Extraer, solo con «preguntarClave»: la línea de comandos con
    // --contrasena-stdin no muestra preguntas de contraseña).
    void Listar(std::vector<std::wstring> partes, std::wstring nombreSuelto, std::function<void(jicaro::Contenido)> alTerminar);
    void Extraer(jicaro::Opciones opciones, std::function<void(jicaro::Resumen)> alTerminar, bool preguntarClave = true);

    // Si el aviso de «terminé» no pudo llegar a la ventana (por ejemplo, sin memoria), queda guardado: el reloj de la
    // ventana lo recoge con esto, para que nunca se quede trabada en «extrayendo»
    void RecogerPendiente();

private:
    template <class T> T Preguntar(std::function<T()> f, T siNo);
    void Esperar();
    // Avisa a la ventana; lo llama el hilo de trabajo al final. No reserva memoria si falla el envío.
    void Entregar();

    wxWindow* m_ventana;
    std::thread m_hilo;
    std::atomic<bool> m_ocupado{ false }, m_cancelar{ false };
    jicaro::Progreso m_progreso;
    std::shared_ptr<std::atomic<bool>> m_vivo = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::function<void()>> m_aviso;   // lo que corre en la ventana al terminar (se arma antes de empezar)
    std::atomic<bool> m_avisoPendiente{ false };
};
