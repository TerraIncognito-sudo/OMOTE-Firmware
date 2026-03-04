"""OMOTE Companion Webapp — FastAPI server."""

import asyncio
import json
import logging

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

import uvicorn

from serial_bridge import bridge

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

app = FastAPI(title="OMOTE Companion")


class ConnectRequest(BaseModel):
    port: str
    baud: int = 115200


# -- REST endpoints --------------------------------------------------------


@app.get("/api/ports")
def get_ports():
    return bridge.list_ports()


@app.post("/api/connect")
def connect(req: ConnectRequest):
    return bridge.connect(req.port, req.baud)


@app.post("/api/disconnect")
def disconnect():
    return bridge.disconnect()


@app.get("/api/status")
def get_status():
    return bridge.status()


# -- WebSocket endpoint ----------------------------------------------------


@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket):
    await ws.accept()
    bridge.add_client(ws)
    # Send current status on connect
    await ws.send_text(json.dumps({"type": "status", "data": bridge.status()}))
    try:
        while True:
            text = await ws.receive_text()
            try:
                msg = json.loads(text)
            except json.JSONDecodeError:
                continue
            if msg.get("type") == "command":
                content = msg.get("content", "")
                bridge.send_to_serial(content)
    except WebSocketDisconnect:
        pass
    finally:
        bridge.remove_client(ws)


# -- Startup / static files -----------------------------------------------


@app.on_event("startup")
async def startup():
    bridge.set_loop(asyncio.get_running_loop())


app.mount("/", StaticFiles(directory="static", html=True), name="static")

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8080)
