# Pinout y conexiones

Conexión entre el puerto **mikroBUS de la BeaglePlay** (SPI maestro) y la **ESP32-C6-DevKitC-1** (SPI esclavo).

## Diagrama de conexión

![Conexión SPI entre la BeaglePlay y la ESP32-C6](img/conexion-spi.svg)

## Tabla de conexiones

| Señal | mikroBUS (pin) | BeaglePlay | ESP32-C6-DevKitC-1 | Dirección |
|---|---|---|---|---|
| SCK | 4 | SPI CLK | **GPIO6** (FSPICLK) | SBC → uC |
| MISO | 5 | SPI POCI | **GPIO2** (FSPIQ) | uC → SBC |
| MOSI | 6 | SPI PICO | **GPIO7** (FSPID) | SBC → uC |
| CS | 3 | SPI CS | **GPIO10** | SBC → uC |
| INT (handshake) | 15 | GPIO de entrada | **GPIO3** | uC → SBC |
| RST (opcional) | 2 | GPIO de salida | **EN** (reset) | SBC → uC |
| GND | 8 (izq.) o 9 (der.) | GND | **GND** | común |
| +3,3 V | 7 | 3,3 V | **no conectar** | — |
| +5 V | 10 | 5 V | **no conectar** | — |

**Numeración de pines mikroBUS usada en este proyecto.** La especificación de MikroElektronika solo define el orden y los nombres de la serigrafía, no numera los pines; el "1" es la marca del pin AN. Aquí se numera como un conector en U, de 1 a 16 (vista superior, con el logo mikroBUS arriba):

| Fila izquierda | Pin | | Fila derecha | Pin |
|---|---|---|---|---|
| AN | 1 | | PWM | **16** |
| RST | 2 | | **INT** | **15** |
| CS | 3 | | RX | 14 |
| SCK | 4 | | TX | 13 |
| MISO | 5 | | SCL | 12 |
| MOSI | 6 | | SDA | 11 |
| +3,3 V | 7 | | +5 V | 10 |
| GND | 8 | | GND | 9 |

La izquierda baja de 1 a 8 y la derecha sube de 9 (GND) a 16 (PWM). **Ante cualquier duda, guíese por el nombre de la serigrafía (CS, SCK, MISO, MOSI, INT, GND), no por el número.**

> Todas las señales son de **3,3 V**. No conectar el pin +5V (10) del mikroBUS a ningún GPIO. La ESP32-C6 se alimenta por su puerto USB; solo se comparte GND con la BeaglePlay.

## Entradas analógicas (ADC1)

![Conexión del generador de señales a la ESP32-C6](img/conexion-analogica.svg)

| Canal | Pin ESP32-C6 | Uso |
|---|---|---|
| CH A | **GPIO0** (ADC1_CH0) | Señal 1 del generador |
| CH B | **GPIO1** (ADC1_CH1) | Señal 2 del generador |

- Rango de entrada: **0 a ~3,1 V** (atenuación 12 dB). El ADC no mide voltajes negativos y puede dañarse por encima de 3,3 V.
- Configurar el generador de señales con **offset positivo** (por ejemplo, seno de 1,5 V de amplitud pico y 1,5 V de offset) y verificar con el osciloscopio/analizador antes de conectar.
- Recomendado (opcional): resistencia de 1 kΩ en serie en cada entrada para limitar corriente.
- Unir la tierra del generador con GND de la ESP32-C6.

## Por qué estos pines

- **GPIO6, GPIO7 y GPIO2** son los pines nativos (IO_MUX) de SPI2 en el ESP32-C6, necesarios para alcanzar velocidades altas como esclavo.
- El CS nativo es el GPIO16, que es el TX de la UART de consola/flasheo de la DevKitC-1; por eso el CS usa GPIO10 (matriz de GPIO).
- GPIO0 y GPIO1 son canales del ADC1 y no se solapan con el SPI. Se evitan los pines de arranque (GPIO4, 5, 8, 9, 15).

## En la BeaglePlay (por definir por el equipo del SBC)

- El puerto mikroBUS está gestionado por defecto por el driver `mikrobus`; para usar el SPI como `/dev/spidevX.Y` hay que deshabilitar ese driver o usar un overlay. **La ruta del dispositivo aún no está determinada.** Se obtiene con `ls /dev/spidev*` una vez configurado.
- Parámetros del maestro: **modo 0** (CPOL=0, CPHA=0), 8 bits por palabra, MSB primero.
- Tamaño máximo de transacción: 4096 bytes, igual al `bufsiz` por defecto de `spidev`.
- La línea INT (handshake) puede leerse con `libgpiod`. Si no se usa, esperar ≥ 2 ms entre transacciones.
