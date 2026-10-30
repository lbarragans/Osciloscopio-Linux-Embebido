import time
import struct
from typing import Optional, Dict, Any

MAGIC_HEADER = 0xE5

class SPIDriver:
    def __init__(self, bus: int = 0, device: int = 0, speed_hz: int = 1000000):
        self.bus = bus
        self.device = device
        self.speed_hz = speed_hz
        self.spi = None

    def open(self) -> bool:
        """Inicializa la interfaz SPI de la BeaglePlay en Modo 0."""
        try:
            import spidev
            self.spi = spidev.SpiDev()
            self.spi.open(self.bus, self.device)
            self.spi.max_speed_hz = self.speed_hz
            self.spi.mode = 0
            return True
        except Exception as e:
            print(f"[SPI Driver Error] No se pudo abrir la interfaz SPI: {e}")
            self.spi = None
            return False

    def close(self):
        """Cierra la conexión del bus SPI."""
        if self.spi:
            self.spi.close()

    def _build_command(self, cmd: int, arg: int = 0) -> list:
        """Construye la trama de comando de 4 bytes con Checksum XOR."""
        arg_lo = arg & 0xFF
        arg_hi = (arg >> 8) & 0xFF
        checksum = cmd ^ arg_lo ^ arg_hi ^ 0x5A
        return [cmd, arg_lo, arg_hi, checksum]

    def _transact(self, cmd: int, arg: int = 0, read_len: int = 16) -> bytes:
        """Envía comando y lee la respuesta en una transacción Full-Duplex."""
        if not self.spi:
            return b""
        
        tx = self._build_command(cmd, arg)
        if read_len > 4:
            tx += [0x00] * (read_len - 4)
            
        time.sleep(0.002)  # Respetar tiempo de guarda ≥ 2ms según protocolo
        rx = self.spi.xfer2(tx)
        return bytes(rx)

    def _parse_header(self, raw_header: bytes) -> Optional[Dict[str, Any]]:
        """Valida Magic Byte (0xE5) y Checksum XOR de la cabecera de 16 bytes."""
        if len(raw_header) < 16:
            return None

        # Formato Little-Endian (<BBBBHHIBBBB)
        magic, state, flags, rate_idx, window, seq, fs_hz, n_channels, bits, fw_ver, chk = struct.unpack(
            "<BBBBHHIBBBB", raw_header[:16]
        )

        if magic != MAGIC_HEADER:
            return None

        calc_chk = 0
        for b in raw_header[:15]:
            calc_chk ^= b

        if calc_chk != chk:
            return None

        return {
            "magic": magic,
            "state": state,       # 0: IDLE, 1: ARMED, 2: READY, 3: ERROR
            "flags": flags,
            "rate_idx": rate_idx,
            "window": window,
            "seq": seq,
            "fs_hz": fs_hz,
            "n_channels": n_channels,
            "bits": bits,
            "fw_version": fw_ver
        }

    def set_rate(self, rate_idx: int) -> bool:
        """Envía el comando SET_RATE (0x01) para cambiar el preset de muestreo."""
        rx = self._transact(cmd=0x01, arg=rate_idx, read_len=16)
        header = self._parse_header(rx)
        return header is not None and header["flags"] & 0x02 == 0

    def capture_frame(self, window: int = 256) -> Optional[Dict[str, Any]]:
        """
        Ejecuta la secuencia completa de lectura:
        1. ARM (0x03)
        2. Polling con NOP (0x00) hasta state == READY (2)
        3. Lectura de 16 + (4 * window) bytes
        4. Desempaquetado de muestras de 12 bits para Canales A y B
        """
        # 1. Armar captura
        self._transact(cmd=0x03, arg=0, read_len=16)

        # 2. Consultar estado mediante polling
        header = None
        for _ in range(50):  # Reintentos de espera
            rx = self._transact(cmd=0x00, arg=0, read_len=16)
            header = self._parse_header(rx)
            if header and header["state"] == 2:  # state == READY
                break
            time.sleep(0.005)

        if not header or header["state"] != 2:
            return None

        # 3. Leer cabecera + bloque de datos
        total_bytes = 16 + (4 * window)
        rx_full = self._transact(cmd=0x00, arg=0, read_len=total_bytes)

        header = self._parse_header(rx_full[:16])
        if not header or header["state"] != 2:
            return None

        # 4. Desempaquetar datos
        data_bytes = rx_full[16:total_bytes]
        actual_window = header["window"]

        # uint16 little-endian: primero A, luego B
        samples = struct.unpack(f"<{2 * actual_window}H", data_bytes[:4 * actual_window])
        a_samples = list(samples[:actual_window])
        b_samples = list(samples[actual_window:2 * actual_window])

        return {
            "seq": header["seq"],
            "rate_idx": header["rate_idx"],
            "fs_hz": header["fs_hz"],
            "window": header["window"],
            "flags": header["flags"],
            "a": a_samples,
            "b": b_samples
        }