# Continuar el proyecto en otro PC

Guía para retomar FolloUp (Waveshare ESP32-S3-ePaper-3.97) desde VS Code en un
PC personal. Todo el código ya está en GitHub; este PC no hace falta.

- Repo: https://github.com/ingmiguelfernando/folloup-waveshare
- Rama: `folloup-waveshare`
- Último commit al escribir esto: `b5e06de` (lector de Biblia)

## 1. Estado actual

| Commit | Qué incluye | Probado en la placa |
|---|---|---|
| `eb113b9` / `4111a83` | `sdkconfig.waveshare` (config del autor), fix de RAM interna para el access point, fix de inicio de SD en OTG | AP y portal: sí. OTG: no |
| `c6e3a24` | Wi-Fi: WPA3 H2E, contraseñas de 64 caracteres, logs de escaneo y motivo de desconexión; aviso "Saved. Gemini not ready" | Wi-Fi conectado: sí |
| `68920c7` | Actualización por Wi-Fi (OTA) desde GitHub Releases (`ota_service`, `ota_prompt_runtime`) | No (CI publicó `build-10`) |
| `8f769b3` | `scripts/bible_json_to_sd.py` (convierte la Biblia JSON al formato de la SD) | Solo en PC |
| `c1265af` | Fuentes con acentos (Latin-1) y lectura de UTF-8 | Solo prueba en PC |
| `b5e06de` | Página "Bible" en el menú principal | Solo prueba en PC |

Los tres últimos commits **nunca se han compilado para el ESP32**. Si la build de
Actions falla, mira la sección 8.

## 2. Preparar el PC

