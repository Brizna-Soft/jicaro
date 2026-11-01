#include "util.hpp"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>
#include <mutex>

#include "comun.hpp"

std::wstring jicaro::CarpetaDatos()
{
    // JICARO_CARPETA_DATOS (una ruta completa) cambia dónde se guardan los ajustes y el registro: sirve para probar una
    // copia de Jícaro sin tocar los ajustes de la que está instalada
    wchar_t propia[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"JICARO_CARPETA_DATOS", propia, MAX_PATH);
    if (n > 3 && n < MAX_PATH && propia[1] == L':' && propia[2] == L'\\')
    {
        std::wstring r(propia, n);
        CreateDirectoryW(r.c_str(), nullptr);
        return r;
    }
    PWSTR ruta = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &ruta))) r = ruta;
    CoTaskMemFree(ruta);
    r += L"\\Jicaro";
    CreateDirectoryW(r.c_str(), nullptr);
    return r;
}

bool jicaro::EscribirSeguro(const std::wstring& ruta, const std::string& contenido)
{
    std::wstring temporal = ruta + L".nuevo";
    HANDLE h = CreateFileW(temporal.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD escritos = 0;
    BOOL ok = WriteFile(h, contenido.data(), (DWORD)contenido.size(), &escritos, nullptr) && escritos == contenido.size();
    ok = ok && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok)
    {
        DeleteFileW(temporal.c_str());
        return false;
    }
    return MoveFileExW(temporal.c_str(), ruta.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool jicaro::LeerArchivo(const std::wstring& ruta, std::string& contenido)
{
    HANDLE h = CreateFileW(ruta.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER t;
    bool ok = GetFileSizeEx(h, &t) && t.QuadPart < 16LL * 1024 * 1024;
    if (ok)
    {
        contenido.resize((size_t)t.QuadPart);
        DWORD leidos = 0;
        ok = contenido.empty() || (ReadFile(h, contenido.data(), (DWORD)contenido.size(), &leidos, nullptr) && leidos == contenido.size());
    }
    CloseHandle(h);
    return ok;
}

void jicaro::Registrar(const std::string& texto)
{
    static std::mutex m;
    std::lock_guard g(m);
    std::wstring ruta = CarpetaDatos() + L"\\registro.txt";
    if (TamanoArchivo(ruta) > 256 * 1024) MoveFileExW(ruta.c_str(), (ruta + L".anterior").c_str(), MOVEFILE_REPLACE_EXISTING);
    HANDLE h = CreateFileW(ruta.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char hora[32];
    snprintf(hora, sizeof(hora), "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::string linea = hora + texto + "\r\n";
    DWORD escritos = 0;
    WriteFile(h, linea.data(), (DWORD)linea.size(), &escritos, nullptr);
    CloseHandle(h);
}

void jicaro::AbrirCarpeta(const std::wstring& carpeta)
{
    if (!ExisteCarpeta(carpeta)) return;
    ShellExecuteW(nullptr, L"open", carpeta.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
