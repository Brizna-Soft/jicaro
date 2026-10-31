// Pruebas del núcleo de Jícaro con archivos de verdad (sin ventana).
//
// Uso: prueba-nucleo.exe <carpeta con los .uu de la suite de pruebas de libarchive> <carpeta de trabajo nueva>
//
// 1. Saca de la suite de libarchive (licencia BSD) los RAR, RAR 5 en varias partes, 7z, ZIP con contraseña, CAB e ISO.
// 2. Arma archivos propios con libarchive: ZIP, TAR (también .gz, .bz2, .xz y .zst), 7z, ISO, ZIP con contraseña y
//    archivos malos: con «../», con rutas absolutas, con enlaces, con nombres raros y una bomba chica.
// 3. Los extrae con el mismo código del programa y revisa el resultado, y que nada salga de la carpeta de destino.
//
// Solo escribe dentro de la carpeta de trabajo. Devuelve la cantidad de fallas (0 = todo bien).

#include <windows.h>
#include <objbase.h>

#include <fcntl.h>
#include <io.h>

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "comun.hpp"
#include "extractor.hpp"
#include "marca.hpp"
#include "rutas.hpp"
#include "volumenes.hpp"

using namespace jicaro;

namespace
{
    int hechas = 0, fallas = 0;
    std::wstring suite, trabajo;

    void Revisar(bool ok, const std::wstring& que)
    {
        hechas++;
        if (!ok) fallas++;
        wprintf(L"  %ls %ls\n", ok ? L"bien " : L"FALLA", que.c_str());
    }

    std::wstring Carpeta(const std::wstring& nombre)
    {
        std::wstring c = trabajo + L"\\" + nombre;
        CrearCarpetas(c);
        return c;
    }

