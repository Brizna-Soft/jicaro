#include "volumenes.hpp"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <regex>

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
        if (!trozos.empty())
        {
            j.tipo = Tipo::ZipPartes;
            j.nombreBase = Limpio(baseZip);
            int ultima = trozos.rbegin()->first;
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
