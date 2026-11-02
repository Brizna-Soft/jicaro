#include "extractor.hpp"

#include <windows.h>
#include <objbase.h>

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <memory>
#include <new>
#include <unordered_map>
#include <unordered_set>

#include "comun.hpp"
#include "marca.hpp"
#include "motor7z.hpp"
#include "rutas.hpp"

namespace
{
    using namespace jicaro;

    constexpr size_t TamanoBloque = 1 << 20;

    // Las contraseñas se borran de la memoria apenas dejan de hacer falta
    void BorrarTexto(std::wstring& s)
    {
        if (!s.empty()) SecureZeroMemory(s.data(), s.size() * sizeof(wchar_t));
        s.clear();
    }

    // Lectura propia con las funciones de Windows. Todas las partes se le muestran a libarchive como un solo archivo
    // seguido (así son los RAR en varias partes y los .001, .002...), con un solo «client data»: con varios,
    // libarchive vuelve a la primera parte al terminar de reconocer el formato sin descartar lo que ya leyó de las
    // siguientes, y los RAR en partes se leían mal. Además cuenta lo leído para el progreso, deja cancelar en medio de
    // una lectura larga y sabe en qué parte va (para avisar si falta la siguiente).
    class Lector
    {
    public:
        Lector(const std::vector<std::wstring>& rutas, Progreso& progreso, const std::atomic<bool>& cancelar)
            : m_progreso(progreso), m_cancelar(cancelar)
        {
            for (auto& r : rutas)
            {
                Parte p;
                p.ruta = r;
                p.inicio = m_total;
                p.tamano = TamanoArchivo(r);
                if (p.tamano < 0 && !m_noAbrio)
                {
                    m_noAbrio = true;
                    m_parteQueFallo = m_partes.size();
                }
                if (p.tamano > 0) m_total += p.tamano;
                m_partes.push_back(p);
            }
            m_progreso.totalComprimido = m_total;
        }

        ~Lector()
        {
            if (a) archive_read_free(a);
            CerrarArchivo();
        }

        Lector(const Lector&) = delete;
        Lector& operator=(const Lector&) = delete;

        bool Abrir(Preguntas* preguntas, bool soloSuelto = false);

        bool Cancelar() const { return m_cancelar.load(); }
        size_t Cantidad() const { return m_partes.size(); }
        int64_t Total() const { return m_total; }
        const std::wstring& RutaParte(size_t i) const { return m_partes[i].ruta; }

        // Contraseñas (solo ZIP): libarchive llama aquí cuando ninguna de las ya dadas sirve
        const char* SiguienteContrasena();
        void PonerEntrada(const std::wstring& ruta) { m_entrada = ruta; }
        // La que se prueba primero, sin preguntar (la de --contrasena-stdin, o la que ya se escribió)
        void PonerPrevia(const std::optional<std::wstring>& clave) { m_previa = clave; }

        la_ssize_t Leer(archive* ar, const void** buffer)
        {
            if (Cancelar())
            {
                cancelado = true;
                archive_set_error(ar, ECANCELED, "Cancelado");
                return -1;
            }
            *buffer = m_buf.data();
            if (m_pos >= m_total)
            {
                finDeLaUltima = true;
                return 0;
            }
            size_t i = ParteDe(m_pos);
            if (!Posicionar(ar, i, m_pos - m_partes[i].inicio)) return -1;
            int64_t quedan = m_partes[i].inicio + m_partes[i].tamano - m_pos;
            DWORD pedir = (DWORD)std::min<int64_t>((int64_t)m_buf.size(), quedan);
            DWORD leidos = 0;
            if (!ReadFile(m_h, m_buf.data(), pedir, &leidos, nullptr) || leidos == 0)
            {
                archive_set_error(ar, EIO, "Error al leer la parte %u", (unsigned)(i + 1));
                CerrarArchivo();
                return -1;
            }
            m_pos += leidos;
            m_posEnArchivo += leidos;
            m_progreso.leidos += leidos;
            return (la_ssize_t)leidos;
        }

        // Saltar hacia adelante sin leer. Si llega al final, libarchive lee y descarta lo que falte.
        la_int64_t Saltar(la_int64_t pedido)
        {
            int64_t posible = std::min<int64_t>(pedido, m_total - m_pos);
            if (posible <= 0) return 0;
            m_pos += posible;
            m_progreso.leidos += posible;
            return posible;
        }

        la_int64_t Buscar(archive* ar, la_int64_t posicion, int desde)
        {
            int64_t nueva = desde == SEEK_SET ? posicion : (desde == SEEK_CUR ? m_pos + posicion : m_total + posicion);
            if (nueva < 0 || nueva > m_total)
            {
                archive_set_error(ar, EINVAL, "Posición fuera del archivo");
                return ARCHIVE_FATAL;
            }
            m_pos = nueva;
            finDeLaUltima = false;
            return nueva;
        }

        void CerrarArchivo()
        {
            if (m_h != INVALID_HANDLE_VALUE) CloseHandle(m_h);
            m_h = INVALID_HANDLE_VALUE;
            m_abierta = SIZE_MAX;
        }

        archive* a = nullptr;
        bool cancelado = false;
        bool finDeLaUltima = false;    // se leyó hasta el final de la última parte
        bool personaCancelo = false;   // no dio la contraseña (o no dio otra después de una que no sirvió)
        bool claveMala = false;        // se dio una contraseña, no sirvió y no hubo otra
        bool m_noAbrio = false;
        size_t m_parteQueFallo = 0;

    private:
        struct Parte
        {
            std::wstring ruta;
            int64_t inicio = 0;    // dónde empieza dentro del archivo seguido
            int64_t tamano = 0;
        };

        size_t ParteDe(int64_t pos) const
        {
            for (size_t i = 0; i + 1 < m_partes.size(); i++)
                if (pos < m_partes[i].inicio + m_partes[i].tamano) return i;
            return m_partes.size() - 1;
        }

        bool Posicionar(archive* ar, size_t i, int64_t desplazamiento)
        {
            if (m_abierta != i)
            {
                CerrarArchivo();
                m_h = CreateFileW(rutas::Larga(m_partes[i].ruta).c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
                if (m_h == INVALID_HANDLE_VALUE)
                {
                    m_noAbrio = true;
                    m_parteQueFallo = i;
                    archive_set_error(ar, ENOENT, "No se pudo abrir la parte %u", (unsigned)(i + 1));
                    return false;
                }
                m_abierta = i;
                m_posEnArchivo = 0;
            }
            if (m_posEnArchivo != desplazamiento)
            {
                LARGE_INTEGER mover;
                mover.QuadPart = desplazamiento;
                if (!SetFilePointerEx(m_h, mover, nullptr, FILE_BEGIN))
                {
                    archive_set_error(ar, EIO, "Error al buscar en la parte %u", (unsigned)(i + 1));
                    return false;
                }
                m_posEnArchivo = desplazamiento;
            }
            return true;
        }

        std::vector<Parte> m_partes;
        Progreso& m_progreso;
        const std::atomic<bool>& m_cancelar;
        int64_t m_total = 0;
        int64_t m_pos = 0;               // posición dentro del archivo seguido
        HANDLE m_h = INVALID_HANDLE_VALUE;
        size_t m_abierta = SIZE_MAX;     // qué parte tiene abierta m_h
        int64_t m_posEnArchivo = 0;
        std::vector<char> m_buf = std::vector<char>(TamanoBloque);
        Preguntas* m_preguntas = nullptr;
        std::vector<std::string> m_candidatas;
        size_t m_siguiente = 0;
        bool m_yaPidio = false;
        std::string m_devuelta;
        std::wstring m_entrada;
        std::optional<std::wstring> m_previa;
        bool m_usoPrevia = false;
    };

    int AlAbrir(archive*, void*) { return ARCHIVE_OK; }
    la_ssize_t AlLeer(archive* a, void* d, const void** buffer) { return static_cast<Lector*>(d)->Leer(a, buffer); }
    la_int64_t AlSaltar(archive*, void* d, la_int64_t pedido) { return static_cast<Lector*>(d)->Saltar(pedido); }
    la_int64_t AlBuscar(archive* a, void* d, la_int64_t posicion, int desde)
    {
        return static_cast<Lector*>(d)->Buscar(a, posicion, desde);
    }
    int AlCerrar(archive*, void* d)
    {
        static_cast<Lector*>(d)->CerrarArchivo();
        return ARCHIVE_OK;
    }
    const char* AlPedirContrasena(archive*, void* d)
    {
        // Nada puede lanzar una excepción a través de libarchive (está escrita en C)
        auto* l = static_cast<Lector*>(d);
        try
        {
            return l->SiguienteContrasena();
        }
        catch (...)
        {
            l->personaCancelo = true;
            return nullptr;
        }
    }

    // La contraseña en UTF-8 y en las páginas de códigos de Windows: los ZIP viejos guardan la clave en la página
    // de códigos de quien los hizo, y los nuevos en UTF-8
    std::vector<std::string> Variantes(const std::wstring& clave)
    {
        std::vector<std::string> r;
        auto agregar = [&r](std::string s) {
            if (!s.empty() && std::find(r.begin(), r.end(), s) == r.end()) r.push_back(std::move(s));
        };
        agregar(Utf8(clave));
        for (UINT pagina : { GetACP(), GetOEMCP() })
        {
            if (pagina == CP_UTF8) continue;
            BOOL perdio = FALSE;
            int n = WideCharToMultiByte(pagina, WC_NO_BEST_FIT_CHARS, clave.data(), (int)clave.size(), nullptr, 0, nullptr,
                                        &perdio);
            if (n <= 0 || perdio) continue;
            std::string s(n, '\0');
            WideCharToMultiByte(pagina, WC_NO_BEST_FIT_CHARS, clave.data(), (int)clave.size(), s.data(), n, nullptr, &perdio);
            if (!perdio) agregar(s);
        }
        return r;
    }

    const char* Lector::SiguienteContrasena()
    {
        if (personaCancelo) return nullptr;
        if (m_siguiente < m_candidatas.size())
        {
            m_devuelta = m_candidatas[m_siguiente++];
            return m_devuelta.c_str();
        }
        if (Cancelar()) return nullptr;
        // Primero la que ya vino, sin preguntar; después se pregunta (si hay a quién). Nunca se prueban claves por
        // cuenta propia: solo variantes de la que se escribió (UTF-8 y las páginas de códigos de Windows).
        const bool otraVez = m_yaPidio;
        std::optional<std::wstring> clave;
        if (m_previa && !m_usoPrevia)
        {
            m_usoPrevia = true;
            clave = m_previa;
        }
        else if (m_preguntas && m_preguntas->contrasena)
            clave = m_preguntas->contrasena(m_entrada, otraVez);
        m_yaPidio = true;
        if (!clave || clave->empty())
        {
            personaCancelo = true;
            claveMala = otraVez;
            return nullptr;
        }
        m_candidatas = Variantes(*clave);
        m_siguiente = 0;
        if (m_candidatas.empty())
        {
            personaCancelo = true;
            return nullptr;
        }
        m_devuelta = m_candidatas[m_siguiente++];
        return m_devuelta.c_str();
    }

    bool Lector::Abrir(Preguntas* preguntas, bool soloSuelto)
    {
        m_preguntas = preguntas;
        a = archive_read_new();
        if (!a) return false;
        // Solo filtros que van dentro del programa. libarchive puede usar programas de fuera (lrzip, lzop, grzip...)
        // para otros formatos: esos no se activan nunca.
        int r = ARCHIVE_OK;
        r = std::min(r, archive_read_support_filter_gzip(a));
        r = std::min(r, archive_read_support_filter_bzip2(a));
        r = std::min(r, archive_read_support_filter_xz(a));
        r = std::min(r, archive_read_support_filter_lzma(a));
        r = std::min(r, archive_read_support_filter_lzip(a));
        r = std::min(r, archive_read_support_filter_zstd(a));
        r = std::min(r, archive_read_support_filter_compress(a));
        if (r != ARCHIVE_OK)
        {
            archive_set_error(a, EINVAL, "Falta un filtro interno");
            return false;
        }
        if (!soloSuelto)
        {
            archive_read_support_format_zip(a);
            archive_read_support_format_rar(a);
            archive_read_support_format_rar5(a);
            archive_read_support_format_7zip(a);
            archive_read_support_format_tar(a);
            archive_read_support_format_iso9660(a);
            archive_read_support_format_cab(a);
        }
        archive_read_support_format_raw(a);     // un .gz, .bz2, .xz o .zst de un solo archivo
        archive_read_support_format_empty(a);
        // Los TAR sin datos del juego de caracteres casi siempre vienen de Linux, en UTF-8
        archive_read_set_format_option(a, "tar", "hdrcharset", "UTF-8");
        if (preguntas) archive_read_set_passphrase_callback(a, this, AlPedirContrasena);

        if (m_noAbrio)
        {
            archive_set_error(a, ENOENT, "Falta la parte %u", (unsigned)(m_parteQueFallo + 1));
            return false;
        }
        archive_read_set_callback_data(a, this);
        archive_read_set_open_callback(a, AlAbrir);
        archive_read_set_read_callback(a, AlLeer);
        archive_read_set_skip_callback(a, AlSaltar);
        archive_read_set_seek_callback(a, AlBuscar);
        archive_read_set_close_callback(a, AlCerrar);
        return archive_read_open1(a) == ARCHIVE_OK;
    }

    int Formato(archive* a) { return archive_format(a) & ARCHIVE_FORMAT_BASE_MASK; }
    bool EsSuelto(archive* a) { return Formato(a) == ARCHIVE_FORMAT_RAW; }
    bool EsSueltoSinCompresion(archive* a)
    {
        return EsSuelto(a) && (archive_filter_count(a) <= 1 || archive_filter_code(a, 0) == ARCHIVE_FILTER_NONE);
    }

