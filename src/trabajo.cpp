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

namespace
{
    // Marca el error y pone el texto; si ni para el texto hay memoria, se avisa igual (sin texto)
    template <class R> void PonerError(R& r, const wchar_t* texto)
    {
        r.estado = jicaro::Estado::Error;
        try
        {
            r.mensaje = texto;
        }
        catch (...)
        {
        }
    }

    constexpr const wchar_t* kNoEmpezoLista = L"No se pudo empezar a leer el archivo. Cierra otros programas y vuelve a intentarlo.";
    constexpr const wchar_t* kNoEmpezoExtraer = L"No se pudo empezar a extraer. Cierra otros programas y vuelve a intentarlo.";
}

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

void Trabajo::Entregar()
{
    bool enviado = false;
    try
    {
        auto aviso = m_aviso;
        enviado = aviso && jicaro::hilos::Enviar([aviso]() { (*aviso)(); });
    }
    catch (...)
    {
    }
    if (!enviado) m_avisoPendiente = true;   // lo recoge el reloj de la ventana
}

void Trabajo::RecogerPendiente()
{
    if (m_avisoPendiente.exchange(false) && m_aviso)
    {
        auto aviso = m_aviso;
        (*aviso)();
    }
}

void Trabajo::Listar(std::vector<std::wstring> partes, std::wstring nombreSuelto, std::function<void(jicaro::Contenido)> alTerminar)
{
    Esperar();
    m_cancelar = false;
    m_ocupado = true;
    m_avisoPendiente = false;
    // Todo lo que reserva memoria se arma aquí, antes de empezar: al final el hilo solo avisa. «alTerminar» se copia (no
    // se mueve) para poder usarlo directo si algo de esto falla.
    std::shared_ptr<jicaro::Contenido> resultado;
    std::shared_ptr<std::function<void()>> aviso;
    try
    {
        resultado = std::make_shared<jicaro::Contenido>();
        auto hecho = std::make_shared<std::atomic<bool>>(false);
        auto vivo = m_vivo;
        aviso = std::make_shared<std::function<void()>>([this, resultado, alTerminar, vivo, hecho]() {
            if (!*vivo || hecho->exchange(true)) return;
            m_ocupado = false;
            alTerminar(std::move(*resultado));
        });
    }
    catch (...)
    {
        // Sin memoria ni para empezar: se avisa directo a la ventana (sale de «leyendo»), sin depender de m_aviso
        m_ocupado = false;
        jicaro::Contenido c;
        PonerError(c, kNoEmpezoLista);
        alTerminar(std::move(c));
        return;
    }
    m_aviso = aviso;
    try
    {
        m_hilo = std::thread([this, partes = std::move(partes), nombreSuelto = std::move(nombreSuelto), resultado]() {
            try
            {
                *resultado = jicaro::Listar(partes, nombreSuelto, m_progreso, m_cancelar);
            }
            catch (...)
            {
                try
                {
                    resultado->mensaje = L"Algo salió mal al leer este archivo.";
                }
                catch (...)
                {
                }
            }
            Entregar();
        });
    }
    catch (...)
    {
        // No se pudo crear el hilo: se avisa ya, con un error
        PonerError(*resultado, kNoEmpezoLista);
        (*aviso)();
    }
}

void Trabajo::Extraer(jicaro::Opciones opciones, std::function<void(jicaro::Resumen)> alTerminar)
{
    Esperar();
    m_cancelar = false;
    m_ocupado = true;
    m_avisoPendiente = false;
    std::shared_ptr<jicaro::Resumen> resultado;
    std::shared_ptr<std::function<void()>> aviso;
    std::shared_ptr<jicaro::Opciones> op;
    try
    {
        resultado = std::make_shared<jicaro::Resumen>();
        auto hecho = std::make_shared<std::atomic<bool>>(false);
        auto vivo = m_vivo;
        aviso = std::make_shared<std::function<void()>>([this, resultado, alTerminar, vivo, hecho]() {
            if (!*vivo || hecho->exchange(true)) return;
            m_ocupado = false;
            alTerminar(std::move(*resultado));
        });
        op = std::make_shared<jicaro::Opciones>(std::move(opciones));
    }
    catch (...)
    {
        // Sin memoria ni para empezar: se avisa directo a la ventana (sale de «extrayendo»), sin depender de m_aviso
        m_ocupado = false;
        jicaro::Resumen r;
        PonerError(r, kNoEmpezoExtraer);
        alTerminar(std::move(r));
        return;
    }
    m_aviso = aviso;
    try
    {
        m_hilo = std::thread([this, op, resultado]() {
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
                // Si ya había un resultado de Extraer (con lo que quedó a medias), se conserva; si no, se marca el error
                if (resultado->estado == jicaro::Estado::Bien && resultado->archivos == 0 && resultado->omitidas.empty())
                    resultado->estado = jicaro::Estado::Error;
                try
                {
                    if (resultado->mensaje.empty()) resultado->mensaje = L"Algo salió mal al extraer.";
                }
                catch (...)
                {
                }
            }
            Entregar();
        });
    }
    catch (...)
    {
        // No se pudo crear el hilo: se avisa ya, con un error
        PonerError(*resultado, kNoEmpezoExtraer);
        (*aviso)();
    }
}
