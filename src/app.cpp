// Jícaro: descompresor gratis de Brizna Soft.
//
// Uso:
//   Jicaro.exe                          ventana vacía (se le puede arrastrar un archivo)
//   Jicaro.exe "<archivo>"              ventana con ese archivo abierto
//   Jicaro.exe --extraer "<archivo>" [--destino "<carpeta>" | --aqui] [--si-existe preguntar|reemplazar|saltar|renombrar]
//              [--contrasena-stdin]
//                                       extrae con una ventana chica de progreso (la usan el clic derecho y Atarraya).
//                                       Sin --destino ni --aqui, extrae en una carpeta nueva con el nombre del archivo,
//                                       al lado de él. Con --contrasena-stdin, la contraseña llega por la entrada estándar
//                                       (una línea en UTF-8) y no se pregunta en una ventana. Nunca va en los
//                                       argumentos. Códigos de salida: ver progreso.hpp.
//   Jicaro.exe --registrar-menu         agrega «Extraer aquí» al clic derecho (lo mismo que la casilla de Ajustes)
//   Jicaro.exe --quitar-menu            lo quita

#include <windows.h>

#include <wx/app.h>
#include <wx/log.h>

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

    enum class Lectura { Bien, Vacia, NoEsUtf8 };

    // La contraseña de --contrasena-stdin: la primera línea de la entrada estándar, en UTF-8 (sin el salto de línea).
    // Espera hasta 15 segundos a que llegue. No se escribe en ningún lado y se borra de la memoria al terminar.
    Lectura LeerClaveDeEntrada(std::wstring& clave)
    {
        clave.clear();
        HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
        if (!h || h == INVALID_HANDLE_VALUE) return Lectura::Vacia;
        DWORD tipo = GetFileType(h);
        if (tipo != FILE_TYPE_PIPE && tipo != FILE_TYPE_DISK) return Lectura::Vacia;   // una consola: no se espera nada
        std::string bytes;
        bytes.reserve(256);
        const ULONGLONG inicio = GetTickCount64();
        while (bytes.size() < 4096)
        {
            if (tipo == FILE_TYPE_PIPE)
            {
                DWORD hay = 0;
                if (!PeekNamedPipe(h, nullptr, 0, nullptr, &hay, nullptr)) break;   // se cerró sin más datos
                if (hay == 0)
                {
                    if (GetTickCount64() - inicio > 15000) break;
                    Sleep(10);
                    continue;
                }
            }
            char c = 0;
            DWORD n = 0;
            if (!ReadFile(h, &c, 1, &n, nullptr) || n == 0) break;
            if (c == '\n') break;
            bytes.push_back(c);
        }
        if (!bytes.empty() && bytes.back() == '\r') bytes.pop_back();
        size_t desde = bytes.rfind("\xEF\xBB\xBF", 0) == 0 ? 3 : 0;   // sin la marca BOM, si vino
        Lectura r = Lectura::Vacia;
        if (bytes.size() > desde)
        {
            int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + desde, (int)(bytes.size() - desde), nullptr, 0);
            if (n <= 0) r = Lectura::NoEsUtf8;
            else
            {
                clave.resize((size_t)n);
                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + desde, (int)(bytes.size() - desde), clave.data(), n);
                r = Lectura::Bien;
            }
        }
        if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size());
        return r;
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
        ayuda::Cerrar();           // tampoco «Buscar actualización»
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
    bool extraer = false, aqui = false, registrar = false, quitar = false, claveStdin = false, claveEnArgumentos = false;
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
        else if (a == L"--contrasena-stdin") claveStdin = true;
        else if (jicaro::EmpiezaSinMayusculas(a, L"--contrasena") || jicaro::EmpiezaSinMayusculas(a, L"--password")
                 || jicaro::EmpiezaSinMayusculas(a, L"--clave"))
            claveEnArgumentos = true;   // nunca se acepta una contraseña en los argumentos (se verían en la lista de procesos)
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

    // wxWidgets avisa de sus propias fallas internas con ventanas en inglés: esas van al registro, no a la pantalla
    delete wxLog::SetActiveTarget(new wxLogStderr());
    MSWEnableDarkMode();   // claro u oscuro, como Windows
    SetAppDisplayName(wxString::FromUTF8(APP_NOMBRE));
    // Si la persona movió la carpeta del programa, el clic derecho se registra de nuevo con la ruta nueva
    if (m_ajustes.menuContextual && !jicaro::menu::ApuntaA(exe)) jicaro::menu::Registrar(exe);

    if (extraer)
    {
        jicaro::Registrar(std::string("Jicaro ") + APP_VERSION_TEXTO + " --extraer" + (aqui ? " --aqui" : "") +
                          (destino.empty() ? "" : " --destino") + (claveStdin ? " --contrasena-stdin" : ""));
        m_conCodigo = true;
        codigoSalida = SALIDA_ERROR;
        if (claveEnArgumentos)
        {
            ayuda::Aviso(L"La contraseña no se puede escribir en la línea de comandos (otros programas la verían). Usa "
                         L"--contrasena-stdin y mándala por la entrada estándar.",
                         L"Jícaro", wxOK | wxICON_WARNING);
            Salir(SALIDA_ERROR);
            return true;
        }
        std::optional<std::wstring> clave;
        if (claveStdin)
        {
            std::wstring leida;
            Lectura l = LeerClaveDeEntrada(leida);
            if (l == Lectura::NoEsUtf8)
            {
                ayuda::Aviso(L"La contraseña que llegó por la entrada estándar no está en UTF-8.", L"Jícaro", wxOK | wxICON_WARNING);
                Salir(SALIDA_ERROR);
                return true;
            }
            if (l == Lectura::Bien) clave = std::move(leida);
        }
        if (archivo.empty() || !jicaro::ExisteArchivo(archivo))
        {
            ayuda::Aviso(L"No encuentro el archivo que hay que extraer. ¿Se movió o se borró?", L"Jícaro", wxOK | wxICON_WARNING);
            Salir(SALIDA_ERROR);
            return true;
        }
        auto modo = !destino.empty() ? VentanaProgreso::Modo::Destino
                                     : (aqui ? VentanaProgreso::Modo::Aqui : VentanaProgreso::Modo::Carpeta);
        auto* v = new VentanaProgreso(archivo, modo, destino, siExiste.value_or(m_ajustes.siExiste), std::move(clave), claveStdin);
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