    std::wstring NombreFormato(archive* a)
    {
        std::wstring base;
        switch (Formato(a))
        {
        case ARCHIVE_FORMAT_ZIP: base = L"ZIP"; break;
        case ARCHIVE_FORMAT_RAR: base = L"RAR"; break;
        case ARCHIVE_FORMAT_RAR_V5: base = L"RAR 5"; break;
        case ARCHIVE_FORMAT_7ZIP: base = L"7z"; break;
        case ARCHIVE_FORMAT_TAR: base = L"TAR"; break;
        case ARCHIVE_FORMAT_ISO9660: base = L"ISO"; break;
        case ARCHIVE_FORMAT_CAB: base = L"CAB"; break;
        case ARCHIVE_FORMAT_RAW:
        case ARCHIVE_FORMAT_EMPTY: break;
        default:
            if (const char* n = archive_format_name(a)) base = Ancho(n);
        }
        std::wstring filtro;
        for (int i = 0; i < archive_filter_count(a) && filtro.empty(); i++)
        {
            switch (archive_filter_code(a, i))
            {
            case ARCHIVE_FILTER_GZIP: filtro = L"gzip"; break;
            case ARCHIVE_FILTER_BZIP2: filtro = L"bzip2"; break;
            case ARCHIVE_FILTER_XZ: filtro = L"xz"; break;
            case ARCHIVE_FILTER_LZMA: filtro = L"lzma"; break;
            case ARCHIVE_FILTER_LZIP: filtro = L"lzip"; break;
            case ARCHIVE_FILTER_ZSTD: filtro = L"zstd"; break;
            case ARCHIVE_FILTER_COMPRESS: filtro = L"compress"; break;
            default: break;
            }
        }
        if (base.empty()) return filtro.empty() ? L"" : filtro + L" (un solo archivo)";
        return filtro.empty() ? base : base + L" + " + filtro;
    }

    std::wstring MensajeCifrado(int formato)
    {
        std::wstring cual = formato == ARCHIVE_FORMAT_7ZIP ? L"7z" : (formato == ARCHIVE_FORMAT_ZIP ? L"ZIP" : L"RAR");
        if (formato == ARCHIVE_FORMAT_ZIP)
            return L"Este ZIP usa un tipo de cifrado que Jícaro no puede abrir.";
        // Solo pasa si falta 7z.dll (sin ella, libarchive lee los RAR y 7z, pero no los que tienen contraseña)
        return L"Este " + cual + L" está protegido con contraseña. Para abrirlo, Jícaro necesita el archivo 7z.dll junto a "
               L"Jicaro.exe, y no está: vuelve a bajar Jícaro y descomprímelo entero.";
    }

    std::wstring Detalle(const char* error)
    {
        std::string e = error ? error : "";
        for (auto& c : e) c = (char)tolower((unsigned char)c);
        auto tiene = [&e](const char* s) { return e.find(s) != std::string::npos; };
        if (tiene("truncated") || tiene("premature") || tiene("unexpected end") || tiene("eof"))
            return L" Parece incompleto (¿se terminó de bajar?).";
        if (tiene("crc") || tiene("checksum"))
            return L" Los datos no coinciden con su suma de control: está dañado, o la contraseña no es la correcta.";
        if (tiene("unsupported") || tiene("not supported"))
            return L" Usa algo que Jícaro todavía no sabe leer.";
        if (tiene("memory"))
            return L" No alcanzó la memoria.";
        return L"";
    }

    // Para la persona: qué pasó cuando libarchive no pudo seguir
    void Clasificar(Lector& l, Estado& estado, std::wstring& mensaje)
    {
        archive* a = l.a;
        int formato = a ? Formato(a) : 0;
        const char* error = a ? archive_error_string(a) : nullptr;
#ifdef JICARO_PRUEBAS
        pruebas::ultimoError = error ? error : "";
#endif
        if (l.cancelado)
        {
            estado = Estado::Cancelado;
            mensaje = L"Se canceló.";
            return;
        }
        if (l.personaCancelo)
        {
            if (l.claveMala)
            {
                estado = Estado::ContrasenaIncorrecta;
                mensaje = L"La contraseña no es correcta, así que no se extrajo.";
            }
            else
            {
                estado = Estado::SinContrasena;
                mensaje = L"Hacía falta la contraseña y no se dio, así que no se extrajo.";
            }
            return;
        }
        if (l.m_noAbrio)
        {
            estado = l.Cantidad() > 1 ? Estado::FaltaParte : Estado::Error;
            mensaje = L"No se pudo abrir «" + NombreDe(l.RutaParte(l.m_parteQueFallo)) + L"». ¿Se movió o se borró?";
            return;
        }
        if (a && archive_read_has_encrypted_entries(a) == 1
            && (formato == ARCHIVE_FORMAT_RAR || formato == ARCHIVE_FORMAT_RAR_V5 || formato == ARCHIVE_FORMAT_7ZIP))
        {
            estado = Estado::CifradoNoSoportado;
            mensaje = MensajeCifrado(formato);
            return;
        }
        if (formato == ARCHIVE_FORMAT_ZIP && error && strstr(error, "Encrypted file is unsupported"))
        {
            estado = Estado::CifradoNoSoportado;
            mensaje = MensajeCifrado(formato);
            return;
        }
        if (l.finDeLaUltima)
        {
            if (l.Cantidad() > 1)
            {
                estado = Estado::FaltaParte;
                mensaje = L"Falta la parte que va después de «" + NombreDe(l.RutaParte(l.Cantidad() - 1)) +
                          L"». Revisa que se hayan bajado todas las partes y que estén en la misma carpeta.";
            }
            else
            {
                estado = Estado::Error;
                mensaje = L"El archivo está incompleto: se acaba antes de tiempo. ¿Se terminó de bajar?";
            }
            return;
        }
        if (a && (formato == 0 || EsSueltoSinCompresion(a)))
        {
            estado = Estado::NoEsArchivo;
            mensaje = L"No es un archivo comprimido que Jícaro conozca, o está dañado.";
            return;
        }
        estado = Estado::Error;
        mensaje = L"El archivo está dañado o no se puede leer." + Detalle(error);
    }

    std::wstring RutaDeEntrada(archive_entry* e)
    {
        std::wstring r;
        if (const wchar_t* w = archive_entry_pathname_w(e)) r = w;
        else if (const char* u = archive_entry_pathname_utf8(e)) r = Ancho(u);
        else if (const char* m = archive_entry_pathname(e)) r = AnchoDesde(CP_ACP, m);
        for (auto& c : r)
            if (c == L'/') c = L'\\';
        while (r.size() > 1 && r.back() == L'\\') r.pop_back();
        return r;
    }

    std::wstring DestinoDeEnlace(archive_entry* e)
    {
        std::wstring r;
        if (const wchar_t* w = archive_entry_hardlink_w(e)) r = w;
        else if (const char* u = archive_entry_hardlink_utf8(e)) r = Ancho(u);
        else if (const char* m = archive_entry_hardlink(e)) r = AnchoDesde(CP_ACP, m);
        for (auto& c : r)
            if (c == L'/') c = L'\\';
        while (r.size() > 1 && r.back() == L'\\') r.pop_back();
        while (r.rfind(L".\\", 0) == 0) r.erase(0, 2);
        return r;
    }

    bool EsEnlaceSimbolico(archive_entry* e)
    {
        return archive_entry_filetype(e) == AE_IFLNK || archive_entry_symlink_w(e) || archive_entry_symlink(e);
    }

    bool EsEnlaceDuro(archive_entry* e)
    {
        return archive_entry_hardlink_w(e) || archive_entry_hardlink(e);
    }

    void CopiarFechas(archive_entry* de, archive_entry* a)
    {
        if (archive_entry_mtime_is_set(de)) archive_entry_set_mtime(a, archive_entry_mtime(de), archive_entry_mtime_nsec(de));
        if (archive_entry_atime_is_set(de)) archive_entry_set_atime(a, archive_entry_atime(de), archive_entry_atime_nsec(de));
        if (archive_entry_birthtime_is_set(de))
            archive_entry_set_birthtime(a, archive_entry_birthtime(de), archive_entry_birthtime_nsec(de));
    }

    // La carpeta de trabajo del proceso pasa a ser la de destino mientras dura la extracción: las banderas SECURE de
    // libarchive revisan rutas relativas a ella
    class CarpetaDeTrabajo
    {
    public:
        explicit CarpetaDeTrabajo(const std::wstring& nueva) : m_nueva(nueva)
        {
            std::vector<wchar_t> b(32768);
            DWORD n = GetCurrentDirectoryW((DWORD)b.size(), b.data());
            m_antes.assign(b.data(), n < b.size() ? n : 0);
            ok = SetCurrentDirectoryW(nueva.c_str()) != FALSE;
        }
        ~CarpetaDeTrabajo()
        {
            if (ok && !m_antes.empty()) SetCurrentDirectoryW(m_antes.c_str());
        }
        // Nadie debe cambiarla en medio: se revisa antes de cada archivo
        bool Sigue() const
        {
            std::vector<wchar_t> b(32768);
            DWORD n = GetCurrentDirectoryW((DWORD)b.size(), b.data());
            if (n == 0 || n >= b.size()) return false;
            std::wstring actual(b.data(), n), esperada = m_nueva;
            while (actual.size() > 3 && actual.back() == L'\\') actual.pop_back();
            while (esperada.size() > 3 && esperada.back() == L'\\') esperada.pop_back();
            return IgualesSinMayusculas(actual, esperada);
        }
        bool ok = false;

    private:
        std::wstring m_antes, m_nueva;
    };

    // Manejador de Windows que se cierra solo (también si algo lanza una excepción)
    struct Manejador
    {
        HANDLE h = INVALID_HANDLE_VALUE;
        Manejador() = default;
        explicit Manejador(HANDLE x) : h(x) {}
        ~Manejador() { Cerrar(); }
        Manejador(const Manejador&) = delete;
        Manejador& operator=(const Manejador&) = delete;
        void Cerrar()
        {
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            h = INVALID_HANDLE_VALUE;
        }
        bool Valido() const { return h != INVALID_HANDLE_VALUE; }
    };

    // Un archivo que crea Jícaro: con CREATE_NEW (nunca abre algo que ya estaba, ni algo que otro programa cree justo en
    // ese momento) y con su propio manejador, para saber que es el suyo y poder quitarlo sin depender de la ruta. Si se
    // destruye sin cerrarlo (por ejemplo, por una excepción), se quita.
    class ArchivoPropio
    {
    public:
        ArchivoPropio() = default;
        ~ArchivoPropio()
        {
            if (h != INVALID_HANDLE_VALUE) Quitar();
        }
        ArchivoPropio(const ArchivoPropio&) = delete;
        ArchivoPropio& operator=(const ArchivoPropio&) = delete;

        // 0 si se creó; si no, el error de Windows (ERROR_FILE_EXISTS: ya hay algo con ese nombre)
        DWORD Crear(const std::wstring& completa)
        {
            h = CreateFileW(rutas::Larga(completa).c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                DWORD e = GetLastError();
                return e ? e : ERROR_CANNOT_MAKE;
            }
            id = IdentidadDe(h);
            pos = 0;
            return 0;
        }

        // Escribe en la posición «desde» (si hay un hueco antes, queda con ceros, como hace libarchive)
        bool Escribir(const void* datos, size_t tamano, int64_t desde)
        {
            if (desde != pos)
            {
                LARGE_INTEGER p;
                p.QuadPart = desde;
                if (!SetFilePointerEx(h, p, nullptr, FILE_BEGIN)) return false;
                pos = desde;
            }
            const char* d = static_cast<const char*>(datos);
            size_t quedan = tamano;
            while (quedan > 0)
            {
                DWORD escritos = 0;
                if (!WriteFile(h, d, (DWORD)std::min<size_t>(quedan, 1u << 30), &escritos, nullptr) || escritos == 0) return false;
                d += escritos;
                quedan -= escritos;
                pos += escritos;
            }
            return true;
        }

        // El hueco del final de un archivo con huecos: el archivo crece hasta su tamaño
        bool Agrandar(int64_t tamano)
        {
            LARGE_INTEGER p;
            p.QuadPart = tamano;
            return SetFilePointerEx(h, p, nullptr, FILE_BEGIN) && SetEndOfFile(h);
        }

        // Pone las fechas y cierra. false si Windows no pudo cerrarlo bien (entonces se quita).
        bool Terminar(const FILETIME* creado, const FILETIME* acceso, const FILETIME* escrito)
        {
            if (creado || acceso || escrito) SetFileTime(h, creado, acceso, escrito);
            bool ok = CloseHandle(h) != FALSE;
            h = INVALID_HANDLE_VALUE;
            return ok;
        }

        // Lo borra por su manejador. true si se borró.
        bool Quitar()
        {
            if (h == INVALID_HANDLE_VALUE) return false;
            FILE_DISPOSITION_INFO borrar = {};
            borrar.DeleteFile = TRUE;
            bool borrado = SetFileInformationByHandle(h, FileDispositionInfo, &borrar, sizeof(borrar)) != FALSE;
            CloseHandle(h);
            h = INVALID_HANDLE_VALUE;
            return borrado;
        }

        bool Abierto() const { return h != INVALID_HANDLE_VALUE; }

        Identidad id;

    private:
        HANDLE h = INVALID_HANDLE_VALUE;
        int64_t pos = 0;
    };

