#pragma once

// Motor de 7-Zip para RAR 4, RAR 5 y 7z (también con contraseña y en partes). Usa 7z.dll, de 7-Zip (Igor Pavlov,
// licencia LGPL con partes BSD y la restricción de unRAR), que va junto a Jicaro.exe y se carga desde esa carpeta con su
// ruta completa. Jícaro solo la usa para leer: no crea RAR ni 7z.
//
// Aquí no hay código de 7-Zip: las interfaces se declaran en motor7z.cpp según los encabezados públicos de 7-Zip
// (IArchive.h, IStream.h, IPassword.h, IProgress.h y PropID.h).
//
// 7z.dll entrega los datos llamando a funciones nuestras, a veces desde sus propios hilos. Para que todo lo que toca el
// disco (crear, escribir, marcar, preguntar) pase en el hilo de Jícaro, Extraer corre 7z.dll en un hilo aparte y le
// pasa cada cosa a «Destino» en el hilo que llamó, una por una, mientras 7z.dll espera.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "extractor.hpp"

namespace jicaro::motor7z
{
    enum class Formato { Rar, Rar5, SieteZ };

    // 7z.dll junto al programa y lista para usar (se carga una sola vez). false si no está o no sirve.
    bool Disponible();

    // Por la firma de la primera parte. nullopt: no es RAR ni 7z (lo lee libarchive).
    std::optional<Formato> Detectar(const std::wstring& primera);

    struct Item
    {
        std::wstring ruta;          // con «\»
        int64_t tamano = -1;        // -1 si no se sabe
        bool carpeta = false;
        bool cifrada = false;
        bool simbolico = false;     // enlace simbólico, unión o punto de reanálisis: Jícaro no los crea
        bool especial = false;      // dispositivo, tubería...: no es un archivo común
        bool alterno = false;       // flujo alterno de NTFS («archivo:flujo»): podría cambiar la marca de la Web
        bool anti = false;          // 7z: marca para borrar un archivo (Jícaro no borra nada)
        std::wstring duro;          // enlace duro o copia (RAR 5): la ruta, dentro del archivo, de lo que copia
        bool conMTime = false, conATime = false, conCTime = false;
        FILETIME mtime{}, atime{}, ctime{};
    };

    enum class Apertura
    {
        Bien,
        NoEsArchivo,
        SinClave,     // las cabeceras tienen contraseña y no se dio
        ClaveMala,    // se dio una contraseña y no abrió (o el archivo está dañado)
        Cancelado,
        SinMemoria,
        Error,
    };

    // Qué hacer con cada archivo que va a salir
    enum class Paso { Escribir, Saltar, Parar };

    // Resultado de cada archivo según 7-Zip (NArchive::NExtract::NOperationResult)
    enum : int32_t
    {
        kBien = 0,
        kMetodoDesconocido = 1,
        kDatosDanados = 2,
        kCrcMalo = 3,
        kNoDisponible = 4,
        kFinInesperado = 5,
        kDatosDespuesDelFin = 6,
        kNoEsArchivo = 7,
        kCabecerasDanadas = 8,
        kClaveIncorrecta = 9,
    };

    // Lo que hace Jícaro con lo que sale. Todo corre en el hilo que llamó a Archivo::Extraer, una cosa a la vez.
    struct Destino
    {
        virtual ~Destino() = default;
        virtual Paso Empieza(uint32_t indice) = 0;
        virtual bool Datos(const void* datos, size_t tamano) = 0;   // false: parar todo
        virtual void Termina(int32_t resultado) = 0;               // solo de los que se aceptaron con Escribir
        // Uno de los pedidos que 7z.dll no llegó a entregar (por ejemplo, un 7z con la contraseña mala o con un método
        // que no conoce: 7z.dll lo da por dañado sin crear nada)
        virtual void SinDatos(uint32_t indice, int32_t resultado) = 0;
        virtual std::optional<std::wstring> Clave() = 0;           // nullopt: no hay (se para)
        virtual bool Memoria(uint64_t bytes) = 0;                  // ¿se permite usar tanta memoria?
    };

    class Archivo
    {
    public:
        Archivo();
        ~Archivo();
        Archivo(const Archivo&) = delete;
        Archivo& operator=(const Archivo&) = delete;

        // «partes»: rutas completas en orden. Un RAR en partes se abre por la primera y 7z.dll pide las demás por su
        // nombre (solo se le dan archivos de la misma carpeta). Un .001, .002... y los 7z se leen como un solo archivo
        // seguido. «clave»: si 7z.dll la pide al abrir (cabeceras con contraseña); se llama una sola vez por intento.
        Apertura Abrir(const std::vector<std::wstring>& partes, Formato formato,
                       std::function<std::optional<std::wstring>()> clave, Progreso& progreso,
                       const std::atomic<bool>& cancelar);

        const std::vector<Item>& Items() const;
        std::wstring NombreFormato() const;              // «RAR», «RAR 5» o «7z»
        bool PidioClave() const;                         // las cabeceras tienen contraseña
        const std::optional<std::wstring>& ClaveUsada() const;
        std::wstring FaltaVolumen() const;               // la parte que según el archivo sigue y no está ("" si no)
        bool Incompleto() const;                         // se acaba antes de tiempo
        bool CabecerasDanadas() const;
        int64_t TotalComprimido() const;
        bool Abierto() const;
        std::wstring NoAbrio() const;                    // la parte que no se pudo leer ("" si no hubo)
        // Todas las partes que se leyeron, también las que 7z.dll pidió por su nombre (por ejemplo «juego2.rar»)
        const std::vector<std::wstring>& Volumenes() const;

        // Extrae esos índices (de menor a mayor) hacia «d». Devuelve lo que devolvió 7z.dll (E_ABORT si se paró).
        HRESULT Extraer(const std::vector<uint32_t>& indices, Destino& d);

        void Cerrar();

    private:
        struct Interno;
        std::unique_ptr<Interno> m;
    };

}

#ifdef JICARO_PRUEBAS
namespace jicaro::pruebas
{
    // Para probar lo que pasa sin 7z.dll (libarchive lee los RAR y 7z como en la versión 0.1)
    inline bool sinMotor7z = false;
}
#endif