    bool LeerTodo(const std::wstring& ruta, std::string& datos)
    {
        HANDLE h = CreateFileW(ruta.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER t;
        GetFileSizeEx(h, &t);
        datos.resize((size_t)t.QuadPart);
        DWORD leidos = 0;
        bool ok = datos.empty() || (ReadFile(h, datos.data(), (DWORD)datos.size(), &leidos, nullptr) && leidos == datos.size());
        CloseHandle(h);
        return ok;
    }

    bool EscribirTodo(const std::wstring& ruta, const std::string& datos)
    {
        HANDLE h = CreateFileW(ruta.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD escritos = 0;
        bool ok = datos.empty() || WriteFile(h, datos.data(), (DWORD)datos.size(), &escritos, nullptr);
        CloseHandle(h);
        return ok;
    }

    int ContarArchivos(const std::wstring& carpeta)
    {
        int n = 0;
        WIN32_FIND_DATAW d;
        HANDLE h = FindFirstFileW((carpeta + L"\\*").c_str(), &d);
        if (h == INVALID_HANDLE_VALUE) return 0;
        do
        {
            std::wstring nombre = d.cFileName;
            if (nombre == L"." || nombre == L"..") continue;
            if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (!(d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) n += ContarArchivos(carpeta + L"\\" + nombre);
            }
            else n++;
        } while (FindNextFileW(h, &d));
        FindClose(h);
        return n;
    }

    // ¿Hay algún archivo que empiece con «fuera» en la carpeta de trabajo (o en la raíz de la unidad)?
    bool HayAlgoFuera(const std::wstring& carpeta)
    {
        WIN32_FIND_DATAW d;
        HANDLE h = FindFirstFileW((carpeta + L"\\*").c_str(), &d);
        if (h == INVALID_HANDLE_VALUE) return false;
        bool hay = false;
        do
        {
            std::wstring nombre = d.cFileName;
            if (nombre == L"." || nombre == L"..") continue;
            if (EmpiezaSinMayusculas(nombre, L"fuera") && !(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) hay = true;
            else if ((d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                     && nombre != L"salida-libarchive-sola")
                hay = hay || HayAlgoFuera(carpeta + L"\\" + nombre);
        } while (FindNextFileW(h, &d) && !hay);
        FindClose(h);
        return hay;
    }

    // ---------- Sacar los archivos de la suite de libarchive (vienen en uuencode, algunos además en .Z) ----------
    // uudecode propio: el filtro «uu» de libarchive no acepta permisos de 4 cifras («begin 0744»), que usan varios
    // archivos de su suite
    std::string UuDecodificar(const std::string& t)
    {
        std::string out;
        size_t i = t.find("begin ");
        if (i == std::string::npos) return out;
        i = t.find('\n', i);
        if (i == std::string::npos) return out;
        i++;
        while (i < t.size())
        {
            size_t fin = t.find('\n', i);
            if (fin == std::string::npos) fin = t.size();
            std::string linea = t.substr(i, fin - i);
            i = fin + 1;
            if (!linea.empty() && linea.back() == '\r') linea.pop_back();
            if (linea.rfind("end", 0) == 0) break;
            if (linea.empty()) continue;
            int n = (linea[0] - 32) & 63;
            auto c = [&linea](size_t k) -> int { return k < linea.size() ? ((linea[k] - 32) & 63) : 0; };
            for (size_t k = 1; n > 0; k += 4)
            {
                int a = c(k), b = c(k + 1), cc = c(k + 2), d = c(k + 3);
                out.push_back((char)((a << 2) | (b >> 4)));
                if (--n == 0) break;
                out.push_back((char)((b << 4) | (cc >> 2)));
                if (--n == 0) break;
                out.push_back((char)((cc << 6) | d));
                --n;
            }
        }
        return out;
    }

    bool Decodificar(const std::wstring& uu, const std::wstring& salida)
    {
        std::string texto;
        if (!LeerTodo(uu, texto)) return false;
        std::string datos = UuDecodificar(texto);
        if (datos.empty()) return false;
        if (TerminaSinMayusculas(uu, L".Z.uu"))
        {
            // Además viene con «compress» (.Z)
            archive* a = archive_read_new();
            archive_read_support_filter_compress(a);
            archive_read_support_format_raw(a);
            std::string sin;
            archive_entry* e = nullptr;
            if (archive_read_open_memory(a, datos.data(), datos.size()) == ARCHIVE_OK && archive_read_next_header(a, &e) == ARCHIVE_OK)
            {
                char buf[1 << 16];
                la_ssize_t n;
                while ((n = archive_read_data(a, buf, sizeof(buf))) > 0) sin.append(buf, (size_t)n);
            }
            archive_read_free(a);
            datos.swap(sin);
        }
        return !datos.empty() && EscribirTodo(salida, datos);
    }

    std::wstring DeLaSuite(const std::wstring& nombreUu, const std::wstring& nombreFinal)
    {
        std::wstring destino = Carpeta(L"libarchive") + L"\\" + nombreFinal;
        if (!ExisteArchivo(destino) && !Decodificar(suite + L"\\" + nombreUu, destino))
            Revisar(false, L"no se pudo sacar " + nombreUu + L" de la suite de libarchive");
        return destino;
    }

    // ---------- Armar archivos propios con libarchive ----------
    struct Item
    {
        std::wstring ruta;
        std::string datos;
        enum Tipo { Archivo, Carpeta, Simbolico, Duro } tipo = Archivo;
        std::wstring enlace;
        int64_t ceros = 0;   // datos de ceros (para la bomba)
    };

    bool Armar(const std::wstring& archivo, const std::vector<Item>& items, const std::function<void(archive*)>& configurar)
    {
        archive* a = archive_write_new();
        configurar(a);
        if (archive_write_open_filename_w(a, archivo.c_str()) != ARCHIVE_OK)
        {
            archive_write_free(a);
            return false;
        }
        static const std::string ceros(1 << 20, '\0');
        for (auto& it : items)
        {
            archive_entry* e = archive_entry_new();
            archive_entry_copy_pathname_w(e, it.ruta.c_str());
            archive_entry_set_mtime(e, 1700000000, 0);
            switch (it.tipo)
            {
            case Item::Carpeta:
                archive_entry_set_filetype(e, AE_IFDIR);
                archive_entry_set_perm(e, 0755);
                break;
            case Item::Simbolico:
                archive_entry_set_filetype(e, AE_IFLNK);
                archive_entry_set_perm(e, 0777);
                archive_entry_copy_symlink_w(e, it.enlace.c_str());
                break;
            case Item::Duro:
                archive_entry_set_filetype(e, AE_IFREG);
                archive_entry_set_perm(e, 0644);
                archive_entry_copy_hardlink_w(e, it.enlace.c_str());
                archive_entry_set_size(e, 0);
                break;
            default:
                archive_entry_set_filetype(e, AE_IFREG);
                archive_entry_set_perm(e, 0644);
                archive_entry_set_size(e, it.ceros ? it.ceros : (la_int64_t)it.datos.size());
            }
            archive_write_header(a, e);
            if (it.tipo == Item::Archivo)
            {
                if (it.ceros)
                    for (int64_t falta = it.ceros; falta > 0; falta -= (int64_t)ceros.size())
                        archive_write_data(a, ceros.data(), (size_t)std::min<int64_t>(falta, (int64_t)ceros.size()));
                else if (!it.datos.empty())
                    archive_write_data(a, it.datos.data(), it.datos.size());
            }
            archive_entry_free(e);
        }
        bool ok = archive_write_close(a) == ARCHIVE_OK;
        archive_write_free(a);
        return ok;
    }

    std::string DatosRepetibles(size_t n)
    {
        std::string s(n, '\0');
        uint32_t x = 12345;
        for (auto& c : s)
        {
            x = x * 1103515245 + 12345;
            c = (char)(x >> 16);
        }
        return s;
    }

    // El contenido de prueba común: lo que debe salir de propio.zip, propio.tar, propio.7z...
    std::map<std::wstring, std::string> Esperado()
    {
        return {
            { L"hola.txt", "Hola desde Jícaro.\r\n" },
            { L"carpeta\\ñandú y canción.txt", "Nombre con eñe y tildes.\r\n" },
            { L"carpeta\\sub\\vacío.txt", "" },
            { L"datos.bin", DatosRepetibles(300000) },
        };
    }

    std::vector<Item> ItemsComunes()
    {
        std::vector<Item> v;
        v.push_back({ L"carpeta", "", Item::Carpeta });
        v.push_back({ L"carpeta/sub", "", Item::Carpeta });
        for (auto& [ruta, datos] : Esperado())
        {
            std::wstring r = ruta;
            for (auto& c : r)
                if (c == L'\\') c = L'/';
            v.push_back({ r, datos });
        }
        return v;
    }

    bool Coincide(const std::wstring& carpeta, const std::map<std::wstring, std::string>& esperado, std::wstring& que)
    {
        for (auto& [ruta, datos] : esperado)
        {
            std::string leido;
            if (!LeerTodo(carpeta + L"\\" + ruta, leido))
            {
                que = L"falta " + ruta;
                return false;
            }
            if (leido != datos)
            {
                que = L"el contenido de " + ruta + L" no coincide";
                return false;
            }
        }
        return true;
    }

    // ---------- Extraer con el código del programa ----------
    struct Prueba
    {
        Opciones op;
        Preguntas pr;
        Progreso progreso;
        std::atomic<bool> cancelar{ false };
        int pidioContrasena = 0, pidioExiste = 0, pidioBomba = 0;
    };

    Resumen Extraer(Prueba& p, const std::wstring& archivo, const std::wstring& destino)
    {
        auto juego = volumenes::Buscar(archivo);
        p.op.partes = juego.partes;
        p.op.nombreSuelto = juego.nombreSuelto;
        p.op.destino = destino;
        return jicaro::Extraer(p.op, p.pr, p.progreso, p.cancelar);
    }

    Resumen ExtraerSimple(const std::wstring& archivo, const std::wstring& destino)
    {
        Prueba p;
        return Extraer(p, archivo, destino);
    }

    std::wstring Texto(const Resumen& r)
    {
        std::wstring t = L"[" + TextoEstado(r.estado) + L" " + r.mensaje + L" | " + std::to_wstring(r.archivos) + L" archivos, " +
                         std::to_wstring(r.omitidas.size()) + L" omitidos";
        for (size_t i = 0; i < r.omitidas.size() && i < 8; i++) t += L"\n        · " + r.omitidas[i].ruta + L": " + r.omitidas[i].motivo;
        return t + L"]";
    }

    bool Omitio(const Resumen& r, const std::wstring& ruta)
    {
        for (auto& o : r.omitidas)
            if (o.ruta == ruta) return true;
        return false;
    }

    // ================================================================================================

    void PruebasPropias()
    {
        wprintf(L"\nArchivos propios (ZIP, TAR, 7z, ISO y comprimidos sueltos)\n");
        std::wstring c = Carpeta(L"propios");
        auto items = ItemsComunes();
        auto zipUtf8 = [](archive* a) {
            archive_write_set_format_zip(a);
            archive_write_set_options(a, "zip:hdrcharset=UTF-8");
        };
        Revisar(Armar(c + L"\\propio.zip", items, zipUtf8), L"armar propio.zip");
        Revisar(Armar(c + L"\\propio.tar", items, [](archive* a) { archive_write_set_format_pax_restricted(a); }), L"armar propio.tar");
        struct { const wchar_t* ext; int (*filtro)(archive*); } filtros[] = {
            { L".tar.gz", archive_write_add_filter_gzip }, { L".tar.bz2", archive_write_add_filter_bzip2 },
            { L".tar.xz", archive_write_add_filter_xz }, { L".tar.zst", archive_write_add_filter_zstd },
        };
        for (auto& f : filtros)
            Revisar(Armar(c + L"\\propio" + f.ext, items, [&](archive* a) {
                        archive_write_set_format_pax_restricted(a);
                        f.filtro(a);
                    }),
                    std::wstring(L"armar propio") + f.ext);
        Revisar(Armar(c + L"\\propio.7z", items, [](archive* a) { archive_write_set_format_7zip(a); }), L"armar propio.7z");
        Revisar(Armar(c + L"\\propio.iso", items, [](archive* a) {
                    archive_write_set_format_iso9660(a);
                    archive_write_set_options(a, "iso9660:joliet,iso9660:rockridge");
                }),
                L"armar propio.iso");

        for (const wchar_t* ext : { L".zip", L".tar", L".tar.gz", L".tar.bz2", L".tar.xz", L".tar.zst", L".7z", L".iso" })
        {
            std::wstring destino = Carpeta(std::wstring(L"salida\\propio") + ext);
            auto r = ExtraerSimple(c + L"\\propio" + ext, destino);
            std::wstring que;
            bool ok = r.estado == jicaro::Estado::Bien && Coincide(destino, Esperado(), que);
            Revisar(ok, std::wstring(L"propio") + ext + L" " + (ok ? L"" : Texto(r) + L" " + que));
        }

        // Un solo archivo comprimido: sale con su nombre sin la extensión
        std::vector<Item> solo = { { L"data", "Un solo archivo comprimido.\r\n" } };
        struct { const wchar_t* ext; int (*filtro)(archive*); } sueltos[] = {
            { L".gz", archive_write_add_filter_gzip }, { L".bz2", archive_write_add_filter_bzip2 },
            { L".xz", archive_write_add_filter_xz }, { L".zst", archive_write_add_filter_zstd },
        };
        for (auto& f : sueltos)
        {
            std::wstring archivo = c + L"\\nota.txt" + f.ext;
            Armar(archivo, solo, [&](archive* a) {
                archive_write_set_format_raw(a);
                f.filtro(a);
            });
            std::wstring destino = Carpeta(std::wstring(L"salida\\suelto") + f.ext);
            auto r = ExtraerSimple(archivo, destino);
            std::string leido;
            bool ok = r.estado == jicaro::Estado::Bien && LeerTodo(destino + L"\\nota.txt", leido) && leido == solo[0].datos;
            Revisar(ok, std::wstring(L"nota.txt") + f.ext + L" sale como nota.txt " + (ok ? L"" : Texto(r)));
        }

        // Solo lo elegido
        {
            Prueba p;
            p.op.elegidas = { L"carpeta" };
            std::wstring destino = Carpeta(L"salida\\elegidos");
            auto r = Extraer(p, c + L"\\propio.zip", destino);
            Revisar(r.estado == jicaro::Estado::Bien && ExisteArchivo(destino + L"\\carpeta\\ñandú y canción.txt")
                        && !ExisteArchivo(destino + L"\\hola.txt"),
                    L"extraer solo la carpeta elegida " + Texto(r));
        }

        // Listar
        {
            Progreso pr;
            std::atomic<bool> no{ false };
            auto lista = Listar({ c + L"\\propio.zip" }, L"", pr, no);
            bool tiene = false;
            for (auto& e : lista.entradas)
                if (e.ruta == L"carpeta\\ñandú y canción.txt") tiene = true;
            Revisar(lista.estado == jicaro::Estado::Bien && lista.completo && lista.formato == L"ZIP" && tiene && lista.entradas.size() == 6,
                    L"listar propio.zip (" + std::to_wstring(lista.entradas.size()) + L" entradas, " + lista.formato + L")");
            auto lista2 = Listar({ c + L"\\propio.tar.xz" }, L"", pr, no);
            Revisar(lista2.formato == L"TAR + xz", L"formato de propio.tar.xz: " + lista2.formato);
        }

        // Trozos .7z.001, .002, .003
        {
            std::string todo;
            LeerTodo(c + L"\\propio.7z", todo);
            size_t tercio = todo.size() / 3 + 1;
            std::wstring t = Carpeta(L"trozos");
            for (int i = 0; i < 3; i++)
                EscribirTodo(t + L"\\trozos.7z.00" + std::to_wstring(i + 1), todo.substr(i * tercio, tercio));
            std::wstring destino = Carpeta(L"salida\\trozos");
            auto r = ExtraerSimple(t + L"\\trozos.7z.002", destino);
            std::wstring que;
            bool ok = r.estado == jicaro::Estado::Bien && Coincide(destino, Esperado(), que);
            Revisar(ok, L".7z cortado en .001, .002 y .003 " + (ok ? L"" : Texto(r) + L" " + que));
        }

        // No es un archivo comprimido; y uno cortado a la mitad
        {
            EscribirTodo(c + L"\\no-soy.zip", "Esto es solo texto, no un ZIP.\r\n");
            auto r = ExtraerSimple(c + L"\\no-soy.zip", Carpeta(L"salida\\no-soy"));
            Revisar(r.estado == jicaro::Estado::NoEsArchivo, L"texto con nombre .zip " + Texto(r));
            std::string todo;
            LeerTodo(c + L"\\propio.tar.gz", todo);
            EscribirTodo(c + L"\\cortado.tar.gz", todo.substr(0, todo.size() / 2));
            r = ExtraerSimple(c + L"\\cortado.tar.gz", Carpeta(L"salida\\cortado"));
            Revisar(r.estado == jicaro::Estado::Error && r.mensaje.find(L"incompleto") != std::wstring::npos,
                    L".tar.gz cortado a la mitad avisa que está incompleto " + Texto(r));
        }
    }

    void PruebasDeContrasena()
    {
        wprintf(L"\nZIP con contraseña\n");
        std::wstring c = Carpeta(L"propios");
        auto items = ItemsComunes();
        Revisar(Armar(c + L"\\clave-zipcrypto.zip", items, [](archive* a) {
                    archive_write_set_format_zip(a);
                    archive_write_set_options(a, "zip:hdrcharset=UTF-8,zip:encryption=zipcrypt");
                    archive_write_set_passphrase(a, Utf8(L"clave ñ").c_str());
                }),
                L"armar un ZIP con ZipCrypto (clave con eñe)");
        Revisar(Armar(c + L"\\clave-aes.zip", items, [](archive* a) {
                    archive_write_set_format_zip(a);
                    archive_write_set_options(a, "zip:hdrcharset=UTF-8,zip:encryption=aes256");
                    archive_write_set_passphrase(a, "secreta");
                }),
                L"armar un ZIP con AES-256");

        // Primero una contraseña mala y después la buena
        auto conClaves = [](std::vector<std::wstring> claves, Prueba& p) {
            p.pr.contrasena = [&p, claves](const std::wstring&, bool) -> std::optional<std::wstring> {
                if (p.pidioContrasena >= (int)claves.size()) return std::nullopt;
                return claves[p.pidioContrasena++];
            };
        };
        {
            Prueba p;
            conClaves({ L"mala", L"clave ñ" }, p);
            std::wstring destino = Carpeta(L"salida\\zipcrypto");
            auto r = Extraer(p, c + L"\\clave-zipcrypto.zip", destino);
            std::wstring que;
            bool ok = r.estado == jicaro::Estado::Bien && Coincide(destino, Esperado(), que) && p.pidioContrasena == 2;
            Revisar(ok, L"ZipCrypto: pide otra vez tras la mala y extrae con la buena " + Texto(r) + L" " + que);
        }
        {
            Prueba p;
            conClaves({ L"secreta" }, p);
            std::wstring destino = Carpeta(L"salida\\aes");
            auto r = Extraer(p, c + L"\\clave-aes.zip", destino);
            std::wstring que;
            bool ok = r.estado == jicaro::Estado::Bien && Coincide(destino, Esperado(), que) && p.pidioContrasena == 1;
            Revisar(ok, L"AES-256 (propio) " + Texto(r) + L" " + que);
        }
        {
            Prueba p;   // la persona cancela
            p.pr.contrasena = [](const std::wstring&, bool) -> std::optional<std::wstring> { return std::nullopt; };
            auto r = Extraer(p, c + L"\\clave-aes.zip", Carpeta(L"salida\\aes-cancelada"));
            Revisar(r.estado == jicaro::Estado::SinContrasena, L"sin contraseña se detiene " + Texto(r));
        }
        // Los de la suite de libarchive
        struct { const wchar_t* uu; const wchar_t* nombre; const wchar_t* clave; } suiteZip[] = {
            { L"test_read_format_zip_traditional_encryption_data.zip.uu", L"zipcrypto-libarchive.zip", L"12345678" },
            { L"test_read_format_zip_winzip_aes128.zip.uu", L"aes128-libarchive.zip", L"password" },
            { L"test_read_format_zip_winzip_aes256.zip.uu", L"aes256-libarchive.zip", L"password" },
            { L"test_read_format_zip_winzip_aes256_large.zip.uu", L"aes256-grande-libarchive.zip", L"password" },
        };
        for (auto& z : suiteZip)
        {
            std::wstring archivo = DeLaSuite(z.uu, z.nombre);
            Prueba p;
            std::wstring clave = z.clave;
            p.pr.contrasena = [&p, clave](const std::wstring&, bool) -> std::optional<std::wstring> {
                p.pidioContrasena++;
                return clave;
            };
            std::wstring destino = Carpeta(std::wstring(L"salida\\") + z.nombre);
            auto r = Extraer(p, archivo, destino);
            Revisar(r.estado == jicaro::Estado::Bien && r.archivos > 0 && r.omitidas.empty(),
                    std::wstring(z.nombre) + L" " + Texto(r));
        }
    }

    void PruebasRarY7z()
    {
        wprintf(L"\nRAR, RAR 5, 7z, CAB e ISO de la suite de libarchive\n");
        struct { const wchar_t* uu; const wchar_t* nombre; } simples[] = {
            { L"test_read_format_rar.rar.uu", L"rar4.rar" },
            { L"test_read_format_rar_compress_best.rar.uu", L"rar4-mejor.rar" },
            { L"test_read_format_rar_unicode.rar.uu", L"rar4-unicode.rar" },
            { L"test_read_format_rar5_compressed.rar.uu", L"rar5.rar" },
            { L"test_read_format_rar5_solid.rar.uu", L"rar5-solido.rar" },
            { L"test_read_format_rar5_stored.rar.uu", L"rar5-guardado.rar" },
            { L"test_read_format_rar5_unicode.rar.uu", L"rar5-unicode.rar" },
            { L"test_read_format_7zip_lzma2.7z.uu", L"lzma2.7z" },
            { L"test_read_format_7zip_bcj2_lzma2_1.7z.uu", L"bcj2.7z" },
            { L"test_read_format_7zip_ppmd.7z.uu", L"ppmd.7z" },
            { L"test_read_format_7zip_zstd.7z.uu", L"zstd.7z" },
            { L"test_read_format_7zip_deflate.7z.uu", L"deflate.7z" },
            { L"test_read_format_cab_1.cab.uu", L"suite-1.cab" },
            { L"test_read_format_cab_3.cab.uu", L"suite-3.cab" },
            { L"test_read_format_cab_lzx_e8.cab.uu", L"lzx.cab" },
            { L"test_read_format_iso_joliet.iso.Z.uu", L"joliet.iso" },
            { L"test_read_format_iso_zisofs.iso.Z.uu", L"zisofs.iso" },
        };
        for (auto& s : simples)
        {
            std::wstring archivo = DeLaSuite(s.uu, s.nombre);
            std::wstring destino = Carpeta(std::wstring(L"salida\\") + s.nombre);
            auto r = ExtraerSimple(archivo, destino);
            Revisar(r.estado == jicaro::Estado::Bien && r.archivos > 0 && ContarArchivos(destino) == r.archivos,
                    std::wstring(s.nombre) + L" " + Texto(r));
        }

        // RAR 4 en varias partes (.part0001.rar ... .part0004.rar)
        std::vector<std::wstring> rar4;
        for (int i = 1; i <= 4; i++)
            rar4.push_back(DeLaSuite(L"test_read_format_rar_multivolume.part000" + std::to_wstring(i) + L".rar.uu",
                                     L"multi.part000" + std::to_wstring(i) + L".rar"));
        auto juego = volumenes::Buscar(rar4[2]);
        Revisar(juego.partes.size() == 4 && juego.faltan.empty(), L"RAR 4: desde la parte 3 encuentra las 4 partes");
        std::wstring dNuevo = Carpeta(L"salida\\rar4-partes");
        auto r = ExtraerSimple(rar4[2], dNuevo);
        Revisar(r.estado == jicaro::Estado::Bien && r.archivos > 0, L"RAR 4 en 4 partes " + Texto(r));

        // Las mismas partes con el estilo viejo: .rar, .r00, .r01, .r02
        std::wstring v = Carpeta(L"viejo");
        const wchar_t* viejas[] = { L"viejo.rar", L"viejo.r00", L"viejo.r01", L"viejo.r02" };
        for (int i = 0; i < 4; i++) CopyFileW(rar4[i].c_str(), (v + L"\\" + viejas[i]).c_str(), FALSE);
        juego = volumenes::Buscar(v + L"\\viejo.r01");
        Revisar(juego.tipo == volumenes::Tipo::RarViejo && juego.partes.size() == 4, L"estilo viejo: .rar + .r00 + .r01 + .r02");
        std::wstring dViejo = Carpeta(L"salida\\rar4-viejo");
        auto r2 = ExtraerSimple(v + L"\\viejo.r01", dViejo);
        Revisar(r2.estado == jicaro::Estado::Bien && r2.archivos == r.archivos && ContarArchivos(dViejo) == ContarArchivos(dNuevo),
                L"RAR 4 con .r00 sale igual que con .part " + Texto(r2));

        // RAR 5 en 8 partes, y en 4 partes «sólido»
        std::vector<std::wstring> rar5;
        for (int i = 1; i <= 8; i++)
            rar5.push_back(DeLaSuite(L"test_read_format_rar5_multiarchive.part0" + std::to_wstring(i) + L".rar.uu",
                                     L"rar5multi.part0" + std::to_wstring(i) + L".rar"));
        r = ExtraerSimple(rar5[4], Carpeta(L"salida\\rar5-partes"));
        Revisar(r.estado == jicaro::Estado::Bien && r.archivos > 0, L"RAR 5 en 8 partes (abierto desde la 5) " + Texto(r));
        std::vector<std::wstring> solido;
        for (int i = 1; i <= 4; i++)
            solido.push_back(DeLaSuite(L"test_read_format_rar5_multiarchive_solid.part0" + std::to_wstring(i) + L".rar.uu",
                                       L"rar5solido.part0" + std::to_wstring(i) + L".rar"));
        r = ExtraerSimple(solido[0], Carpeta(L"salida\\rar5-solido-partes"));
        Revisar(r.estado == jicaro::Estado::Bien && r.archivos > 0, L"RAR 5 sólido en 4 partes " + Texto(r));

        // Listar el RAR 5 en partes
        {
            Progreso pr;
            std::atomic<bool> no{ false };
            auto lista = Listar(volumenes::Buscar(rar5[0]).partes, L"", pr, no);
            Revisar(lista.estado == jicaro::Estado::Bien && lista.completo && lista.formato == L"RAR 5" && !lista.entradas.empty(),
                    L"listar el RAR 5 en partes (" + std::to_wstring(lista.entradas.size()) + L" entradas)");
        }

        // Falta una parte en medio, y falta la última
        std::wstring h = Carpeta(L"hueco");
        for (int i = 1; i <= 8; i++)
            if (i != 5) CopyFileW(rar5[i - 1].c_str(), (h + L"\\hueco.part0" + std::to_wstring(i) + L".rar").c_str(), FALSE);
        juego = volumenes::Buscar(h + L"\\hueco.part01.rar");
        Revisar(juego.faltan.size() == 1 && juego.faltan[0] == L"hueco.part05.rar", L"avisa que falta hueco.part05.rar");
        std::wstring u = Carpeta(L"sin-ultima");
        for (int i = 1; i <= 7; i++) CopyFileW(rar5[i - 1].c_str(), (u + L"\\corto.part0" + std::to_wstring(i) + L".rar").c_str(), FALSE);
        r = ExtraerSimple(u + L"\\corto.part01.rar", Carpeta(L"salida\\sin-ultima"));
        Revisar(r.estado == jicaro::Estado::FaltaParte && r.mensaje.find(L"corto.part07.rar") != std::wstring::npos,
                L"sin la última parte avisa que falta la que sigue a la 7 " + Texto(r));

        // Con contraseña: libarchive no los abre y Jícaro lo dice claro
        struct { const wchar_t* uu; const wchar_t* nombre; } cifrados[] = {
            { L"test_read_format_rar5_encrypted_filenames.rar.uu", L"rar5-clave-nombres.rar" },
            { L"test_read_format_rar4_encrypted_filenames.rar.uu", L"rar4-clave-nombres.rar" },
            { L"test_read_format_rar_encryption_header.rar.uu", L"rar4-clave-cabecera.rar" },
            { L"test_read_format_7zip_encryption.7z.uu", L"clave.7z" },
            { L"test_read_format_7zip_encryption_header.7z.uu", L"clave-cabecera.7z" },
        };
        for (auto& s : cifrados)
        {
            std::wstring archivo = DeLaSuite(s.uu, s.nombre);
            std::wstring destino = Carpeta(std::wstring(L"salida\\") + s.nombre);
            auto rc = ExtraerSimple(archivo, destino);
            Revisar(rc.estado == jicaro::Estado::CifradoNoSoportado && rc.archivos == 0 && rc.mensaje.find(L"7-Zip") != std::wstring::npos,
                    std::wstring(s.nombre) + L" avisa que no se puede abrir " + Texto(rc));
        }
        // Algunos con contraseña y otros sin: salen los que no tienen y avisa de los otros
        struct { const wchar_t* uu; const wchar_t* nombre; } mezcla[] = {
            { L"test_read_format_rar5_encrypted.rar.uu", L"rar5-clave.rar" },
            { L"test_read_format_rar4_encrypted.rar.uu", L"rar4-clave.rar" },
        };
        for (auto& s : mezcla)
        {
            std::wstring archivo = DeLaSuite(s.uu, s.nombre);
            auto rc = ExtraerSimple(archivo, Carpeta(std::wstring(L"salida\\") + s.nombre));
            Revisar(rc.estado == jicaro::Estado::Bien && rc.archivos > 0 && rc.cifradas > 0 && rc.mensaje.find(L"7-Zip") != std::wstring::npos,
                    std::wstring(s.nombre) + L": salen los que no tienen contraseña y avisa de los otros " + Texto(rc));
        }
        // Parte con contraseña y parte sin: salen los que no tienen
        {
            std::wstring archivo = DeLaSuite(L"test_read_format_7zip_encryption_partially.7z.uu", L"medio-clave.7z");
            auto rc = ExtraerSimple(archivo, Carpeta(L"salida\\medio-clave"));
            Revisar(rc.estado == jicaro::Estado::Bien && rc.archivos > 0 && !rc.omitidas.empty(),
                    L"7z con algunos archivos con contraseña: salen los demás " + Texto(rc));
        }

        // Enlaces dentro de un RAR 5
        {
            std::wstring archivo = DeLaSuite(L"test_read_format_rar5_symlink.rar.uu", L"rar5-enlaces.rar");
            auto rc = ExtraerSimple(archivo, Carpeta(L"salida\\rar5-enlaces"));
            bool omitioEnlace = false;
            for (auto& o : rc.omitidas)
                if (o.motivo.find(L"enlace") != std::wstring::npos) omitioEnlace = true;
            Revisar(rc.estado == jicaro::Estado::Bien && omitioEnlace, L"RAR 5 con enlaces: los salta " + Texto(rc));
        }
    }

    void PruebasMalas()
    {
        wprintf(L"\nArchivos malos\n");
        std::wstring c = Carpeta(L"malos");

        Armar(c + L"\\malo-puntos.zip",
              { { L"bien.txt", "bien" }, { L"../fuera-1.txt", "x" }, { L"a/../../fuera-2.txt", "x" }, { L"..\\fuera-3.txt", "x" },
                { L"sub/../../../fuera-4.txt", "x" } },
              [](archive* a) { archive_write_set_format_zip(a); });
        std::wstring d = Carpeta(L"salida\\malo-puntos\\destino");
        auto r = ExtraerSimple(c + L"\\malo-puntos.zip", d);
        Revisar(r.estado == jicaro::Estado::Bien && r.archivos == 1 && r.omitidas.size() == 4 && ExisteArchivo(d + L"\\bien.txt"),
                L"ZIP con «../»: solo sale bien.txt " + Texto(r));

        // Rutas absolutas. La última apunta, sin letra de unidad, a una carpeta de trabajo real («\Users\...»):
        // libarchive sola la escribiría ahí aunque tenga ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS.
        std::wstring raiz = trabajo.substr(2) + L"\\fuera-raiz.txt";   // sin «C:»
        for (auto& ch : raiz)
            if (ch == L'\\') ch = L'/';
        Armar(c + L"\\malo-absoluto.tar",
              { { L"bien.txt", "bien" }, { L"/fuera-abs.txt", "x" }, { L"C:/fuera-abs2.txt", "x" }, { L"\\\\servidor\\c\\fuera-unc.txt", "x" },
                { raiz, "x" } },
              [](archive* a) { archive_write_set_format_pax_restricted(a); });
        d = Carpeta(L"salida\\malo-absoluto\\destino");
        r = ExtraerSimple(c + L"\\malo-absoluto.tar", d);
        Revisar(r.estado == jicaro::Estado::Bien && r.archivos == 1 && r.omitidas.size() == 4 && !ExisteArchivo(trabajo + L"\\fuera-raiz.txt"),
                L"TAR con rutas absolutas: se saltan todas " + Texto(r));

        Armar(c + L"\\malo-enlaces.tar",
              { { L"original.txt", "original" }, { L"copia.txt", "", Item::Duro, L"original.txt" },
                { L"enlace", "", Item::Simbolico, L"../../.." }, { L"enlace/dentro.txt", "dentro" },
                { L"duro-fuera", "", Item::Duro, L"../../fuera-duro.txt" }, { L"duro-abs", "", Item::Duro, L"C:/Windows/win.ini" } },
              [](archive* a) { archive_write_set_format_pax_restricted(a); });
        d = Carpeta(L"salida\\malo-enlaces\\destino");
        r = ExtraerSimple(c + L"\\malo-enlaces.tar", d);
        std::string copia;
        LeerTodo(d + L"\\copia.txt", copia);
        DWORD atributosEnlace = GetFileAttributesW((d + L"\\enlace").c_str());
        Revisar(r.estado == jicaro::Estado::Bien && copia == "original" && Omitio(r, L"enlace") && Omitio(r, L"duro-fuera")
                    && Omitio(r, L"duro-abs") && atributosEnlace != INVALID_FILE_ATTRIBUTES
                    && !(atributosEnlace & FILE_ATTRIBUTE_REPARSE_POINT),
                L"TAR con enlaces: no crea ninguno y copia el enlace duro interno " + Texto(r));

        Armar(c + L"\\malo-nombres.zip",
              { { L"CON.txt", "1" }, { L"dir/a:b.txt", "2" }, { L"x.txt:Zone.Identifier", "3" }, { L"punto. ", "4" }, { L"aux", "5" } },
              [](archive* a) { archive_write_set_format_zip(a); });
        d = Carpeta(L"salida\\malo-nombres");
        r = ExtraerSimple(c + L"\\malo-nombres.zip", d);
        Revisar(r.estado == jicaro::Estado::Bien && r.archivos == 5 && ExisteArchivo(d + L"\\_CON.txt") && ExisteArchivo(d + L"\\dir\\a_b.txt")
                    && ExisteArchivo(d + L"\\x.txt_Zone.Identifier") && ExisteArchivo(d + L"\\punto") && ExisteArchivo(d + L"\\_aux"),
                L"nombres raros (CON, «:», flujos ocultos, punto final) " + Texto(r));

        // Una unión que ya estaba en la carpeta de destino y lleva fuera de ella
        Armar(c + L"\\trampa.zip", { { L"trampa/x.txt", "x" }, { L"bien.txt", "bien" } }, [](archive* a) { archive_write_set_format_zip(a); });
        d = Carpeta(L"salida\\union\\destino");
        std::wstring afuera = Carpeta(L"salida\\union\\afuera");
        _wsystem((L"cmd.exe /d /c mklink /J \"" + d + L"\\trampa\" \"" + afuera + L"\" >nul").c_str());
        r = ExtraerSimple(c + L"\\trampa.zip", d);
        Revisar(r.estado == jicaro::Estado::Bien && r.archivos == 1 && Omitio(r, L"trampa\\x.txt") && ContarArchivos(afuera) == 0,
                L"no escribe a través de una unión (junction) que ya estaba " + Texto(r));
        RemoveDirectoryW((d + L"\\trampa").c_str());   // solo la unión

        Revisar(!HayAlgoFuera(trabajo), L"ningún archivo «fuera-*» quedó en la carpeta de trabajo");
        Revisar(!ExisteArchivo(L"C:\\fuera-abs.txt") && !ExisteArchivo(L"C:\\fuera-abs2.txt"), L"nada en C:\\");

        // Lo mismo con libarchive sola (sin las comprobaciones de Jícaro), para ver por qué hacen falta
        {
            archive* a = archive_read_new();
            archive_read_support_format_tar(a);
            archive_read_open_filename_w(a, (c + L"\\malo-absoluto.tar").c_str(), 10240);
            archive* w = archive_write_disk_new();
            archive_write_disk_set_options(w, ARCHIVE_EXTRACT_SECURE_SYMLINKS | ARCHIVE_EXTRACT_SECURE_NODOTDOT
                                                  | ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS | ARCHIVE_EXTRACT_NO_OVERWRITE);
            std::wstring sola = Carpeta(L"salida-libarchive-sola");
            wchar_t antes[MAX_PATH];
            GetCurrentDirectoryW(MAX_PATH, antes);
            SetCurrentDirectoryW(sola.c_str());
            archive_entry* e;
            while (archive_read_next_header(a, &e) == ARCHIVE_OK)
            {
                std::wstring nombre = archive_entry_pathname_w(e);
                if (nombre.find(L"fuera-raiz") == std::wstring::npos) continue;   // solo la ruta «\Users\...»
                if (archive_write_header(w, e) == ARCHIVE_OK)
                {
                    const void* b;
                    size_t t;
                    la_int64_t o;
                    while (archive_read_data_block(a, &b, &t, &o) == ARCHIVE_OK) archive_write_data_block(w, b, t, o);
                }
                archive_write_finish_entry(w);
            }
            archive_write_free(w);
            archive_read_free(a);
            SetCurrentDirectoryW(antes);
            bool escribio = ExisteArchivo(trabajo + L"\\fuera-raiz.txt");
            wprintf(L"  (dato) libarchive sola, con las tres banderas SECURE, %ls la ruta «\\Users\\...\\fuera-raiz.txt»\n",
                    escribio ? L"SÍ escribió fuera con" : L"no escribió");
            if (escribio) DeleteFileW((trabajo + L"\\fuera-raiz.txt").c_str());
        }
    }

    void PruebasDeBomba()
    {
        wprintf(L"\nBomba de descompresión chica\n");
        std::wstring c = Carpeta(L"malos");
        std::wstring bomba = c + L"\\bomba-chica.zip";
        Armar(bomba, { { L"ceros.bin", "", Item::Archivo, L"", 600LL << 20 } }, [](archive* a) {
            archive_write_set_format_zip(a);
            archive_write_set_options(a, "zip:compression-level=9");
        });
        int64_t pesa = TamanoArchivo(bomba);
        wprintf(L"  bomba-chica.zip pesa %ls y trae 600 MB de ceros\n", TextoTamano(pesa).c_str());

        // Con los topes de siempre: pregunta al pasar de 512 MB y 100 veces el tamaño. Se responde «Detener».
        {
            Prueba p;
            std::wstring motivo;
            p.pr.bomba = [&p, &motivo](const std::wstring& m) {
                p.pidioBomba++;
                motivo = m;
                return false;
            };
            std::wstring d = Carpeta(L"salida\\bomba-detenida");
            auto r = Extraer(p, bomba, d);
            Revisar(r.estado == jicaro::Estado::Detenido && p.pidioBomba == 1 && r.quitoAMedias && !ExisteArchivo(d + L"\\ceros.bin"),
                    L"pregunta una vez, se detiene y quita el archivo a medias " + Texto(r));
            wprintf(L"  pregunta: %ls\n", motivo.c_str());
        }
        // Si la persona dice «Seguir», termina
        {
            Prueba p;
            p.pr.bomba = [&p](const std::wstring&) {
                p.pidioBomba++;
                return true;
            };
            std::wstring d = Carpeta(L"salida\\bomba-seguida");
            auto r = Extraer(p, bomba, d);
            Revisar(r.estado == jicaro::Estado::Bien && p.pidioBomba == 1 && TamanoArchivo(d + L"\\ceros.bin") == (600LL << 20),
                    L"si se sigue, termina (600 MB) " + Texto(r));
            DeleteFileW((d + L"\\ceros.bin").c_str());
        }
        // Cancelar en medio
        {
            Prueba p;
            p.pr.bomba = [](const std::wstring&) { return true; };
            std::wstring d = Carpeta(L"salida\\bomba-cancelada");
            std::thread t([&p]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                p.cancelar = true;
            });
            auto r = Extraer(p, bomba, d);
            t.join();
            Revisar(r.estado == jicaro::Estado::Cancelado && !ExisteArchivo(d + L"\\ceros.bin"),
                    L"cancelar en medio quita el archivo a medias " + Texto(r));
        }
    }

    void PruebasSiExiste()
    {
        wprintf(L"\nArchivos que ya existen\n");
        std::wstring zip = trabajo + L"\\propios\\propio.zip";
        std::wstring d = Carpeta(L"salida\\existe");
        ExtraerSimple(zip, d);
        {
            Prueba p;
            p.op.siExiste = SiExiste::Saltar;
            auto r = Extraer(p, zip, d);
            Revisar(r.estado == jicaro::Estado::Bien && r.saltados == 4 && r.archivos == 0, L"saltar los que existen " + Texto(r));
        }
        {
            Prueba p;
            p.op.siExiste = SiExiste::Renombrar;
            auto r = Extraer(p, zip, d);
            Revisar(r.estado == jicaro::Estado::Bien && r.archivos == 4 && ExisteArchivo(d + L"\\hola (2).txt"),
                    L"guardar con otro nombre: hola (2).txt " + Texto(r));
        }
        {
            // «Reemplazar» manda lo anterior a la Papelera; en la prueba, la Papelera es una carpeta
            std::wstring papelera = Carpeta(L"papelera-de-prueba");
            int mandados = 0;
            pruebas::papelera = [&](const std::wstring& ruta) {
                mandados++;
                return MoveFileW(ruta.c_str(), (papelera + L"\\" + std::to_wstring(mandados) + L"-" + NombreDe(ruta)).c_str()) != FALSE;
            };
            EscribirTodo(d + L"\\hola.txt", "versión vieja");
            Prueba p;
            p.pr.existe = [&p](const std::wstring&, bool& todos) {
                p.pidioExiste++;
                todos = true;
                return Decision::Reemplazar;
            };
            auto r = Extraer(p, zip, d);
            std::string hola;
            LeerTodo(d + L"\\hola.txt", hola);
            Revisar(r.estado == jicaro::Estado::Bien && p.pidioExiste == 1 && mandados == 4 && hola == Esperado()[L"hola.txt"]
                        && ContarArchivos(papelera) == 4,
                    L"reemplazar pregunta una vez («para todos») y manda lo anterior a la Papelera " + Texto(r));
            pruebas::papelera = nullptr;
        }
        {
            Prueba p;
            p.pr.existe = [](const std::wstring&, bool&) { return Decision::Cancelar; };
            auto r = Extraer(p, zip, d);
            Revisar(r.estado == jicaro::Estado::Cancelado, L"cancelar en la pregunta " + Texto(r));
        }
        {
            // Una entrada con el mismo nombre que el archivo que se está leyendo: nunca se reemplaza
            std::wstring c = Carpeta(L"mismo");
            Armar(c + L"\\mismo.zip", { { L"mismo.zip", "otro" } }, [](archive* a) { archive_write_set_format_zip(a); });
            int64_t antes = TamanoArchivo(c + L"\\mismo.zip");
            Prueba p;
            p.op.siExiste = SiExiste::Reemplazar;
            auto r = Extraer(p, c + L"\\mismo.zip", c);
            Revisar(r.estado == jicaro::Estado::Bien && TamanoArchivo(c + L"\\mismo.zip") == antes && ExisteArchivo(c + L"\\mismo (2).zip"),
                    L"no reemplaza el archivo comprimido que está leyendo " + Texto(r));
        }
    }

    void PruebasDeMarca()
    {
        wprintf(L"\nMarca de la Web (Zone.Identifier)\n");
        std::wstring c = Carpeta(L"marca");
        std::wstring zip = c + L"\\bajado.zip";
        CopyFileW((trabajo + L"\\propios\\propio.zip").c_str(), zip.c_str(), FALSE);
        EscribirTodo(zip + L":Zone.Identifier",
                     "[ZoneTransfer]\r\nZoneId=3\r\nReferrerUrl=https://ejemplo.org/\r\nHostUrl=https://ejemplo.org/bajado.zip\r\n");
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        Revisar(marca::Zona(zip) == 3, L"el archivo de prueba tiene la marca (zona 3, internet)");
        std::wstring d = Carpeta(L"salida\\marca");
        auto inicio = std::chrono::steady_clock::now();
        auto r = ExtraerSimple(zip, d);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - inicio).count();
        bool todos = true;
        for (auto& [ruta, datos] : Esperado())
            if (marca::Zona(d + L"\\" + ruta) != 3) todos = false;
        Revisar(r.estado == jicaro::Estado::Bien && r.zona == 3 && r.marcados == 4 && todos, L"los 4 archivos extraídos quedan marcados " + Texto(r));
        wprintf(L"  (dato) extraer y marcar 4 archivos tardó %.0f ms\n", ms);

        // Sin marca no se marca nada
        std::wstring d2 = Carpeta(L"salida\\sin-marca");
        auto r2 = ExtraerSimple(trabajo + L"\\propios\\propio.zip", d2);
        Revisar(r2.zona == 0 && r2.marcados == 0 && marca::Zona(d2 + L"\\hola.txt") == 0, L"sin marca, lo extraído tampoco la lleva");

        // Muchos archivos marcados: cuánto tarda
        std::vector<Item> muchos;
        for (int i = 0; i < 500; i++) muchos.push_back({ L"m/" + std::to_wstring(i) + L".txt", "x" });
        std::wstring zipMuchos = c + L"\\muchos.zip";
        Armar(zipMuchos, muchos, [](archive* a) { archive_write_set_format_zip(a); });
        EscribirTodo(zipMuchos + L":Zone.Identifier", "[ZoneTransfer]\r\nZoneId=3\r\n");
        std::wstring d3 = Carpeta(L"salida\\muchos-marcados");
        inicio = std::chrono::steady_clock::now();
        auto r3 = ExtraerSimple(zipMuchos, d3);
        ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - inicio).count();
        Revisar(r3.estado == jicaro::Estado::Bien && r3.marcados == 500, L"500 archivos marcados " + Texto(r3));
        wprintf(L"  (dato) extraer y marcar 500 archivos tardó %.0f ms (%.1f ms cada uno)\n", ms, ms / 500);

        // Programas (.exe y .cmd): van por los servicios de adjuntos de Windows, que además avisan al antivirus
        std::vector<Item> programas;
        for (int i = 0; i < 20; i++) programas.push_back({ L"p/prueba-" + std::to_wstring(i) + (i % 2 ? L".exe" : L".cmd"), "rem prueba\r\n" });
        std::wstring zipProgramas = c + L"\\programas.zip";
        Armar(zipProgramas, programas, [](archive* a) { archive_write_set_format_zip(a); });
        EscribirTodo(zipProgramas + L":Zone.Identifier", "[ZoneTransfer]\r\nZoneId=3\r\n");
        std::wstring d4 = Carpeta(L"salida\\programas-marcados");
        inicio = std::chrono::steady_clock::now();
        auto r4 = ExtraerSimple(zipProgramas, d4);
        ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - inicio).count();
        bool todosProgramas = true;
        for (auto& it : programas)
        {
            std::wstring ruta = d4 + L"\\" + it.ruta;
            for (auto& ch : ruta)
                if (ch == L'/') ch = L'\\';
            if (marca::Zona(ruta) != 3) todosProgramas = false;
        }
        Revisar(r4.estado == jicaro::Estado::Bien && r4.marcados == 20 && todosProgramas, L"20 programas marcados " + Texto(r4));
        wprintf(L"  (dato) extraer y marcar 20 programas tardó %.0f ms (%.1f ms cada uno)\n", ms, ms / 20);
        std::string zona;
        LeerTodo(d4 + L"\\p\\prueba-1.exe:Zone.Identifier", zona);
        wprintf(L"  (dato) marca de un .exe extraído: %hs\n", zona.c_str());
        CoUninitialize();
    }
}