    // Segundos y nanosegundos desde 1970 a FILETIME (cada 100 ns desde 1601)
    bool AFiletime(int64_t segundos, long nano, FILETIME& f)
    {
        if (segundos < -11644473600LL || segundos > 900000000000LL) return false;
        int64_t v = segundos * 10000000 + nano / 100 + 116444736000000000LL;
        if (v < 0) return false;
        f.dwLowDateTime = (DWORD)(uint64_t)v;
        f.dwHighDateTime = (DWORD)((uint64_t)v >> 32);
        return true;
    }

    struct LiberarEntrada
    {
        void operator()(archive_entry* e) const { archive_entry_free(e); }
    };
    using EntradaPropia = std::unique_ptr<archive_entry, LiberarEntrada>;

    // Al cerrar el escritor se ponen las fechas de las carpetas: tiene que pasar con la carpeta de trabajo todavía puesta
    struct CerrarEscritor
    {
        void operator()(archive* w) const
        {
            archive_write_close(w);
            archive_write_free(w);
        }
    };
    using EscritorPropio = std::unique_ptr<archive, CerrarEscritor>;

    struct ComIniciado
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ~ComIniciado()
        {
            if (SUCCEEDED(hr)) CoUninitialize();
        }
    };

    // Algo que ya se extrajo en esta misma extracción (para los enlaces duros): su ruta y su identidad en el disco
    struct Hecho
    {
        std::wstring ruta;
        Identidad id;
    };

    // El archivo que se está escribiendo en este momento (para limpiarlo si algo falla en medio)
    struct EnCurso
    {
        std::wstring completa, relativa;
        Identidad id;
        bool activo = false;
    };

    class Extraccion
    {
    public:
        Extraccion(const Opciones& op, Preguntas& pr, Progreso& prog, const std::atomic<bool>& cancelar, bool soloSuelto = false)
            : m_op(op), m_pr(pr), m_prog(prog), m_cancelar(cancelar), m_politica(op.siExiste), m_soloSuelto(soloSuelto)
        {
        }

        Resumen Ejecutar();
        // libarchive toma por «TAR vacío» un .gz (o .xz...) de un archivo que empieza con muchos ceros
        bool TarVacio() const { return m_tarVacio; }

    private:
        using Tipo = Omitida::Tipo;
        class Destino7z;

        void Bucle(Lector& lector, CarpetaDeTrabajo& trabajo, archive* w);
        void EjecutarLibarchive();
        void Ejecutar7z(motor7z::Formato formato);
        void Bucle7z(motor7z::Archivo& arc, CarpetaDeTrabajo& trabajo, archive* w);
        // 7z con un método que 7z.dll no trae: esos archivos se prueban con libarchive
        void ProbarConLibarchive(const std::vector<motor7z::Item>& items, const std::vector<uint32_t>& indices,
                                 CarpetaDeTrabajo& trabajo, archive* w);
        // La contraseña que toca ahora: primero la que ya se tiene; si no (o si «otraVez»), se pregunta
        std::optional<std::wstring> PedirClave(const std::wstring& entrada, bool otraVez);
        bool Paro() const;
        // La carpeta de trabajo y el escritor de libarchive (con sus defensas), y la limpieza si algo falla en medio
        void ConEscritor(const std::function<void(CarpetaDeTrabajo&, archive*)>& cuerpo);
        bool Elegida(const std::wstring& cruda) const;
        void Omitir(const std::wstring& ruta, const std::wstring& motivo, Tipo tipo = Tipo::Seguridad);
        std::optional<std::wstring> Preparar(const std::wstring& cruda, bool carpeta);
        bool SinDesvios(const std::wstring& relativa, std::wstring& donde);
        bool SigueSeguro(const std::wstring& relativa) const;
        Decision Decidir(const std::wstring& completa);
        bool Preguntar(const std::wstring& motivo);
        bool EsParte(const std::wstring& completa) const;
        std::wstring RelativaDe(const std::wstring& completa) const;
        void Marcar(const std::wstring& completa, const std::wstring& relativa);
        bool Controlar(int64_t nuevos);
        bool ControlarAntes(int64_t anunciado);
        bool CopiarDatos(ArchivoPropio& destino, archive_entry* e, const std::wstring& cruda, bool& fatal, bool& incompleto);
        // Crea el archivo (y las carpetas de arriba que falten) con la revisión de todo el camino. false: no se creó
        // (ya se dijo por qué).
        bool CrearPropio(ArchivoPropio& f, const std::wstring& completa, const std::wstring& relativa, const std::wstring& cruda);
        // Quita lo que quedó a medias: por su manejador; si Windows no deja, como QuitarAMedias
        void QuitarPropio(ArchivoPropio& f, const std::wstring& completa, const std::wstring& relativa);
        bool CopiarEnlace(const Hecho& origen, const std::wstring& relativa, const std::wstring& cruda);
        void QuitarAMedias(const std::wstring& completa, const std::wstring& relativa, const Identidad& id);

        const Opciones& m_op;
        Preguntas& m_pr;
        Progreso& m_prog;
        const std::atomic<bool>& m_cancelar;
        SiExiste m_politica;
        Resumen m_res;
        Lector* m_lector = nullptr;
        std::wstring m_destino;
        std::vector<std::wstring> m_partesCompletas;
        std::vector<Identidad> m_idPartes;
        std::unordered_set<std::wstring> m_elegidas;
        std::unordered_map<std::wstring, Hecho> m_hechos;  // ruta en el archivo → archivo extraído
        EnCurso m_enCurso;
        std::wstring m_referente;
        unsigned long m_zona = 0;
        int64_t m_libreAlEmpezar = -1;
        int64_t m_proximoChequeo = 64LL << 20;
        bool m_preguntoProporcion = false, m_preguntoLibre = false, m_preguntoCantidad = false;
        bool m_parar = false;      // la persona canceló en una pregunta
        bool m_detenido = false;   // la persona detuvo ante una posible bomba
        bool m_sinEspacio = false;
        int m_formatoCifrado = 0;
        bool m_soloSuelto = false, m_tarVacio = false;
        int64_t m_comprimido = 1;               // lo que pesa el archivo comprimido (para la proporción de las bombas)
        std::optional<std::wstring> m_clave;    // la contraseña que se está usando (RAR y 7z)
        bool m_dioClave = false;                // se dio alguna: si no sirve, es «contraseña incorrecta»
        int64_t m_incompletos = 0;              // RAR y 7z: archivos que se acabaron antes de tiempo
        motor7z::Formato m_formato7z = motor7z::Formato::SieteZ;
    };

    void Extraccion::Omitir(const std::wstring& ruta, const std::wstring& motivo, Tipo tipo)
    {
        m_res.omitidas.push_back({ ruta, motivo, tipo });
        if (tipo == Tipo::Seguridad) m_res.rechazados++;
        else if (tipo == Tipo::Falla) m_res.fallidos++;
    }

    bool Extraccion::Elegida(const std::wstring& cruda) const
    {
        if (m_elegidas.empty() || m_elegidas.count(cruda)) return true;
        for (size_t p = cruda.find(L'\\'); p != std::wstring::npos; p = cruda.find(L'\\', p + 1))
            if (m_elegidas.count(cruda.substr(0, p))) return true;
        return false;
    }

    // ¿Es una de las partes que se están leyendo? Se compara la identidad en el disco, no solo el texto: así también
    // cuenta el nombre corto 8.3 («JUEGO~1.RAR») de una parte.
    bool Extraccion::EsParte(const std::wstring& completa) const
    {
        for (auto& p : m_partesCompletas)
            if (IgualesSinMayusculas(p, completa)) return true;
        Identidad id = IdentidadDe(completa);
        if (!id.ok) return false;
        for (auto& p : m_idPartes)
            if (p == id) return true;
        return false;
    }

    std::wstring Extraccion::RelativaDe(const std::wstring& completa) const
    {
        size_t corte = m_destino.back() == L'\\' ? m_destino.size() : m_destino.size() + 1;
        return completa.size() > corte ? completa.substr(corte) : completa;
    }

    bool Extraccion::Preguntar(const std::wstring& motivo)
    {
        if (!m_pr.bomba) return false;   // sin nadie a quien preguntar, se detiene
        return m_pr.bomba(motivo);
    }

    Decision Extraccion::Decidir(const std::wstring& completa)
    {
        switch (m_politica)
        {
        case SiExiste::Reemplazar: return Decision::Reemplazar;
        case SiExiste::Saltar: return Decision::Saltar;
        case SiExiste::Renombrar: return Decision::Renombrar;
        default: break;
        }
        if (!m_pr.existe) return Decision::Renombrar;
        bool paraTodos = false;
        Decision d = m_pr.existe(completa, paraTodos);
        if (paraTodos)
        {
            if (d == Decision::Reemplazar) m_politica = SiExiste::Reemplazar;
            else if (d == Decision::Saltar) m_politica = SiExiste::Saltar;
            else if (d == Decision::Renombrar) m_politica = SiExiste::Renombrar;
        }
        return d;
    }

    // Revisión de cada entrada: todo el camino, cada vez (sin recordar carpetas ya revisadas, que otro programa podría
    // cambiar por una unión mientras se extrae)
    bool Extraccion::SinDesvios(const std::wstring& relativa, std::wstring& donde)
    {
        return rutas::SinDesvios(m_destino, relativa, &donde);
    }

    // La misma revisión justo antes de lo que Jícaro hace por su cuenta con una ruta (escribir, Papelera, copias, marca,
    // borrar lo que quedó a medias): achica el tiempo en que otro programa podría cambiar una carpeta por una unión
    bool Extraccion::SigueSeguro(const std::wstring& relativa) const
    {
        return rutas::DentroDe(m_destino, rutas::Unir(m_destino, relativa)) && rutas::SinDesvios(m_destino, relativa);
    }

    std::optional<std::wstring> Extraccion::Preparar(const std::wstring& cruda, bool carpeta)
    {
        auto limpia = rutas::Limpiar(cruda);
        if (!limpia.ok)
        {
            // La carpeta raíz («.» o «./», como en las ISO) no es nada que extraer: se salta sin avisar
            if (!(carpeta && limpia.motivo == rutas::Motivo::Vacia)) Omitir(cruda, rutas::TextoMotivo(limpia.motivo));
            return std::nullopt;
        }
        std::wstring relativa = limpia.relativa;
        std::wstring completa = rutas::Unir(m_destino, relativa);
        if (!rutas::DentroDe(m_destino, completa))
        {
            Omitir(cruda, L"saldría de la carpeta de destino");
            return std::nullopt;
        }
        std::wstring donde;
        if (!SinDesvios(relativa, donde))
        {
            Omitir(cruda, L"pasaría por un enlace que ya estaba en la carpeta («" + NombreDe(donde) + L"»)");
            return std::nullopt;
        }
        DWORD atributos = GetFileAttributesW(rutas::Larga(completa).c_str());
        if (atributos == INVALID_FILE_ATTRIBUTES) return relativa;
        if (carpeta)
        {
            if (atributos & FILE_ATTRIBUTE_DIRECTORY) return relativa;   // ya existe: se usa la misma
            Omitir(cruda, L"ya hay un archivo con ese nombre donde iría la carpeta");
            return std::nullopt;
        }
        if (atributos & FILE_ATTRIBUTE_DIRECTORY)
        {
            Omitir(cruda, L"ya hay una carpeta con ese nombre");
            return std::nullopt;
        }
        // Ya existe un archivo con ese nombre. Nunca se reemplaza una de las partes que se están leyendo.
        Decision d = EsParte(completa) ? Decision::Renombrar : Decidir(completa);
        switch (d)
        {
        case Decision::Cancelar:
            m_parar = true;
            return std::nullopt;
        case Decision::Saltar:
            m_res.saltados++;
            return std::nullopt;
        case Decision::Renombrar:
            return RelativaDe(rutas::NombreLibre(completa));
        case Decision::Reemplazar:
            // La pregunta pudo tardar: se revisa todo de nuevo antes de tocar el archivo que ya estaba
            if (!SigueSeguro(relativa) || EsParte(completa))
            {
                Omitir(cruda, L"la carpeta cambió mientras se preguntaba; no se tocó nada");
                return std::nullopt;
            }
            if (!MandarAPapelera(completa))
            {
                Omitir(cruda, L"no se pudo mandar el archivo que ya estaba a la Papelera", Tipo::Falla);
                return std::nullopt;
            }
            return relativa;
        }
        return std::nullopt;
    }

    void Extraccion::Marcar(const std::wstring& completa, const std::wstring& relativa)
    {
        if (!marca::HayQueMarcar(m_zona)) return;
        if (!SigueSeguro(relativa))
        {
            m_res.sinMarca++;
            return;
        }
        if (marca::Pasar(completa, m_referente, m_zona)) m_res.marcados++;
        else if (!ExisteArchivo(completa)) m_res.quitadosPorWindows++;
        else m_res.sinMarca++;
    }

    // Lo que quedó a medias se borra solo si es el mismo archivo que esta extracción creó (por su identidad en el
    // disco). Si no se puede saber o no se puede borrar, se dice cuál es y se le pone la marca de la Web igual.
    void Extraccion::QuitarAMedias(const std::wstring& completa, const std::wstring& relativa, const Identidad& id)
    {
        if (id.ok && SigueSeguro(relativa) && BorrarSiEsElMismo(completa, id))
        {
            if (m_cancelar || m_detenido || m_sinEspacio) m_res.quitoAMedias = true;
            return;
        }
        if (!ExisteArchivo(completa)) return;
        if (!id.ok || IdentidadDe(completa) == id)
        {
            m_res.aMedias = completa;
            Marcar(completa, relativa);
        }
    }

