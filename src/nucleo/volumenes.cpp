#include "volumenes.hpp"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <regex>
#include <vector>

#include "comun.hpp"
#include "rutas.hpp"

namespace
{
    // Nombres de los archivos de una carpeta que empiezan con «inicio» (se filtran después con el nombre largo:
    // el patrón de Windows también acepta los nombres cortos 8.3)
    std::vector<std::wstring> NombresEn(const std::wstring& carpeta, const std::wstring& inicio)
    {
        std::vector<std::wstring> r;
        WIN32_FIND_DATAW d;
        HANDLE h = FindFirstFileExW(jicaro::rutas::Larga(jicaro::rutas::Unir(carpeta, L"*")).c_str(), FindExInfoBasic, &d,
                                    FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) return r;
        do
        {
            if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && jicaro::EmpiezaSinMayusculas(d.cFileName, inicio))
                r.push_back(d.cFileName);
        } while (FindNextFileW(h, &d));
        FindClose(h);
        return r;
    }

    std::wstring ConCeros(int n, size_t ancho)
    {
        std::wstring s = std::to_wstring(n);
        while (s.size() < ancho) s.insert(s.begin(), L'0');
        return s;
    }

    // Sin puntos ni espacios al final (Windows no los deja en una carpeta)
    std::wstring Limpio(std::wstring nombre)
    {
        while (!nombre.empty() && (nombre.back() == L'.' || nombre.back() == L' ')) nombre.pop_back();
        return nombre.empty() ? L"extraido" : nombre;
    }

    std::wstring NombreSuelto(const std::wstring& nombre)
    {
        static const std::pair<const wchar_t*, const wchar_t*> cambios[] = {
            { L".tgz", L".tar" }, { L".tbz2", L".tar" }, { L".tbz", L".tar" }, { L".txz", L".tar" }, { L".tzst", L".tar" },
            { L".tlz", L".tar" }, { L".gz", L"" }, { L".bz2", L"" }, { L".xz", L"" }, { L".zst", L"" }, { L".lzma", L"" },
            { L".lz", L"" }, { L".z", L"" },
        };
        for (auto& [ext, nuevo] : cambios)
        {
            std::wstring_view e(ext);
            if (nombre.size() > e.size() && jicaro::TerminaSinMayusculas(nombre, e))
                return Limpio(nombre.substr(0, nombre.size() - e.size()) + nuevo);
        }
        return Limpio(nombre);
    }

    // ¿En cuántas partes se cortó este ZIP? Lo dice el registro final del ZIP (el número de la parte que lo trae; en un
    // ZIP64, el total de partes). 0: es un ZIP de una sola parte; -1: no se pudo leer (no parece un ZIP).
    //
    // El comentario del ZIP puede traer algo con la misma forma que el registro final: por eso cada candidato se
    // comprueba (partes coherentes y, si la lista empieza en este archivo, que de verdad esté ahí) y, ante uno que no
    // se puede comprobar, se prefiere uno anterior que sí.
    int UltimaParteDelZip(const std::wstring& ruta)
    {
        HANDLE h = CreateFileW(jicaro::rutas::Larga(ruta).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return -1;
        int r = -1;
        LARGE_INTEGER tam{};
        if (GetFileSizeEx(h, &tam) && tam.QuadPart >= 22)
        {
            // El registro final (22 bytes y un comentario de hasta 65535) está al final; antes puede ir el de ZIP64 (20)
            const int64_t leer = std::min<int64_t>(tam.QuadPart, 22 + 65535 + 20);
            const int64_t inicioBufer = tam.QuadPart - leer;
            std::vector<unsigned char> b((size_t)leer);
            LARGE_INTEGER desde;
            desde.QuadPart = inicioBufer;
            DWORD n = 0;
            if (SetFilePointerEx(h, desde, nullptr, FILE_BEGIN) && ReadFile(h, b.data(), (DWORD)leer, &n, nullptr) && n == (DWORD)leer)
            {
                auto l16 = [&](size_t p) { return (uint32_t)b[p] | ((uint32_t)b[p + 1] << 8); };
                auto l32 = [&](size_t p) { return l16(p) | (l16(p + 2) << 16); };
                // ¿Empieza la lista (el directorio central) en esta posición del archivo?
                auto hayLista = [&](int64_t pos) {
                    unsigned char f[4] = {};
                    LARGE_INTEGER p;
                    p.QuadPart = pos;
                    DWORD leidos = 0;
                    return SetFilePointerEx(h, p, nullptr, FILE_BEGIN) && ReadFile(h, f, 4, &leidos, nullptr) && leidos == 4
                        && f[0] == 'P' && f[1] == 'K' && f[2] == 1 && f[3] == 2;
                };
                int sinComprobar = -1;   // un candidato aceptable que no se pudo comprobar del todo
                for (size_t p = b.size() - 22 + 1; p-- > 0;)
                {
                    if (b[p] != 'P' || b[p + 1] != 'K' || b[p + 2] != 5 || b[p + 3] != 6) continue;
                    if (p + 22 + l16(p + 20) != b.size()) continue;   // el comentario tiene que llegar justo al final
                    const uint32_t estaParte = l16(p + 4), parteLista = l16(p + 6), enEsta = l16(p + 8), total = l16(p + 10);
                    const uint32_t tamLista = l32(p + 12), posLista = l32(p + 16);
                    if (estaParte == 0xFFFF || parteLista == 0xFFFF || posLista == 0xFFFFFFFF)
                    {
                        // ZIP64: el localizador de 20 bytes va justo antes y trae el total de partes
                        if (p < 20 || b[p - 20] != 'P' || b[p - 19] != 'K' || b[p - 18] != 6 || b[p - 17] != 7) continue;
                        const uint32_t partes = l32(p - 20 + 16);
                        if (partes < 1 || partes > 100000) continue;
                        r = (int)partes - 1;
                        break;
                    }
                    if (parteLista > estaParte || enEsta > total || estaParte > 100000) continue;
                    if (parteLista == estaParte)
                    {
                        // La lista empieza en este archivo: tiene que estar antes del registro final y empezar con su firma
                        const int64_t posRegistro = inicioBufer + (int64_t)p;
                        if (total == 0)
                        {
                            if (estaParte != 0) continue;
                            r = 0;
                            break;
                        }
                        if ((int64_t)posLista + (int64_t)tamLista > posRegistro || !hayLista(posLista)) continue;
                        r = (int)estaParte;
                        break;
                    }
                    // La lista empieza en una parte anterior: no se puede comprobar aquí; se prefiere uno anterior que sí
                    if (sinComprobar < 0 && total > 0 && tamLista > 0) sinComprobar = (int)estaParte;
                }
                if (r < 0) r = sinComprobar;
            }
        }
        CloseHandle(h);
        return r;
    }
    // Parte vieja número k: 0 = .rar, 1 = .r00, ... 100 = .r99, 101 = .s00 ...
    std::wstring ExtensionVieja(int k)
    {
        if (k == 0) return L"rar";
        int i = k - 1;
        std::wstring e(1, (wchar_t)(L'r' + i / 100));
        return e + ConCeros(i % 100, 2);
    }
}

