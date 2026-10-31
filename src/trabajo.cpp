#include "trabajo.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>

#include "comun.hpp"
#include "dialogos.hpp"
#include "hilos.hpp"
#include "util.hpp"

Trabajo::~Trabajo()
{
    *m_vivo = false;
    m_cancelar = true;
    if (m_hilo.joinable()) m_hilo.join();
}

void Trabajo::Esperar()
{
    if (m_hilo.joinable()) m_hilo.join();
}

template <class T> T Trabajo::Preguntar(std::function<T()> f, T siNo)
{
    struct Estado
    {
        std::mutex m;
        std::condition_variable cv;
        bool listo = false, abandonado = false;
        std::optional<T> valor;
    };
    auto e = std::make_shared<Estado>();
    auto vivo = m_vivo;
    bool enviado = jicaro::hilos::Enviar([e, f, vivo]() {
        {
            std::lock_guard g(e->m);
            if (e->abandonado || !*vivo)
            {
                e->listo = true;
                e->cv.notify_all();
                return;
            }
        }
        T v = f();
        std::lock_guard g(e->m);
        e->valor = v;
        e->listo = true;
        e->cv.notify_all();
    });
    if (!enviado) return siNo;
    std::unique_lock l(e->m);
    while (!e->listo)
    {
        if (m_cancelar)
        {
            e->abandonado = true;
            return siNo;
        }
        e->cv.wait_for(l, std::chrono::milliseconds(100));
    }
    return e->valor ? *e->valor : siNo;
}

void Trabajo::Listar(std::vector<std::wstring> partes, std::wstring nombreSuelto, std::function<void(jicaro::Contenido)> alTerminar)
{
    Esperar();
    m_cancelar = false;
    m_ocupado = true;
    auto vivo = m_vivo;
    // Todo lo que puede fallar (también por falta de memoria) queda dentro de try: nada escapa del hilo
    auto entregar = [this, alTerminar, vivo](std::shared_ptr<jicaro::Contenido> c) {
        if (!jicaro::hilos::Enviar([this, c, alTerminar, vivo]() {
                if (!*vivo) return;
                m_ocupado = false;
                alTerminar(std::move(*c));
            }))
            m_ocupado = false;
    };
    try
    {
        auto resultado = std::make_shared<jicaro::Contenido>();
        m_hilo = std::thread([this, partes = std::move(partes), nombreSuelto = std::move(nombreSuelto), resultado, entregar]() {
            try
            {
                *resultado = jicaro::Listar(partes, nombreSuelto, m_progreso, m_cancelar);
            }
            catch (...)
            {
                resultado->mensaje = L"Algo salió mal al leer este archivo.";
            }
            try
            {
                entregar(resultado);
            }
            catch (...)
            {
                m_ocupado = false;
            }
        });
    }
    catch (...)
    {
        // No se pudo ni empezar (sin memoria o sin hilos): se avisa igual, desde la ventana
        m_ocupado = false;
        jicaro::Contenido c;
        c.mensaje = L"No se pudo empezar a leer el archivo.";
        alTerminar(std::move(c));
    }
}

void Trabajo::Extraer(jicaro::Opciones opciones, std::function<void(jicaro::Resumen)> alTerminar)
{
    Esperar();
    m_cancelar = false;
    m_ocupado = true;
    auto vivo = m_vivo;
    auto entregar = [this, alTerminar, vivo](std::shared_ptr<jicaro::Resumen> r) {
        if (!jicaro::hilos::Enviar([this, r, alTerminar, vivo]() {
                if (!*vivo) return;
                m_ocupado = false;
                alTerminar(std::move(*r));
            }))
            m_ocupado = false;
    };
    try
    {
        auto resultado = std::make_shared<jicaro::Resumen>();
        auto op = std::make_shared<jicaro::Opciones>(std::move(opciones));
        m_hilo = std::thread([this, op, resultado, entregar]() {
            try
            {
                std::wstring archivo = op->partes.empty() ? L"" : op->partes.front();
                wxWindow* ventana = m_ventana;
                jicaro::Preguntas pr;
                pr.contrasena = [this, archivo, ventana](const std::wstring& entrada, bool otraVez) {
                    return Preguntar<std::optional<std::wstring>>(
                        [=]() { return dialogos::PedirContrasena(ventana, archivo, entrada, otraVez); }, std::nullopt);
                };
                pr.existe = [this, ventana](const std::wstring& ruta, bool& paraTodos) {
                    auto r = Preguntar<std::pair<jicaro::Decision, bool>>(
                        [=]() {
                            bool todos = false;
                            auto d = dialogos::PreguntarSiExiste(ventana, ruta, todos);
                            return std::make_pair(d, todos);
                        },
                        std::make_pair(jicaro::Decision::Cancelar, false));
                    paraTodos = r.second;
                    return r.first;
                };
                pr.bomba = [this, ventana](const std::wstring& motivo) {
                    return Preguntar<bool>([=]() { return dialogos::PreguntarBomba(ventana, motivo); }, false);
                };
                *resultado = jicaro::Extraer(*op, pr, m_progreso, m_cancelar);
                jicaro::Registrar("extraer: " + jicaro::Utf8(jicaro::TextoEstado(resultado->estado)) + " (" +
                                  std::to_string(op->partes.size()) + " partes, " + std::to_string(resultado->archivos) +
                                  " archivos, " + std::to_string(resultado->rechazados) + " por seguridad, " +
                                  std::to_string(resultado->fallidos) + " con fallas, marca " +
                                  std::to_string(resultado->zona) + ")");
            }
            catch (...)
            {
                resultado->estado = jicaro::Estado::Error;
                resultado->mensaje = L"Algo salió mal al extraer.";
            }
            try
            {
                entregar(resultado);
            }
            catch (...)
            {
                m_ocupado = false;
            }
        });
    }
    catch (...)
    {
        m_ocupado = false;
        jicaro::Resumen r;
        r.mensaje = L"No se pudo empezar a extraer.";
        alTerminar(std::move(r));
    }
}