    // Topes contra las bombas mientras crece lo extraído (datos y huecos). false: hay que parar.
    bool Extraccion::Controlar(int64_t nuevos)
    {
        const int64_t comprimido = std::max<int64_t>(m_comprimido, 1);
        m_res.escritos += nuevos;
        m_prog.escritos += nuevos;
        if (!m_preguntoProporcion && m_res.escritos >= m_op.limites.bytesMinimos
            && (double)m_res.escritos > m_op.limites.proporcion * (double)comprimido)
        {
            m_preguntoProporcion = true;
            std::wstring motivo = L"Lo extraído ya ocupa " + TextoTamano(m_res.escritos) + L", más de " +
                TextoNumero(m_res.escritos / comprimido) + L" veces lo que pesa el archivo comprimido (" +
                TextoTamano(comprimido) + L").";
            if (!Preguntar(motivo))
            {
                m_detenido = true;
                return false;
            }
        }
        if (m_res.escritos >= m_proximoChequeo)
        {
            m_proximoChequeo = m_res.escritos + (64LL << 20);
            int64_t libre = EspacioLibre(m_destino);
            if (libre >= 0 && libre < m_op.limites.libreMinimo)
            {
                m_sinEspacio = true;
                return false;
            }
            if (!m_preguntoLibre && m_libreAlEmpezar > 0 && m_res.escritos >= (1LL << 30)
                && (double)m_res.escritos > m_op.limites.parteDelLibre * (double)m_libreAlEmpezar)
            {
                m_preguntoLibre = true;
                std::wstring motivo = L"Lo extraído ya ocupa " + TextoTamano(m_res.escritos) +
                    L", más de la mitad del espacio que estaba libre en el disco al empezar.";
                if (!Preguntar(motivo))
                {
                    m_detenido = true;
                    return false;
                }
            }
        }
        return true;
    }

    // Los mismos topes, con el tamaño que anuncia la entrada antes de escribirla: un archivo con huecos (o una copia de
    // un enlace duro) puede ocupar mucho de golpe. false: hay que parar.
    bool Extraccion::ControlarAntes(int64_t anunciado)
    {
        if (anunciado <= 0) return true;
        const int64_t comprimido = std::max<int64_t>(m_comprimido, 1);
        int64_t libre = EspacioLibre(m_destino);
        if (libre >= 0 && anunciado > libre - m_op.limites.libreMinimo)
        {
            m_sinEspacio = true;
            return false;
        }
        int64_t proyectado = m_res.escritos + anunciado;
        if (!m_preguntoProporcion && proyectado >= m_op.limites.bytesMinimos
            && (double)proyectado > m_op.limites.proporcion * (double)comprimido)
        {
            m_preguntoProporcion = true;
            std::wstring motivo = L"Lo extraído llegaría a ocupar " + TextoTamano(proyectado) + L", más de " +
                TextoNumero(proyectado / comprimido) + L" veces lo que pesa el archivo comprimido (" +
                TextoTamano(comprimido) + L").";
            if (!Preguntar(motivo))
            {
                m_detenido = true;
                return false;
            }
        }
        if (!m_preguntoLibre && m_libreAlEmpezar > 0 && proyectado >= (1LL << 30)
            && (double)proyectado > m_op.limites.parteDelLibre * (double)m_libreAlEmpezar)
        {
            m_preguntoLibre = true;
            std::wstring motivo = L"Lo extraído llegaría a ocupar " + TextoTamano(proyectado) +
                L", más de la mitad del espacio que estaba libre en el disco al empezar.";
            if (!Preguntar(motivo))
            {
                m_detenido = true;
                return false;
            }
        }
        return true;
    }

    // Copia los datos de la entrada actual al archivo abierto en «w». Devuelve true si llegó al final completo.
    // Cuenta para los topes lo que crece el archivo (datos y huecos, también el hueco final que libarchive agrega al
    // cerrarlo). «incompleto»: los datos se acabaron antes del tamaño que dice el archivo (pasa, por ejemplo, con un
    // RAR 5 en partes al que le falta la última: libarchive no lo marca como error).
    bool Extraccion::CrearPropio(ArchivoPropio& f, const std::wstring& completa, const std::wstring& relativa, const std::wstring& cruda)
    {
        std::wstring carpeta = CarpetaDe(completa);
        if (!SigueSeguro(relativa) || (!ExisteCarpeta(carpeta) && !CrearCarpetas(carpeta)) || !SigueSeguro(relativa))
        {
            Omitir(cruda, L"la carpeta cambió mientras se extraía; no se tocó");
            return false;
        }
        DWORD e = f.Crear(completa);
        if (e == 0) return true;
        Omitir(cruda,
               (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS) ? L"apareció otro archivo con ese nombre mientras se extraía; no se tocó"
                                                                     : L"no se pudo crear el archivo",
               Tipo::Falla);
        return false;
    }

    void Extraccion::QuitarPropio(ArchivoPropio& f, const std::wstring& completa, const std::wstring& relativa)
    {
        Identidad id = f.id;
        if (f.Quitar())
        {
            if (m_cancelar || m_detenido || m_sinEspacio) m_res.quitoAMedias = true;
            return;
        }
        QuitarAMedias(completa, relativa, id);
    }

    bool Extraccion::CopiarDatos(ArchivoPropio& destino, archive_entry* e, const std::wstring& cruda, bool& fatal, bool& incompleto)
    {
        archive* a = m_lector->a;
        const bool conTamano = archive_entry_size_is_set(e) && archive_entry_sparse_count(e) == 0;
        const int64_t tamano = conTamano ? archive_entry_size(e) : 0;
        const int64_t logico = archive_entry_size_is_set(e) ? archive_entry_size(e) : -1;
        int64_t hasta = 0;
        const void* bloque = nullptr;
        size_t tam = 0;
        la_int64_t desde = 0;
        for (;;)
        {
            int r = archive_read_data_block(a, &bloque, &tam, &desde);
            if (r == ARCHIVE_EOF)
            {
                // Menos datos que el tamaño anunciado. En ISO puede ser un hueco de ceros al final (zisofs), que no es
                // una falla; en RAR, o si se acabó todo el archivo comprimido, es que falta algo.
                int formato = Formato(a);
                if (conTamano && hasta < tamano
                    && (formato == ARCHIVE_FORMAT_RAR || formato == ARCHIVE_FORMAT_RAR_V5
                        || (m_lector->finDeLaUltima && formato != ARCHIVE_FORMAT_ISO9660)))
                {
                    incompleto = true;
                    return false;
                }
                // El hueco del final también ocupa: el archivo crece hasta el tamaño anunciado (contado en los topes)
                if (logico > hasta)
                {
                    if (!Controlar(logico - hasta)) return false;
                    if (!destino.Agrandar(logico))
                    {
                        Omitir(cruda, L"no se pudo escribir en el disco", Tipo::Falla);
                        return false;
                    }
                }
                return true;
            }
            if (r < ARCHIVE_WARN)
            {
                fatal = r == ARCHIVE_FATAL || m_lector->personaCancelo || m_lector->cancelado;
                if (!fatal) Omitir(cruda, L"no se pudo leer." + Detalle(archive_error_string(a)), Tipo::Falla);
                return false;
            }
            if (m_cancelar) return false;
            if (tam == 0) continue;
            // Nada fuera de lo que anuncia el archivo: ni antes del principio, ni más allá del tamaño anunciado (como
            // hacía el escritor de libarchive). Si no cuadra, está dañado y se quita.
            if (desde < 0 || (int64_t)tam < 0 || desde > INT64_MAX - (int64_t)tam
                || (logico >= 0 && (desde > logico || (int64_t)tam > logico - desde)))
            {
                Omitir(cruda, L"está dañado: trae más datos que el tamaño que anuncia", Tipo::Falla);
                return false;
            }
            int64_t nuevoHasta = std::max<int64_t>(hasta, desde + (int64_t)tam);
            int64_t crecimiento = nuevoHasta - hasta;   // datos y el hueco que haya antes de ellos
            hasta = nuevoHasta;
            if (!destino.Escribir(bloque, tam, desde))
            {
                int64_t libre = EspacioLibre(m_destino);
                if (libre >= 0 && libre < (64LL << 20)) m_sinEspacio = true;
                else Omitir(cruda, L"no se pudo escribir en el disco", Tipo::Falla);
                return false;
            }
            if (crecimiento > 0 && !Controlar(crecimiento)) return false;
        }
    }

