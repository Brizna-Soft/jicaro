#include "rutas.hpp"

#include <windows.h>

#include <cwctype>
#include <vector>

#include "comun.hpp"

namespace
{
    bool EsSeparador(wchar_t c) { return c == L'\\' || c == L'/'; }
    bool EsLetraAscii(wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); }

    // Caracteres que Windows no acepta en un nombre, y los que se parecen a una barra
    bool Prohibido(wchar_t c)
    {
        if (c < 32 || c == 127) return true;
        switch (c)
        {
        case L'<': case L'>': case L':': case L'"': case L'|': case L'?': case L'*':
        // Parecidos a «/» y «\» (por si algún programa los convierte en barras de verdad)
        case 0x2044: case 0x2215: case 0x29F8: case 0xFF0F:
        case 0x2216: case 0x29F5: case 0x29F9: case 0xFE68: case 0xFF3C:
            return true;
        default:
            return false;
        }
    }

    // CON, PRN, AUX, NUL, COM0-9, LPT0-9 (con o sin extensión), CONIN$ y CONOUT$: Windows los trata como
    // dispositivos, no como archivos
    bool NombreReservado(const std::wstring& nombre)
    {
        std::wstring base = nombre.substr(0, nombre.find(L'.'));
        while (!base.empty() && base.back() == L' ') base.pop_back();
        for (auto& c : base) c = (wchar_t)towupper(c);
        static const wchar_t* fijos[] = { L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$" };
        for (auto* f : fijos)
            if (base == f) return true;
        if (base.size() == 4 && (base.compare(0, 3, L"COM") == 0 || base.compare(0, 3, L"LPT") == 0))
        {
            wchar_t d = base[3];
            return (d >= L'0' && d <= L'9') || d == 0x00B9 || d == 0x00B2 || d == 0x00B3;
        }
        return false;
    }

    // NTFS acepta hasta 255 caracteres por nombre: se recorta y se conserva la extensión
    std::wstring Recortar(std::wstring parte)
    {
        if (parte.size() <= 255) return parte;
        auto punto = parte.find_last_of(L'.');
        std::wstring ext = (punto != std::wstring::npos && parte.size() - punto <= 20) ? parte.substr(punto) : L"";
        std::wstring base = parte.substr(0, 255 - ext.size());
        if (!base.empty() && IS_HIGH_SURROGATE(base.back())) base.pop_back();
        while (!base.empty() && (base.back() == L'.' || base.back() == L' ')) base.pop_back();
        if (base.empty()) base = L"_";
        return base + ext;
    }

    // Ruta completa y normalizada por Windows, sin «\\?\» delante
    std::wstring Normalizar(std::wstring ruta)
    {
        if (ruta.rfind(L"\\\\?\\UNC\\", 0) == 0) ruta = L"\\\\" + ruta.substr(8);
        else if (ruta.rfind(L"\\\\?\\", 0) == 0) ruta = ruta.substr(4);
        std::vector<wchar_t> buf(32768);
        DWORD n = GetFullPathNameW(ruta.c_str(), (DWORD)buf.size(), buf.data(), nullptr);
        if (n == 0 || n >= buf.size()) return ruta;
        return std::wstring(buf.data(), n);
    }
}

jicaro::rutas::Limpia jicaro::rutas::Limpiar(std::wstring_view ruta)
{
    Limpia r;
    if (ruta.empty())
    {
        r.motivo = Motivo::Vacia;
        return r;
    }
    // Absolutas: «\algo», «\\servidor\...», «\\?\...», «/algo» y «C:...» (también «C:algo», relativa a la unidad)
    if (EsSeparador(ruta[0]) || (ruta.size() >= 2 && ruta[1] == L':' && EsLetraAscii(ruta[0])))
    {
        r.motivo = Motivo::Absoluta;
        return r;
    }

    std::wstring salida;
    size_t i = 0;
    while (i < ruta.size())
    {
        size_t fin = i;
        while (fin < ruta.size() && !EsSeparador(ruta[fin])) fin++;
        std::wstring parte(ruta.substr(i, fin - i));
        i = fin + 1;
        if (parte.empty() || parte == L".") continue;
        if (parte == L"..")
        {
            r.motivo = Motivo::SubeDeCarpeta;
            return r;
        }
        for (auto& c : parte)
            if (Prohibido(c)) c = L'_';
        // Windows quita los puntos y espacios del final de un nombre: se quitan aquí para que la ruta que se revisa
        // sea la misma que se crea. Si no queda nada («...», «. .»), se usa «_».
        while (!parte.empty() && (parte.back() == L'.' || parte.back() == L' ')) parte.pop_back();
        if (parte.empty()) parte = L"_";
        if (NombreReservado(parte)) parte = L"_" + parte;
        parte = Recortar(parte);
        if (!salida.empty()) salida += L'\\';
        salida += parte;
    }
    if (salida.empty())
    {
        r.motivo = Motivo::Vacia;
        return r;
    }
    if (salida.size() > 30000)
    {
        r.motivo = Motivo::MuyLarga;
        return r;
    }
    r.ok = true;
    r.relativa = salida;
    return r;
}

