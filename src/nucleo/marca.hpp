#pragma once

// «Marca de la Web»: Windows guarda en el flujo oculto Zone.Identifier de un archivo bajado de internet de qué zona
// viene. Si el archivo comprimido la tiene, Jícaro se la pasa a lo que extrae, para que Windows (SmartScreen, Office,
// el antivirus) siga avisando antes de abrir programas que vienen de internet.
//
// Cómo se hace, según Microsoft (learn.microsoft.com):
//  - Leer la zona: objeto «Persistent Zone Identifier» (CLSID_PersistentZoneIdentifier) con IPersistFile::Load
//    e IZoneIdentifier::GetId.
//  - Ponerla: servicios de adjuntos (IAttachmentExecute). SetReferrer con el archivo comprimido es lo que la
//    documentación indica para los contenedores: lo extraído hereda la zona del archivo del que sale.
//  - Si eso falla, se escribe la zona con IZoneIdentifier::SetId e IPersistFile::Save.
//
// Hace falta COM iniciado en el hilo que llama (CoInitializeEx).

#include <string>
#include <vector>

namespace jicaro::marca
{
    // 3 = Internet, 4 = sitios restringidos; 0 si no tiene marca (o es de esta PC o de la red local)
    unsigned long Zona(const std::wstring& archivo);

    // La zona más desconfiada entre todas las partes; «referente» queda con la parte que la tiene
    unsigned long ZonaDelJuego(const std::vector<std::wstring>& partes, std::wstring& referente);

    // ¿Esa zona pide marcar lo extraído?
    bool HayQueMarcar(unsigned long zona);

    // Marca un archivo extraído como venido de la misma zona que «referente». Conserva las fechas del archivo.
    bool Pasar(const std::wstring& extraido, const std::wstring& referente, unsigned long zona);
}