    // Un enlace duro sin datos propios se guarda como copia de lo que ya se extrajo en esta extracción. Se copia por
    // bloques con los mismos topes que lo demás: miles de enlaces al mismo archivo grande serían una bomba.
    // false: hay que parar (cancelar, bomba o sin espacio).
    bool Extraccion::CopiarEnlace(const Hecho& origen, const std::wstring& relativa, const std::wstring& cruda)
    {
        std::wstring completa = rutas::Unir(m_destino, relativa);
        Manejador de(CreateFileW(rutas::Larga(origen.ruta).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!de.Valido() || !origen.id.ok || !(IdentidadDe(de.h) == origen.id))
        {
            Omitir(cruda, L"el archivo al que apunta cambió o ya no está; no se copió", Tipo::Falla);
            return true;
        }
        LARGE_INTEGER tamano{};
        GetFileSizeEx(de.h, &tamano);
        if (!ControlarAntes(tamano.QuadPart)) return false;
        if (!SigueSeguro(relativa))
        {
            Omitir(cruda, L"la carpeta cambió mientras se extraía; no se copió");
            return true;
        }
        Manejador a(CreateFileW(rutas::Larga(completa).c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!a.Valido())
        {
            Omitir(cruda, L"no se pudo crear la copia", Tipo::Falla);
            return true;
        }
        Identidad id = IdentidadDe(a.h);
        bool ok = true, parar = false;
        try
        {
            std::vector<char> buf(TamanoBloque);
            for (;;)
            {
                if (m_cancelar)
                {
                    ok = false;
                    parar = true;
                    break;
                }
                DWORD leidos = 0, escritos = 0;
                if (!ReadFile(de.h, buf.data(), (DWORD)buf.size(), &leidos, nullptr))
                {
                    ok = false;
                    break;
                }
                if (leidos == 0) break;
                if (!WriteFile(a.h, buf.data(), leidos, &escritos, nullptr) || escritos != leidos)
                {
                    int64_t libre = EspacioLibre(m_destino);
                    if (libre >= 0 && libre < (64LL << 20))
                    {
                        m_sinEspacio = true;
                        parar = true;
                    }
                    ok = false;
                    break;
                }
                if (!Controlar(leidos))
                {
                    ok = false;
                    parar = true;
                    break;
                }
            }
        }
        catch (...)
        {
            ok = false;
            parar = true;
            m_parar = true;
        }
        if (ok)
        {
            FILETIME creado, acceso, escrito;
            if (GetFileTime(de.h, &creado, &acceso, &escrito)) SetFileTime(a.h, &creado, &acceso, &escrito);
            a.Cerrar();
            m_res.archivos++;
            m_prog.archivos++;
            m_hechos[cruda] = { completa, id };
            Marcar(completa, relativa);
            return true;
        }
        // Lo creó esta extracción y se tiene abierto: se borra por su manejador, sin depender de la ruta
        FILE_DISPOSITION_INFO borrar = {};
        borrar.DeleteFile = TRUE;
        bool borrado = SetFileInformationByHandle(a.h, FileDispositionInfo, &borrar, sizeof(borrar)) != FALSE;
        a.Cerrar();
        if (borrado && parar) m_res.quitoAMedias = true;
        if (!borrado) QuitarAMedias(completa, relativa, id);
        if (!parar) Omitir(cruda, L"no se pudo copiar", Tipo::Falla);
        return !parar;
    }

    void Extraccion::Bucle(Lector& lector, CarpetaDeTrabajo& trabajo, archive* w)
    {
        int64_t numero = 0;
        int fallasSeguidas = 0;
        archive_entry* e = nullptr;
        auto carpetaCambio = [&]() {
            m_res.estado = Estado::Error;
            m_res.mensaje = L"Algo cambió la carpeta de trabajo en medio de la extracción; se detuvo por seguridad.";
        };
        for (;;)
        {
            if (m_cancelar)
            {
                m_res.estado = Estado::Cancelado;
                break;
            }
            int r = archive_read_next_header(lector.a, &e);
            if (r == ARCHIVE_EOF)
            {
                m_tarVacio = numero == 0 && Formato(lector.a) == ARCHIVE_FORMAT_TAR && archive_filter_count(lector.a) > 1
                    && archive_filter_code(lector.a, 0) != ARCHIVE_FILTER_NONE;
                break;
            }
            if (r == ARCHIVE_RETRY || r == ARCHIVE_FAILED)
            {
                if (++fallasSeguidas > 100)
                {
                    m_res.estado = Estado::Error;
                    m_res.mensaje = L"El archivo está dañado: demasiadas entradas que no se pueden leer.";
                    break;
                }
                if (r == ARCHIVE_FAILED && e) Omitir(RutaDeEntrada(e), L"está dañado." + Detalle(archive_error_string(lector.a)), Tipo::Falla);
                continue;
            }
            if (r < ARCHIVE_WARN)
            {
                Clasificar(lector, m_res.estado, m_res.mensaje);
                break;
            }
            fallasSeguidas = 0;
            numero++;
            if (numero == 1 && EsSueltoSinCompresion(lector.a))
            {
                m_res.estado = Estado::NoEsArchivo;
                m_res.mensaje = L"No es un archivo comprimido que Jícaro conozca, o está dañado.";
                break;
            }
            std::wstring cruda = EsSuelto(lector.a)
                ? (m_op.nombreSuelto.empty() ? std::wstring(L"archivo") : m_op.nombreSuelto)
                : RutaDeEntrada(e);
            lector.PonerEntrada(cruda);
            if (!Elegida(cruda)) continue;
            m_prog.PonerActual(cruda);

            if (numero > m_op.limites.entradas && !m_preguntoCantidad)
            {
                m_preguntoCantidad = true;
                if (!Preguntar(L"El archivo trae más de " + TextoNumero(m_op.limites.entradas) +
                               L" archivos. Puede ser una trampa para llenar el disco."))
                {
                    m_detenido = true;
                    break;
                }
            }

            if (EsEnlaceSimbolico(e))
            {
                Omitir(cruda, L"es un enlace simbólico; Jícaro no crea enlaces por seguridad");
                continue;
            }
            int tipo = archive_entry_filetype(e);
            bool conDatos = archive_entry_size_is_set(e) && archive_entry_size(e) > 0;
            bool duro = EsEnlaceDuro(e);
            if (!duro && tipo != 0 && tipo != AE_IFREG && tipo != AE_IFDIR)
            {
                Omitir(cruda, L"no es un archivo común (es un dispositivo o algo parecido)");
                continue;
            }
            if (archive_entry_is_data_encrypted(e) && Formato(lector.a) != ARCHIVE_FORMAT_ZIP)
            {
                Omitir(cruda, L"tiene contraseña de un tipo que Jícaro no puede abrir", Tipo::Cifrado);
                m_res.cifradas++;
                m_formatoCifrado = Formato(lector.a);
                continue;
            }

            if (tipo == AE_IFDIR && !duro)
            {
                auto relativa = Preparar(cruda, true);
                if (m_parar) break;
                if (!relativa) continue;
                if (!trabajo.Sigue())
                {
                    carpetaCambio();
                    break;
                }
                if (!SigueSeguro(*relativa))
                {
                    Omitir(cruda, L"la carpeta cambió mientras se extraía; no se tocó");
                    continue;
                }
                EntradaPropia s(archive_entry_new());
                archive_entry_copy_pathname_w(s.get(), relativa->c_str());
                archive_entry_set_filetype(s.get(), AE_IFDIR);
                archive_entry_set_perm(s.get(), 0755);
                CopiarFechas(e, s.get());
                if (archive_write_header(w, s.get()) < ARCHIVE_WARN) Omitir(cruda, L"no se pudo crear la carpeta", Tipo::Falla);
                else m_res.carpetas++;
                archive_write_finish_entry(w);
                continue;
            }

            // Enlace duro sin datos propios: se copia el archivo al que apunta, si ya se extrajo aquí
            std::optional<Hecho> origen;
            if (duro && !conDatos)
            {
                auto it = m_hechos.find(DestinoDeEnlace(e));
                if (it == m_hechos.end())
                {
                    Omitir(cruda, L"es un enlace a algo que no se extrajo; Jícaro no crea enlaces");
                    continue;
                }
                origen = it->second;
            }

            auto relativa = Preparar(cruda, false);
            if (m_parar) break;
            if (!relativa) continue;
            std::wstring completa = rutas::Unir(m_destino, *relativa);

            if (origen)
            {
                if (!CopiarEnlace(*origen, *relativa, cruda)) break;
                continue;
            }

            // Topes con el tamaño anunciado (también el de los archivos con huecos)
            if (archive_entry_size_is_set(e) && !ControlarAntes(archive_entry_size(e))) break;
            if (!trabajo.Sigue())
            {
                carpetaCambio();
                break;
            }
            if (!SigueSeguro(*relativa) || GetFileAttributesW(rutas::Larga(completa).c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                Omitir(cruda, L"la carpeta cambió mientras se extraía; no se tocó");
                continue;
            }

            // Jícaro crea el archivo con CREATE_NEW y su propio manejador: nunca toca algo que no creó, y si algo falla lo
            // quita por ese manejador (si una excepción corta todo, lo quita el destructor)
            ArchivoPropio nuevo;
            if (!CrearPropio(nuevo, completa, *relativa, cruda)) continue;
            bool fatal = false, incompleto = false;
            bool completo = CopiarDatos(nuevo, e, cruda, fatal, incompleto);
            bool cerrado = false;
            if (completo)
            {
                FILETIME creado{}, acceso{}, escrito{};
                bool hayC = archive_entry_birthtime_is_set(e) && AFiletime(archive_entry_birthtime(e), archive_entry_birthtime_nsec(e), creado);
                bool hayA = archive_entry_atime_is_set(e) && AFiletime(archive_entry_atime(e), archive_entry_atime_nsec(e), acceso);
                bool hayM = archive_entry_mtime_is_set(e) && AFiletime(archive_entry_mtime(e), archive_entry_mtime_nsec(e), escrito);
                cerrado = nuevo.Terminar(hayC ? &creado : nullptr, hayA ? &acceso : nullptr, hayM ? &escrito : nullptr);
            }
            if (!completo || !cerrado)
            {
                if (nuevo.Abierto()) QuitarPropio(nuevo, completa, *relativa);
                else QuitarAMedias(completa, *relativa, nuevo.id);
                if (m_cancelar || m_detenido || m_sinEspacio) break;
                if (fatal)
                {
                    Clasificar(lector, m_res.estado, m_res.mensaje);
                    break;
                }
                if (incompleto)
                {
                    if (lector.finDeLaUltima)
                    {
                        // Se acabó el archivo antes de tiempo: falta una parte o no se terminó de bajar
                        Clasificar(lector, m_res.estado, m_res.mensaje);
                        break;
                    }
                    Omitir(cruda, L"está incompleto o dañado", Tipo::Falla);
                    continue;
                }
                if (completo) Omitir(cruda, L"no se pudo terminar de escribir", Tipo::Falla);
                continue;
            }
            m_res.archivos++;
            m_prog.archivos++;
            m_hechos[cruda] = { completa, nuevo.id };
            Marcar(completa, *relativa);
        }
    }

    bool Extraccion::Paro() const
    {
        return m_cancelar || m_parar || m_detenido || m_sinEspacio || m_res.estado != Estado::Bien;
    }

    std::optional<std::wstring> Extraccion::PedirClave(const std::wstring& entrada, bool otraVez)
    {
        if (!otraVez && m_clave) return m_clave;
        if (!m_pr.contrasena || m_cancelar) return std::nullopt;
        auto c = m_pr.contrasena(entrada, otraVez);
        if (!c || c->empty()) return std::nullopt;
        m_dioClave = true;
        m_clave = c;
        return c;
    }

    void Extraccion::ConEscritor(const std::function<void(CarpetaDeTrabajo&, archive*)>& cuerpo)
    {
        CarpetaDeTrabajo trabajo(m_destino);
        if (!trabajo.ok)
        {
            m_res.mensaje = L"No se pudo usar la carpeta de destino (¿la ruta es muy larga?).";
            return;
        }
        // Defensas de libarchive: sin rutas absolutas, sin «..», sin escribir a través de enlaces y sin reemplazar nada
        // (Jícaro pregunta antes y manda lo anterior a la Papelera)
        EscritorPropio w(archive_write_disk_new());
        if (!w)
        {
            m_res.mensaje = L"No alcanzó la memoria para extraer.";
            return;
        }
        archive_write_disk_set_options(w.get(), ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_SECURE_SYMLINKS
                                                    | ARCHIVE_EXTRACT_SECURE_NODOTDOT | ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS
                                                    | ARCHIVE_EXTRACT_NO_OVERWRITE);
        m_res.estado = Estado::Bien;
        try
        {
            cuerpo(trabajo, w.get());
        }
        catch (...)
        {
            // Sin memoria u otra falla rara: se conserva lo que ya se sabe y se limpia el archivo en curso
            m_res.estado = Estado::Error;
            try
            {
                if (m_enCurso.activo)
                {
                    archive_write_finish_entry(w.get());
                    m_enCurso.activo = false;
                    QuitarAMedias(m_enCurso.completa, m_enCurso.relativa, m_enCurso.id);
                }
                m_res.mensaje = L"No alcanzó la memoria, o algo salió mal, y la extracción se detuvo.";
            }
            catch (...)
            {
            }
        }
        // «w» se cierra aquí, todavía con la carpeta de trabajo puesta (fechas de las carpetas)
    }

    void Extraccion::EjecutarLibarchive()
    {
        Lector lector(m_partesCompletas, m_prog, m_cancelar);
        m_lector = &lector;
        m_comprimido = lector.Total();
        lector.PonerPrevia(m_clave);
        if (!lector.Abrir(&m_pr, m_soloSuelto)) Clasificar(lector, m_res.estado, m_res.mensaje);
        else ConEscritor([&](CarpetaDeTrabajo& trabajo, archive* w) { Bucle(lector, trabajo, w); });
        m_lector = nullptr;
    }

    // ---------- RAR y 7z con 7z.dll: las mismas defensas, con los datos que entrega 7z.dll ----------

    uint64_t MemoriaPermitida()
    {
        uint64_t limite = 4ULL << 30;   // lo mismo que 7-Zip deja usar sin preguntar
        MEMORYSTATUSEX m{};
        m.dwLength = sizeof(m);
        if (GlobalMemoryStatusEx(&m) && m.ullTotalPhys > 0) limite = std::min<uint64_t>(limite, m.ullTotalPhys / 4 * 3);
        return limite;
    }

    // FILETIME (cada 100 ns desde 1601) a segundos y nanosegundos desde 1970
    bool AUnix(const FILETIME& f, int64_t& segundos, long& nano)
    {
        uint64_t t = ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime;
        if (t > (uint64_t)INT64_MAX) return false;
        int64_t v = (int64_t)t - 116444736000000000LL;
        segundos = v / 10000000;
        int64_t resto = v % 10000000;
        if (resto < 0)
        {
            resto += 10000000;
            segundos--;
        }
        nano = (long)(resto * 100);
        return true;
    }

    void PonerFechas(const motor7z::Item& it, archive_entry* e)
    {
        int64_t s = 0;
        long ns = 0;
        if (it.conMTime && AUnix(it.mtime, s, ns)) archive_entry_set_mtime(e, (time_t)s, ns);
        if (it.conATime && AUnix(it.atime, s, ns)) archive_entry_set_atime(e, (time_t)s, ns);
        if (it.conCTime && AUnix(it.ctime, s, ns)) archive_entry_set_birthtime(e, (time_t)s, ns);
    }

    std::wstring MensajeSinTope()
    {
        return L"Windows no dejó poner el tope de memoria con el que Jícaro abre los RAR y los 7z de forma segura, así que no se "
               L"abrió. Cierra otros programas y vuelve a intentarlo.";
    }

    // ZIP con 7z.dll: 7-Zip pasa la contraseña a la página de códigos ANSI de Windows. Los ZIP hechos en Linux o en Mac la
    // guardan en UTF-8, y los muy viejos en la página OEM: es la misma contraseña escrita de otra forma (lo mismo que se
    // prueba con libarchive). Cada forma va «disfrazada» de texto que la página ANSI convierte justo en esos bytes.
    std::vector<std::wstring> VariantesZip(const std::wstring& clave)
    {
        std::vector<std::wstring> r;
        if (std::all_of(clave.begin(), clave.end(), [](wchar_t c) { return c < 0x80; })) return r;
        auto disfrazar = [](const std::string& bytes) -> std::optional<std::wstring> {
            std::wstring w;
            for (unsigned char b : bytes)
            {
                wchar_t c = 0;
                if (MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(&b), 1, &c, 1) != 1) return std::nullopt;
                w.push_back(c);
            }
            BOOL perdio = FALSE;
            int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), (int)w.size(), nullptr, 0, nullptr, &perdio);
            if (n != (int)bytes.size() || perdio) return std::nullopt;
            std::string vuelta((size_t)n, '\0');
            WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), (int)w.size(), vuelta.data(), n, nullptr, &perdio);
            if (perdio || vuelta != bytes) return std::nullopt;
            return w;
        };
        auto agregar = [&](std::optional<std::wstring> v) {
            if (v && *v != clave && std::find(r.begin(), r.end(), *v) == r.end()) r.push_back(std::move(*v));
        };
        agregar(disfrazar(Utf8(clave)));
        BOOL perdio = FALSE;
        int n = WideCharToMultiByte(GetOEMCP(), WC_NO_BEST_FIT_CHARS, clave.data(), (int)clave.size(), nullptr, 0, nullptr, &perdio);
        if (n > 0 && !perdio)
        {
            std::string oem((size_t)n, '\0');
            WideCharToMultiByte(GetOEMCP(), WC_NO_BEST_FIT_CHARS, clave.data(), (int)clave.size(), oem.data(), n, nullptr, &perdio);
            if (!perdio) agregar(disfrazar(oem));
        }
        return r;
    }

    std::wstring MensajeZipSinMotor()
    {
        return L"Para abrir un ZIP en partes (.z01, .z02... y .zip), Jícaro necesita el archivo 7z.dll junto a Jicaro.exe, y "
               L"no está: vuelve a bajar Jícaro y descomprímelo entero.";
    }

    std::wstring MensajeFalta(const std::wstring& parte)
    {
        return L"Falta la parte «" + parte + L"». Revisa que se hayan bajado todas las partes y que estén en la misma carpeta.";
    }

    // Lo que hace Jícaro con cada archivo que entrega 7z.dll: lo mismo que con libarchive (rutas, topes, marca de la
    // Web, limpieza de lo que queda a medias)
    class Extraccion::Destino7z final : public motor7z::Destino
    {
    public:
        Destino7z(Extraccion& x, const motor7z::Archivo& arc, CarpetaDeTrabajo& trabajo, archive* w)
            : x(x), m_items(arc.Items()), m_trabajo(trabajo), m_w(w), m_faltaVolumen(!arc.FaltaVolumen().empty())
        {
        }

        std::vector<uint32_t> clavesMalas;   // no salieron porque la contraseña no sirvió (se puede probar otra)
        std::vector<uint32_t> metodoDesconocido;   // un método que 7z.dll no trae (por ejemplo, zstd en un 7z)
        bool sinClave = false;               // se pidió la contraseña y no se dio
        bool memoriaNegada = false;
        uint64_t memoriaPedida = 0;
        bool carpetaCambio = false;

        ~Destino7z() override
        {
            // Si algo lanzó una excepción en medio, el archivo a medias se quita por su manejador
            try
            {
                if (m_activo) Abandonar();
            }
            catch (...)
            {
            }
        }

        motor7z::Paso Empieza(uint32_t indice) override
        {
            // Si 7z.dll empieza otro sin haber dicho cómo terminó el anterior, el anterior se da por fallido (se quita)
            if (m_activo)
            {
                Abandonar();
                x.Omitir(m_cruda, L"no se terminó de extraer", Tipo::Falla);
            }
            if (x.m_cancelar || indice >= m_items.size()) return motor7z::Paso::Parar;
            const auto& it = m_items[indice];
            m_cruda = it.ruta;
            m_cifrada = it.cifrada;
            m_indice = indice;
            m_item = &it;
            x.m_prog.PonerActual(m_cruda);
            auto relativa = x.Preparar(m_cruda, false);
            if (x.m_parar) return motor7z::Paso::Parar;
            if (!relativa) return motor7z::Paso::Saltar;
            std::wstring completa = rutas::Unir(x.m_destino, *relativa);
            // Topes con el tamaño anunciado, antes de crear nada
            if (it.tamano > 0 && !x.ControlarAntes(it.tamano)) return motor7z::Paso::Parar;
            if (!m_trabajo.Sigue())
            {
                carpetaCambio = true;
                x.m_res.estado = Estado::Error;
                x.m_res.mensaje = L"Algo cambió la carpeta de trabajo en medio de la extracción; se detuvo por seguridad.";
                return motor7z::Paso::Parar;
            }
            // Jícaro crea el archivo (CREATE_NEW y su propio manejador): nunca toca algo que no creó
            if (!x.CrearPropio(m_archivo, completa, *relativa, m_cruda)) return motor7z::Paso::Saltar;
            m_activo = true;
            m_completa = completa;
            m_relativa = *relativa;
            m_escrito = 0;
            m_demasiado = false;
            m_fallaEscritura = false;
            return motor7z::Paso::Escribir;
        }

        bool Datos(const void* datos, size_t tamano) override
        {
            if (!m_activo) return true;
            if (x.m_cancelar) return false;
            if (m_fallaEscritura || m_demasiado) return true;   // lo que falta de este archivo se descarta
            // Más datos que el tamaño que anuncia: no se escriben (al terminar, se quita por dañado)
            if (m_item->tamano >= 0 && (int64_t)tamano > m_item->tamano - m_escrito)
            {
                m_demasiado = true;
                return true;
            }
            if (!m_archivo.Escribir(datos, tamano, m_escrito))
            {
                int64_t libre = EspacioLibre(x.m_destino);
                if (libre >= 0 && libre < (64LL << 20))
                {
                    x.m_sinEspacio = true;
                    return false;
                }
                m_fallaEscritura = true;
                return true;
            }
            m_escrito += (int64_t)tamano;
            return x.Controlar((int64_t)tamano);
        }

        void Termina(int32_t resultado) override
        {
            if (!m_activo) return;
            // Tiene que llegar justo el tamaño que anuncia el archivo: nada crece a escondidas de los topes
            const bool tamanoBien = !m_demasiado && (m_item->tamano < 0 || m_escrito == m_item->tamano);
            if (resultado == motor7z::kBien && !m_fallaEscritura && tamanoBien)
            {
                const auto& it = *m_item;
                m_activo = false;
                if (!m_archivo.Terminar(it.conCTime ? &it.ctime : nullptr, it.conATime ? &it.atime : nullptr,
                                        it.conMTime ? &it.mtime : nullptr))
                {
                    x.QuitarAMedias(m_completa, m_relativa, m_archivo.id);
                    x.Omitir(m_cruda, L"no se pudo terminar de escribir", Tipo::Falla);
                    return;
                }
                x.m_res.archivos++;
                x.m_prog.archivos++;
                x.m_hechos[m_cruda] = { m_completa, m_archivo.id };
                x.Marcar(m_completa, m_relativa);
                return;
            }
            Abandonar();
            if (m_fallaEscritura)
            {
                x.Omitir(m_cruda, L"no se pudo escribir en el disco", Tipo::Falla);
                return;
            }
            if (resultado == motor7z::kBien)
            {
                x.Omitir(m_cruda, L"está dañado: no trae el tamaño que anuncia", Tipo::Falla);
                return;
            }
            Fallo(m_indice, m_cruda, m_cifrada, resultado);
        }

        // Quita el archivo a medias por su manejador (es el que se creó aquí). Si Windows no deja, se dice cuál quedó y
        // se le pone la marca de la Web.
        void Abandonar()
        {
            m_activo = false;
            x.QuitarPropio(m_archivo, m_completa, m_relativa);
        }
        void SinDatos(uint32_t indice, int32_t resultado) override
        {
            if (resultado == motor7z::kBien || indice >= m_items.size()) return;
            Fallo(indice, m_items[indice].ruta, m_items[indice].cifrada, resultado);
        }

        void Fallo(uint32_t indice, const std::wstring& cruda, bool cifrada, int32_t resultado)
        {
            // La contraseña no sirvió: a veces 7z.dll lo sabe (RAR 5 casi siempre trae con qué comprobarla). Si no, un dato
            // que no cuadra en un archivo con contraseña casi siempre es la contraseña (salvo que falte una parte), así
            // que se puede probar otra; el aviso dice «o el archivo está dañado».
            if (resultado == motor7z::kClaveIncorrecta
                || (cifrada && !m_faltaVolumen && (resultado == motor7z::kDatosDanados || resultado == motor7z::kCrcMalo)))
            {
                clavesMalas.push_back(indice);
                return;
            }
            if (m_faltaVolumen && resultado != motor7z::kMetodoDesconocido) resultado = motor7z::kFinInesperado;
            switch (resultado)
            {
            case motor7z::kFinInesperado:
            case motor7z::kNoDisponible:
                x.m_incompletos++;
                x.Omitir(cruda, L"está incompleto (¿falta una parte o no se terminó de bajar?)", Tipo::Falla);
                break;
            case motor7z::kMetodoDesconocido: metodoDesconocido.push_back(indice); break;
            default: x.Omitir(cruda, L"está dañado: los datos no coinciden con su suma de control", Tipo::Falla); break;
            }
        }

        // El primero que se va a pedir: 7z pide la contraseña antes de empezar el primer archivo de cada bloque
        std::wstring proximo;

        std::optional<std::wstring> Clave() override
        {
            if (x.m_cancelar) return std::nullopt;
            auto c = x.PedirClave(m_cruda.empty() ? proximo : m_cruda, false);
            if (!c) sinClave = true;
            return c;
        }

        bool Memoria(uint64_t bytes) override
        {
            if (bytes <= MemoriaPermitida()) return true;
            memoriaNegada = true;
            memoriaPedida = bytes;
            return false;
        }

        // Después de cada vuelta de 7z.dll: lo que quedó a medias se quita y se dice por qué se cortó
        void Despues(HRESULT hr)
        {
            if (m_activo) Abandonar();
            if (hr == S_OK || x.Paro()) return;   // si se detuvo por algo que ya se sabe, eso es lo que se dice
            if (sinClave)
            {
                x.m_res.estado = x.m_dioClave ? Estado::ContrasenaIncorrecta : Estado::SinContrasena;
                x.m_res.mensaje = x.m_dioClave ? L"La contraseña no es correcta, así que no se extrajo todo."
                                               : L"Hacía falta la contraseña y no se dio, así que no se extrajo.";
                return;
            }
            x.m_res.estado = Estado::Error;
            if (memoriaNegada)
                x.m_res.mensaje = L"Este archivo necesita " + TextoTamano((int64_t)memoriaPedida) +
                    L" de memoria para descomprimirse, más de lo que Jícaro se permite usar en esta PC.";
            else if (hr == E_OUTOFMEMORY)
                x.m_res.mensaje = L"No alcanzó la memoria para extraer este archivo.";
            else
                x.m_res.mensaje = L"El archivo está dañado o no se puede leer.";
        }

    private:
        Extraccion& x;
        const std::vector<motor7z::Item>& m_items;
        CarpetaDeTrabajo& m_trabajo;
        archive* m_w;
        std::wstring m_cruda, m_completa, m_relativa;
        uint32_t m_indice = 0;
        const motor7z::Item* m_item = nullptr;
        ArchivoPropio m_archivo;   // el archivo que se está escribiendo (lo creó Jícaro)
        bool m_cifrada = false, m_activo = false, m_fallaEscritura = false, m_demasiado = false;
        const bool m_faltaVolumen;
        int64_t m_escrito = 0;
    };

    void Extraccion::Bucle7z(motor7z::Archivo& arc, CarpetaDeTrabajo& trabajo, archive* w)
    {
        const auto& items = arc.Items();
        int64_t elegidos = 0;
        for (const auto& it : items)
            if (Elegida(it.ruta)) elegidos++;
        if (elegidos > m_op.limites.entradas && !m_preguntoCantidad)
        {
            m_preguntoCantidad = true;
            if (!Preguntar(L"El archivo trae más de " + TextoNumero(m_op.limites.entradas) +
                           L" archivos. Puede ser una trampa para llenar el disco."))
            {
                m_detenido = true;
                return;
            }
        }

        // Primero lo que no se extrae y las carpetas; los archivos van a 7z.dll en orden
        std::vector<uint32_t> pedir, enlaces;
        for (uint32_t i = 0; i < (uint32_t)items.size(); i++)
        {
            if (m_cancelar)
            {
                m_res.estado = Estado::Cancelado;
                return;
            }
            const auto& it = items[i];
            const std::wstring& cruda = it.ruta;
            if (!Elegida(cruda)) continue;
            if (it.anti)
            {
                Omitir(cruda, L"es una marca para borrar un archivo; Jícaro no borra nada");
                continue;
            }
            if (it.alterno)
            {
                Omitir(cruda, L"es un flujo alterno de NTFS; Jícaro no los crea por seguridad");
                continue;
            }
            if (it.simbolico)
            {
                Omitir(cruda, L"es un enlace simbólico; Jícaro no crea enlaces por seguridad");
                continue;
            }
            if (it.especial)
            {
                Omitir(cruda, L"no es un archivo común (es un dispositivo o algo parecido)");
                continue;
            }
            // Con contraseña y sin de dónde sacarla (ni la que vino ni a quién preguntar): se salta y lo demás sale igual
            if (it.cifrada && !it.carpeta && !m_clave && !m_pr.contrasena)
            {
                Omitir(cruda, L"tiene contraseña y no se dio", Tipo::Clave);
                m_res.sinClave++;
                continue;
            }
            // Enlace duro o copia (RAR 5): al final se copia lo que ya salió, como con libarchive
            if (!it.duro.empty() && !it.carpeta)
            {
                enlaces.push_back(i);
                continue;
            }
            if (it.carpeta)
            {
                m_prog.PonerActual(cruda);
                auto relativa = Preparar(cruda, true);
                if (m_parar) return;
                if (!relativa) continue;
                if (!trabajo.Sigue())
                {
                    m_res.estado = Estado::Error;
                    m_res.mensaje = L"Algo cambió la carpeta de trabajo en medio de la extracción; se detuvo por seguridad.";
                    return;
                }
                if (!SigueSeguro(*relativa))
                {
                    Omitir(cruda, L"la carpeta cambió mientras se extraía; no se tocó");
                    continue;
                }
                EntradaPropia s(archive_entry_new());
                if (!s) throw std::bad_alloc();
                archive_entry_copy_pathname_w(s.get(), relativa->c_str());
                archive_entry_set_filetype(s.get(), AE_IFDIR);
                archive_entry_set_perm(s.get(), 0755);
                PonerFechas(it, s.get());
                if (archive_write_header(w, s.get()) < ARCHIVE_WARN) Omitir(cruda, L"no se pudo crear la carpeta", Tipo::Falla);
                else m_res.carpetas++;
                archive_write_finish_entry(w);
                continue;
            }
            pedir.push_back(i);
        }

        Destino7z d(*this, arc, trabajo, w);
        if (!pedir.empty()) d.proximo = items[pedir.front()].ruta;
        // ZIP: antes de pedir otra contraseña, la misma escrita de otra forma (UTF-8, OEM), solo con los que fallaron
        auto probarVariantes = [&]() {
            if (m_formato7z != motor7z::Formato::Zip || !m_clave || d.clavesMalas.empty() || Paro()) return;
            const std::wstring original = *m_clave;
            for (auto& v : VariantesZip(original))
            {
                if (d.clavesMalas.empty() || Paro()) break;
                m_clave = v;
                std::vector<uint32_t> otraVez;
                otraVez.swap(d.clavesMalas);
                d.Despues(arc.Extraer(otraVez, d));
                BorrarTexto(v);
            }
            if (m_clave) BorrarTexto(*m_clave);
            m_clave = original;
        };
        HRESULT hr = arc.Extraer(pedir, d);
        d.Despues(hr);
        probarVariantes();
        // Los que no salieron porque la contraseña no sirvió: se pregunta otra (si hay a quién) y se prueban solo esos.
        // Nunca se prueban claves por cuenta propia.
        while (!d.clavesMalas.empty() && !Paro() && m_pr.contrasena)
        {
            auto c = PedirClave(items[d.clavesMalas.front()].ruta, true);
            if (!c) break;
            std::vector<uint32_t> otraVez;
            otraVez.swap(d.clavesMalas);
            hr = arc.Extraer(otraVez, d);
            d.Despues(hr);
            probarVariantes();
        }
        for (uint32_t i : d.clavesMalas)
        {
            Omitir(items[i].ruta, L"la contraseña no sirvió (o el archivo está dañado)", Tipo::Clave);
            m_res.clavesMalas++;
        }
        if (!d.metodoDesconocido.empty()) ProbarConLibarchive(items, d.metodoDesconocido, trabajo, w);

        // Enlaces duros y copias: se copia el archivo que ya salió en esta extracción, con los mismos topes
        for (uint32_t i : enlaces)
        {
            if (Paro()) break;
            const auto& it = items[i];
            auto o = m_hechos.find(it.duro);
            if (o == m_hechos.end())
            {
                Omitir(it.ruta, L"es un enlace a algo que no se extrajo; Jícaro no crea enlaces");
                continue;
            }
            Hecho origen = o->second;
            m_prog.PonerActual(it.ruta);
            auto relativa = Preparar(it.ruta, false);
            if (m_parar) break;
            if (!relativa) continue;
            if (!CopiarEnlace(origen, *relativa, it.ruta)) break;
        }
    }

    void Extraccion::ProbarConLibarchive(const std::vector<motor7z::Item>& items, const std::vector<uint32_t>& indices,
                                         CarpetaDeTrabajo& trabajo, archive* w)
    {
        // 7z con un método que 7z.dll no trae (por ejemplo zstd, del 7-Zip modificado): se prueban esos con libarchive,
        // que sí lo conoce, con las mismas defensas
        if (m_formato7z == motor7z::Formato::SieteZ && !Paro())
        {
            auto elegidas = std::move(m_elegidas);
            m_elegidas.clear();
            for (uint32_t i : indices) m_elegidas.insert(items[i].ruta);
            const Estado estadoAntes = m_res.estado;
            const std::wstring mensajeAntes = m_res.mensaje;
            {
                Lector lector(m_partesCompletas, m_prog, m_cancelar);
                m_lector = &lector;
                lector.PonerPrevia(m_clave);
                if (lector.Abrir(&m_pr, false)) Bucle(lector, trabajo, w);
                m_lector = nullptr;
            }
            m_elegidas = std::move(elegidas);
            // Una falla de libarchive aquí no cambia el resultado de todo lo demás
            if (m_res.estado != Estado::Bien && !m_cancelar && !m_parar && !m_detenido && !m_sinEspacio)
            {
                m_res.estado = estadoAntes;
                m_res.mensaje = mensajeAntes;
            }
        }
        for (uint32_t i : indices)
        {
            const std::wstring& cruda = items[i].ruta;
            bool omitida = std::any_of(m_res.omitidas.begin(), m_res.omitidas.end(), [&](const Omitida& o) { return o.ruta == cruda; });
            if (!m_hechos.count(cruda) && !omitida) Omitir(cruda, L"usa un método de compresión que Jícaro no conoce", Tipo::Falla);
        }
    }

    void Extraccion::Ejecutar7z(motor7z::Formato formato)
    {
        m_formato7z = formato;
        motor7z::Archivo arc;
        motor7z::Apertura ap = motor7z::Apertura::Error;
        // Si la lista tiene contraseña, 7z.dll la pide al abrir. Si no sirve, se pregunta otra (si hay a quién).
        for (int intento = 0;; intento++)
        {
            const bool otraVez = intento > 0;
            ap = arc.Abrir(m_partesCompletas, formato, [this, otraVez]() { return PedirClave(L"", otraVez); }, m_prog, m_cancelar);
            if (ap != motor7z::Apertura::ClaveMala) break;
            if (m_clave) BorrarTexto(*m_clave);
            m_clave.reset();
            if (!m_pr.contrasena || m_cancelar) break;
        }
        m_comprimido = std::max<int64_t>(arc.TotalComprimido(), 1);
        const bool varias = m_partesCompletas.size() > 1;
        switch (ap)
        {
        case motor7z::Apertura::Bien: break;
        case motor7z::Apertura::Cancelado:
            m_res.estado = Estado::Cancelado;
            m_res.mensaje = L"Se canceló.";
            return;
        case motor7z::Apertura::SinClave:
            m_res.estado = m_dioClave ? Estado::ContrasenaIncorrecta : Estado::SinContrasena;
            m_res.mensaje = m_dioClave ? L"La contraseña no es correcta, así que no se pudo abrir."
                                       : L"Hacía falta la contraseña y no se dio, así que no se extrajo.";
            return;
        case motor7z::Apertura::ClaveMala:
            m_res.estado = Estado::ContrasenaIncorrecta;
            m_res.mensaje = L"La contraseña no es correcta (o el archivo está dañado), así que no se pudo abrir.";
            return;
        case motor7z::Apertura::SinMemoria:
            m_res.estado = Estado::Error;
            m_res.mensaje = L"No alcanzó la memoria para abrir este archivo.";
            return;
        case motor7z::Apertura::SinTope:
            m_res.estado = Estado::Error;
            m_res.mensaje = MensajeSinTope();
            return;
        default:
            if (!arc.NoAbrio().empty())
            {
                m_res.estado = varias ? Estado::FaltaParte : Estado::Error;
                m_res.mensaje = L"No se pudo abrir «" + NombreDe(arc.NoAbrio()) + L"». ¿Se movió o se borró?";
            }
            else if (!arc.FaltaVolumen().empty())
            {
                m_res.estado = Estado::FaltaParte;
                m_res.mensaje = MensajeFalta(arc.FaltaVolumen());
            }
            else if (arc.Incompleto())
            {
                m_res.estado = varias ? Estado::FaltaParte : Estado::Error;
                m_res.mensaje = varias ? L"Falta la parte que va después de «" + NombreDe(m_partesCompletas.back()) +
                                             L"». Revisa que se hayan bajado todas las partes y que estén en la misma carpeta."
                                       : L"El archivo está incompleto: se acaba antes de tiempo. ¿Se terminó de bajar?";
            }
            else if (ap == motor7z::Apertura::NoEsArchivo)
            {
                m_res.estado = Estado::NoEsArchivo;
                m_res.mensaje = L"No es un archivo comprimido que Jícaro conozca, o está dañado.";
            }
            else
            {
                m_res.estado = Estado::Error;
                m_res.mensaje = L"El archivo está dañado o no se puede leer.";
            }
            return;
        }
        if (arc.ClaveUsada())
        {
            m_clave = arc.ClaveUsada();
            m_dioClave = true;
        }
        // Las partes que 7z.dll abrió por su nombre y Jícaro no había juntado (por ejemplo «juego2.rar» después de
        // «juego1.rar») cuentan igual: para la marca de la Web, para el tamaño y para no reemplazarlas nunca
        bool otras = false;
        for (const auto& v : arc.Volumenes())
        {
            std::wstring completa = RutaCompleta(v);
            bool ya = std::any_of(m_partesCompletas.begin(), m_partesCompletas.end(),
                                  [&](const std::wstring& p) { return IgualesSinMayusculas(p, completa); });
            if (ya) continue;
            m_partesCompletas.push_back(completa);
            m_idPartes.push_back(IdentidadDe(completa));
            if (int64_t t = TamanoArchivo(completa); t > 0) m_comprimido += t;
            otras = true;
        }
        if (otras && m_op.pasarMarca)
        {
            m_zona = marca::ZonaDelJuego(m_partesCompletas, m_referente);
            m_res.zona = marca::HayQueMarcar(m_zona) ? m_zona : 0;
        }
        ConEscritor([&](CarpetaDeTrabajo& trabajo, archive* w) { Bucle7z(arc, trabajo, w); });

        // Falta una parte que según el archivo sigue (aunque nada haya fallado: lo que trae esa parte ni se ve), o algo se
        // acabó antes de tiempo
        if (m_res.estado == Estado::Bien && !m_cancelar && !m_parar && !m_detenido && !m_sinEspacio)
        {
            std::wstring falta = arc.FaltaVolumen();
            if (!falta.empty())
            {
                m_res.estado = Estado::FaltaParte;
                m_res.mensaje = MensajeFalta(falta);
            }
            else if (m_incompletos > 0 && m_res.archivos == 0)
            {
                m_res.estado = varias ? Estado::FaltaParte : Estado::Error;
                m_res.mensaje = varias ? L"Falta la parte que va después de «" + NombreDe(m_partesCompletas.back()) +
                                             L"». Revisa que se hayan bajado todas las partes y que estén en la misma carpeta."
                                       : L"El archivo está incompleto: se acaba antes de tiempo. ¿Se terminó de bajar?";
            }
        }
    }
    Resumen Extraccion::Ejecutar()
    {
        m_prog.Reiniciar();
        m_prog.totalEsperado = m_op.totalEsperado;
        if (m_op.partes.empty())
        {
            m_res.mensaje = L"No hay nada que extraer.";
            return m_res;
        }
        for (auto& p : m_op.partes)
        {
            m_partesCompletas.push_back(RutaCompleta(p));
            m_idPartes.push_back(IdentidadDe(m_partesCompletas.back()));
        }
        for (auto& e : m_op.elegidas)
        {
            std::wstring r = e;
            for (auto& c : r)
                if (c == L'/') c = L'\\';
            m_elegidas.insert(r);
        }

        m_destino = RutaCompleta(m_op.destino);
        while (m_destino.size() > 3 && m_destino.back() == L'\\') m_destino.pop_back();
        if (m_destino.empty() || !CrearCarpetas(m_destino) || !ExisteCarpeta(m_destino))
        {
            m_res.mensaje = L"No se pudo crear la carpeta de destino.";
            return m_res;
        }
        m_libreAlEmpezar = EspacioLibre(m_destino);

        ComIniciado com;   // para la marca de la Web y la Papelera
        if (m_op.pasarMarca) m_zona = marca::ZonaDelJuego(m_partesCompletas, m_referente);
        m_res.zona = marca::HayQueMarcar(m_zona) ? m_zona : 0;
        if (m_op.contrasena && !m_op.contrasena->empty())
        {
            m_clave = m_op.contrasena;
            m_dioClave = true;
        }

        // Los RAR, los 7z y los ZIP en partes, con 7z.dll (si está junto al programa); lo demás, con libarchive
        std::optional<motor7z::Formato> formato;
        if (!m_soloSuelto && motor7z::Disponible()) formato = motor7z::DetectarPartes(m_partesCompletas);
        if (formato) Ejecutar7z(*formato);
        else if (motor7z::EsZipEnPartes(m_partesCompletas))
        {
            // libarchive no junta los ZIP en partes: sin 7z.dll no se pueden abrir
            m_res.estado = Estado::Error;
            m_res.mensaje = MensajeZipSinMotor();
        }
        else EjecutarLibarchive();
        if (m_clave) BorrarTexto(*m_clave);
        m_clave.reset();

        if (m_parar || (m_cancelar && m_res.estado == Estado::Bien))
        {
            m_res.estado = Estado::Cancelado;
            m_res.mensaje = L"Se canceló.";
        }
        else if (m_detenido)
        {
            m_res.estado = Estado::Detenido;
            m_res.mensaje = L"Se detuvo para no llenar el disco.";
        }
        else if (m_sinEspacio)
        {
            m_res.estado = Estado::SinEspacio;
            m_res.mensaje = L"No queda espacio suficiente en el disco de destino.";
        }
        else if (m_res.estado == Estado::Bien && m_res.cifradas > 0)
        {
            // Con contraseña de un tipo que Jícaro no abre: se dice claro qué hacer
            if (m_res.archivos == 0)
            {
                m_res.estado = Estado::CifradoNoSoportado;
                m_res.mensaje = MensajeCifrado(m_formatoCifrado);
            }
            else
            {
                m_res.mensaje = L"Se extrajeron " + TextoNumero(m_res.archivos) + L" archivos, pero " +
                    TextoNumero(m_res.cifradas) + L" tienen una contraseña que Jícaro no pudo abrir (falta 7z.dll junto a "
                    L"Jicaro.exe, o es un cifrado que no conoce).";
            }
        }
        else if (m_res.estado == Estado::Bien && m_res.clavesMalas > 0)
        {
            if (m_res.archivos == 0)
            {
                m_res.estado = Estado::ContrasenaIncorrecta;
                m_res.mensaje = L"La contraseña no es correcta, así que no se extrajo.";
            }
            else
            {
                m_res.mensaje = L"Se extrajeron " + TextoNumero(m_res.archivos) + L" archivos, pero " +
                    TextoNumero(m_res.clavesMalas) + L" no salieron porque la contraseña no sirvió.";
            }
        }
        else if (m_res.estado == Estado::Bien && m_res.sinClave > 0)
        {
            if (m_res.archivos == 0)
            {
                m_res.estado = Estado::SinContrasena;
                m_res.mensaje = L"Hacía falta la contraseña y no se dio, así que no se extrajo.";
            }
            else
            {
                m_res.mensaje = L"Se extrajeron " + TextoNumero(m_res.archivos) + L" archivos, pero " +
                    TextoNumero(m_res.sinClave) + L" tienen contraseña y no se dio.";
            }
        }
        if (m_res.mensaje.empty()) m_res.mensaje = TextoEstado(m_res.estado);
        return m_res;
    }
}
static jicaro::Contenido ListarAdentro(const std::vector<std::wstring>& partes, const std::wstring& nombreSuelto,
                                       jicaro::Progreso& progreso, const std::atomic<bool>& cancelar, bool soloSuelto)
{
    using namespace jicaro;
    Contenido c;
    progreso.Reiniciar();
    std::vector<std::wstring> completas;
    for (auto& p : partes) completas.push_back(RutaCompleta(p));
    Lector l(completas, progreso, cancelar);
    if (!l.Abrir(nullptr, soloSuelto))
    {
        Clasificar(l, c.estado, c.mensaje);
        return c;
    }
    archive_entry* e = nullptr;
    int fallasSeguidas = 0;
    size_t letras = 0;
    for (;;)
    {
        int r = archive_read_next_header(l.a, &e);
        if (r == ARCHIVE_EOF)
        {
            c.completo = true;
            break;
        }
        if (r == ARCHIVE_RETRY || r == ARCHIVE_FAILED)
        {
            if (++fallasSeguidas > 100)
            {
                c.estado = Estado::Error;
                c.mensaje = L"El archivo está dañado: demasiadas entradas que no se pueden leer.";
                break;
            }
            continue;
        }
        if (r < ARCHIVE_WARN)
        {
            Clasificar(l, c.estado, c.mensaje);
            break;
        }
        fallasSeguidas = 0;
        // Para listar un TAR comprimido hay que descomprimirlo: si crece demasiado, la lista queda a medias (al
        // extraer, los topes contra bombas preguntan antes de llenar el disco)
        int64_t descomprimido = archive_filter_bytes(l.a, 0);
        if (descomprimido > (1LL << 30) && (double)descomprimido > 100.0 * (double)std::max<int64_t>(l.Total(), 1))
        {
            c.estado = Estado::Bien;
            c.mensaje = L"La lista quedó a medias: lo que trae crece demasiado al descomprimirlo (puede ser una bomba de "
                        L"descompresión). Si lo extraes, Jícaro pregunta antes de llenar el disco.";
            c.formato = NombreFormato(l.a);
            return c;
        }
        if (c.entradas.empty() && EsSueltoSinCompresion(l.a))
        {
            c.estado = Estado::NoEsArchivo;
            c.mensaje = L"No es un archivo comprimido que Jícaro conozca, o está dañado.";
            c.formato = NombreFormato(l.a);
            return c;
        }
        Entrada en;
        bool suelto = EsSuelto(l.a);
        en.ruta = suelto ? (nombreSuelto.empty() ? std::wstring(L"archivo") : nombreSuelto) : RutaDeEntrada(e);
        int tipo = archive_entry_filetype(e);
        en.carpeta = tipo == AE_IFDIR && !EsEnlaceDuro(e);
        en.enlace = EsEnlaceSimbolico(e) || EsEnlaceDuro(e);
        en.cifrada = archive_entry_is_encrypted(e) != 0;
        en.tamano = (!en.carpeta && archive_entry_size_is_set(e)) ? archive_entry_size(e) : -1;
        en.fecha = archive_entry_mtime_is_set(e) ? (int64_t)archive_entry_mtime(e) : 0;
        if (en.carpeta && (en.ruta.empty() || en.ruta == L"." || en.ruta == L".\\")) continue;   // la carpeta raíz de una ISO
        if (en.tamano > 0) c.total += en.tamano;
        if (en.cifrada) c.cifradas++;
        progreso.PonerActual(en.ruta);
        progreso.archivos++;
        letras += en.ruta.size();
        c.entradas.push_back(std::move(en));
        if (suelto)
        {
            // Un solo archivo: no hace falta descomprimirlo entero para mostrarlo
            c.suelto = true;
            c.completo = true;
            break;
        }
        // Topes para la memoria: con más que esto, la lista se muestra a medias (extraer todo sigue funcionando)
        if (c.entradas.size() >= 1000000 || letras >= (64u << 20))
        {
            c.estado = Estado::Bien;
            c.mensaje = L"El archivo trae demasiados archivos para mostrarlos todos: la lista está a medias. «Extraer todo» funciona igual.";
            c.formato = NombreFormato(l.a);
            return c;
        }
    }
    c.formato = NombreFormato(l.a);
    c.cifradoSoportado = Formato(l.a) == ARCHIVE_FORMAT_ZIP;
    if (c.completo || (c.mensaje.empty() && !c.entradas.empty())) c.estado = Estado::Bien;
    if (c.completo && c.entradas.empty() && c.mensaje.empty()) c.mensaje = L"El archivo está vacío.";
    return c;
}

