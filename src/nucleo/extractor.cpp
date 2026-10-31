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
#include <unordered_map>
#include <unordered_set>

#include "comun.hpp"
#include "marca.hpp"
#include "rutas.hpp"

namespace
{
    using namespace jicaro;

    constexpr size_t TamanoBloque = 1 << 20;

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

        bool Abrir(Preguntas* preguntas);

        bool Cancelar() const { return m_cancelar.load(); }
        size_t Cantidad() const { return m_partes.size(); }
        int64_t Total() const { return m_total; }
        const std::wstring& RutaParte(size_t i) const { return m_partes[i].ruta; }

        // Contraseñas (solo ZIP): libarchive llama aquí cuando ninguna de las ya dadas sirve
        const char* SiguienteContrasena();
        void PonerEntrada(const std::wstring& ruta) { m_entrada = ruta; }

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
        bool personaCancelo = false;   // no dio la contraseña
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
        return static_cast<Lector*>(d)->SiguienteContrasena();
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
        if (personaCancelo || !m_preguntas || !m_preguntas->contrasena) return nullptr;
        if (m_siguiente < m_candidatas.size())
        {
            m_devuelta = m_candidatas[m_siguiente++];
            return m_devuelta.c_str();
        }
        if (Cancelar()) return nullptr;
        auto clave = m_preguntas->contrasena(m_entrada, m_yaPidio);
        m_yaPidio = true;
        if (!clave || clave->empty())
        {
            personaCancelo = true;
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

    bool Lector::Abrir(Preguntas* preguntas)
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
        archive_read_support_format_zip(a);
        archive_read_support_format_rar(a);
        archive_read_support_format_rar5(a);
        archive_read_support_format_7zip(a);
        archive_read_support_format_tar(a);
        archive_read_support_format_iso9660(a);
        archive_read_support_format_cab(a);
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
        return L"Este " + cual + L" está protegido con contraseña, y Jícaro todavía no puede abrir los " + cual +
               L" con contraseña. Para este archivo usa otro programa, como 7-Zip o WinRAR.";
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
            estado = Estado::SinContrasena;
            mensaje = L"Hacía falta la contraseña y no se dio, así que no se extrajo.";
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

    class Extraccion
    {
    public:
        Extraccion(const Opciones& op, Preguntas& pr, Progreso& prog, const std::atomic<bool>& cancelar)
            : m_op(op), m_pr(pr), m_prog(prog), m_cancelar(cancelar), m_politica(op.siExiste)
        {
        }

        Resumen Ejecutar();

    private:
        bool Elegida(const std::wstring& cruda) const;
        void Omitir(const std::wstring& ruta, const std::wstring& motivo) { m_res.omitidas.push_back({ ruta, motivo }); }
        std::optional<std::wstring> Preparar(const std::wstring& cruda, bool carpeta);
        bool SinDesvios(const std::wstring& relativa, std::wstring& donde);
        Decision Decidir(const std::wstring& completa);
        bool Preguntar(const std::wstring& motivo);
        bool EsParte(const std::wstring& completa) const;
        std::wstring RelativaDe(const std::wstring& completa) const;
        void Marcar(const std::wstring& completa);
        bool CopiarDatos(archive* w, archive_entry* e, const std::wstring& cruda, bool& fatal, bool& incompleto);

        const Opciones& m_op;
        Preguntas& m_pr;
        Progreso& m_prog;
        const std::atomic<bool>& m_cancelar;
        SiExiste m_politica;
        Resumen m_res;
        Lector* m_lector = nullptr;
        std::wstring m_destino;
        std::vector<std::wstring> m_partesCompletas;
        std::unordered_set<std::wstring> m_elegidas;
        std::unordered_set<std::wstring> m_revisadas;      // carpetas ya revisadas (en minúsculas, relativas)
        std::unordered_map<std::wstring, std::wstring> m_hechos;   // ruta en el archivo → archivo extraído
        std::wstring m_referente;
        unsigned long m_zona = 0;
        int64_t m_libreAlEmpezar = -1;
        int64_t m_proximoChequeo = 64LL << 20;
        bool m_preguntoProporcion = false, m_preguntoLibre = false, m_preguntoCantidad = false;
        bool m_parar = false;      // la persona canceló en una pregunta
        bool m_detenido = false;   // la persona detuvo ante una posible bomba
        bool m_sinEspacio = false;
        int m_formatoCifrado = 0;
    };

    bool Extraccion::Elegida(const std::wstring& cruda) const
    {
        if (m_elegidas.empty() || m_elegidas.count(cruda)) return true;
        for (size_t p = cruda.find(L'\\'); p != std::wstring::npos; p = cruda.find(L'\\', p + 1))
            if (m_elegidas.count(cruda.substr(0, p))) return true;
        return false;
    }

    bool Extraccion::EsParte(const std::wstring& completa) const
    {
        for (auto& p : m_partesCompletas)
            if (IgualesSinMayusculas(p, completa)) return true;
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

    bool Extraccion::SinDesvios(const std::wstring& relativa, std::wstring& donde)
    {
        size_t corte = relativa.find_last_of(L'\\');
        if (corte != std::wstring::npos)
        {
            std::wstring padre = relativa.substr(0, corte);
            if (!m_revisadas.count(Minusculas(padre)))
            {
                if (!rutas::SinDesvios(m_destino, padre, &donde)) return false;
                for (std::wstring p = padre;;)
                {
                    m_revisadas.insert(Minusculas(p));
                    auto c = p.find_last_of(L'\\');
                    if (c == std::wstring::npos) break;
                    p.resize(c);
                }
            }
        }
        // El último tramo (el archivo o la carpeta misma)
        std::wstring completa = rutas::Unir(m_destino, relativa);
        WIN32_FIND_DATAW d;
        HANDLE h = FindFirstFileExW(rutas::Larga(completa).c_str(), FindExInfoBasic, &d, FindExSearchNameMatch, nullptr, 0);
        if (h != INVALID_HANDLE_VALUE)
        {
            FindClose(h);
            if ((d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(d.dwReserved0))
            {
                donde = completa;
                return false;
            }
        }
        return true;
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
            if (!MandarAPapelera(completa))
            {
                Omitir(cruda, L"no se pudo mandar el archivo que ya estaba a la Papelera");
                return std::nullopt;
            }
            return relativa;
        }
        return std::nullopt;
    }

    void Extraccion::Marcar(const std::wstring& completa)
    {
        if (!marca::HayQueMarcar(m_zona)) return;
        if (marca::Pasar(completa, m_referente, m_zona)) m_res.marcados++;
        else if (!ExisteArchivo(completa)) m_res.quitadosPorWindows++;
        else m_res.sinMarca++;
    }

    // Copia los datos de la entrada actual al archivo abierto en «w». Devuelve true si llegó al final completo.
    // «incompleto»: los datos se acabaron antes del tamaño que dice el archivo (pasa, por ejemplo, con un RAR 5 en
    // partes al que le falta la última: libarchive no lo marca como error).
    bool Extraccion::CopiarDatos(archive* w, archive_entry* e, const std::wstring& cruda, bool& fatal, bool& incompleto)
    {
        archive* a = m_lector->a;
        const int64_t comprimido = std::max<int64_t>(m_lector->Total(), 1);
        const bool conTamano = archive_entry_size_is_set(e) && archive_entry_sparse_count(e) == 0;
        const int64_t tamano = conTamano ? archive_entry_size(e) : 0;
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
                return true;
            }
            if (r < ARCHIVE_WARN)
            {
                fatal = r == ARCHIVE_FATAL || m_lector->personaCancelo || m_lector->cancelado;
                if (!fatal) Omitir(cruda, L"no se pudo leer." + Detalle(archive_error_string(a)));
                return false;
            }
            if (m_cancelar) return false;
            if (tam == 0) continue;
            hasta = std::max<int64_t>(hasta, desde + (int64_t)tam);
            if (archive_write_data_block(w, bloque, tam, desde) < ARCHIVE_OK)
            {
                int64_t libre = EspacioLibre(m_destino);
                if (libre >= 0 && libre < (64LL << 20)) m_sinEspacio = true;
                else Omitir(cruda, L"no se pudo escribir en el disco");
                return false;
            }
            m_res.escritos += (int64_t)tam;
            m_prog.escritos += (int64_t)tam;

            // Topes contra las bombas de descompresión
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
        for (auto& p : m_op.partes) m_partesCompletas.push_back(RutaCompleta(p));
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

        // COM: para la marca de la Web y la Papelera
        HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (m_op.pasarMarca) m_zona = marca::ZonaDelJuego(m_partesCompletas, m_referente);
        m_res.zona = marca::HayQueMarcar(m_zona) ? m_zona : 0;

        {
            Lector lector(m_partesCompletas, m_prog, m_cancelar);
            m_lector = &lector;
            if (!lector.Abrir(&m_pr))
            {
                Clasificar(lector, m_res.estado, m_res.mensaje);
            }
            else
            {
                CarpetaDeTrabajo trabajo(m_destino);
                if (!trabajo.ok)
                {
                    m_res.mensaje = L"No se pudo usar la carpeta de destino (¿la ruta es muy larga?).";
                }
                else
                {
                    archive* w = archive_write_disk_new();
                    // Defensas de libarchive: sin rutas absolutas, sin «..», sin escribir a través de enlaces y sin
                    // reemplazar nada (Jícaro pregunta antes y manda lo anterior a la Papelera)
                    archive_write_disk_set_options(w, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_SECURE_SYMLINKS
                                                          | ARCHIVE_EXTRACT_SECURE_NODOTDOT
                                                          | ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS
                                                          | ARCHIVE_EXTRACT_NO_OVERWRITE);
                    m_res.estado = Estado::Bien;
                    int64_t numero = 0;
                    int fallasSeguidas = 0;
                    archive_entry* e = nullptr;
                    for (;;)
                    {
                        if (m_cancelar)
                        {
                            m_res.estado = Estado::Cancelado;
                            break;
                        }
                        int r = archive_read_next_header(lector.a, &e);
                        if (r == ARCHIVE_EOF) break;
                        if (r == ARCHIVE_RETRY || r == ARCHIVE_FAILED)
                        {
                            if (++fallasSeguidas > 100)
                            {
                                m_res.estado = Estado::Error;
                                m_res.mensaje = L"El archivo está dañado: demasiadas entradas que no se pueden leer.";
                                break;
                            }
                            if (r == ARCHIVE_FAILED && e) Omitir(RutaDeEntrada(e), L"está dañado." + Detalle(archive_error_string(lector.a)));
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
                        if (!trabajo.Sigue())
                        {
                            m_res.estado = Estado::Error;
                            m_res.mensaje = L"Algo cambió la carpeta de trabajo en medio de la extracción; se detuvo por seguridad.";
                            break;
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
                            Omitir(cruda, L"tiene contraseña de un tipo que Jícaro no puede abrir");
                            m_res.cifradas++;
                            m_formatoCifrado = Formato(lector.a);
                            continue;
                        }

                        if (tipo == AE_IFDIR && !duro)
                        {
                            auto relativa = Preparar(cruda, true);
                            if (m_parar) break;
                            if (!relativa) continue;
                            archive_entry* s = archive_entry_new();
                            archive_entry_copy_pathname_w(s, relativa->c_str());
                            archive_entry_set_filetype(s, AE_IFDIR);
                            archive_entry_set_perm(s, 0755);
                            CopiarFechas(e, s);
                            if (archive_write_header(w, s) < ARCHIVE_WARN) Omitir(cruda, L"no se pudo crear la carpeta");
                            else m_res.carpetas++;
                            archive_write_finish_entry(w);
                            archive_entry_free(s);
                            continue;
                        }

                        // Enlace duro sin datos propios: se copia el archivo al que apunta, si ya se extrajo aquí
                        std::wstring copiarDe;
                        if (duro && !conDatos)
                        {
                            auto it = m_hechos.find(DestinoDeEnlace(e));
                            if (it == m_hechos.end() || !ExisteArchivo(it->second))
                            {
                                Omitir(cruda, L"es un enlace a algo que no se extrajo; Jícaro no crea enlaces");
                                continue;
                            }
                            copiarDe = it->second;
                        }

                        auto relativa = Preparar(cruda, false);
                        if (m_parar) break;
                        if (!relativa) continue;
                        std::wstring completa = rutas::Unir(m_destino, *relativa);

                        if (!copiarDe.empty())
                        {
                            if (CopyFileW(rutas::Larga(copiarDe).c_str(), rutas::Larga(completa).c_str(), TRUE))
                            {
                                m_res.archivos++;
                                m_prog.archivos++;
                                m_hechos[cruda] = completa;
                                Marcar(completa);
                            }
                            else Omitir(cruda, L"no se pudo copiar");
                            continue;
                        }

                        if (archive_entry_size_is_set(e))
                        {
                            int64_t libre = EspacioLibre(m_destino);
                            if (libre >= 0 && archive_entry_size(e) > libre - m_op.limites.libreMinimo)
                            {
                                m_sinEspacio = true;
                                break;
                            }
                        }

                        archive_entry* s = archive_entry_new();
                        archive_entry_copy_pathname_w(s, relativa->c_str());
                        archive_entry_set_filetype(s, AE_IFREG);
                        archive_entry_set_perm(s, 0644);
                        if (archive_entry_size_is_set(e)) archive_entry_set_size(s, archive_entry_size(e));
                        CopiarFechas(e, s);
                        if (archive_write_header(w, s) < ARCHIVE_WARN)
                        {
                            Omitir(cruda, L"no se pudo crear el archivo");
                            archive_write_finish_entry(w);
                            archive_entry_free(s);
                            continue;
                        }
                        bool fatal = false, incompleto = false;
                        bool completo = CopiarDatos(w, e, cruda, fatal, incompleto);
                        int fin = archive_write_finish_entry(w);
                        archive_entry_free(s);
                        if (!completo || fin < ARCHIVE_WARN)
                        {
                            // Lo que quedó a medias lo creó Jícaro en esta misma extracción: se quita
                            DeleteFileW(rutas::Larga(completa).c_str());
                            if (m_cancelar || m_detenido || m_sinEspacio)
                            {
                                m_res.quitoAMedias = true;
                                break;
                            }
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
                                Omitir(cruda, L"está incompleto o dañado");
                                continue;
                            }
                            if (completo) Omitir(cruda, L"no se pudo terminar de escribir");
                            continue;
                        }
                        m_res.archivos++;
                        m_prog.archivos++;
                        m_hechos[cruda] = completa;
                        Marcar(completa);
                    }
                    // Cierra antes de volver a la carpeta de trabajo anterior: aquí se ponen las fechas de las carpetas
                    archive_write_close(w);
                    archive_write_free(w);
                }
            }
            m_lector = nullptr;
        }
        if (SUCCEEDED(com)) CoUninitialize();

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
            // Con contraseña de un tipo que libarchive no abre: se dice claro qué hacer
            if (m_res.archivos == 0)
            {
                m_res.estado = Estado::CifradoNoSoportado;
                m_res.mensaje = MensajeCifrado(m_formatoCifrado);
            }
            else
            {
                m_res.mensaje = L"Se extrajeron " + TextoNumero(m_res.archivos) + L" archivos, pero " +
                    TextoNumero(m_res.cifradas) + L" tienen una contraseña de un tipo que Jícaro todavía no puede abrir. "
                    L"Para esos usa otro programa, como 7-Zip o WinRAR.";
            }
        }
        if (m_res.mensaje.empty()) m_res.mensaje = TextoEstado(m_res.estado);
        return m_res;
    }
}

