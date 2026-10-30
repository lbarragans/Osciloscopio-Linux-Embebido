import time
import math
from pathlib import Path
from typing import List
from fastapi import FastAPI, HTTPException
from pydantic import BaseModel
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles

from spi_driver import SPIDriver

app = FastAPI()

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# Bandera de simulación
USE_MOCK = False

seq_counter = 0
current_rate_idx = 1
RATES_HZ = [100, 1000, 10000, 40000]

# Inicializar Driver SPI
spi_hw = SPIDriver(bus=0, device=0)

if not USE_MOCK:
    spi_hw.open()

class ConfigPayload(BaseModel):
    rate_idx: int

def generar_frame_simulado(window: int = 256):
    global seq_counter
    seq_counter += 1
    t_now = time.time()
    
    a_samples: List[int] = []
    b_samples: List[int] = []
    fs = RATES_HZ[current_rate_idx]
    
    for i in range(window):
        t = t_now + (i / fs)
        val_a = int(2048 + 1500 * math.sin(2 * math.pi * 5 * t))
        val_b = int(2048 + 1200 * math.cos(2 * math.pi * 2 * t))
        a_samples.append(max(0, min(4095, val_a)))
        b_samples.append(max(0, min(4095, val_b)))
        
    return {
        "seq": seq_counter,
        "rate_idx": current_rate_idx,
        "fs_hz": fs,
        "window": window,
        "flags": 0,
        "a": a_samples,
        "b": b_samples
    }

@app.get("/api/scope/frame")
def get_frame():
    if USE_MOCK:
        return generar_frame_simulado()
    else:
        frame = spi_hw.capture_frame(window=256)
        if frame:
            return frame
        raise HTTPException(status_code=500, detail="Error de comunicación SPI con la ESP32-C6")

@app.post("/api/scope/config")
def set_config(config: ConfigPayload):
    global current_rate_idx
    if 0 <= config.rate_idx <= 3:
        current_rate_idx = config.rate_idx
        if not USE_MOCK:
            spi_hw.set_rate(current_rate_idx)
        return {"status": "ok", "rate_idx": current_rate_idx}
    raise HTTPException(status_code=400, detail="rate_idx inválido (debe ser de 0 a 3)")

# Servir archivos estáticos del dashboard
DASHBOARD_DIR = Path(__file__).parent.parent.parent / "dashboard"
if DASHBOARD_DIR.exists():
    app.mount("/", StaticFiles(directory=str(DASHBOARD_DIR), html=True), name="dashboard")