// RAR y 7z con 7z.dll. Si la lista tiene contraseña, se prueba «clave» y después se pregunta; nunca se prueban claves
// por cuenta propia.
static jicaro::Contenido ListarCon7z(jicaro::motor7z::Formato formato, const std::vector<std::wstring>& completas,
                                     jicaro::Progreso& progreso, const std::atomic<bool>& cancelar,
                                     jicaro::Preguntas* preguntas, const std::optional<std::wstring>& clave)
{
    using namespace jicaro;
    Contenido c;
    c.cifradoSoportado = true;
    motor7z::Archivo arc;
    std::optional<std::wstring> actual = clave;
    if (actual && actual->empty()) actual.reset();
    bool dio = actual.has_value();
    motor7z::Apertura ap = motor7z::Apertura::Error;
    for (int intento = 0;; intento++)
    {
        const bool otraVez = intento > 0;
        auto pedir = [&actual, &dio, preguntas, &cancelar, otraVez]() -> std::optional<std::wstring> {
            if (!otraVez && actual) return actual;
            if (!preguntas || !preguntas->contrasena || cancelar) return std::nullopt;
            auto r = preguntas->contrasena(L"", otraVez);
            if (!r || r->empty()) return std::nullopt;
            dio = true;
            actual = r;
            return r;
        };
        ap = arc.Abrir(completas, formato, pedir, progreso, cancelar);
        if (ap != motor7z::Apertura::ClaveMala) break;
        if (actual) BorrarTexto(*actual);
        actual.reset();
        if (!preguntas || !preguntas->contrasena || cancelar) break;
    }
    if (actual) BorrarTexto(*actual);
    c.formato = arc.NombreFormato();
    const bool varias = completas.size() > 1;
    switch (ap)
    {
    case motor7z::Apertura::Bien: break;
    case motor7z::Apertura::Cancelado:
        c.estado = Estado::Cancelado;
        c.mensaje = L"Se canceló.";
        return c;
    case motor7z::Apertura::SinClave:
        c.estado = dio ? Estado::ContrasenaIncorrecta : Estado::SinContrasena;
        c.mensaje = dio ? L"La contraseña no es correcta: sin la buena no se puede ver qué trae este archivo."
                        : L"Este archivo tiene contraseña hasta para ver qué trae. Ábrelo otra vez para escribirla.";
        return c;
    case motor7z::Apertura::ClaveMala:
        c.estado = Estado::ContrasenaIncorrecta;
        c.mensaje = L"La contraseña no es correcta (o el archivo está dañado): no se puede ver qué trae.";
        return c;
    case motor7z::Apertura::SinMemoria:
        c.mensaje = L"No alcanzó la memoria para leer la lista de este archivo.";
        return c;
    case motor7z::Apertura::SinTope:
        c.mensaje = MensajeSinTope();
        return c;
    default:
        if (!arc.NoAbrio().empty())
        {
            c.estado = varias ? Estado::FaltaParte : Estado::Error;
            c.mensaje = L"No se pudo abrir «" + NombreDe(arc.NoAbrio()) + L"». ¿Se movió o se borró?";
        }
        else if (!arc.FaltaVolumen().empty())
        {
            c.estado = Estado::FaltaParte;
            c.faltan.push_back(arc.FaltaVolumen());
            c.mensaje = MensajeFalta(arc.FaltaVolumen());
        }
        else if (arc.Incompleto())
        {
            c.estado = varias ? Estado::FaltaParte : Estado::Error;
            c.mensaje = varias ? L"Falta la parte que va después de «" + NombreDe(completas.back()) +
                                     L"». Revisa que se hayan bajado todas las partes y que estén en la misma carpeta."
                               : L"El archivo está incompleto: se acaba antes de tiempo. ¿Se terminó de bajar?";
        }
        else if (ap == motor7z::Apertura::NoEsArchivo)
        {
            c.estado = Estado::NoEsArchivo;
            c.mensaje = L"No es un archivo comprimido que Jícaro conozca, o está dañado.";
        }
        else
        {
            c.estado = Estado::Error;
            c.mensaje = L"El archivo está dañado o no se puede leer.";
        }
        return c;
    }
    if (arc.PidioClave() && arc.ClaveUsada()) c.clave = *arc.ClaveUsada();

    size_t letras = 0;
    for (const auto& it : arc.Items())
    {
        Entrada en;
        en.ruta = it.ruta;
        en.carpeta = it.carpeta;
        en.enlace = it.simbolico;
        en.especial = it.alterno || it.especial || it.anti;
        en.cifrada = it.cifrada;
        en.tamano = en.carpeta ? -1 : it.tamano;
        int64_t s = 0;
        long ns = 0;
        if (it.conMTime && AUnix(it.mtime, s, ns) && s > 0) en.fecha = s;
        if (en.carpeta && (en.ruta.empty() || en.ruta == L".")) continue;
        if (en.tamano > 0 && !en.enlace && !en.especial) c.total += en.tamano;
        if (en.cifrada && !en.carpeta) c.cifradas++;
        letras += en.ruta.size();
        c.entradas.push_back(std::move(en));
        // Topes para la memoria: con más que esto, la lista se muestra a medias (extraer todo sigue funcionando)
        if (c.entradas.size() >= 1000000 || letras >= (64u << 20))
        {
            c.estado = Estado::Bien;
            c.mensaje = L"El archivo trae demasiados archivos para mostrarlos todos: la lista está a medias. «Extraer todo» funciona igual.";
            return c;
        }
    }
    progreso.archivos = (int64_t)c.entradas.size();
    c.completo = true;
    c.estado = Estado::Bien;
    if (!arc.FaltaVolumen().empty())
    {
        c.faltan.push_back(arc.FaltaVolumen());
        c.mensaje = MensajeFalta(arc.FaltaVolumen());
    }
    else if (arc.Incompleto())
        c.mensaje = L"El archivo parece incompleto (¿se terminó de bajar?): puede que no salga todo.";
    else if (c.entradas.empty())
        c.mensaje = L"El archivo está vacío.";
    return c;
}