1. Instala [Git](https://git-scm.com/), [VS Code](https://code.visualstudio.com/)
   con la extensión GitHub Copilot, y Python 3.8 o superior.
2. Clona el repo y cambia a la rama:
   ```bash
   git clone https://github.com/ingmiguelfernando/folloup-waveshare.git
   cd folloup-waveshare
   git checkout folloup-waveshare
   ```
3. Abre la carpeta en VS Code. No hace falta instalar ESP-IDF: la compilación la
   hace GitHub Actions en cada push.
4. Navegador para flashear: Chrome o Edge (esptool-js usa Web Serial).

## 3. Obtener el firmware

1. Entra a **Actions** en GitHub y abre la ejecución del commit `b5e06de`.
2. Si terminó en verde:
   - descarga el artifact **firmware** (trae `followup-waveshare.bin`,
     `followup-app.bin`, `.elf` y `version.txt`);
   - también queda una Release `build-N` con los mismos binarios.
3. Si falló, copia el error del paso de build y sigue la sección 8.

## 4. Flashear por cable (una última vez)

La placa todavía tiene un firmware sin OTA, así que esta vez hay que usar cable.

1. Abre https://espressif.github.io/esptool-js/ y conecta la placa por USB-C.
2. Pulsa **Connect**. Si no la detecta, mantén BOOT pulsado mientras enchufas el
   USB.
3. Escribe `followup-waveshare.bin` en la dirección **`0x0`**. No borres la
   flash: se conservan el Wi-Fi y la API key de Gemini.
4. Reinicia y abre la pestaña **Console** a 115200 para ver el log.

En el log del arranque deberías ver, entre otras:

```text
OtaService: Running firmware ci-N-xxxxxxx (ota_0) pending_verify=0 url=https://github.com/...
WifiService: Before Wi-Fi start: internal DMA free=... largest=... min=...
```

Pega la línea `internal DMA free=... largest=...` a Copilot: dice cuánto margen
de RAM interna queda.

## 5. Preparar la microSD

1. Formato **FAT32**.
2. Genera la Biblia en el PC (el texto RVR1960 tiene copyright: **nunca** lo
   subas al repo; `.gitignore` ya bloquea `bible/`, `/sd/` y `RVR*.json`):
   ```bash
   python3 scripts/bible_json_to_sd.py RVR1960_vid_149.json --out ./sd
   ```
   El JSON lo descargas tú desde el enlace que ya tenías. El script imprime la
   lista de caracteres especiales que usa el texto; guárdala por si falta
   alguno en las fuentes.
3. Copia la carpeta `sd/bible` a la **raíz** de la microSD. Debe quedar
   `bible/rvr1960/index.tsv`, `GEN.txt`, `GEN.idx`, etc.
4. Inserta la SD en la placa y reinicia.

## 6. Probar la Biblia

Entra desde el menú principal → **Bible**.

| Botón | Acción |
|---|---|
| Giratorio arriba / abajo | Página anterior / siguiente (cruza capítulos y libros) |
| OK (FN o toque corto de BOOT) | Menú: Next chapter, Previous chapter, Go to book, Go to chapter, Text size, About, Exit |
| Mantener BOOT | Grabar (igual que en el resto de la app) |
| PWR corto | Pantalla de bloqueo con hora y fecha |

Qué revisar:

- Que "Génesis", "creación" y los signos `¿` `¡` se vean bien (fuentes Latin-1).
- Que el menú principal muestre las 6 opciones sin tapar la barra inferior.
- Al salir y volver a entrar, que retome en la misma página.
- Cada 6 páginas hace un refresco completo (parpadeo) para limpiar el fantasma.
- En el log: `BibleService: Loaded ... (66 books) from /sdcard/bible/rvr1960`.

Si ves `No Bible found on the SD card`, la carpeta no está en la raíz o la SD no
montó (mira `StorageService` / `SdCard` en el log).

## 7. Probar la actualización por Wi-Fi (OTA)

1. Con la placa conectada al Wi-Fi, haz cualquier cambio pequeño y haz push (por
   ejemplo, una línea en este archivo).
2. Espera a que Actions termine y publique la Release `build-N+1`.
3. Reinicia la placa o desconecta y reconecta el Wi-Fi. Unos 30 s después de
   conectar debería aparecer **"Update to ci-...?" → Install now / Later**.
4. Tras "Install now" verás "Updating 20%…", luego "Update installed. Restarting".
5. En el log del nuevo arranque: `pending_verify=1` y luego
   `Confirmed new firmware after network connect`.

Si una versión nueva rompe el Wi-Fi, no se confirma y al siguiente reinicio vuelve
sola a la anterior. Si eliges "Later", no vuelve a preguntar por esa versión
hasta el siguiente reinicio.

## 8. Si la build de Actions falla

Los últimos commits solo se comprobaron con `clang` en el Mac, no con el
compilador del ESP32. Pide a Copilot:

> La build de GitHub Actions falla con este error: <pega el error>. Lee AGENTS.md
> y docs/app-architecture.md y corrígelo sin cambiar el comportamiento.

Archivos más probables:

- `components/ota_service/ota_service.cpp` (API de `esp_https_ota`, `esp_app_desc.h`)
- `components/bible_service/bible_service.cpp`
- `main/bible_page_runtime.cpp`, `main/bible_page_coordinator.cpp`
- `components/epaper_ui/bitmap_font.cpp`, `font_renderer.cpp`

## 9. Contexto para Copilot en el otro PC

La memoria de Copilot de este PC no viaja. Empieza la conversación con algo así:

> Lee AGENTS.md, docs/app-architecture.md, docs/handoff-notes.md,
> docs/waveshare-epaper-hardware-spec.md y docs/continue-on-another-pc.md antes
> de proponer cambios. No ejecutes builds locales; se compila en GitHub Actions.

Datos clave que conviene recordar (también están en los docs):

- Compilar siempre con `sdkconfig.waveshare`; `sdkconfig.defaults` solo
  reinicia en bucle con `ESP_ERR_NO_MEM`.
- La RAM interna (512 KB compartidos IRAM/DRAM) es el límite real; los 8 MB de
  PSRAM no sirven para buffers DMA ni para el beacon del access point.
- **No regenerar** `components/epaper_ui/generated_epaper_fonts.cpp`: el
  generador actual da métricas distintas. Los acentos van en
  `generated_epaper_font_extensions.cpp`
  (`scripts/generate_epaper_font_extensions.py`, solo macOS).
- Textos de la interfaz en **inglés** por ahora; el contenido (Biblia) en español.
- `main/app_shell.cpp` es solo orquestación; la lógica va en servicios o runtimes.
- El texto bíblico nunca se sube al repo.

## 10. Siguientes pasos (pendientes)

1. **Validar en la placa**: Biblia, fuentes con acentos, OTA y OTG (con la placa
   conectada directo a un computador).
2. **Rediseño de la interfaz** (propuesto):
   - inicio con cuadrícula de iconos: Capturar (grabar / transcribir / resumir),
     Notas, Tareas, Seguimiento, Biblioteca (libros), Biblia, Audios, Fotos,
     Asistente, Ajustes;
   - navegación: arriba/abajo recorren, OK abre, mantener abajo = atrás en todas
     las pantallas, mantener BOOT = grabar;
   - barra inferior con pistas según la pantalla («Back | Select | < | >») en vez
     de la barra de iconos;
   - mantener la pantalla de bloqueo actual (hora, fecha, día).
3. **Lector de libros**: `.txt` primero (reutilizando la paginación de la Biblia),
   EPUB después.
4. **Sensor de temperatura y humedad (SHTC3)**: notas en
   `docs/waveshare-epaper-hardware-spec.md` §9.2.
5. **Sincronizar notas, tareas y audio con el iPhone por Wi-Fi**: API local
   autenticada con token, `followup.local` (mDNS) y la app Atajos.
6. **Portal Wi-Fi**: responder antes de reiniciar el Wi-Fi, arrancar el AP en el
   canal del router, límites de tiempo en `fetch`, contraseña en el AP.
7. **Revisar el fork [mach1na/folloup](https://github.com/mach1na/folloup)**, que
   dice mejorar la RAM y la batería.
8. **Xiaozhi**: es un firmware completo y pesado; lo realista es como firmware
   aparte en otra partición, lo que exige rediseñar particiones junto con OTA.

## 11. Problemas conocidos y soluciones

| Síntoma | Causa / solución |
|---|---|
| Reinicio en bucle con `ESP_ERR_NO_MEM` | Se compiló sin `sdkconfig.waveshare` |
| `wifi:alloc eb len=752 ... fail` + `LoadProhibited` | Sin RAM interna para el AP; no activar `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` |
| El móvil recibe `169.254.x.x` | DHCP del AP sin memoria; mismo origen |
| `run -> init (0xf00)` y `RECONNECT_EXHAUSTED` | Contraseña de Wi-Fi incorrecta (motivo 15) |
| "Saved. Gemini not ready" | API key no validada; busca `Gemini authentication failed` |
| No hay transcripciones | Falta SD o no se eligió Note/Task/Idea tras grabar |
| El portal se queda en "Conectando" | Al conectar, el Wi-Fi se reinicia y corta al móvil; recarga la página tras ~60 s |
