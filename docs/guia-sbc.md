# Guía para el equipo del SBC (BeaglePlay)

Para **Juan Camilo Giraldo** y **Daniela Barragán**. Resume lo que ya existe, lo que falta y qué verificar en la BeaglePlay. Lo marcado con ⚠️ **no está verificado** en la placa real: confírmenlo y corrijan este documento.

## 1. Estado actual

| Parte | Estado | Dónde |
|---|---|---|
| Firmware ESP32-C6 (SPI esclavo + ADC) | Compila, se flashea y arranca. Sin probar con un maestro SPI | `firmware/`, `release/` |
| Protocolo SPI | Definido y con pruebas unitarias en PC | [protocolo-spi.md](protocolo-spi.md) |
| Cableado mikroBUS ↔ ESP32-C6 | Asignado | [hardware-pinout.md](hardware-pinout.md) |
| Maestro SPI de referencia (Python) | Escrito, **nunca ejecutado en hardware** | `tools/spi_master_test.py` |
| Dashboard HTML (Chart.js) | Funciona con datos simulados; falta el backend | `dashboard/` |
| **Backend en el SBC** (lee SPI y expone la API) | **Por hacer** | — |
| Servidor apache/nginx configurado | **Por hacer** | — |

## 2. Qué hay que construir

```
ESP32-C6 ══SPI══► [backend en BeaglePlay] ──JSON──► nginx/apache ──HTTP──► navegador (dashboard/)
```

1. **Habilitar el SPI de la mikroBUS** como `/dev/spidevX.Y` (sección 4).
2. **Backend** (Python con `spidev`, o C) que cada 1–4 s: arma la secuencia ARM → espera READY → lee la trama → guarda la última ventana.
3. **API HTTP** que el dashboard ya espera (contrato en [dashboard/README.md](../dashboard/README.md)):
   - `GET /api/scope/frame` → `{seq, rate_idx, fs_hz, window, flags, a:[raw], b:[raw]}`
   - `POST /api/scope/config` con `{"rate_idx": 0..3}` → envía `SET_RATE` por SPI.
4. **nginx o apache** sirviendo `dashboard/` y haciendo proxy de `/api/` al backend.
5. **Medir** la tasa real de SPI (el enunciado pide estimar la teórica y verificarla experimentalmente).

Punto de partida del backend: `tools/spi_master_test.py` ya implementa el handshake, el checksum de comandos y la lectura de tramas. Léanlo antes de escribir el suyo.

## 3. Lo esencial del protocolo (resumen)

Detalle completo en [protocolo-spi.md](protocolo-spi.md).

- SPI **modo 0**, 8 bits, MSB primero. BeaglePlay = **maestro**; ESP32-C6 = **esclavo**.
- Comando de 4 bytes: `[cmd, arg_lo, arg_hi, chk]`, con `chk = cmd ^ lo ^ hi ^ 0x5A`.
- Comandos: `SET_RATE=1` (preset 0..3), `SET_WINDOW=2` (64..1020), `ARM=3`, `RESET=4`, `SET_TESTMODE=5`.
- Respuesta: encabezado de 16 bytes (magic `0xE5`, estado, flags, preset, ventana, seq, fs_hz…) y, si el estado es READY, las muestras del canal A y luego del B (uint16 little-endian).
- Presets: 0 = 100, 1 = 1.000, 2 = 10.000, 3 = 40.000 Sa/s por canal.
- Un comando surte efecto **después** de su transacción: se ve en el siguiente encabezado.
- Esperar **≥ 2 ms entre transacciones** o usar la línea INT (GPIO3).
- Trama completa = `16 + 4 × ventana` bytes, máx. 4096 (el `bufsiz` por defecto de spidev).
- Voltaje: `V ≈ raw / 4095 × 3,1`.

## 4. Contexto de la BeaglePlay

Datos generales de la placa (conocidos): SoC TI **AM625** (Cortex-A53), Linux (imagen Debian de beagleboard.org), un socket **mikroBUS** con lógica de **3,3 V**. El ESP32-C6 también es de 3,3 V, así que no se necesitan adaptadores de nivel. **Solo se comparte GND; no conectar 3V3/5V entre placas** (cada una se alimenta por su USB).

### Pines usados (mikroBUS)