jicaro::Contenido jicaro::Listar(const std::vector<std::wstring>& partes, const std::wstring& nombreSuelto,
                                 Progreso& progreso, const std::atomic<bool>& cancelar, Preguntas* preguntas,
                                 const std::optional<std::wstring>& clave)
{
    try
    {
        // Los RAR, los 7z y los ZIP en partes, con 7z.dll (si está junto al programa)
        if (!partes.empty())
        {
            std::vector<std::wstring> completas;
            for (auto& p : partes) completas.push_back(RutaCompleta(p));
            if (motor7z::Disponible())
            {
                progreso.Reiniciar();
                if (auto f = motor7z::DetectarPartes(completas)) return ListarCon7z(*f, completas, progreso, cancelar, preguntas, clave);
            }
            else if (motor7z::EsZipEnPartes(completas))
            {
                Contenido c;
                c.mensaje = MensajeZipSinMotor();
                return c;
            }
        }
        Contenido c = ListarAdentro(partes, nombreSuelto, progreso, cancelar, false);
        // Un .gz de un archivo que empieza con ceros parece un TAR vacío: si el nombre no dice TAR, se lee como suelto
        if (c.completo && c.entradas.empty() && c.formato.rfind(L"TAR + ", 0) == 0 && !nombreSuelto.empty()
            && !TerminaSinMayusculas(nombreSuelto, L".tar"))
            c = ListarAdentro(partes, nombreSuelto, progreso, cancelar, true);
        return c;
    }
    catch (const std::bad_alloc&)
    {
        Contenido c;
        c.mensaje = L"No alcanzó la memoria para leer la lista de este archivo.";
        return c;
    }
    catch (...)
    {
        Contenido c;
        c.mensaje = L"Algo salió mal al leer este archivo.";
        return c;
    }
}