jicaro::Contenido jicaro::Listar(const std::vector<std::wstring>& partes, const std::wstring& nombreSuelto,
                                 Progreso& progreso, const std::atomic<bool>& cancelar)
{
    Contenido c;
    progreso.Reiniciar();
    std::vector<std::wstring> completas;
    for (auto& p : partes) completas.push_back(RutaCompleta(p));
    Lector l(completas, progreso, cancelar);
    if (!l.Abrir(nullptr))
    {
        Clasificar(l, c.estado, c.mensaje);
        return c;
    }
    archive_entry* e = nullptr;
    int fallasSeguidas = 0;
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
        c.entradas.push_back(std::move(en));
        if (suelto)
        {
            // Un solo archivo: no hace falta descomprimirlo entero para mostrarlo
            c.suelto = true;
            c.completo = true;
            break;
        }
        if (c.entradas.size() >= 5000000)
        {
            c.estado = Estado::Bien;
            c.mensaje = L"El archivo trae demasiados archivos para mostrarlos todos.";
            c.formato = NombreFormato(l.a);
            return c;
        }
    }
    c.formato = NombreFormato(l.a);
    if (c.completo || (c.mensaje.empty() && !c.entradas.empty())) c.estado = Estado::Bien;
    if (c.completo && c.entradas.empty() && c.mensaje.empty()) c.mensaje = L"El archivo está vacío.";
    return c;
}

jicaro::Resumen jicaro::Extraer(const Opciones& opciones, Preguntas& preguntas, Progreso& progreso,
                                const std::atomic<bool>& cancelar)
{
    Extraccion x(opciones, preguntas, progreso, cancelar);
    return x.Ejecutar();
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
    case Estado::NoEsArchivo: return L"No es un archivo comprimido que Jícaro conozca, o está dañado.";
    case Estado::SinEspacio: return L"No queda espacio suficiente en el disco.";
    case Estado::Detenido: return L"Se detuvo para no llenar el disco.";
    default: return L"No se pudo extraer.";
    }
}
