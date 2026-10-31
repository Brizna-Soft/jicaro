// Jícaro: descompresor gratis de Brizna Soft.
//
// Uso:
//   Jicaro.exe                          ventana vacía (se le puede arrastrar un archivo)
//   Jicaro.exe "<archivo>"              ventana con ese archivo abierto
//   Jicaro.exe --extraer "<archivo>" [--destino "<carpeta>" | --aqui] [--si-existe preguntar|reemplazar|saltar|renombrar]
//                                       extrae con una ventana chica de progreso (la usan el clic derecho y Atarraya).
//                                       Sin --destino ni --aqui, extrae en una carpeta nueva con el nombre del archivo,
//                                       al lado de él. Códigos de salida: ver progreso.hpp.
//   Jicaro.exe --registrar-menu         agrega «Extraer aquí» al clic derecho (lo mismo que la casilla de Ajustes)
//   Jicaro.exe --quitar-menu            lo quita

#include <windows.h>

#include <wx/app.h>

#include <clocale>
#include <optional>
#include <vector>

#include "ajustes.hpp"
#include "ayuda.hpp"
#include "comun.hpp"
#include "hilos.hpp"
#include "menucontextual.hpp"
#include "progreso.hpp"
#include "util.hpp"
#include "ventana.hpp"
#include "version.h"

namespace
{
    int codigoSalida = 0;

    std::wstring EsteExe()
    {
        std::vector<wchar_t> buf(32768);
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        return std::wstring(buf.data(), n);
    }
}

void PonerCodigoDeSalida(int codigo)
{
    codigoSalida = codigo;
}

class Aplicacion : public wxApp
{
public:
    bool OnInit() override;
    int OnRun() override
    {
        int r = wxApp::OnRun();
        return m_conCodigo ? codigoSalida : r;
    }
    int OnExit() override
    {
        jicaro::hilos::Cerrar();   // los hilos que terminen tarde ya no mandan nada a la aplicación
        return wxApp::OnExit();
    }

private:
    void Salir(int codigo)
    {
        m_conCodigo = true;
        codigoSalida = codigo;
        CallAfter([this]() { ExitMainLoop(); });
    }

    jicaro::Ajustes m_ajustes;
    bool m_conCodigo = false;
};

wxIMPLEMENT_APP(Aplicacion);

bool Aplicacion::OnInit()
{
    // Los nombres de los ZIP y RAR viejos vienen en la página de códigos de Windows: libarchive necesita conocerla
    setlocale(LC_CTYPE, "");

    std::wstring archivo, destino;
    bool extraer = false, aqui = false, registrar = false, quitar = false;
    std::optional<jicaro::SiExiste> siExiste;
    for (int i = 1; i < argc; i++)
    {
        std::wstring a = argv[i].ToStdWstring();
        if (a == L"--extraer" && i + 1 < argc)
        {
            extraer = true;
            archivo = argv[++i].ToStdWstring();
        }
        else if (a == L"--destino" && i + 1 < argc) destino = argv[++i].ToStdWstring();
        else if (a == L"--aqui") aqui = true;
        else if (a == L"--si-existe" && i + 1 < argc)
        {
            std::wstring v = jicaro::Minusculas(argv[++i].ToStdWstring());
            if (v == L"reemplazar") siExiste = jicaro::SiExiste::Reemplazar;
            else if (v == L"saltar") siExiste = jicaro::SiExiste::Saltar;
            else if (v == L"renombrar") siExiste = jicaro::SiExiste::Renombrar;
            else siExiste = jicaro::SiExiste::Preguntar;
        }
        else if (a == L"--registrar-menu") registrar = true;
        else if (a == L"--quitar-menu") quitar = true;
        else if (!a.empty() && a[0] != L'-' && archivo.empty()) archivo = a;
    }
    // Rutas completas ya, antes de que nada cambie la carpeta de trabajo
    if (!archivo.empty()) archivo = jicaro::RutaCompleta(archivo);
    if (!destino.empty()) destino = jicaro::RutaCompleta(destino);
    m_ajustes.Cargar();
    std::wstring exe = EsteExe();

    if (registrar || quitar)
    {
        bool ok = registrar ? jicaro::menu::Registrar(exe) : jicaro::menu::Quitar();
        if (ok)
        {
            m_ajustes.menuContextual = registrar;
            m_ajustes.Guardar();
        }
        jicaro::Registrar(std::string(registrar ? "registrar menu: " : "quitar menu: ") + (ok ? "bien" : "falla"));
        Salir(ok ? 0 : 1);
        return true;
    }

    MSWEnableDarkMode();   // claro u oscuro, como Windows
    SetAppDisplayName(wxString::FromUTF8(APP_NOMBRE));
    // Si la persona movió la carpeta del programa, el clic derecho se registra de nuevo con la ruta nueva
    if (m_ajustes.menuContextual && !jicaro::menu::ApuntaA(exe)) jicaro::menu::Registrar(exe);

    if (extraer)
    {
        jicaro::Registrar(std::string("Jicaro ") + APP_VERSION_TEXTO + " --extraer" + (aqui ? " --aqui" : "") +
                          (destino.empty() ? "" : " --destino"));
        m_conCodigo = true;
        codigoSalida = SALIDA_ERROR;
        if (archivo.empty() || !jicaro::ExisteArchivo(archivo))
        {
            ayuda::Aviso(L"No encuentro el archivo que hay que extraer. ¿Se movió o se borró?", L"Jícaro", wxOK | wxICON_WARNING);
            Salir(SALIDA_ERROR);
            return true;
        }
        auto modo = !destino.empty() ? VentanaProgreso::Modo::Destino
                                     : (aqui ? VentanaProgreso::Modo::Aqui : VentanaProgreso::Modo::Carpeta);
        auto* v = new VentanaProgreso(archivo, modo, destino, siExiste.value_or(m_ajustes.siExiste));
        if (!v->Empezar())
        {
            v->Destroy();
            Salir(codigoSalida);
        }
        return true;
    }

    jicaro::Registrar(std::string("Jicaro ") + APP_VERSION_TEXTO);
    auto* v = new VentanaPrincipal(m_ajustes);
    v->Show();
    if (!archivo.empty()) v->CallAfter([v, archivo]() { v->AbrirArchivo(archivo); });
    return true;
}
