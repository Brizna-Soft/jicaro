#include "motor7z.hpp"

#include <windows.h>
#include <oleauto.h>
#include <propidl.h>
#include <psapi.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <thread>

#include "comun.hpp"
#include "rutas.hpp"

namespace jicaro::motor7z
{
    namespace
    {
        using UInt32 = uint32_t;
        using Int32 = int32_t;
        using UInt64 = uint64_t;
        using Int64 = int64_t;

        // ---------- Interfaces de 7-Zip, como las publica en sus encabezados ----------
        // Cada una se identifica con {23170F69-40C1-278A-0000-00gg00ss0000} (gg: grupo, ss: número)
        constexpr GUID Iid(unsigned char grupo, unsigned char numero)
        {
            return GUID{ 0x23170F69, 0x40C1, 0x278A, { 0, 0, 0, grupo, 0, numero, 0, 0 } };
        }
        constexpr GUID IID_ISequentialInStream = Iid(3, 0x01);
        constexpr GUID IID_ISequentialOutStream = Iid(3, 0x02);
        constexpr GUID IID_IInStream = Iid(3, 0x03);
        constexpr GUID IID_IStreamGetSize = Iid(3, 0x06);
        constexpr GUID IID_IProgress = Iid(0, 0x05);
        constexpr GUID IID_ICryptoGetTextPassword = Iid(5, 0x10);
        constexpr GUID IID_IArchiveOpenCallback = Iid(6, 0x10);
        constexpr GUID IID_IArchiveExtractCallback = Iid(6, 0x20);
        constexpr GUID IID_IArchiveOpenVolumeCallback = Iid(6, 0x30);
        constexpr GUID IID_IInArchive = Iid(6, 0x60);
        constexpr GUID IID_IArchiveRequestMemoryUseCallback = Iid(6, 0x09);

        // Los formatos: {23170F69-40C1-278A-1000-000110ff0000} (ff: número del formato en 7-Zip)
        GUID ClaseDe(Formato f)
        {
            unsigned char id = f == Formato::Rar ? 0x03 : (f == Formato::Rar5 ? 0xCC : 0x07);
            return GUID{ 0x23170F69, 0x40C1, 0x278A, { 0x10, 0x00, 0x00, 0x01, 0x10, id, 0x00, 0x00 } };
        }

        // Propiedades que se leen: su lugar en la lista de PropID.h de 7-Zip
        enum : PROPID
        {
            kpidPath = 3,
            kpidName = 4,
            kpidIsDir = 6,
            kpidSize = 7,
            kpidAttrib = 9,
            kpidCTime = 10,
            kpidATime = 11,
            kpidMTime = 12,
            kpidEncrypted = 15,
            kpidIsAnti = 21,
            kpidPosixAttrib = 53,
            kpidSymLink = 54,
            kpidError = 55,
            kpidIsAltStream = 63,
            kpidErrorFlags = 71,
            kpidHardLink = 90,
            kpidCopyLink = 95,
        };
        constexpr UInt32 kErrorFinInesperado = 1u << 5;                // kpv_ErrorFlags_UnexpectedEnd
        constexpr UInt32 kErrorCabeceras = (1u << 1) | (1u << 2);      // HeadersError, EncryptedHeadersError
        constexpr Int32 kExtraer = 0;                                  // NAskMode::kExtract
        constexpr UInt32 kPermitir = 1u << 0;                          // NRequestMemoryAnswerFlags::k_Allow

