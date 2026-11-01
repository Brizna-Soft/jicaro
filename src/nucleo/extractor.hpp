#pragma once

// Leer y extraer archivos comprimidos: los RAR y los 7z con 7z.dll de 7-Zip (ver motor7z.hpp), y lo demás con libarchive
// (BSD). Sin wxWidgets: lo usan la ventana, la línea de comandos y las pruebas. Las preguntas a la persona (contraseña, archivo que ya existe, posible bomba) llegan por funciones que
// el hilo de trabajo llama y que esperan la respuesta.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace jicaro
{
    struct Entrada
    {
        std::wstring ruta;      // tal como viene en el archivo (con «\»)
        int64_t tamano = -1;    // -1 si no se sabe
        int64_t fecha = 0;      // segundos desde 1970; 0 si no se sabe
        bool carpeta = false;
        bool enlace = false;    // enlace simbólico o duro: Jícaro no los crea
        bool especial = false;  // flujo alterno de NTFS, dispositivo o marca de borrado: no se extrae
        bool cifrada = false;
    };

    enum class Estado
    {
        Bien,
        Cancelado,
        Error,
        FaltaParte,
        CifradoNoSoportado,     // un cifrado que Jícaro no abre (ZIP con «cifrado fuerte», o RAR y 7z sin 7z.dll)
        SinContrasena,          // pedía contraseña y no se dio ninguna
        ContrasenaIncorrecta,   // se dio una contraseña y no sirvió
        NoEsArchivo,
        SinEspacio,
        Detenido,               // la persona lo detuvo ante una posible bomba de descompresión
    };

    struct Contenido
    {
        Estado estado = Estado::Error;
        std::wstring mensaje;       // para la persona, en español
        std::wstring formato;       // «RAR 5», «ZIP», «7z», «TAR + gzip»...
        std::vector<Entrada> entradas;
        int64_t total = 0;          // suma de los tamaños conocidos
        int cifradas = 0;
        bool cifradoSoportado = false;   // Jícaro puede abrir esas contraseñas (ZIP, y RAR y 7z con 7z.dll)
        bool completo = false;      // se leyó hasta el final
        bool suelto = false;        // un solo archivo comprimido (.gz, .bz2, .xz, .zst)
        std::vector<std::wstring> faltan;   // partes que según el archivo siguen y no están
        std::wstring clave;         // la contraseña que abrió la lista (cabeceras con contraseña): quien la usa la borra
    };

    // Topes contra las bombas de descompresión. Cada pregunta se hace una sola vez por extracción.
    struct Limites
    {
        int64_t bytesMinimos = 512LL << 20;   // desde aquí cuenta la proporción
        double proporcion = 100.0;            // lo extraído contra el tamaño del archivo comprimido
        double parteDelLibre = 0.5;           // lo extraído contra el espacio libre que había al empezar
        int64_t entradas = 200000;            // cantidad de archivos
        int64_t libreMinimo = 512LL << 20;    // se detiene si el disco queda con menos que esto
    };

    enum class SiExiste { Preguntar, Reemplazar, Saltar, Renombrar };
    enum class Decision { Reemplazar, Saltar, Renombrar, Cancelar };

    struct Preguntas
    {
        // otraVez: la contraseña anterior no sirvió. Vacío si la persona cancela.
        std::function<std::optional<std::wstring>(const std::wstring& archivo, bool otraVez)> contrasena;
        // Ya existe un archivo con ese nombre. «paraTodos»: hacer lo mismo con los demás.
        std::function<Decision(const std::wstring& ruta, bool& paraTodos)> existe;
        // Puede ser una bomba de descompresión. true: seguir.
        std::function<bool(const std::wstring& motivo)> bomba;
    };

    struct Progreso
    {
        std::atomic<int64_t> leidos{ 0 };          // bytes leídos del archivo comprimido
        std::atomic<int64_t> totalComprimido{ 0 };
        std::atomic<int64_t> escritos{ 0 };
        std::atomic<int64_t> archivos{ 0 };
        std::atomic<int64_t> totalEsperado{ 0 };    // lo que se espera escribir, si se sabe (la ventana lo saca de la lista)

        void PonerActual(const std::wstring& r)
        {
            std::lock_guard g(m);
            actual = r;
        }
        std::wstring Actual()
        {
            std::lock_guard g(m);
            return actual;
        }
        void Reiniciar()
        {
            leidos = 0; totalComprimido = 0; escritos = 0; archivos = 0; totalEsperado = 0;
            PonerActual(L"");
        }

    private:
        std::mutex m;
        std::wstring actual;
    };

    struct Opciones
    {
        std::vector<std::wstring> partes;     // en orden (la primera abre el juego)
        std::wstring destino;                 // carpeta; se crea si no existe
        SiExiste siExiste = SiExiste::Preguntar;
        std::vector<std::wstring> elegidas;   // rutas como salen en Listar; vacío: todo
        Limites limites;
        bool pasarMarca = true;               // marca de la Web a lo extraído (si el archivo la tiene)
        std::wstring nombreSuelto;            // nombre para un .gz, .bz2, .xz o .zst de un solo archivo
        int64_t totalEsperado = 0;            // lo que se espera escribir (para el progreso); 0 si no se sabe
        // Contraseña para probar primero, sin preguntar (la de la lista o la de --contrasena-stdin). Si no sirve, se
        // pregunta con Preguntas::contrasena; si no hay a quién preguntar, queda como «contraseña incorrecta».
        std::optional<std::wstring> contrasena;
    };

    struct Omitida
    {
        std::wstring ruta;
        std::wstring motivo;
        enum class Tipo
        {
            Seguridad,   // ruta peligrosa, enlace, dispositivo: se salta a propósito
            Cifrado,     // contraseña de un tipo que Jícaro no abre
            Falla,       // no se pudo leer, escribir o copiar
            Clave,       // la contraseña no sirvió
        } tipo = Tipo::Seguridad;
    };

    struct Resumen
    {
        Estado estado = Estado::Error;
        std::wstring mensaje;               // para la persona
        int64_t archivos = 0, carpetas = 0, saltados = 0, escritos = 0;
        int64_t cifradas = 0;               // con contraseña de un tipo que Jícaro no abre
        int64_t clavesMalas = 0;            // que no salieron porque la contraseña no sirvió
        int64_t sinClave = 0;               // que no salieron porque tienen contraseña y no se dio
        int64_t rechazados = 0;             // saltados por seguridad
        int64_t fallidos = 0;               // que no se pudieron extraer por un error
        std::vector<Omitida> omitidas;      // todo lo anterior, con su ruta y su motivo
        unsigned long zona = 0;             // 3 o 4 si se pasó la marca de la Web
        int64_t marcados = 0, sinMarca = 0, quitadosPorWindows = 0;
        bool quitoAMedias = false;          // se borró el archivo que quedó a medias al detenerse
        std::wstring aMedias;               // archivo a medias que no se pudo borrar (vacío si no hay)

        // Salió todo lo que se pidió, sin avisos
        bool Limpio() const
        {
            return estado == Estado::Bien && omitidas.empty() && sinMarca == 0 && quitadosPorWindows == 0 && aMedias.empty();
        }
    };

    // Lee la lista del contenido. «nombreSuelto» se usa para un .gz de un solo archivo. Si la lista misma tiene
    // contraseña (RAR o 7z con las cabeceras cifradas), se prueba «clave» y después se pregunta con
    // preguntas->contrasena; la que sirvió vuelve en Contenido::clave.
    Contenido Listar(const std::vector<std::wstring>& partes, const std::wstring& nombreSuelto, Progreso& progreso,
                     const std::atomic<bool>& cancelar, Preguntas* preguntas = nullptr,
                     const std::optional<std::wstring>& clave = std::nullopt);

    // Extrae. Usa la carpeta de trabajo del proceso: no puede haber dos extracciones a la vez en el mismo proceso.
    Resumen Extraer(const Opciones& opciones, Preguntas& preguntas, Progreso& progreso, const std::atomic<bool>& cancelar);

    // Textos para la persona
    std::wstring TextoEstado(Estado estado);
}
