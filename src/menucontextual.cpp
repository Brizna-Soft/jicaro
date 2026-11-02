#include "menucontextual.hpp"

#include <windows.h>

namespace
{
    // Los tipos de archivo que Jícaro abre. SystemFileAssociations vale para la extensión aunque otro programa la tenga
    // como predeterminada, y no cambia cuál programa abre el archivo con doble clic.
    const wchar_t* const Extensiones[] = { L".zip", L".rar", L".7z", L".tar", L".gz", L".tgz", L".bz2", L".tbz2", L".tbz",
                                           L".xz", L".txz", L".zst", L".tzst", L".iso", L".cab", L".001", L".z01" };
    const wchar_t* const Base = L"Software\\Classes\\SystemFileAssociations\\";

    struct Verbo
    {
        const wchar_t* clave;
        const wchar_t* texto;
        const wchar_t* opciones;
    };
    const Verbo Verbos[] = {
        { L"Jicaro.ExtraerAqui", L"Extraer aquí", L" --aqui" },
        { L"Jicaro.ExtraerEnCarpeta", L"Extraer en una carpeta con su nombre", L"" },
    };

    bool PonerTexto(HKEY clave, const wchar_t* nombre, const std::wstring& valor)
    {
        return RegSetValueExW(clave, nombre, 0, REG_SZ, reinterpret_cast<const BYTE*>(valor.c_str()),
                              (DWORD)((valor.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    }

    std::wstring Comando(const std::wstring& exe, const wchar_t* opciones)
    {
        return L"\"" + exe + L"\" --extraer \"%1\"" + opciones;
    }

    // Borra la clave solo si quedó vacía (sin subclaves ni valores): así no queda basura y nunca se toca lo de otros
    // programas
    void BorrarSiVacia(const std::wstring& ruta)
    {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, ruta.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return;
        DWORD subclaves = 0, valores = 0;
        bool vacia = RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, &subclaves, nullptr, nullptr, &valores, nullptr, nullptr,
                                      nullptr, nullptr) == ERROR_SUCCESS
            && subclaves == 0 && valores == 0;
        RegCloseKey(k);
        if (vacia) RegDeleteKeyW(HKEY_CURRENT_USER, ruta.c_str());
    }

    std::wstring LeerTexto(const std::wstring& ruta, const wchar_t* nombre)
    {
        wchar_t buf[2048] = {};
        DWORD tam = sizeof(buf) - sizeof(wchar_t);
        if (RegGetValueW(HKEY_CURRENT_USER, ruta.c_str(), nombre, RRF_RT_REG_SZ, nullptr, buf, &tam) != ERROR_SUCCESS) return L"";
        return buf;
    }
}

bool jicaro::menu::Registrar(const std::wstring& exe)
{
    bool ok = true;
    for (auto* ext : Extensiones)
    {
        for (auto& v : Verbos)
        {
            std::wstring ruta = std::wstring(Base) + ext + L"\\shell\\" + v.clave;
            HKEY k;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, ruta.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
            {
                ok = false;
                continue;
            }
            ok = PonerTexto(k, L"MUIVerb", v.texto) && ok;
            ok = PonerTexto(k, L"Icon", L"\"" + exe + L"\",0") && ok;
            RegCloseKey(k);
            HKEY c;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, (ruta + L"\\command").c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &c,
                                nullptr) != ERROR_SUCCESS)
            {
                ok = false;
                continue;
            }
            ok = PonerTexto(c, nullptr, Comando(exe, v.opciones)) && ok;
            RegCloseKey(c);
        }
    }
    if (!ok) Quitar();   // a medias no sirve: se quita lo que se alcanzó a poner
    return ok;
}

bool jicaro::menu::Quitar()
{
    bool ok = true;
    for (auto* ext : Extensiones)
    {
        std::wstring shell = std::wstring(Base) + ext + L"\\shell";
        for (auto& v : Verbos)
        {
            LSTATUS r = RegDeleteTreeW(HKEY_CURRENT_USER, (shell + L"\\" + v.clave).c_str());
            if (r == ERROR_SUCCESS) RegDeleteKeyW(HKEY_CURRENT_USER, (shell + L"\\" + v.clave).c_str());
            else if (r != ERROR_FILE_NOT_FOUND) ok = false;
        }
        BorrarSiVacia(shell);
        BorrarSiVacia(std::wstring(Base) + ext);
    }
    return ok && !EstaRegistrado();
}

bool jicaro::menu::EstaRegistrado()
{
    for (auto* ext : Extensiones)
        for (auto& v : Verbos)
        {
            HKEY k;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, (std::wstring(Base) + ext + L"\\shell\\" + v.clave).c_str(), 0, KEY_READ, &k)
                == ERROR_SUCCESS)
            {
                RegCloseKey(k);
                return true;
            }
        }
    return false;
}

bool jicaro::menu::ApuntaA(const std::wstring& exe)
{
    for (auto* ext : Extensiones)
        for (auto& v : Verbos)
            if (LeerTexto(std::wstring(Base) + ext + L"\\shell\\" + v.clave + L"\\command", nullptr) != Comando(exe, v.opciones))
                return false;
    return true;
}
