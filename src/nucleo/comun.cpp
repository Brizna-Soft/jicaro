#include "comun.hpp"

#include <windows.h>
#include <shellapi.h>

#include <cwctype>
#include <vector>

#include "rutas.hpp"

std::wstring jicaro::Ancho(std::string_view utf8)
{
    return AnchoDesde(CP_UTF8, utf8);
}

std::wstring jicaro::AnchoDesde(unsigned pagina, std::string_view texto)
{
    if (texto.empty()) return {};
    int n = MultiByteToWideChar(pagina, 0, texto.data(), (int)texto.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring r(n, L'\0');
    MultiByteToWideChar(pagina, 0, texto.data(), (int)texto.size(), r.data(), n);
    return r;
}

std::string jicaro::Utf8(std::wstring_view texto)
{
    if (texto.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, texto.data(), (int)texto.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string r(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, texto.data(), (int)texto.size(), r.data(), n, nullptr, nullptr);
    return r;
}

std::wstring jicaro::Minusculas(std::wstring texto)
{
    if (texto.empty()) return texto;
    std::wstring r(texto.size(), L'\0');
    int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, texto.data(), (int)texto.size(), r.data(),
                          (int)r.size(), nullptr, nullptr, 0);
    return n == (int)r.size() ? r : texto;
}

bool jicaro::IgualesSinMayusculas(std::wstring_view a, std::wstring_view b)
{
    return a.size() == b.size()
        && CompareStringOrdinal(a.data(), (int)a.size(), b.data(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

bool jicaro::EmpiezaSinMayusculas(std::wstring_view texto, std::wstring_view inicio)
{
    return texto.size() >= inicio.size() && IgualesSinMayusculas(texto.substr(0, inicio.size()), inicio);
}

bool jicaro::TerminaSinMayusculas(std::wstring_view texto, std::wstring_view fin)
{
    return texto.size() >= fin.size() && IgualesSinMayusculas(texto.substr(texto.size() - fin.size()), fin);
}

std::wstring jicaro::TextoTamano(int64_t bytes)
{
    if (bytes < 0) return L"?";
    const wchar_t* unidades[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    wchar_t buf[64];
    if (u == 0) swprintf(buf, 64, L"%lld B", (long long)bytes);
    else swprintf(buf, 64, v < 10 ? L"%.2f %s" : (v < 100 ? L"%.1f %s" : L"%.0f %s"), v, unidades[u]);
    return buf;
}

std::wstring jicaro::TextoNumero(int64_t n)
{
    std::wstring digitos = std::to_wstring(n < 0 ? -n : n);
    std::wstring r;
    int cuenta = 0;
    for (auto i = digitos.rbegin(); i != digitos.rend(); ++i)
    {
        if (cuenta && cuenta % 3 == 0) r.insert(r.begin(), L',');
        r.insert(r.begin(), *i);
        cuenta++;
    }
    return n < 0 ? L"-" + r : r;
}

std::wstring jicaro::RutaCompleta(const std::wstring& ruta)
{
    if (ruta.empty()) return ruta;
    std::vector<wchar_t> buf(32768);
    DWORD n = GetFullPathNameW(ruta.c_str(), (DWORD)buf.size(), buf.data(), nullptr);
    if (n == 0 || n >= buf.size()) return ruta;
    return std::wstring(buf.data(), n);
}

std::wstring jicaro::CarpetaDe(const std::wstring& ruta)
{
    auto p = ruta.find_last_of(L"\\/");
    if (p == std::wstring::npos) return L"";
    if (p == 2 && ruta.size() > 2 && ruta[1] == L':') return ruta.substr(0, 3);   // «C:\»
    return ruta.substr(0, p);
}

std::wstring jicaro::NombreDe(const std::wstring& ruta)
{
    auto p = ruta.find_last_of(L"\\/");
    return p == std::wstring::npos ? ruta : ruta.substr(p + 1);
}

std::wstring jicaro::CarpetaDelPrograma()
{
    std::vector<wchar_t> buf(32768);
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
    return CarpetaDe(std::wstring(buf.data(), n));
}

bool jicaro::ExisteArchivo(const std::wstring& ruta)
{
    DWORD a = GetFileAttributesW(rutas::Larga(ruta).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool jicaro::ExisteCarpeta(const std::wstring& ruta)
{
    DWORD a = GetFileAttributesW(rutas::Larga(ruta).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int64_t jicaro::TamanoArchivo(const std::wstring& ruta)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(rutas::Larga(ruta).c_str(), GetFileExInfoStandard, &d)) return -1;
    return ((int64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

int64_t jicaro::EspacioLibre(const std::wstring& carpeta)
{
    ULARGE_INTEGER libre;
    std::wstring c = carpeta;
    if (!c.empty() && c.back() != L'\\') c += L'\\';
    if (!GetDiskFreeSpaceExW(rutas::Larga(c).c_str(), &libre, nullptr, nullptr)) return -1;
    return (int64_t)libre.QuadPart;
}

bool jicaro::CrearCarpetas(const std::wstring& ruta)
{
    if (ruta.empty()) return false;
    if (ExisteCarpeta(ruta)) return true;
    std::wstring arriba = CarpetaDe(ruta);
    if (!arriba.empty() && arriba != ruta && !ExisteCarpeta(arriba) && !CrearCarpetas(arriba)) return false;
    return CreateDirectoryW(rutas::Larga(ruta).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

#ifdef JICARO_PRUEBAS
std::function<bool(const std::wstring&)> jicaro::pruebas::papelera;
std::string jicaro::pruebas::ultimoError;
#endif

bool jicaro::MandarAPapelera(const std::wstring& ruta)
{
    if (GetFileAttributesW(rutas::Larga(ruta).c_str()) == INVALID_FILE_ATTRIBUTES) return true;
#ifdef JICARO_PRUEBAS
    if (pruebas::papelera) return pruebas::papelera(ruta);
#endif
    std::wstring doble = ruta;
    doble.push_back(L'\0');   // SHFileOperation pide la lista terminada en dos ceros
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = doble.c_str();
    // FOF_WANTNUKEWARNING: si el disco no tiene Papelera (por ejemplo, algunas memorias USB), Windows pregunta antes
    // de borrar para siempre, en vez de borrar sin avisar
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOF_WANTNUKEWARNING;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted
        && GetFileAttributesW(rutas::Larga(ruta).c_str()) == INVALID_FILE_ATTRIBUTES;
}