| Señal | Pin mikroBUS | ESP32-C6 |
|---|---|---|
| CS | 3 | GPIO10 |
| SCK | 4 | GPIO6 |
| MISO | 5 | GPIO2 |
| MOSI | 6 | GPIO7 |
| INT (handshake) | 15 | GPIO3 |
| RST (opcional) | 2 | EN |
| GND | 8 ó 9 | GND |

Numeración en U (izquierda 1–8 hacia abajo, derecha 9–16 hacia arriba; ver tabla completa en [hardware-pinout.md](hardware-pinout.md)). La especificación mikroBUS no numera los pines, así que **guíense por el nombre de la serigrafía** y verifíquenlo contra el esquemático de la BeaglePlay antes de conectar. Fuentes: documentación y esquemático en <https://docs.beagleboard.org/boards/beagleplay/>.

### Habilitar `/dev/spidev` ⚠️

Por defecto el socket mikroBUS lo gestiona el driver `mikrobus` de BeagleBoard.org y puede no exponer `spidev`. Pasos para investigar en la placa:

```bash
cat /proc/device-tree/model                 # confirmar la placa
uname -r
ls /dev/spidev* /sys/bus/spi/devices/ 2>&1  # ¿ya existe algún spidev?
ls /boot/firmware/overlays | grep -i -E "spi|mikrobus"
cat /boot/firmware/extlinux/extlinux.conf   # dónde se cargan overlays
dmesg | grep -i -E "spi|mikrobus"
```

Caminos posibles (elijan el que funcione y **documenten el que usaron** en la sección 6):
- Un overlay de device tree que declare un nodo `spidev` en el SPI del socket mikroBUS y se cargue desde `extlinux.conf`.
- Configurar el puerto mikroBUS con el driver `mikrobus` usando un manifiesto de dispositivo SPI genérico.
- Mientras no haya spidev, pueden desarrollar el backend y la API contra datos simulados (como hace el dashboard) y conectar el SPI al final.

Una vez exista el dispositivo:

```bash
sudo apt install python3-spidev python3-pip
ls -l /dev/spidev*                # anoten la ruta (ej. /dev/spidev0.0)
sudo python3 tools/spi_master_test.py --dev /dev/spidevX.Y --test-mode
```

(Revisen `--help` del script para los argumentos exactos.)

## 5. Primera prueba de integración (orden sugerido)

1. Flashear la ESP32-C6 ([guia-equipo.md](guia-equipo.md)); el monitor serie debe mostrar `scope_link: SPI esclavo listo`.
2. Cablear según la tabla de la sección 4.
3. Con **modo test** (`SET_TESTMODE`) el ESP32 envía un patrón conocido: sirve para validar el SPI **sin** señal analógica.
4. Verificar encabezado: magic `0xE5`, checksum correcto, `seq` incrementando.
5. Probar los 4 presets y comprobar `fs_hz` en el encabezado.
6. Medir velocidad: subir `--speed` gradualmente (1, 5, 10, 20, 40 MHz) y registrar a cuál empiezan los errores. Ver tasas teóricas en [firmware.md](firmware.md).
7. Conectar el generador de señales (0–3,1 V, solo positivo) y comparar con el dashboard.

Límites teóricos para el informe: el esclavo ESP32-C6 admite hasta **40 MHz** (5 MB/s); el SPI del AM62x como maestro ⚠️ debe confirmarse en su hoja de datos (se citó 50 MHz). El flujo real a 40 kSa/s con 2 canales es solo ~160 kB/s.

## 6. Registrar lo que descubran

Completen esta tabla y hagan commit a `main` para que el resto tenga la información:

| Dato | Valor |
|---|---|
| Imagen/versión de la BeaglePlay | _pendiente_ |
| Ruta spidev | _pendiente_ |
| Método para habilitarlo (overlay / driver) | _pendiente_ |
| Velocidad SPI máx. sin errores | _pendiente_ |
| Servidor HTTP elegido (apache/nginx) y config | _pendiente_ |

## 7. Notas

- El dashboard puede probarse ya, sin hardware: `cd dashboard && python3 -m http.server 8080`.
- Requisito de refresco del dashboard: entre 0,25 y 1 Hz. Con el preset de 100 Sa/s una ventana de 256 muestras dura 2,56 s, así que ajusten el tamaño de ventana para que la captura quepa en el periodo de refresco.
- Dudas sobre el firmware: David Henao Rojas.