        struct ISequentialInStream : public IUnknown
        {
            virtual HRESULT STDMETHODCALLTYPE Read(void* datos, UInt32 tamano, UInt32* leidos) = 0;
        };
        struct IInStream : public ISequentialInStream
        {
            virtual HRESULT STDMETHODCALLTYPE Seek(Int64 desplazamiento, UInt32 desde, UInt64* nueva) = 0;
        };
        struct IStreamGetSize : public IUnknown
        {
            virtual HRESULT STDMETHODCALLTYPE GetSize(UInt64* tamano) = 0;
        };
        struct ISequentialOutStream : public IUnknown
        {
            virtual HRESULT STDMETHODCALLTYPE Write(const void* datos, UInt32 tamano, UInt32* escritos) = 0;
        };
        struct IProgress : public IUnknown
        {
            virtual HRESULT STDMETHODCALLTYPE SetTotal(UInt64 total) = 0;
            virtual HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64* hecho) = 0;
        };
        struct IArchiveOpenCallback : public IUnknown
        {
            virtual HRESULT STDMETHODCALLTYPE SetTotal(const UInt64* archivos, const UInt64* bytes) = 0;
            virtual HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64* archivos, const UInt64* bytes) = 0;
        };
        struct IArchiveExtractCallback : public IProgress
        {
            virtual HRESULT STDMETHODCALLTYPE GetStream(UInt32 indice, ISequentialOutStream** salida, Int32 modo) = 0;
            virtual HRESULT STDMETHODCALLTYPE PrepareOperation(Int32 modo) = 0;
            virtual HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 resultado) = 0;
        };
        struct IArchiveOpenVolumeCallback : public IUnknown
        {
            virtual HRESULT STDMETHODCALLTYPE GetProperty(PROPID id, PROPVARIANT* valor) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetStream(const wchar_t* nombre, IInStream** entrada) = 0;
        };
        struct ICryptoGetTextPassword : public IUnknown
        {
            virtual HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* clave) = 0;
        };
        struct IArchiveRequestMemoryUseCallback : public IUnknown
        {
            virtual HRESULT STDMETHODCALLTYPE RequestMemoryUse(UInt32 banderas, UInt32 tipoDeIndice, UInt32 indice,
                                                               const wchar_t* ruta, UInt64 necesita, UInt64* permitido,
                                                               UInt32* respuesta) = 0;
        };
        struct IInArchive : public IUnknown
        {
            virtual HRESULT STDMETHODCALLTYPE Open(IInStream* entrada, const UInt64* maxInicio, IArchiveOpenCallback* cb) = 0;
            virtual HRESULT STDMETHODCALLTYPE Close() = 0;
            virtual HRESULT STDMETHODCALLTYPE GetNumberOfItems(UInt32* cantidad) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetProperty(UInt32 indice, PROPID id, PROPVARIANT* valor) = 0;
            virtual HRESULT STDMETHODCALLTYPE Extract(const UInt32* indices, UInt32 cantidad, Int32 probar,
                                                      IArchiveExtractCallback* cb) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetArchiveProperty(PROPID id, PROPVARIANT* valor) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetNumberOfProperties(UInt32* cantidad) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetPropertyInfo(UInt32 indice, BSTR* nombre, PROPID* id, VARTYPE* tipo) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetNumberOfArchiveProperties(UInt32* cantidad) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetArchivePropertyInfo(UInt32 indice, BSTR* nombre, PROPID* id, VARTYPE* tipo) = 0;
        };

        // ---------- 7z.dll: solo de la carpeta del programa, con su ruta completa ----------
        using FuncionCrear = HRESULT(WINAPI*)(const GUID* clase, const GUID* iid, void** objeto);

        FuncionCrear Crear()
        {
            static const FuncionCrear crear = []() -> FuncionCrear {
                std::wstring ruta = CarpetaDelPrograma() + L"\\7z.dll";
                // Sus dependencias, solo de System32: nunca de la carpeta de trabajo ni del PATH
                HMODULE m = LoadLibraryExW(ruta.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (!m) return nullptr;
                return reinterpret_cast<FuncionCrear>(reinterpret_cast<void*>(GetProcAddress(m, "CreateObject")));
            }();
            return crear;
        }

        // ---------- Ayudas para COM ----------
        template <class T> class Ptr
        {
        public:
            Ptr() = default;
            explicit Ptr(T* p) : m_p(p) {}   // se queda con la referencia que ya trae
            ~Ptr() { Soltar(); }
            Ptr(const Ptr&) = delete;
            Ptr& operator=(const Ptr&) = delete;
            T* get() const { return m_p; }
            T* operator->() const { return m_p; }
            explicit operator bool() const { return m_p != nullptr; }
            void Soltar()
            {
                if (m_p) m_p->Release();
                m_p = nullptr;
            }
            void Poner(T* p)
            {
                Soltar();
                m_p = p;
            }

        private:
            T* m_p = nullptr;
        };

        // Un objeto COM nuestro: se borra solo cuando 7z.dll y Jícaro lo sueltan
        template <class... Interfaces> class Objeto : public Interfaces...
        {
        public:
            ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++m_refs; }
            ULONG STDMETHODCALLTYPE Release() override
            {
                long r = --m_refs;
                if (r == 0) delete this;
                return (ULONG)r;
            }

        protected:
            virtual ~Objeto() = default;
            HRESULT Dar(void* interfaz, void** salida)
            {
                *salida = interfaz;
                AddRef();
                return S_OK;
            }

        private:
            std::atomic<long> m_refs{ 1 };
        };

        struct Valor
        {
            PROPVARIANT v;
            Valor() { PropVariantInit(&v); }
            ~Valor() { PropVariantClear(&v); }
            Valor(const Valor&) = delete;
            Valor& operator=(const Valor&) = delete;
        };

        std::optional<std::wstring> Texto(const PROPVARIANT& v)
        {
            if (v.vt != VT_BSTR || !v.bstrVal) return std::nullopt;
            return std::wstring(v.bstrVal, SysStringLen(v.bstrVal));
        }
        bool Si(const PROPVARIANT& v) { return v.vt == VT_BOOL && v.boolVal != VARIANT_FALSE; }
        std::optional<uint64_t> Numero(const PROPVARIANT& v)
        {
            switch (v.vt)
            {
            case VT_UI1: return v.bVal;
            case VT_UI2: return v.uiVal;
            case VT_UI4: return v.ulVal;
            case VT_UI8: return v.uhVal.QuadPart;
            case VT_I2: if (v.iVal >= 0) return (uint64_t)v.iVal; break;
            case VT_I4: if (v.lVal >= 0) return (uint64_t)v.lVal; break;
            case VT_I8: if (v.hVal.QuadPart >= 0) return (uint64_t)v.hVal.QuadPart; break;
            default: break;
            }
            return std::nullopt;
        }

        void Borrar(std::wstring& s)
        {
            if (!s.empty()) SecureZeroMemory(s.data(), s.size() * sizeof(wchar_t));
            s.clear();
        }

        // «juego.7z.001»: la extensión es un número
        bool EsNumerada(const std::wstring& ruta)
        {
            std::wstring n = NombreDe(ruta);
            size_t punto = n.rfind(L'.');
            if (punto == std::wstring::npos || punto + 1 >= n.size()) return false;
            for (size_t i = punto + 1; i < n.size(); i++)
                if (!iswdigit(n[i])) return false;
            return true;
        }

        // ---------- Lectura: una o varias partes seguidas, como un solo archivo ----------
        class Flujo final : public Objeto<IInStream, IStreamGetSize>
        {
        public:
            Flujo(const std::vector<std::wstring>& rutas, Progreso* progreso, const std::atomic<bool>* cancelar)
                : m_progreso(progreso), m_cancelar(cancelar)
            {
                for (auto& r : rutas)
                {
                    Trozo t;
                    t.ruta = r;
                    t.inicio = m_total;
                    t.tamano = TamanoArchivo(r);
                    if (t.tamano < 0 && m_falta.empty()) m_falta = r;
                    if (t.tamano > 0) m_total += t.tamano;
                    m_trozos.push_back(std::move(t));
                }
            }

            const std::wstring& Falta() const { return m_falta; }

            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** salida) override
            {
                if (!salida) return E_POINTER;
                *salida = nullptr;
                if (iid == IID_IUnknown || iid == IID_ISequentialInStream || iid == IID_IInStream)
                    return Dar(static_cast<IInStream*>(this), salida);
                if (iid == IID_IStreamGetSize) return Dar(static_cast<IStreamGetSize*>(this), salida);
                return E_NOINTERFACE;
            }

            // Nada puede lanzar una excepción hacia 7z.dll: todo se convierte en un código de error
            HRESULT STDMETHODCALLTYPE Read(void* datos, UInt32 tamano, UInt32* leidos) override
            {
                if (leidos) *leidos = 0;
                try
                {
                    return Leer(datos, tamano, leidos);
                }
                catch (const std::bad_alloc&)
                {
                    return E_OUTOFMEMORY;
                }
                catch (...)
                {
                    return E_FAIL;
                }
            }

            HRESULT Leer(void* datos, UInt32 tamano, UInt32* leidos)
            {
                if (tamano == 0) return S_OK;
                if (m_cancelar && *m_cancelar) return E_ABORT;
                std::lock_guard g(m_m);
                if (m_pos >= m_total) return S_OK;
                size_t i = TrozoDe(m_pos);
                DWORD falla = Posicionar(i, m_pos - m_trozos[i].inicio);
                if (falla) return HRESULT_FROM_WIN32(falla);
                int64_t quedan = m_trozos[i].inicio + m_trozos[i].tamano - m_pos;
                DWORD pedir = (DWORD)std::min<int64_t>((int64_t)tamano, quedan);
                DWORD n = 0;
                if (!ReadFile(m_h, datos, pedir, &n, nullptr))
                {
                    DWORD e = GetLastError();
                    CerrarArchivo();
                    return HRESULT_FROM_WIN32(e ? e : ERROR_READ_FAULT);
                }
                if (n == 0)
                {
                    CerrarArchivo();
                    return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);   // la parte se achicó mientras se leía
                }
                m_pos += n;
                m_posEnArchivo += n;
                if (m_progreso) m_progreso->leidos += n;
                if (leidos) *leidos = n;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE Seek(Int64 desplazamiento, UInt32 desde, UInt64* nueva) override
            {
                try
                {
                    return Mover(desplazamiento, desde, nueva);
                }
                catch (...)
                {
                    return E_FAIL;
                }
            }

            HRESULT Mover(Int64 desplazamiento, UInt32 desde, UInt64* nueva)
            {
                std::lock_guard g(m_m);
                int64_t base = desde == 0 ? 0 : (desde == 1 ? m_pos : (desde == 2 ? m_total : -1));
                if (base < 0) return STG_E_INVALIDFUNCTION;
                if ((desplazamiento > 0 && base > INT64_MAX - desplazamiento)) return STG_E_INVALIDFUNCTION;
                int64_t n = base + desplazamiento;
                if (n < 0) return HRESULT_FROM_WIN32(ERROR_NEGATIVE_SEEK);
                m_pos = n;
                if (nueva) *nueva = (UInt64)n;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE GetSize(UInt64* tamano) override
            {
                if (!tamano) return E_POINTER;
                *tamano = (UInt64)m_total;
                return S_OK;
            }

        private:
            ~Flujo() override { CerrarArchivo(); }

            struct Trozo
            {
                std::wstring ruta;
                int64_t inicio = 0, tamano = 0;
            };

            size_t TrozoDe(int64_t pos) const
            {
                for (size_t i = 0; i + 1 < m_trozos.size(); i++)
                    if (pos < m_trozos[i].inicio + m_trozos[i].tamano) return i;
                return m_trozos.size() - 1;
            }

            DWORD Posicionar(size_t i, int64_t desplazamiento)
            {
                if (m_abierto != i)
                {
                    CerrarArchivo();
                    m_h = CreateFileW(rutas::Larga(m_trozos[i].ruta).c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (m_h == INVALID_HANDLE_VALUE)
                    {
                        DWORD e = GetLastError();
                        return e ? e : ERROR_OPEN_FAILED;
                    }
                    m_abierto = i;
                    m_posEnArchivo = 0;
                }
                if (m_posEnArchivo != desplazamiento)
                {
                    LARGE_INTEGER mover;
                    mover.QuadPart = desplazamiento;
                    if (!SetFilePointerEx(m_h, mover, nullptr, FILE_BEGIN))
                    {
                        DWORD e = GetLastError();
                        CerrarArchivo();
                        return e ? e : ERROR_SEEK;
                    }
                    m_posEnArchivo = desplazamiento;
                }
                return 0;
            }

            void CerrarArchivo()
            {
                if (m_h != INVALID_HANDLE_VALUE) CloseHandle(m_h);
                m_h = INVALID_HANDLE_VALUE;
                m_abierto = SIZE_MAX;
            }

            std::mutex m_m;
            std::vector<Trozo> m_trozos;
            std::wstring m_falta;
            int64_t m_total = 0, m_pos = 0, m_posEnArchivo = 0;
            HANDLE m_h = INVALID_HANDLE_VALUE;
            size_t m_abierto = SIZE_MAX;
            Progreso* m_progreso;
            const std::atomic<bool>* m_cancelar;
        };

        // ---------- Al abrir: las demás partes de un RAR y la contraseña de las cabeceras ----------
        class AlAbrir final : public Objeto<IArchiveOpenCallback, IArchiveOpenVolumeCallback, ICryptoGetTextPassword>
        {
        public:
            AlAbrir(const std::vector<std::wstring>& partes, bool porVolumenes, std::function<std::optional<std::wstring>()> clave,
                    Progreso* progreso, const std::atomic<bool>* cancelar)
                : m_partes(partes), m_porVolumenes(porVolumenes), m_pedir(std::move(clave)), m_progreso(progreso), m_cancelar(cancelar)
            {
                m_nombre = NombreDe(partes.front());
                m_carpeta = CarpetaDe(partes.front());
            }

            bool Pidio() const { return m_pidio; }
            bool SinClave() const { return m_sinClave; }
            const std::vector<std::wstring>& Usadas() const { return m_usadas; }
            const std::optional<std::wstring>& Clave() const { return m_clave; }
            const std::vector<std::wstring>& Faltan() const { return m_faltan; }
            const std::wstring& NoAbrio() const { return m_noAbrio; }

            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** salida) override
            {
                if (!salida) return E_POINTER;
                *salida = nullptr;
                if (iid == IID_IUnknown || iid == IID_IArchiveOpenCallback) return Dar(static_cast<IArchiveOpenCallback*>(this), salida);
                if (iid == IID_IArchiveOpenVolumeCallback && m_porVolumenes)
                    return Dar(static_cast<IArchiveOpenVolumeCallback*>(this), salida);
                if (iid == IID_ICryptoGetTextPassword) return Dar(static_cast<ICryptoGetTextPassword*>(this), salida);
                return E_NOINTERFACE;
            }

            HRESULT STDMETHODCALLTYPE SetTotal(const UInt64*, const UInt64*) override { return Seguir(); }
            HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64*, const UInt64*) override { return Seguir(); }

            // 7z.dll pregunta el nombre de la primera parte para saber cómo se llaman las siguientes
            HRESULT STDMETHODCALLTYPE GetProperty(PROPID id, PROPVARIANT* valor) override
            {
                if (!valor) return E_POINTER;
                PropVariantClear(valor);
                if (id == kpidName)
                {
                    valor->bstrVal = SysAllocStringLen(m_nombre.data(), (UINT)m_nombre.size());
                    if (!valor->bstrVal) return E_OUTOFMEMORY;
                    valor->vt = VT_BSTR;
                }
                return S_OK;
            }

            // La parte que sigue, por su nombre. Solo un nombre, sin carpetas: solo se abren archivos de la misma carpeta
            // que la primera parte.
            HRESULT STDMETHODCALLTYPE GetStream(const wchar_t* nombre, IInStream** entrada) override
            {
                if (!entrada) return E_POINTER;
                *entrada = nullptr;
                if (m_cancelar && *m_cancelar) return E_ABORT;
                try
                {
                    std::wstring n = nombre ? nombre : L"";
                    m_pedidas++;
                    std::wstring ruta = Buscar(n);
                    if (ruta.empty())
                    {
                        m_faltan.push_back(n);
                        return S_FALSE;
                    }
                    auto* f = new Flujo({ ruta }, m_progreso, m_cancelar);
                    if (!f->Falta().empty())
                    {
                        if (m_noAbrio.empty()) m_noAbrio = ruta;
                        f->Release();
                        m_faltan.push_back(n);
                        return S_FALSE;
                    }
                    m_usadas.push_back(ruta);
                    *entrada = f;
                    return S_OK;
                }
                catch (...)
                {
                    return E_OUTOFMEMORY;
                }
            }

            HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* clave) override
            {
                if (!clave) return E_POINTER;
                *clave = nullptr;
                try
                {
                    m_pidio = true;
                    if (!m_clave)
                    {
                        // Se pregunta una sola vez por intento: si no sirve, quien llamó decide si pregunta otra vez
                        if (!m_pedir || m_yaPregunto)
                        {
                            m_sinClave = true;
                            return E_ABORT;
                        }
                        m_yaPregunto = true;
                        auto c = m_pedir();
                        if (!c || c->empty())
                        {
                            m_sinClave = true;
                            return E_ABORT;
                        }
                        m_clave = std::move(c);
                    }
                    *clave = SysAllocStringLen(m_clave->data(), (UINT)m_clave->size());
                    return *clave ? S_OK : E_OUTOFMEMORY;
                }
                catch (...)
                {
                    m_sinClave = true;
                    return E_ABORT;
                }
            }

        private:
            ~AlAbrir() override
            {
                if (m_clave) Borrar(*m_clave);
            }

            HRESULT Seguir() const { return (m_cancelar && *m_cancelar) ? E_ABORT : S_OK; }

            std::wstring Buscar(const std::wstring& n) const
            {
                if (n.empty() || n.find_first_of(L"\\/:") != std::wstring::npos || n == L"." || n == L"..") return L"";
                for (auto& p : m_partes)
                    if (IgualesSinMayusculas(NombreDe(p), n)) return p;
                std::wstring enCarpeta = m_carpeta + L"\\" + n;
                DWORD a = GetFileAttributesW(rutas::Larga(enCarpeta).c_str());
                if (a != INVALID_FILE_ATTRIBUTES && !(a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return enCarpeta;
                // Partes con otro nombre (por ejemplo, renombradas): la que sigue en el juego que encontró Jícaro
                if (m_pedidas < m_partes.size()) return m_partes[m_pedidas];
                return L"";
            }

            std::vector<std::wstring> m_partes;
            bool m_porVolumenes;
            std::function<std::optional<std::wstring>()> m_pedir;
            Progreso* m_progreso;
            const std::atomic<bool>* m_cancelar;
            std::wstring m_nombre, m_carpeta, m_noAbrio;
            std::vector<std::wstring> m_faltan, m_usadas;
            size_t m_pedidas = 0;
            std::optional<std::wstring> m_clave;
            bool m_pidio = false, m_yaPregunto = false, m_sinClave = false;
        };

        // ---------- El paso de cada cosa entre el hilo de 7z.dll y el de Jícaro ----------
        struct Evento
        {
            enum class Tipo { Empieza, Datos, Termina, SinDatos, Clave, Memoria } tipo = Tipo::Empieza;
            uint32_t indice = 0;
            const void* datos = nullptr;
            size_t tamano = 0;
            int32_t resultado = 0;
            uint64_t memoria = 0;
        };

        struct Respuesta
        {
            Paso paso = Paso::Parar;
            bool seguir = false;
            std::optional<std::wstring> clave;
        };

        // Una cosa a la vez: 7z.dll manda y espera hasta que Jícaro responde. Cerrar() suelta a 7z.dll con «parar».
        class Canal
        {
        public:
            Respuesta Enviar(const Evento& e)
            {
                std::unique_lock l(m_m);
                m_cv.wait(l, [&] { return !m_ocupado || m_cerrado; });
                if (m_cerrado) return {};
                m_ocupado = true;
                m_evento = e;
                m_hayEvento = true;
                m_hayRespuesta = false;
                m_cv.notify_all();
                m_cv.wait(l, [&] { return m_hayRespuesta || m_cerrado; });
                Respuesta r;
                if (m_hayRespuesta) r = std::move(m_respuesta);
                m_respuesta = Respuesta{};
                m_hayEvento = false;
                m_hayRespuesta = false;
                m_ocupado = false;
                m_cv.notify_all();
                return r;
            }

            // false: 7z.dll ya terminó
            bool Recibir(Evento& e)
            {
                std::unique_lock l(m_m);
                m_cv.wait(l, [&] { return (m_hayEvento && !m_hayRespuesta) || m_fin; });
                if (m_hayEvento && !m_hayRespuesta)
                {
                    e = m_evento;
                    return true;
                }
                return false;
            }

            void Responder(Respuesta r)
            {
                std::lock_guard l(m_m);
                m_respuesta = std::move(r);
                m_hayRespuesta = true;
                m_cv.notify_all();
            }

            void Terminar()
            {
                std::lock_guard l(m_m);
                m_fin = true;
                m_cv.notify_all();
            }

            void Cerrar()
            {
                std::lock_guard l(m_m);
                m_cerrado = true;
                m_cv.notify_all();
            }

            bool Cerrado()
            {
                std::lock_guard l(m_m);
                return m_cerrado;
            }

        private:
            std::mutex m_m;
            std::condition_variable m_cv;
            Evento m_evento;
            Respuesta m_respuesta;
            bool m_ocupado = false, m_hayEvento = false, m_hayRespuesta = false, m_fin = false, m_cerrado = false;
        };

        // Donde 7z.dll escribe los datos de un archivo: pasan tal cual a Jícaro
        class Salida final : public Objeto<ISequentialOutStream>
        {
        public:
            explicit Salida(std::shared_ptr<Canal> canal) : m_canal(std::move(canal)) {}

            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** salida) override
            {
                if (!salida) return E_POINTER;
                *salida = nullptr;
                if (iid == IID_IUnknown || iid == IID_ISequentialOutStream) return Dar(static_cast<ISequentialOutStream*>(this), salida);
                return E_NOINTERFACE;
            }

            HRESULT STDMETHODCALLTYPE Write(const void* datos, UInt32 tamano, UInt32* escritos) override
            {
                if (escritos) *escritos = 0;
                if (tamano == 0) return S_OK;
                try
                {
                    Evento e;
                    e.tipo = Evento::Tipo::Datos;
                    e.datos = datos;
                    e.tamano = tamano;
                    if (!m_canal->Enviar(e).seguir) return E_ABORT;
                    if (escritos) *escritos = tamano;
                    return S_OK;
                }
                catch (...)
                {
                    return E_ABORT;
                }
            }

        private:
            std::shared_ptr<Canal> m_canal;
        };

        class AlExtraer final : public Objeto<IArchiveExtractCallback, ICryptoGetTextPassword, IArchiveRequestMemoryUseCallback>
        {
        public:
            AlExtraer(std::shared_ptr<Canal> canal, const std::atomic<bool>* cancelar, const std::vector<uint32_t>& pedidos)
                : m_canal(std::move(canal)), m_cancelar(cancelar), m_pedidos(pedidos)
            {
            }

            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** salida) override
            {
                if (!salida) return E_POINTER;
                *salida = nullptr;
                if (iid == IID_IUnknown || iid == IID_IProgress || iid == IID_IArchiveExtractCallback)
                    return Dar(static_cast<IArchiveExtractCallback*>(this), salida);
                if (iid == IID_ICryptoGetTextPassword) return Dar(static_cast<ICryptoGetTextPassword*>(this), salida);
                if (iid == IID_IArchiveRequestMemoryUseCallback) return Dar(static_cast<IArchiveRequestMemoryUseCallback*>(this), salida);
                return E_NOINTERFACE;
            }

            HRESULT STDMETHODCALLTYPE SetTotal(UInt64) override { return S_OK; }
            // 7z.dll la llama seguido, a veces desde otro hilo: así se puede cancelar en medio de algo largo
            HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64*) override
            {
                try
                {
                    return ((m_cancelar && *m_cancelar) || m_canal->Cerrado()) ? E_ABORT : S_OK;
                }
                catch (...)
                {
                    return E_ABORT;
                }
            }

            HRESULT STDMETHODCALLTYPE GetStream(UInt32 indice, ISequentialOutStream** salida, Int32 modo) override
            {
                if (!salida) return E_POINTER;
                *salida = nullptr;
                m_aceptado = false;
                m_reportar = false;
                if (modo != kExtraer)
                {
                    // 7z.dll lo pasa de largo: o no lo pidió Jícaro (otro del mismo bloque), o lo da por dañado sin
                    // entregar sus datos. De los pedidos, el resultado se avisa igual.
                    m_indice = indice;
                    m_reportar = std::binary_search(m_pedidos.begin(), m_pedidos.end(), indice);
                    return S_OK;
                }
                try
                {
                    Evento e;
                    e.tipo = Evento::Tipo::Empieza;
                    e.indice = indice;
                    Paso p = m_canal->Enviar(e).paso;
                    if (p == Paso::Parar) return E_ABORT;
                    if (p == Paso::Saltar) return S_OK;
                    *salida = new Salida(m_canal);
                    m_indice = indice;
                    m_aceptado = true;
                    return S_OK;
                }
                catch (...)
                {
                    return E_ABORT;
                }
            }

            HRESULT STDMETHODCALLTYPE PrepareOperation(Int32) override { return S_OK; }

            HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 resultado) override
            {
                bool aceptado = m_aceptado.exchange(false), reportar = m_reportar.exchange(false);
                if (!aceptado && !reportar) return S_OK;
                try
                {
                    Evento e;
                    e.tipo = aceptado ? Evento::Tipo::Termina : Evento::Tipo::SinDatos;
                    e.indice = m_indice;
                    e.resultado = resultado;
                    return m_canal->Enviar(e).seguir ? S_OK : E_ABORT;
                }
                catch (...)
                {
                    return E_ABORT;
                }
            }

            HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* clave) override
            {
                if (!clave) return E_POINTER;
                *clave = nullptr;
                try
                {
                    Evento e;
                    e.tipo = Evento::Tipo::Clave;
                    Respuesta r = m_canal->Enviar(e);
                    if (!r.clave || r.clave->empty()) return E_ABORT;
                    *clave = SysAllocStringLen(r.clave->data(), (UINT)r.clave->size());
                    Borrar(*r.clave);
                    return *clave ? S_OK : E_OUTOFMEMORY;
                }
                catch (...)
                {
                    return E_ABORT;
                }
            }

            // Un RAR 5 (o RAR 7) con un diccionario enorme: Jícaro decide si se permite tanta memoria
            HRESULT STDMETHODCALLTYPE RequestMemoryUse(UInt32, UInt32, UInt32, const wchar_t*, UInt64 necesita, UInt64* permitido,
                                                       UInt32* respuesta) override
            {
                bool si = false;
                try
                {
                    Evento e;
                    e.tipo = Evento::Tipo::Memoria;
                    e.memoria = necesita;
                    si = m_canal->Enviar(e).seguir;
                }
                catch (...)
                {
                }
                if (respuesta) *respuesta = si ? kPermitir : 0;
                if (si && permitido && *permitido < necesita) *permitido = necesita;
                return S_OK;
            }

        private:
            std::shared_ptr<Canal> m_canal;
            const std::atomic<bool>* m_cancelar;
            const std::vector<uint32_t>& m_pedidos;   // ordenados
            std::atomic<bool> m_aceptado{ false }, m_reportar{ false };
            std::atomic<uint32_t> m_indice{ 0 };
        };
    }

    // ---------------------------------------------------------------------------------------------------------------

    namespace
    {
        // Mientras 7z.dll lee la lista de un archivo, el proceso tiene un tope de memoria (un «Job» de Windows): una
        // lista comprimida enorme (una trampa) falla en vez de llenar la memoria de la PC. Los programas que Jícaro abra
        // no quedan dentro. Si Windows no deja poner el tope, se sigue sin él.
        HANDLE TrabajoDeMemoria()
        {
            static HANDLE job = []() -> HANDLE {
                HANDLE j = CreateJobObjectW(nullptr, nullptr);
                if (!j) return nullptr;
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION l{};
                l.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
                if (!SetInformationJobObject(j, JobObjectExtendedLimitInformation, &l, sizeof(l))
                    || !AssignProcessToJobObject(j, GetCurrentProcess()))
                {
                    CloseHandle(j);
                    return nullptr;
                }
                return j;
            }();
            return job;
        }

        void PonerTope(uint64_t bytes)
        {
            HANDLE j = TrabajoDeMemoria();
            if (!j) return;
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION l{};
            l.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK | (bytes ? JOB_OBJECT_LIMIT_PROCESS_MEMORY : 0);
            l.ProcessMemoryLimit = (SIZE_T)bytes;
            SetInformationJobObject(j, JobObjectExtendedLimitInformation, &l, sizeof(l));
        }

        struct TopeAlAbrir
        {
            TopeAlAbrir()
            {
                PROCESS_MEMORY_COUNTERS_EX m{};
                uint64_t usada = 0;
                if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m), sizeof(m)))
                    usada = m.PrivateUsage;
                PonerTope(usada + (2ULL << 30));   // 2 GB más de lo que ya usa: de sobra para una lista de verdad
            }
            ~TopeAlAbrir() { PonerTope(0); }
            TopeAlAbrir(const TopeAlAbrir&) = delete;
            TopeAlAbrir& operator=(const TopeAlAbrir&) = delete;
        };
    }

    struct Archivo::Interno
    {
        Ptr<IInArchive> arc;
        std::vector<std::wstring> volumenes;
        Formato formato = Formato::SieteZ;
        std::vector<Item> items;
        bool pidioClave = false, incompleto = false, danado = false;
        std::optional<std::wstring> clave;
        std::wstring faltaVolumen, noAbrio;
        int64_t total = 0;
        const std::atomic<bool>* cancelar = nullptr;
        Progreso* progreso = nullptr;

        void LeerEstado(const AlAbrir& cb)
        {
            incompleto = danado = false;
            faltaVolumen.clear();
            {
                Valor f;
                if (SUCCEEDED(arc->GetArchiveProperty(kpidErrorFlags, &f.v)))
                    if (auto n = Numero(f.v))
                    {
                        incompleto = (*n & kErrorFinInesperado) != 0;
                        danado = (*n & kErrorCabeceras) != 0;
                    }
            }
            {
                // «Missing volume : nombre»: según el archivo, hay otra parte después y no está
                Valor e;
                const std::wstring prefijo = L"Missing volume : ";
                if (SUCCEEDED(arc->GetArchiveProperty(kpidError, &e.v)))
                    if (auto t = Texto(e.v); t && t->rfind(prefijo, 0) == 0) faltaVolumen = NombreDe(t->substr(prefijo.size()));
            }
            if (faltaVolumen.empty() && incompleto && !cb.Faltan().empty()) faltaVolumen = cb.Faltan().front();
        }

        bool LeerItems()
        {
            UInt32 n = 0;
            if (FAILED(arc->GetNumberOfItems(&n))) return false;
            if (n > 4000000) return false;   // más que esto no cabe en la memoria de forma razonable
            items.clear();
            items.reserve(n);
            for (UInt32 i = 0; i < n; i++)
            {
                Item it;
                {
                    Valor v;
                    if (SUCCEEDED(arc->GetProperty(i, kpidPath, &v.v)))
                        if (auto t = Texto(v.v)) it.ruta = *t;
                }
                for (auto& c : it.ruta)
                    if (c == L'/') c = L'\\';
                while (it.ruta.size() > 1 && it.ruta.back() == L'\\') it.ruta.pop_back();
                {
                    Valor v;
                    if (SUCCEEDED(arc->GetProperty(i, kpidIsDir, &v.v))) it.carpeta = Si(v.v);
                }
                {
                    Valor v;
                    if (SUCCEEDED(arc->GetProperty(i, kpidSize, &v.v)))
                        if (auto t = Numero(v.v); t && *t <= (uint64_t)INT64_MAX) it.tamano = (int64_t)*t;
                }
                {
                    Valor v;
                    if (SUCCEEDED(arc->GetProperty(i, kpidEncrypted, &v.v))) it.cifrada = Si(v.v);
                }
                auto fecha = [&](PROPID id, bool& hay, FILETIME& f) {
                    Valor v;
                    if (SUCCEEDED(arc->GetProperty(i, id, &v.v)) && v.v.vt == VT_FILETIME)
                    {
                        f = v.v.filetime;
                        hay = f.dwHighDateTime != 0 || f.dwLowDateTime != 0;
                    }
                };
                fecha(kpidMTime, it.conMTime, it.mtime);
                fecha(kpidATime, it.conATime, it.atime);
                fecha(kpidCTime, it.conCTime, it.ctime);
                uint32_t atributos = 0;
                std::optional<uint32_t> posix;
                {
                    Valor v;
                    if (SUCCEEDED(arc->GetProperty(i, kpidAttrib, &v.v)))
                        if (auto t = Numero(v.v)) atributos = (uint32_t)*t;
                }
                {
                    Valor v;
                    if (SUCCEEDED(arc->GetProperty(i, kpidPosixAttrib, &v.v)))
                        if (auto t = Numero(v.v)) posix = (uint32_t)*t;
                }
                {
                    Valor v;
                    if (SUCCEEDED(arc->GetProperty(i, kpidSymLink, &v.v)))
                        if (auto t = Texto(v.v); t && !t->empty()) it.simbolico = true;
                }
                {
                    Valor v;
                    if (SUCCEEDED(arc->GetProperty(i, kpidIsAltStream, &v.v))) it.alterno = Si(v.v);
                }
                {
                    Valor v;
                    if (SUCCEEDED(arc->GetProperty(i, kpidIsAnti, &v.v))) it.anti = Si(v.v);
                }
                for (PROPID id : { (PROPID)kpidHardLink, (PROPID)kpidCopyLink })
                {
                    Valor v;
                    if (it.duro.empty() && SUCCEEDED(arc->GetProperty(i, id, &v.v)))
                        if (auto t = Texto(v.v); t && !t->empty()) it.duro = *t;
                }
                for (auto& c : it.duro)
                    if (c == L'/') c = L'\\';
                while (it.duro.rfind(L".\\", 0) == 0) it.duro.erase(0, 2);
                // Los atributos de Windows y el modo de Unix (que 7-Zip pone en la mitad alta, con la marca 0x8000)
                if (atributos & FILE_ATTRIBUTE_REPARSE_POINT) it.simbolico = true;
                uint32_t modo = posix ? *posix : ((atributos & 0x8000) ? (atributos >> 16) : 0);
                uint32_t tipo = modo & 0170000;
                if (tipo == 0120000) it.simbolico = true;
                else if (tipo == 0020000 || tipo == 0060000 || tipo == 0010000 || tipo == 0140000) it.especial = true;
                items.push_back(std::move(it));
            }
            return true;
        }
    };

    bool Disponible()
    {
#ifdef JICARO_PRUEBAS
        if (jicaro::pruebas::sinMotor7z) return false;
#endif
        return Crear() != nullptr;
    }

    std::optional<Formato> Detectar(const std::wstring& primera)
    {
        HANDLE h = CreateFileW(rutas::Larga(primera).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return std::nullopt;
        unsigned char b[8] = {};
        DWORD n = 0;
        BOOL ok = ReadFile(h, b, sizeof(b), &n, nullptr);
        CloseHandle(h);
        if (!ok) return std::nullopt;
        static const unsigned char rar5[] = { 'R', 'a', 'r', '!', 0x1A, 0x07, 0x01, 0x00 };
        static const unsigned char rar4[] = { 'R', 'a', 'r', '!', 0x1A, 0x07, 0x00 };
        static const unsigned char sietez[] = { '7', 'z', 0xBC, 0xAF, 0x27, 0x1C };
        if (n >= sizeof(rar5) && memcmp(b, rar5, sizeof(rar5)) == 0) return Formato::Rar5;
        if (n >= sizeof(rar4) && memcmp(b, rar4, sizeof(rar4)) == 0) return Formato::Rar;
        if (n >= sizeof(sietez) && memcmp(b, sietez, sizeof(sietez)) == 0) return Formato::SieteZ;
        return std::nullopt;
    }

    Archivo::Archivo() : m(std::make_unique<Interno>()) {}

    Archivo::~Archivo() { Cerrar(); }

    void Archivo::Cerrar()
    {
        if (m->arc)
        {
            m->arc->Close();
            m->arc.Soltar();
        }
        m->items.clear();
        m->items.shrink_to_fit();
        m->pidioClave = m->incompleto = m->danado = false;
        if (m->clave) Borrar(*m->clave);
        m->clave.reset();
        m->faltaVolumen.clear();
        m->noAbrio.clear();
        m->volumenes.clear();
    }

    Apertura Archivo::Abrir(const std::vector<std::wstring>& partes, Formato formato,
                            std::function<std::optional<std::wstring>()> clave, Progreso& progreso,
                            const std::atomic<bool>& cancelar)
    {
        Cerrar();
        m->formato = formato;
        m->cancelar = &cancelar;
        m->progreso = &progreso;
        FuncionCrear crear = Disponible() ? Crear() : nullptr;
        if (!crear || partes.empty()) return Apertura::Error;
        m->total = 0;
        for (auto& p : partes)
            if (int64_t t = TamanoArchivo(p); t > 0) m->total += t;
        progreso.totalComprimido = m->total;

        GUID clase = ClaseDe(formato);
        IInArchive* a = nullptr;
        if (FAILED(crear(&clase, &IID_IInArchive, reinterpret_cast<void**>(&a))) || !a) return Apertura::Error;
        m->arc.Poner(a);

        // Un RAR en partes con sus nombres de siempre (.part1.rar, .r00...): se abre la primera y 7z.dll pide las
        // demás. Un .001, .002... (y todos los 7z, que se cortan en trozos): se lee todo seguido.
        bool porVolumenes = formato != Formato::SieteZ && !EsNumerada(partes.front());
        Ptr<Flujo> flujo(new Flujo(porVolumenes ? std::vector<std::wstring>{ partes.front() } : partes, &progreso, &cancelar));
        if (!flujo->Falta().empty())
        {
            m->noAbrio = flujo->Falta();
            m->arc.Soltar();
            return Apertura::Error;
        }
        Ptr<AlAbrir> cb(new AlAbrir(partes, porVolumenes, std::move(clave), &progreso, &cancelar));
        const UInt64 soloAlPrincipio = 0;   // la firma ya se vio al principio: no se busca más adentro
        HRESULT hr = E_FAIL;
        {
            TopeAlAbrir tope;
            hr = m->arc->Open(flujo.get(), &soloAlPrincipio, cb.get());
        }
        m->volumenes = porVolumenes ? std::vector<std::wstring>{ partes.front() } : partes;
        for (auto& u : cb->Usadas()) m->volumenes.push_back(u);
        m->pidioClave = cb->Pidio();
        if (cb->Clave()) m->clave = *cb->Clave();
        if (m->noAbrio.empty()) m->noAbrio = cb->NoAbrio();
        try
        {
            m->LeerEstado(*cb.get());
        }
        catch (...)
        {
        }
        if (cancelar) return Apertura::Cancelado;
        if (hr != S_OK)
        {
            Apertura r = cb->SinClave() ? Apertura::SinClave
                       : hr == E_OUTOFMEMORY ? Apertura::SinMemoria
                       : m->pidioClave ? Apertura::ClaveMala
                       : hr == S_FALSE ? Apertura::NoEsArchivo
                       : Apertura::Error;
            m->arc->Close();
            m->arc.Soltar();
            return r;
        }
        bool leyo = false;
        try
        {
            leyo = m->LeerItems();
        }
        catch (...)
        {
            leyo = false;
        }
        if (!leyo)
        {
            m->items.clear();
            m->arc->Close();
            m->arc.Soltar();
            return Apertura::SinMemoria;
        }
        return Apertura::Bien;
    }

    const std::vector<Item>& Archivo::Items() const { return m->items; }

    std::wstring Archivo::NombreFormato() const
    {
        switch (m->formato)
        {
        case Formato::Rar: return L"RAR";
        case Formato::Rar5: return L"RAR 5";
        default: return L"7z";
        }
    }

    bool Archivo::PidioClave() const { return m->pidioClave; }
    const std::optional<std::wstring>& Archivo::ClaveUsada() const { return m->clave; }
    std::wstring Archivo::FaltaVolumen() const { return m->faltaVolumen; }
    bool Archivo::Incompleto() const { return m->incompleto; }
    bool Archivo::CabecerasDanadas() const { return m->danado; }
    int64_t Archivo::TotalComprimido() const { return m->total; }
    bool Archivo::Abierto() const { return (bool)m->arc; }
    std::wstring Archivo::NoAbrio() const { return m->noAbrio; }
    const std::vector<std::wstring>& Archivo::Volumenes() const { return m->volumenes; }

    HRESULT Archivo::Extraer(const std::vector<uint32_t>& indices, Destino& d)
    {
        if (!m->arc) return E_FAIL;
        if (indices.empty()) return S_OK;
        auto canal = std::make_shared<Canal>();
        Ptr<AlExtraer> cb(new AlExtraer(canal, m->cancelar, indices));
        IInArchive* arc = m->arc.get();
        AlExtraer* callback = cb.get();
        HRESULT hr = E_FAIL;
        std::thread hilo;
        try
        {
            hilo = std::thread([&hr, arc, &indices, callback, canal]() {
                HRESULT r = E_FAIL;
                try
                {
                    r = arc->Extract(indices.data(), (UInt32)indices.size(), 0, callback);
                }
                catch (...)
                {
                    r = E_FAIL;
                }
                hr = r;
                canal->Terminar();
            });
        }
        catch (...)
        {
            return E_OUTOFMEMORY;
        }
        try
        {
            Evento e;
            while (canal->Recibir(e))
            {
                Respuesta r;
                switch (e.tipo)
                {
                case Evento::Tipo::Empieza: r.paso = d.Empieza(e.indice); break;
                case Evento::Tipo::Datos: r.seguir = d.Datos(e.datos, e.tamano); break;
                case Evento::Tipo::Termina:
                    d.Termina(e.resultado);
                    r.seguir = true;
                    break;
                case Evento::Tipo::SinDatos:
                    d.SinDatos(e.indice, e.resultado);
                    r.seguir = true;
                    break;
                case Evento::Tipo::Clave: r.clave = d.Clave(); break;
                case Evento::Tipo::Memoria: r.seguir = d.Memoria(e.memoria); break;
                }
                canal->Responder(std::move(r));
            }
        }
        catch (...)
        {
            // Que 7z.dll termine (todo lo que pida recibe «parar») antes de seguir con la falla
            canal->Cerrar();
            hilo.join();
            throw;
        }
        hilo.join();
        return hr;
    }
}
