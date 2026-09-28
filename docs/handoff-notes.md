# Notas de traspaso (sesión con Copilot, 2026-09-28)

Contexto para retomar el trabajo en una conversación nueva.

## Hardware y repos

- Placa: Waveshare ESP32-S3-ePaper-3.97 (ESP32-S3R8, 8 MB PSRAM octal, 16 MB flash, AXP2101, ES8311, SSD1677 800x480).
- Docs oficiales: https://docs.waveshare.com/ESP32-S3-ePaper-3.97/Resources-And-Documents
- Upstream: https://github.com/alxv2016/folloup-sticky, rama `folloup-waveshare`. El release v0.01 es para reTerminal Sticky y **no** sirve en esta placa.
- Fork del usuario: https://github.com/ingmiguelfernando/folloup-waveshare (rama `folloup-waveshare`), clonado en `~/Documents/personal/folloup-waveshare`.
- En el Mac del usuario no hay ESP-IDF ni `gh`; se compila solo en GitHub Actions.

## Compilar y flashear

- `.github/workflows/build.yml`: ESP-IDF v5.5.4, target esp32s3. Se ejecuta en cada push a `folloup-waveshare` y también a mano (workflow_dispatch).
- Hace `cp sdkconfig.waveshare sdkconfig`, luego `idf.py build`, `idf.py size` (queda en el resumen del job) y `python -m esptool merge_bin`.
- El resultado es el artifact **firmware** (`followup-waveshare.bin`).
- Flasheo: https://espressif.github.io/esptool-js/ en Chrome o Edge, dirección `0x0`. Si no conecta, mantener BOOT pulsado mientras se enchufa el USB. No hace falta borrar la flash para actualizar (se conservan el Wi-Fi y la API key guardados en NVS).
- Logs: pestaña Console de esptool-js a 115200 (el USB Serial/JTAG está activado como consola secundaria).
- `esptool.py` no existe en el contenedor de CI (daba exit 127); hay que usar `python -m esptool`.

## Problemas encontrados y estado

1. **Reinicio en bucle usando solo `sdkconfig.defaults`**:
   - Error: `ESP_ERR_NO_MEM` en `InputCallbackDispatcher::Initialize` (`main/input_callback_dispatcher.cpp:84`); antes ya fallaba `Failed to create shutdown task`.
   - El autor dejó de versionar `sdkconfig` en el commit `f0b56e9`. Su último `sdkconfig` está en el commit `896b393` y con ese sí arranca.
   - Igualar en `sdkconfig.defaults` solo los valores de memoria del autor (ALWAYSINTERNAL=16384, RESERVE_INTERNAL=32768, main stack 3584, TASK_CREATE_ALLOW_EXT_MEM) **no bastó**.
   - Solución: `sdkconfig.waveshare` = `sdkconfig` del autor (`896b393`) más 3 cambios:
     - `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`
     - `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`
     - `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`
   - `sdkconfig.defaults` tiene cambios del usuario, pero en la práctica no influye porque el `sdkconfig` completo tiene prioridad.
2. **Access point sin DHCP**:
   - El iPhone y el Mac se asocian a `Followup-XXXXXX` (red abierta, portal en `http://192.168.4.1`), pero reciben `169.254.x.x` y el portal no carga.
   - Hipótesis (sin logs): RAM interna agotada. Los framebuffers ocupan ~96 KB a propósito (`components/epaper_panel/epaper_panel.cpp`), hay más de 20 tareas y los buffers de Wi-Fi están en RAM interna, así que se descartan los paquetes que llegan.
   - Arreglo aplicado: los buffers de Wi-Fi/LWIP y de mbedTLS pasan a PSRAM (commit `eb113b9`). **Pendiente de probar en la placa.**
   - Atajo mientras tanto: IP manual en el cliente (`192.168.4.2`, máscara `255.255.255.0`, router `192.168.4.1`).
3. **"Enable OTG" siempre falla** ("OTG failed"):
   - `components/storage_service/usb_storage_backend.cpp` iniciaba la SD a 40 MHz, sin pull-ups internas y sin fijar el slot. El montaje normal de la app usa slot 1, 20 MHz y pull-ups.
   - Corregido para que use lo mismo (commit `eb113b9`). **Pendiente de probar.**
   - Si el problema sigue, puede ser falta de memoria para TinyUSB.
   - Ojo: sin cable USB conectado sale otro aviso distinto ("no USB cable").

Build de `eb113b9`: éxito en CI.

## Uso del dispositivo

- Botones: BOOT (GPIO0: grabar manteniendo pulsado / confirmar), arriba/centro/abajo (GPIO4/5/6), PWR (tecla del AXP2101).
- PWR: pulsar ~1 s enciende; ~1 s con el equipo encendido abre la confirmación de apagado; 6 s fuerza el apagado. El botón sobrante probablemente es RESET.
- Configuración inicial desde el portal del access point:
  - API key de Gemini (https://aistudio.google.com/). No meterla en `sdkconfig*` porque el repo es público.
  - Zona horaria (por defecto `North_America_Eastern`).
  - Wi-Fi de 2.4 GHz, al final: al conectarse, el access point se apaga.

## Siguientes pasos

1. Flashear el artifact de `eb113b9` y probar el DHCP del access point y el OTG (con cable USB a un computador).
2. Si siguen fallando, capturar el log por USB y revisar si aparecen `Failed to start Wi-Fi backend`, `Captive DNS task create failed`, `SD init for USB mode failed` o `Install TinyUSB driver failed`.
3. Opcional: abrir un issue en upstream sobre el `NO_MEM` con `sdkconfig.defaults`.
