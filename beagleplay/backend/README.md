# Backend - Osciloscopio BeaglePlay

Servidor centralizado en Python con **FastAPI** para la BeaglePlay. Encargado de exponer la API REST para la transmisión de muestras de la señal del osciloscopio, gestionar la configuración de la frecuencia de muestreo, ejecutar la comunicación SPI con la ESP32-C6 y servir los archivos estáticos del dashboard web.

---

## 🛠️ Arquitectura de Software

El servidor unifica el acceso web, el driver de hardware y la transmisión de datos bajo el mismo origen (puerto `8000`), evitando bloqueos por políticas CORS.

```
beagleplay/backend/
├── main.py        # Servidor principal FastAPI, rutas API y servidor estático
├── spi_driver.py  # Driver del bus SPI (Protocolo v1: cabeceras 16B, checksum XOR, Little-Endian)
└── README.md      # Documentación técnica del módulo backend
```

---

## 🔌 Driver SPI (`spi_driver.py`)

El módulo `SPIDriver` implementa la comunicación Full-Duplex en **Modo SPI 0** hacia la ESP32-C6 respetando la especificación del *Protocolo SPI versión 1*:

* **Trama de comando (4 bytes):** Envío de instrucciones (`NOP`, `SET_RATE`, `ARM`, etc.) con cálculo de checksum `cmd ^ arg_lo ^ arg_hi ^ 0x5A`.
* **Validación de respuesta:** Verificación del byte mágico (`0xE5`) y checksum de la cabecera de 16 bytes.
* **Desempaquetado de datos:** Conversión de bytes crudos Little-Endian a muestras `uint16` de 12 bits (0–4095) para los Canales A y B.
* **Tiempos de guarda:** Control de pausas (≥ 2 ms) entre transacciones SPI para estabilidad de enlace.

---

## 🛰️ Contrato de API Implementado

### 1. Obtener Ventana de Muestras
* **Ruta:** `GET /api/scope/frame`
* **Descripción:** Devuelve la última ventana de 256 muestras de 12 bits para los Canales A y B.
* **Respuesta JSON (`200 OK`):**
```json
{
  "seq": 5,
  "rate_idx": 1,
  "fs_hz": 1000,
  "window": 256,
  "flags": 0,
  "a": [2048, 2051, 2055],
  "b": [1020, 1018, 1015]
}
```

### 2. Configurar Frecuencia de Muestreo
* **Ruta:** `POST /api/scope/config`
* **Cuerpo JSON:**
```json
{
  "rate_idx": 1
}
```

* **Tabla de Presets (`rate_idx`):**

| `rate_idx` | Frecuencia de Muestreo (`fs_hz`) |
| :---: | :---: |
| `0` | 100 Sa/s |
| `1` | 1.000 Sa/s (1 kSa/s) |
| `2` | 10.000 Sa/s (10 kSa/s) |
| `3` | 40.000 Sa/s (40 kSa/s) |

---

## ⚙️ Modos de Operación (`USE_MOCK`)

En `main.py` se controla el origen de los datos mediante la bandera global:

* **`USE_MOCK = True` (Modo Simulación):** Genera matemático-discretamente ondas senoidales sintéticas para pruebas de interfaz sin necesidad del hardware de captura.
* **`USE_MOCK = False` (Modo SPI Real):** Inicializa `/dev/spidev0.0` mediante `spi_driver.py` para la adquisición física de datos desde la ESP32-C6.

---

## 💻 Ejecución del Servidor

```bash
cd beagleplay/backend
python3 -m uvicorn main:app --host 0.0.0.0 --port 8000
```

### Accesos
* **Dashboard Web:** `http://<IP_BEAGLEPLAY>:8000/`
* **Documentación Interactiva (Swagger UI):** `http://<IP_BEAGLEPLAY>:8000/docs`