jicaro::Resumen jicaro::Extraer(const Opciones& opciones, Preguntas& preguntas, Progreso& progreso,
                                const std::atomic<bool>& cancelar)
{
    try
    {
        Extraccion x(opciones, preguntas, progreso, cancelar);
        Resumen r = x.Ejecutar();
        // Un .gz de un archivo que empieza con ceros parece un TAR vacío: si el nombre no dice TAR, se lee como suelto
        if (x.TarVacio() && r.estado == Estado::Bien && r.archivos == 0 && r.omitidas.empty() && !opciones.nombreSuelto.empty()
            && !TerminaSinMayusculas(opciones.nombreSuelto, L".tar"))
        {
            Extraccion suelto(opciones, preguntas, progreso, cancelar, true);
            r = suelto.Ejecutar();
        }
        return r;
    }
    catch (const std::bad_alloc&)
    {
        Resumen r;
        r.mensaje = L"No alcanzó la memoria para extraer este archivo.";
        return r;
    }
    catch (...)
    {
        Resumen r;
        r.mensaje = L"Algo salió mal al extraer.";
        return r;
    }
}

std::wstring jicaro::TextoEstado(Estado estado)
{
    switch (estado)
    {
    case Estado::Bien: return L"Listo.";
    case Estado::Cancelado: return L"Se canceló.";
    case Estado::FaltaParte: return L"Falta una parte del archivo.";
    case Estado::CifradoNoSoportado: return L"Este archivo tiene un tipo de contraseña que Jícaro todavía no puede abrir.";
    case Estado::SinContrasena: return L"Hace falta la contraseña.";
    case Estado::ContrasenaIncorrecta: return L"La contraseña no es correcta.";
    case Estado::NoEsArchivo: return L"No es un archivo comprimido que Jícaro conozca, o está dañado.";
    case Estado::SinEspacio: return L"No queda espacio suficiente en el disco.";
    case Estado::Detenido: return L"Se detuvo para no llenar el disco.";
    default: return L"No se pudo extraer.";
    }
}
