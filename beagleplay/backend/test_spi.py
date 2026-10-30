import time
from spi_driver import SPIDriver

def probar_spi():
    print("=== PRUEBA DE COMUNICACIÓN SPI (MODO TEST) ===")
    
    # 1. Inicializar bus SPI en /dev/spidev0.0
    driver = SPIDriver(bus=0, device=0, speed_hz=1000000) # 1 MHz
    if not driver.open():
        print("❌ ERROR: No se pudo abrir /dev/spidev0.0")
        return

    print("✓ Bus SPI abierto correctamente en /dev/spidev0.0")

    # 2. Activar modo de prueba en la ESP32-C6 (cmd 0x05, arg 1)
    print("\nEnviando comando SET_TESTMODE (0x05)...")
    rx = driver._transact(cmd=0x05, arg=1, read_len=16)
    header = driver._parse_header(rx)

    if not header:
        print("❌ ERROR: No se recibió cabecera válida (Magic 0xE5 no detectado o Checksum erróneo).")
        print(f"Bytes recibidos: {[hex(b) for b in rx[:16]]}")
        driver.close()
        return

    print(f"✓ ¡Respuesta recibida! Magic: {hex(header['magic'])}, State: {header['state']}, Flags: {bin(header['flags'])}")

    # 3. Configurar tasa (1 kSa/s) y armar captura
    print("\nConfigurando SET_RATE (1 kSa/s) y armando captura...")
    driver.set_rate(1)
    
    frame = driver.capture_frame(window=256)

    if frame:
        print("\n🎉 ¡ÉXITO! COMUNICACIÓN SPI REAL CONFIRMADA")
        print(f"• Secuencia (seq): {frame['seq']}")
        print(f"• Frecuencia (fs_hz): {frame['fs_hz']} Hz")
        print(f"• Muestras recibidas: {len(frame['a'])} en CH A, {len(frame['b'])} en CH B")
        print(f"• Primeras 10 muestras CH A (Rampa): {frame['a'][:10]}")
        print(f"• Primeras 10 muestras CH B (Rampa): {frame['b'][:10]}")
    else:
        print("❌ ERROR: No se pudo completar la captura de trama.")

    driver.close()

if __name__ == "__main__":
    probar_spi()