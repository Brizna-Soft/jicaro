// Pruebas de unidad de la comprobación de rutas y de la búsqueda de partes (sin ventana).
// Uso: prueba-rutas.exe   (devuelve la cantidad de fallas; 0 = todo bien)
//
// Solo escribe dentro de una carpeta de prueba nueva en %TEMP%, que borra al terminar.

#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <string>
#include <vector>

#include "comun.hpp"
#include "rutas.hpp"
#include "volumenes.hpp"

using namespace jicaro;
using rutas::Motivo;

namespace
{
    int hechas = 0, fallas = 0;

    void Revisar(bool ok, const std::wstring& que)
    {
        hechas++;
        if (!ok)
        {
            fallas++;
            wprintf(L"  FALLA: %ls\n", que.c_str());
        }
    }

    void Aceptada(const std::wstring& entrada, const std::wstring& esperada)
    {
        auto r = rutas::Limpiar(entrada);
        Revisar(r.ok && r.relativa == esperada,
                L"«" + entrada + L"» debía quedar «" + esperada + L"» y quedó «" + r.relativa + L"»" + (r.ok ? L"" : L" (rechazada)"));
    }

    void Rechazada(const std::wstring& entrada, Motivo motivo)
    {
        auto r = rutas::Limpiar(entrada);
        Revisar(!r.ok && r.motivo == motivo, L"«" + entrada + L"» debía rechazarse");
    }

