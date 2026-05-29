"""
Servidor Puente SSE Bidireccional — ESP32 SIM7670G <-> AWS EC2
==============================================================
Flujo:
  Cualquier ESP32  →  POST /publish        →  Encola en queue[target_id]
  Cualquier ESP32  →  GET  /subscribe?device_id=X  →  Lee de queue[X] via SSE
"""

import asyncio
import json
import logging
import time
from contextlib import asynccontextmanager
from typing import Optional

from fastapi import FastAPI, Request, HTTPException, Query
from fastapi.responses import StreamingResponse
from pydantic import BaseModel

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger("bridge")

# ── Una cola por dispositivo destino ─────────────────────────────────────────
QUEUE_MAXSIZE = 50
queues: dict[str, asyncio.Queue] = {}
last_seen: dict[str, float] = {}


def get_or_create_queue(device_id: str) -> asyncio.Queue:
    if device_id not in queues:
        queues[device_id] = asyncio.Queue(maxsize=QUEUE_MAXSIZE)
        log.info(f"📬 Cola creada para dispositivo '{device_id}'")
    return queues[device_id]


class Message(BaseModel):
    device_id: str           # Quién envía
    target_id: str           # ← NUEVO: a quién va dirigido el mensaje
    payload: str
    timestamp: Optional[float] = None


@asynccontextmanager
async def lifespan(app: FastAPI):
    log.info("🚀 Servidor puente bidireccional iniciado")
    yield
    log.info("🛑 Servidor puente detenido")


app = FastAPI(title="ESP32 SSE Bridge — Bidireccional", lifespan=lifespan)


@app.post("/publish", status_code=202)
async def publish(msg: Message, request: Request):
    client_ip = request.client.host if request.client else "desconocido"
    last_seen[msg.device_id] = time.time()

    if msg.timestamp is None:
        msg.timestamp = time.time()

    # Enrutar a la cola del DESTINATARIO, no del emisor
    queue = get_or_create_queue(msg.target_id)

    try:
        if queue.full():
            try:
                dropped = queue.get_nowait()
                log.warning(f"⚠️  Cola '{msg.target_id}' llena — descartando: {dropped}")
            except asyncio.QueueEmpty:
                pass
        queue.put_nowait(msg.dict())
    except Exception as e:
        log.error(f"Error al encolar: {e}")
        raise HTTPException(status_code=500, detail="Error interno al encolar")

    log.info(
        f"📨 {msg.device_id} → {msg.target_id} ({client_ip}): '{msg.payload[:60]}'"
    )
    return {
        "status": "queued",
        "target": msg.target_id,
        "queue_size": queue.qsize(),
    }


@app.get("/subscribe")
async def subscribe(
    request: Request,
    device_id: str = Query(..., description="ID del dispositivo suscriptor"),
):
    """
    Cada ESP32 se suscribe a SU PROPIA cola usando su device_id.
    Mantiene la conexión abierta recibiendo mensajes que otros le envíen.
    """
    last_seen[device_id] = time.time()
    client_ip = request.client.host if request.client else "desconocido"
    log.info(f"📡 '{device_id}' suscrito desde {client_ip}")

    queue = get_or_create_queue(device_id)

    async def event_generator():
        KEEPALIVE_INTERVAL = 20
        POLL_TIMEOUT = 25   # ligeramente > keep-alive para garantizar el ping

        try:
            while True:
                if await request.is_disconnected():
                    log.info(f"🔌 '{device_id}' desconectado")
                    break

                try:
                    msg_data = await asyncio.wait_for(
                        queue.get(), timeout=POLL_TIMEOUT
                    )
                    last_seen[device_id] = time.time()
                    event = f"data: {json.dumps(msg_data)}\n\n"
                    log.info(
                        f"📤 → '{device_id}': '{msg_data.get('payload', '')[:60]}'"
                    )
                    yield event

                except asyncio.TimeoutError:
                    yield ": keep-alive\n\n"
                    last_seen[device_id] = time.time()
                    log.debug(f"💓 Keep-alive → '{device_id}'")

        except asyncio.CancelledError:
            log.info(f"🔌 Stream cancelado para '{device_id}'")
        except Exception as e:
            log.error(f"❌ Error en stream de '{device_id}': {e}")

    headers = {
        "Content-Type": "text/event-stream",
        "Cache-Control": "no-cache",
        "Connection": "keep-alive",
        "X-Accel-Buffering": "no",
        "Transfer-Encoding": "chunked",
    }

    return StreamingResponse(
        event_generator(),
        media_type="text/event-stream",
        headers=headers,
    )


@app.get("/status")
async def status():
    now = time.time()
    return {
        "status": "ok",
        "active_queues": list(queues.keys()),
        "queue_sizes": {k: v.qsize() for k, v in queues.items()},
        "last_seen": {
            k: round(now - v, 1) for k, v in last_seen.items()
        },
    }