std::wstring jicaro::volumenes::NombreSinExtension(const std::wstring& nombre)
{
    static const wchar_t* conocidas[] = {
        L".tar.gz", L".tar.bz2", L".tar.xz", L".tar.zst", L".tar.lzma", L".tar.lz", L".tar.z",
        L".tgz", L".tbz2", L".tbz", L".txz", L".tzst", L".tlz",
        L".zip", L".rar", L".7z", L".tar", L".gz", L".bz2", L".xz", L".zst", L".lzma", L".lz", L".z", L".iso", L".cab",
    };
    for (auto* ext : conocidas)
    {
        std::wstring_view e(ext);
        if (nombre.size() > e.size() && TerminaSinMayusculas(nombre, e))
            return Limpio(nombre.substr(0, nombre.size() - e.size()));
    }
    auto p = nombre.find_last_of(L'.');
    if (p != std::wstring::npos && p > 0) return Limpio(nombre.substr(0, p));
    return Limpio(nombre + L" (extraído)");
}

jicaro::volumenes::Juego jicaro::volumenes::Buscar(const std::wstring& ruta)
{
    Juego j;
    std::wstring completa = RutaCompleta(ruta);
    j.carpeta = CarpetaDe(completa);
    std::wstring nombre = NombreDe(completa);
    std::wsmatch m;

    // 1. «nombre.part01.rar»
    static const std::wregex nuevo(LR"(^(.*)\.part(\d{1,4})\.rar$)", std::regex::icase);
    if (std::regex_match(nombre, m, nuevo))
    {
        std::wstring base = m[1].str();
        size_t ancho = (size_t)m[2].length();
        std::map<int, std::wstring> encontradas;
        for (auto& n : NombresEn(j.carpeta, base))
        {
            std::wsmatch m2;
            if (!std::regex_match(n, m2, nuevo) || !IgualesSinMayusculas(m2[1].str(), base)) continue;
            int k = std::stoi(m2[2].str());
            if (k <= 0) continue;
            auto it = encontradas.find(k);
            if (it == encontradas.end() || (size_t)m2[2].length() == ancho) encontradas[k] = n;
        }
        j.tipo = Tipo::RarNuevo;
        j.nombreBase = Limpio(base);
        int ultima = encontradas.empty() ? 1 : encontradas.rbegin()->first;
        for (int k = 1; k <= ultima; k++)
        {
            auto it = encontradas.find(k);
            if (it != encontradas.end())
            {
                j.partes.push_back(rutas::Unir(j.carpeta, it->second));
                if (j.faltan.empty()) j.seguidas++;
            }
            else j.faltan.push_back(base + L".part" + ConCeros(k, ancho) + L".rar");
        }
        return j;
    }

    // 2. ZIP en partes (WinZip, Info-ZIP, WinRAR): «nombre.z01», «nombre.z02»... y al final «nombre.zip», que trae la
    //    lista. Solo si hay al menos una .zNN: un «nombre.zip» solo es un ZIP de siempre.
    static const std::wregex zipParte(LR"(^(.*)\.z(\d{2,3})$)", std::regex::icase);
    std::wstring baseZip;
    if (std::regex_match(nombre, m, zipParte)) baseZip = m[1].str();
    else if (nombre.size() > 4 && TerminaSinMayusculas(nombre, L".zip")) baseZip = nombre.substr(0, nombre.size() - 4);
    if (!baseZip.empty())
    {
        std::map<int, std::wstring> trozos;
        std::wstring zip;
        for (auto& n : NombresEn(j.carpeta, baseZip + L"."))
        {
            if (n.size() <= baseZip.size() + 1) continue;
            std::wstring ext = n.substr(baseZip.size() + 1);
            if (IgualesSinMayusculas(ext, L"zip")) zip = n;
            else if (ext.size() >= 3 && ext.size() <= 4 && towlower(ext[0]) == L'z'
                     && std::all_of(ext.begin() + 1, ext.end(), [](wchar_t c) { return iswdigit(c) != 0; }))
            {
                int k = std::stoi(ext.substr(1));
                if (k >= 1) trozos[k] = n;
            }
        }
        // Si está el .zip, su registro final dice si de verdad es un ZIP en partes y cuántas son: un «fotos.z02» ajeno al
        // lado de un «fotos.zip» común no lo convierte en partido, y si solo está el .zip se sabe qué partes faltan
        int ultimaSegunZip = zip.empty() ? -1 : UltimaParteDelZip(rutas::Unir(j.carpeta, zip));
        if (ultimaSegunZip == 0) trozos.clear();
        if (!trozos.empty() || ultimaSegunZip > 0)
        {
            j.tipo = Tipo::ZipPartes;
            j.nombreBase = Limpio(baseZip);
            int ultima = ultimaSegunZip > 0 ? ultimaSegunZip : trozos.rbegin()->first;
            for (int k = 1; k <= ultima; k++)
            {
                auto it = trozos.find(k);
                if (it != trozos.end()) j.partes.push_back(rutas::Unir(j.carpeta, it->second));
                else j.faltan.push_back(baseZip + L".z" + ConCeros(k, 2));
            }
            if (zip.empty()) j.faltan.push_back(baseZip + L".zip");
            else j.partes.push_back(rutas::Unir(j.carpeta, zip));
            // Sin la última parte (la lista) o con un hueco no se puede leer nada: o están todas, o ninguna
            j.seguidas = j.faltan.empty() ? j.partes.size() : 0;
            return j;
        }
    }

    // 3. Estilo viejo: «nombre.rar», «nombre.r00», «nombre.r01»... (después de .r99 sigue .s00)
    static const std::wregex viejo(LR"(^(.*)\.([r-z])(\d\d)$)", std::regex::icase);
    std::wstring base;
    bool esParteVieja = std::regex_match(nombre, m, viejo);
    if (esParteVieja) base = m[1].str();
    else if (nombre.size() > 4 && TerminaSinMayusculas(nombre, L".rar")) base = nombre.substr(0, nombre.size() - 4);
    if (!base.empty())
    {
        std::map<int, std::wstring> encontradas;
        for (auto& n : NombresEn(j.carpeta, base + L"."))
        {
            if (n.size() != base.size() + 4) continue;
            std::wstring ext = n.substr(base.size() + 1);
            if (IgualesSinMayusculas(ext, L"rar")) encontradas[0] = n;
            else
            {
                wchar_t letra = (wchar_t)towlower(ext[0]);
                if (letra >= L'r' && letra <= L'z' && iswdigit(ext[1]) && iswdigit(ext[2]))
                    encontradas[1 + (letra - L'r') * 100 + (ext[1] - L'0') * 10 + (ext[2] - L'0')] = n;
            }
        }
        bool hayMas = encontradas.size() >= 2 || (esParteVieja && encontradas.count(0) == 0);
        // Una parte .s00, .t00... sin el .rar de base puede ser otra cosa: solo .r00-.r99 cuentan solas
        if (esParteVieja && encontradas.count(0) == 0 && towlower(m[2].str()[0]) != L'r') hayMas = false;
        if (hayMas)
        {
            j.tipo = Tipo::RarViejo;
            j.nombreBase = Limpio(base);
            int ultima = encontradas.rbegin()->first;
            for (int k = 0; k <= ultima; k++)
            {
                auto it = encontradas.find(k);
                if (it != encontradas.end())
                {
                    j.partes.push_back(rutas::Unir(j.carpeta, it->second));
                    if (j.faltan.empty()) j.seguidas++;
                }
                else j.faltan.push_back(base + L"." + ExtensionVieja(k));
            }
            return j;
        }
    }

    // 4. Trozos numerados: «nombre.7z.001», «nombre.7z.002»...
    static const std::wregex numerado(LR"(^(.*)\.(\d{3})$)");
    if (std::regex_match(nombre, m, numerado) && std::stoi(m[2].str()) >= 1)
    {
        std::wstring b = m[1].str();
        std::map<int, std::wstring> encontradas;
        for (auto& n : NombresEn(j.carpeta, b + L"."))
        {
            if (n.size() != b.size() + 4) continue;
            std::wstring num = n.substr(b.size() + 1);
            if (iswdigit(num[0]) && iswdigit(num[1]) && iswdigit(num[2]))
            {
                int k = std::stoi(num);
                if (k >= 1) encontradas[k] = n;
            }
        }
        j.tipo = Tipo::Numerado;
        j.nombreBase = NombreSinExtension(b);
        j.nombreSuelto = NombreSuelto(b);
        int ultima = encontradas.empty() ? 1 : encontradas.rbegin()->first;
        for (int k = 1; k <= ultima; k++)
        {
            auto it = encontradas.find(k);
            if (it != encontradas.end())
            {
                j.partes.push_back(rutas::Unir(j.carpeta, it->second));
                if (j.faltan.empty()) j.seguidas++;
            }
            else j.faltan.push_back(b + L"." + ConCeros(k, 3));
        }
        return j;
    }

    // 5. Un solo archivo
    j.tipo = Tipo::Uno;
    j.partes.push_back(completa);
    j.seguidas = 1;
    j.nombreBase = NombreSinExtension(nombre);
    j.nombreSuelto = NombreSuelto(nombre);
    return j;
}
