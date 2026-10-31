#include "marca.hpp"

#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <urlmon.h>

#include "comun.hpp"
#include "rutas.hpp"

namespace
{
    // Identifica a Jícaro ante los servicios de adjuntos de Windows
    const GUID GuidJicaro = { 0x4a1c7e52, 0x93b0, 0x4d6e, { 0xb8, 0xf1, 0x2c, 0x5d, 0x0e, 0x9a, 0x7b, 0x36 } };

    // Programas, scripts y accesos directos: con estos vale la pena avisar también al antivirus
    bool EsDeRiesgo(const std::wstring& ruta)
    {
        static const wchar_t* extensiones[] = {
            L".exe", L".com", L".scr", L".pif", L".bat", L".cmd", L".ps1", L".psm1", L".vbs", L".vbe", L".js", L".jse",
            L".wsf", L".wsh", L".hta", L".msi", L".msp", L".msix", L".msixbundle", L".appx", L".appxbundle", L".lnk",
            L".url", L".reg", L".dll", L".sys", L".cpl", L".jar", L".chm", L".inf", L".scf",
        };
        for (auto* e : extensiones)
            if (jicaro::TerminaSinMayusculas(ruta, e)) return true;
        return false;
    }

    // Escribe la zona con IZoneIdentifier (objeto «Persistent Zone Identifier»)
    bool PonerZona(const std::wstring& archivo, DWORD zona)
    {
        IZoneIdentifier* zi = nullptr;
        if (FAILED(CoCreateInstance(CLSID_PersistentZoneIdentifier, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&zi))))
            return false;
        bool ok = false;
        if (SUCCEEDED(zi->SetId(zona)))
        {
            IPersistFile* pf = nullptr;
            if (SUCCEEDED(zi->QueryInterface(IID_PPV_ARGS(&pf))))
            {
                ok = SUCCEEDED(pf->Save(jicaro::rutas::Larga(archivo).c_str(), TRUE));
                pf->Release();
            }
        }
        zi->Release();
        return ok;
    }
}

unsigned long jicaro::marca::Zona(const std::wstring& archivo)
{
    IPersistFile* pf = nullptr;
    if (FAILED(CoCreateInstance(CLSID_PersistentZoneIdentifier, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pf))))
        return 0;
    DWORD zona = 0;
    if (SUCCEEDED(pf->Load(rutas::Larga(archivo).c_str(), STGM_READ)))
    {
        IZoneIdentifier* zi = nullptr;
        if (SUCCEEDED(pf->QueryInterface(IID_PPV_ARGS(&zi))))
        {
            DWORD z = 0;
            HRESULT hr = zi->GetId(&z);
            if (SUCCEEDED(hr)) zona = z;
            else if (hr == E_ACCESSDENIED) zona = URLZONE_UNTRUSTED;   // así responde con la zona «no confiable»
            zi->Release();
        }
    }
    pf->Release();
    return zona;
}

bool jicaro::marca::HayQueMarcar(unsigned long zona)
{
    return zona == URLZONE_INTERNET || zona == URLZONE_UNTRUSTED;
}

unsigned long jicaro::marca::ZonaDelJuego(const std::vector<std::wstring>& partes, std::wstring& referente)
{
    unsigned long mejor = 0;
    referente.clear();
    for (auto& p : partes)
    {
        unsigned long z = Zona(p);
        if (HayQueMarcar(z) && (!HayQueMarcar(mejor) || z > mejor))
        {
            mejor = z;
            referente = p;
        }
    }
    return mejor;
}

bool jicaro::marca::Pasar(const std::wstring& extraido, const std::wstring& referente, unsigned long zona)
{
    std::wstring largo = rutas::Larga(extraido);

    // Escribir la marca puede cambiar la fecha de modificación: se guardan las fechas y se ponen de nuevo al final
    FILETIME creado{}, acceso{}, escrito{};
    bool conFechas = false;
    HANDLE h = CreateFileW(largo.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        conFechas = GetFileTime(h, &creado, &acceso, &escrito) != FALSE;
        CloseHandle(h);
    }

    bool ok = false, sigue = true;
    if (EsDeRiesgo(extraido))
    {
        // Programas y scripts: servicios de adjuntos, con el archivo comprimido como «referente» (así lo indica
        // Microsoft para los contenedores). Además avisa al antivirus, que puede revisar o quitar el archivo.
        IAttachmentExecute* ae = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_AttachmentServices, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ae))))
        {
            HRESULT hr = ae->SetClientGuid(GuidJicaro);
            if (SUCCEEDED(hr)) hr = ae->SetLocalPath(extraido.c_str());
            if (SUCCEEDED(hr)) hr = ae->SetReferrer(referente.c_str());
            if (SUCCEEDED(hr)) hr = ae->Save();
            ok = SUCCEEDED(hr);
            ae->Release();
        }
        sigue = ExisteArchivo(extraido);   // el antivirus pudo quitarlo
        if (sigue && (!ok || !HayQueMarcar(Zona(extraido)))) ok = PonerZona(extraido, zona);
    }
    else
    {
        // Lo demás (datos, imágenes, documentos): la misma marca, escrita directo, que es mucho más rápido
        ok = PonerZona(extraido, zona);
    }

    if (sigue && conFechas)
    {
        h = CreateFileW(largo.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            SetFileTime(h, &creado, &acceso, &escrito);
            CloseHandle(h);
        }
    }
    return ok && sigue;
}