std::wstring jicaro::rutas::Unir(const std::wstring& carpeta, const std::wstring& relativa)
{
    if (carpeta.empty()) return relativa;
    if (carpeta.back() == L'\\') return carpeta + relativa;
    return carpeta + L'\\' + relativa;
}

bool jicaro::rutas::DentroDe(const std::wstring& carpeta, const std::wstring& ruta)
{
    std::wstring c = Normalizar(carpeta), r = Normalizar(ruta);
    if (c.empty() || r.empty()) return false;
    while (c.size() > 3 && c.back() == L'\\') c.pop_back();
    std::wstring prefijo = c.back() == L'\\' ? c : c + L'\\';
    return r.size() > prefijo.size() && EmpiezaSinMayusculas(r, prefijo);
}

bool jicaro::rutas::SinDesvios(const std::wstring& carpeta, const std::wstring& relativa, std::wstring* donde)
{
    std::wstring actual = carpeta;
    while (actual.size() > 3 && actual.back() == L'\\') actual.pop_back();
    size_t i = 0;
    while (i < relativa.size())
    {
        size_t fin = relativa.find(L'\\', i);
        if (fin == std::wstring::npos) fin = relativa.size();
        if (actual.back() != L'\\') actual += L'\\';
        actual.append(relativa, i, fin - i);
        i = fin + 1;

        WIN32_FIND_DATAW d;
        HANDLE h = FindFirstFileExW(Larga(actual).c_str(), FindExInfoBasic, &d, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) return true;   // no existe: lo de más abajo tampoco
        FindClose(h);
        // Enlaces simbólicos y uniones (junction) llevan a otro lugar del disco. Otros puntos de análisis, como los
        // archivos de OneDrive, no desvían la ruta.
        if ((d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(d.dwReserved0))
        {
            if (donde) *donde = actual;
            return false;
        }
    }
    return true;
}

std::wstring jicaro::rutas::Larga(const std::wstring& ruta)
{
    if (ruta.size() < 240 || ruta.rfind(L"\\\\?\\", 0) == 0) return ruta;
    if (ruta.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + ruta.substr(2);
    if (ruta.size() >= 3 && ruta[1] == L':' && ruta[2] == L'\\') return L"\\\\?\\" + ruta;
    return ruta;
}

std::wstring jicaro::rutas::NombreLibre(const std::wstring& ruta)
{
    std::wstring carpeta = CarpetaDe(ruta), nombre = NombreDe(ruta);
    auto punto = nombre.find_last_of(L'.');
    bool conExtension = punto != std::wstring::npos && punto > 0;
    std::wstring base = conExtension ? nombre.substr(0, punto) : nombre;
    std::wstring ext = conExtension ? nombre.substr(punto) : L"";
    for (int i = 2; i < 100000; i++)
    {
        std::wstring prueba = Unir(carpeta, base + L" (" + std::to_wstring(i) + L")" + ext);
        if (GetFileAttributesW(Larga(prueba).c_str()) == INVALID_FILE_ATTRIBUTES) return prueba;
    }
    return ruta;
}

std::wstring jicaro::rutas::TextoMotivo(Motivo motivo)
{
    switch (motivo)
    {
    case Motivo::Vacia: return L"no tiene nombre";
    case Motivo::Absoluta: return L"trae una ruta completa (como C:\\... o \\...) que saldría de la carpeta";
    case Motivo::SubeDeCarpeta: return L"trae «..» en la ruta y saldría de la carpeta";
    case Motivo::MuyLarga: return L"la ruta es demasiado larga";
    default: return L"";
    }
}