int wmain(int argc, wchar_t** argv)
{
    _setmode(_fileno(stdout), _O_U8TEXT);
    setlocale(LC_CTYPE, "");
    if (argc == 3 && std::wstring(argv[1]) == L"--diagnostico")
    {
        // Lista un archivo con el lector de libarchive y con el de Jícaro, para comparar
        auto juego = volumenes::Buscar(argv[2]);
        std::vector<const wchar_t*> nombres;
        for (auto& p : juego.partes) nombres.push_back(p.c_str());
        nombres.push_back(nullptr);
        archive* a = archive_read_new();
        archive_read_support_filter_all(a);
        archive_read_support_format_all(a);
        int r = archive_read_open_filenames_w(a, nombres.data(), 10240);
        wprintf(L"libarchive: abrir=%d (%zu partes)\n", r, juego.partes.size());
        archive_entry* e;
        while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN)
            wprintf(L"  %ls (%lld)\n", archive_entry_pathname_w(e), (long long)archive_entry_size(e));
        wprintf(L"  fin=%d %hs\n", r, archive_error_string(a) ? archive_error_string(a) : "");
        archive_read_free(a);
        Progreso pr;
        std::atomic<bool> no{ false };
        auto lista = Listar(juego.partes, juego.nombreSuelto, pr, no);
        wprintf(L"Jícaro: %zu entradas, %ls, %ls [%hs]\n", lista.entradas.size(), TextoEstado(lista.estado).c_str(),
                lista.mensaje.c_str(), pruebas::ultimoError.c_str());
        for (auto& en : lista.entradas) wprintf(L"  %ls (%lld)\n", en.ruta.c_str(), (long long)en.tamano);
        return 0;
    }
    if (argc < 3)
    {
        wprintf(L"Uso: prueba-nucleo.exe <carpeta con los .uu de libarchive> <carpeta de trabajo nueva>\n");
        return 100;
    }
    suite = RutaCompleta(argv[1]);
    trabajo = RutaCompleta(argv[2]);
    if (ExisteCarpeta(trabajo))
    {
        wprintf(L"La carpeta de trabajo ya existe: usa una nueva.\n");
        return 101;
    }
    CrearCarpetas(trabajo);
    wprintf(L"libarchive %hs\n", archive_version_details());

    PruebasPropias();
    PruebasDeContrasena();
    PruebasRarY7z();
    PruebasMalas();
    PruebasDeBomba();
    PruebasSiExiste();
    PruebasDeMarca();

    wprintf(L"\n%d pruebas, %d fallas\n", hechas, fallas);
    return fallas;
}