    void CrearVacio(const std::wstring& ruta)
    {
        HANDLE h = CreateFileW(ruta.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }

    void BorrarCarpeta(const std::wstring& carpeta)
    {
        WIN32_FIND_DATAW d;
        HANDLE h = FindFirstFileW((carpeta + L"\\*").c_str(), &d);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                std::wstring n = d.cFileName;
                if (n == L"." || n == L"..") continue;
                std::wstring r = carpeta + L"\\" + n;
                if (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                {
                    if (!RemoveDirectoryW(r.c_str())) DeleteFileW(r.c_str());   // solo el enlace, no lo que hay detrás
                }
                else if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) BorrarCarpeta(r);
                else DeleteFileW(r.c_str());
            } while (FindNextFileW(h, &d));
            FindClose(h);
        }
        RemoveDirectoryW(carpeta.c_str());
    }

    void PruebasDeLimpiar()
    {
        wprintf(L"Limpiar rutas\n");
        Aceptada(L"a.txt", L"a.txt");
        Aceptada(L"carpeta/archivo.txt", L"carpeta\\archivo.txt");
        Aceptada(L"carpeta\\sub\\x.txt", L"carpeta\\sub\\x.txt");
        Aceptada(L"./a/./b.txt", L"a\\b.txt");
        Aceptada(L"a//b", L"a\\b");
        Aceptada(L"dir/", L"dir");
        Aceptada(L"ñandú/canción.mp3", L"ñandú\\canción.mp3");
        Aceptada(L"dir/a:b.txt", L"dir\\a_b.txt");
        Aceptada(L"archivo.txt:Zone.Identifier", L"archivo.txt_Zone.Identifier");
        Aceptada(L"x<>|?*\".txt", L"x______.txt");
        Aceptada(L"tab\there", L"tab_here");
        Aceptada(L"CON", L"_CON");
        Aceptada(L"con.txt", L"_con.txt");
        Aceptada(L"dir/NUL", L"dir\\_NUL");
        Aceptada(L"COM1.log", L"_COM1.log");
        Aceptada(L"lpt9", L"_lpt9");
        Aceptada(L"COM\u00B9", L"_COM\u00B9");
        Aceptada(L"CONOUT$", L"_CONOUT$");
        Aceptada(L"CONSOLE.txt", L"CONSOLE.txt");
        Aceptada(L"COM10", L"COM10");
        Aceptada(L"nombre. ", L"nombre");
        Aceptada(L"...", L"_");
        Aceptada(L". .", L"_");
        Aceptada(L"a/.../b", L"a\\_\\b");
        Aceptada(L"a/.. /b", L"a\\_\\b");
        Aceptada(L"barra\u2215falsa", L"barra_falsa");
        Aceptada(L"\uFF3Cx", L"_x");
        std::wstring largo(300, L'a');
        auto r = rutas::Limpiar(largo + L".txt");
        Revisar(r.ok && r.relativa.size() == 255 && r.relativa.ends_with(L".txt"), L"nombre de 304 letras recortado a 255");

        Rechazada(L"", Motivo::Vacia);
        Rechazada(L".", Motivo::Vacia);
        Rechazada(L"./", Motivo::Vacia);
        Rechazada(L"././", Motivo::Vacia);
        Rechazada(L"..", Motivo::SubeDeCarpeta);
        Rechazada(L"../x", Motivo::SubeDeCarpeta);
        Rechazada(L"a/../../x", Motivo::SubeDeCarpeta);
        Rechazada(L"a\\..\\b", Motivo::SubeDeCarpeta);
        Rechazada(L"a/..", Motivo::SubeDeCarpeta);
        Rechazada(L"..\\..\\Windows\\System32\\x.dll", Motivo::SubeDeCarpeta);
        Rechazada(L"/etc/passwd", Motivo::Absoluta);
        Rechazada(L"\\Windows\\x.dll", Motivo::Absoluta);
        Rechazada(L"C:\\Windows\\x.dll", Motivo::Absoluta);
        Rechazada(L"c:x.txt", Motivo::Absoluta);
        Rechazada(L"a:b.txt", Motivo::Absoluta);
        Rechazada(L"\\\\servidor\\compartida\\x", Motivo::Absoluta);
        Rechazada(L"\\\\?\\C:\\x", Motivo::Absoluta);
        Rechazada(L"//servidor/x", Motivo::Absoluta);
    }

    void PruebasDeDentroDe()
    {
        wprintf(L"Dentro de la carpeta\n");
        Revisar(rutas::DentroDe(L"C:\\destino", L"C:\\destino\\a.txt"), L"C:\\destino\\a.txt está dentro");
        Revisar(rutas::DentroDe(L"C:\\destino\\", L"C:\\DESTINO\\A.TXT"), L"sin importar mayúsculas");
        Revisar(!rutas::DentroDe(L"C:\\destino", L"C:\\destino"), L"la carpeta misma no cuenta como dentro");
        Revisar(!rutas::DentroDe(L"C:\\destino", L"C:\\destino2\\a.txt"), L"C:\\destino2 no está dentro de C:\\destino");
        Revisar(!rutas::DentroDe(L"C:\\destino", L"C:\\destino\\..\\fuera.txt"), L"«..» sale de la carpeta");
        Revisar(!rutas::DentroDe(L"C:\\destino", L"D:\\destino\\a.txt"), L"otra unidad");
        Revisar(rutas::DentroDe(L"D:\\", L"D:\\a.txt"), L"raíz de una unidad");
        Revisar(rutas::Unir(L"D:\\", L"a\\b") == L"D:\\a\\b", L"unir con la raíz");
    }

    void PruebasEnDisco(const std::wstring& base)
    {
        wprintf(L"Enlaces en la carpeta de destino\n");
        std::wstring destino = base + L"\\destino", fuera = base + L"\\fuera";
        CreateDirectoryW(destino.c_str(), nullptr);
        CreateDirectoryW(fuera.c_str(), nullptr);
        CreateDirectoryW((destino + L"\\normal").c_str(), nullptr);
        // Una unión (junction) que lleva fuera de la carpeta: se crea sin ser administrador
        std::wstring orden = L"cmd.exe /d /c mklink /J \"" + destino + L"\\enlace\" \"" + fuera + L"\" >nul";
        _wsystem(orden.c_str());
        bool hayUnion = (GetFileAttributesW((destino + L"\\enlace").c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        Revisar(hayUnion, L"se pudo crear la unión de prueba");
        std::wstring donde;
        Revisar(rutas::SinDesvios(destino, L"normal\\a.txt"), L"carpeta normal");
        Revisar(rutas::SinDesvios(destino, L"no-existe\\x\\y.txt"), L"carpetas que no existen");
        Revisar(!rutas::SinDesvios(destino, L"enlace\\a.txt", &donde) && donde.ends_with(L"\\enlace"), L"pasar por la unión");
        Revisar(!rutas::SinDesvios(destino, L"enlace"), L"la unión misma");
        // Enlace simbólico (solo si Windows lo deja crear sin ser administrador, con el modo de programador)
        if (CreateSymbolicLinkW((destino + L"\\simbolico").c_str(), fuera.c_str(),
                                SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
            Revisar(!rutas::SinDesvios(destino, L"simbolico\\a.txt"), L"pasar por un enlace simbólico");
        else
            wprintf(L"  (sin enlace simbólico: Windows no deja crearlo sin el modo de programador)\n");

        wprintf(L"Nombre libre\n");
        CrearVacio(destino + L"\\x.txt");
        Revisar(rutas::NombreLibre(destino + L"\\x.txt") == destino + L"\\x (2).txt", L"x (2).txt");
        CrearVacio(destino + L"\\x (2).txt");
        Revisar(rutas::NombreLibre(destino + L"\\x.txt") == destino + L"\\x (3).txt", L"x (3).txt");
        CrearVacio(destino + L"\\sin-extension");
        Revisar(rutas::NombreLibre(destino + L"\\sin-extension") == destino + L"\\sin-extension (2)", L"sin extensión");
    }

    void PruebasDePartes(const std::wstring& base)
    {
        wprintf(L"Partes\n");
        std::wstring c = base + L"\\partes";
        CreateDirectoryW(c.c_str(), nullptr);
        for (int i = 1; i <= 3; i++) CrearVacio(c + L"\\peli.part0" + std::to_wstring(i) + L".rar");
        auto j = volumenes::Buscar(c + L"\\peli.part02.rar");
        Revisar(j.tipo == volumenes::Tipo::RarNuevo && j.partes.size() == 3 && j.faltan.empty() && j.nombreBase == L"peli"
                    && j.partes[0].ends_with(L"peli.part01.rar") && j.partes[2].ends_with(L"peli.part03.rar"),
                L"part01..part03 desde la 2");

        for (int i = 1; i <= 5; i++)
            if (i != 3) CrearVacio(c + L"\\hueco.part" + std::to_wstring(i) + L".rar");
        j = volumenes::Buscar(c + L"\\hueco.part1.rar");
        Revisar(j.partes.size() == 4 && j.faltan.size() == 1 && j.faltan[0] == L"hueco.part3.rar", L"falta la parte 3");

        for (int i = 1; i <= 12; i++) CrearVacio(c + L"\\Mi Juego.part" + std::to_wstring(i) + L".rar");
        j = volumenes::Buscar(c + L"\\Mi Juego.part7.rar");
        Revisar(j.partes.size() == 12 && j.partes[1].ends_with(L".part2.rar") && j.partes[11].ends_with(L".part12.rar")
                    && j.nombreBase == L"Mi Juego",
                L"part1..part12 en orden de número");

        CrearVacio(c + L"\\viejo.rar");
        CrearVacio(c + L"\\viejo.r00");
        CrearVacio(c + L"\\viejo.r01");
        j = volumenes::Buscar(c + L"\\viejo.r01");
        Revisar(j.tipo == volumenes::Tipo::RarViejo && j.partes.size() == 3 && j.partes[0].ends_with(L"viejo.rar")
                    && j.partes[1].ends_with(L"viejo.r00") && j.partes[2].ends_with(L"viejo.r01"),
                L".rar + .r00 + .r01 desde la última");
        j = volumenes::Buscar(c + L"\\viejo.rar");
        Revisar(j.tipo == volumenes::Tipo::RarViejo && j.partes.size() == 3, L".rar + .r00 + .r01 desde la primera");

        CrearVacio(c + L"\\sinpartes.r02");
        j = volumenes::Buscar(c + L"\\sinpartes.r02");
        Revisar(j.tipo == volumenes::Tipo::RarViejo && j.faltan.size() == 3 && j.faltan[0] == L"sinpartes.rar",
                L"faltan .rar, .r00 y .r01");

        CrearVacio(c + L"\\solo.rar");
        j = volumenes::Buscar(c + L"\\solo.rar");
        Revisar(j.tipo == volumenes::Tipo::Uno && j.partes.size() == 1 && j.nombreBase == L"solo", L"un solo .rar");

        CrearVacio(c + L"\\fotos.z01");
        CrearVacio(c + L"\\fotos.z02");
        CrearVacio(c + L"\\fotos.zip");
        j = volumenes::Buscar(c + L"\\fotos.zip");
        Revisar(j.tipo == volumenes::Tipo::ZipPartes && j.partes.size() == 3 && j.partes[0].ends_with(L"fotos.z01")
                    && j.partes[2].ends_with(L"fotos.zip") && j.faltan.empty() && j.seguidas == 3 && j.nombreBase == L"fotos",
                L"ZIP en partes: .z01 + .z02 + .zip desde el .zip");
        j = volumenes::Buscar(c + L"\\fotos.z02");
        Revisar(j.tipo == volumenes::Tipo::ZipPartes && j.partes.size() == 3, L"ZIP en partes desde la .z02");
        CrearVacio(c + L"\\cortado.z01");
        CrearVacio(c + L"\\cortado.z03");
        j = volumenes::Buscar(c + L"\\cortado.z03");
        Revisar(j.tipo == volumenes::Tipo::ZipPartes && j.faltan.size() == 2 && j.faltan[0] == L"cortado.z02" && j.faltan[1] == L"cortado.zip"
                    && j.seguidas == 0,
                L"ZIP en partes: faltan cortado.z02 y cortado.zip");
        CrearVacio(c + L"\\solo.zip");
        j = volumenes::Buscar(c + L"\\solo.zip");
        Revisar(j.tipo == volumenes::Tipo::Uno && j.partes.size() == 1, L"un ZIP solo no es un ZIP en partes");

        CrearVacio(c + L"\\datos.7z.001");
        CrearVacio(c + L"\\datos.7z.002");
        j = volumenes::Buscar(c + L"\\datos.7z.002");
        Revisar(j.tipo == volumenes::Tipo::Numerado && j.partes.size() == 2 && j.nombreBase == L"datos", L".7z.001 y .002");

        CrearVacio(c + L"\\foto.tar.gz");
        j = volumenes::Buscar(c + L"\\foto.tar.gz");
        Revisar(j.tipo == volumenes::Tipo::Uno && j.nombreBase == L"foto" && j.nombreSuelto == L"foto.tar", L"foto.tar.gz");
        CrearVacio(c + L"\\log.txt.gz");
        j = volumenes::Buscar(c + L"\\log.txt.gz");
        Revisar(j.nombreSuelto == L"log.txt", L"log.txt.gz se llama log.txt al extraer");

        Revisar(volumenes::NombreSinExtension(L"juego.tar.zst") == L"juego", L"juego.tar.zst");
        Revisar(volumenes::NombreSinExtension(L"Disco.ISO") == L"Disco", L"Disco.ISO");
        Revisar(volumenes::NombreSinExtension(L"archivo") == L"archivo (extraído)", L"sin extensión");
        Revisar(volumenes::NombreSinExtension(L"raro..zip") == L"raro", L"puntos al final");
    }
}

int wmain()
{
    _setmode(_fileno(stdout), _O_U8TEXT);
    wchar_t temporal[MAX_PATH];
    GetTempPathW(MAX_PATH, temporal);
    std::wstring base = std::wstring(temporal) + L"jicaro-prueba-rutas-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(base.c_str(), nullptr);

    PruebasDeLimpiar();
    PruebasDeDentroDe();
    PruebasEnDisco(base);
    PruebasDePartes(base);

    BorrarCarpeta(base);
    wprintf(L"\n%d pruebas, %d fallas\n", hechas, fallas);
    return fallas;
}
