"""
ESP32 Chat System — Flask Application
Sistema de mensajería bidireccional para ESP32
"""
from flask import Flask, render_template, jsonify, request, Response
import sqlite3
import json
import time
from datetime import datetime
from threading import Lock
import queue

app = Flask(__name__)

# ── Database ──
DB_FILE = 'chat_messages.db'
db_lock = Lock()

# ── SSE Clients ──
sse_clients = []
message_queue = queue.Queue()


def init_db():
    """Inicializar base de datos SQLite"""
    with db_lock:
        conn = sqlite3.connect(DB_FILE)
        c = conn.cursor()
        c.execute('''
            CREATE TABLE IF NOT EXISTS messages (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                target_id TEXT NOT NULL,
                payload TEXT NOT NULL,
                timestamp REAL NOT NULL,
                created_at DATETIME DEFAULT CURRENT_TIMESTAMP
            )
        ''')
        c.execute('CREATE INDEX IF NOT EXISTS idx_messages_device ON messages(device_id)')
        c.execute('CREATE INDEX IF NOT EXISTS idx_messages_timestamp ON messages(timestamp DESC)')
        conn.commit()
        conn.close()
    print("[DB] Base de datos inicializada")


def get_db():
    """Obtener conexión a la base de datos"""
    conn = sqlite3.connect(DB_FILE)
    conn.row_factory = sqlite3.Row
    return conn


def save_message(device_id, target_id, payload, timestamp):
    """Guardar mensaje en la base de datos con hora de Colombia"""
    with db_lock:
        conn = get_db()
        c = conn.cursor()
        
        # Convertir timestamp a hora de Colombia
        from datetime import datetime, timezone, timedelta
        if timestamp:
            dt = datetime.fromtimestamp(timestamp, tz=timezone.utc)
            # Convertir a Colombia (UTC-5)
            colombia_tz = timezone(timedelta(hours=-5))
            dt_colombia = dt.astimezone(colombia_tz)
            created_at = dt_colombia.strftime('%Y-%m-%d %H:%M:%S')
        else:
            # Usar hora actual de Colombia
            colombia_tz = timezone(timedelta(hours=-5))
            dt_colombia = datetime.now(colombia_tz)
            created_at = dt_colombia.strftime('%Y-%m-%d %H:%M:%S')
            timestamp = dt_colombia.timestamp()
        
        c.execute(
            'INSERT INTO messages (device_id, target_id, payload, timestamp, created_at) VALUES (?, ?, ?, ?, ?)',
            (device_id, target_id, payload, timestamp, created_at)
        )
        message_id = c.lastrowid
        conn.commit()
        conn.close()
        return message_id


def get_recent_messages(limit=100):
    """Obtener mensajes recientes"""
    conn = get_db()
    c = conn.cursor()
    c.execute(
        'SELECT * FROM messages ORDER BY id DESC LIMIT ?',
        (limit,)
    )
    rows = c.fetchall()
    conn.close()
    
    messages = []
    for row in rows:
        messages.append({
            'id': row['id'],
            'device_id': row['device_id'],
            'target_id': row['target_id'],
            'payload': row['payload'],
            'timestamp': row['timestamp'],
            'created_at': row['created_at']
        })
    
    return list(reversed(messages))


def get_conversation(device1, device2, limit=50):
    """Obtener conversación entre dos dispositivos"""
    conn = get_db()
    c = conn.cursor()
    c.execute('''
        SELECT * FROM messages 
        WHERE (device_id = ? AND target_id = ?) 
           OR (device_id = ? AND target_id = ?)
        ORDER BY id DESC LIMIT ?
    ''', (device1, device2, device2, device1, limit))
    rows = c.fetchall()
    conn.close()
    
    messages = []
    for row in rows:
        messages.append({
            'id': row['id'],
            'device_id': row['device_id'],
            'target_id': row['target_id'],
            'payload': row['payload'],
            'timestamp': row['timestamp'],
            'created_at': row['created_at']
        })
    
    return list(reversed(messages))


def broadcast_to_sse(message_data):
    """Enviar mensaje a todos los clientes SSE conectados"""
    message_queue.put(message_data)


# ══════════════════════════════════════════
#  ROUTES
# ══════════════════════════════════════════

@app.route('/')
def index():
    """Página principal del dashboard"""
    return render_template('index.html')


@app.route('/api/messages/recent')
def api_recent_messages():
    """API: Obtener mensajes recientes"""
    limit = request.args.get('limit', 100, type=int)
    messages = get_recent_messages(limit=min(limit, 500))
    return jsonify({'messages': messages, 'count': len(messages)})


@app.route('/api/messages/conversation')
def api_conversation():
    """API: Obtener conversación entre dos dispositivos"""
    device1 = request.args.get('device1')
    device2 = request.args.get('device2')
    limit = request.args.get('limit', 50, type=int)
    
    if not device1 or not device2:
        return jsonify({'error': 'Faltan parámetros device1 y device2'}), 400
    
    messages = get_conversation(device1, device2, limit=min(limit, 200))
    return jsonify({'messages': messages, 'count': len(messages)})


@app.route('/api/publish', methods=['POST'])
def api_publish():
    """API: Recibir mensajes de ESP32 (compatibilidad con main.py)"""
    try:
        data = request.get_json()
        device_id = data.get('device_id')
        target_id = data.get('target_id')
        payload = data.get('payload')
        timestamp = data.get('timestamp', time.time())
        
        if not device_id or not target_id or not payload:
            return jsonify({'error': 'Faltan campos requeridos'}), 400
        
        # Guardar en base de datos
        message_id = save_message(device_id, target_id, payload, timestamp)
        
        # Broadcast a clientes web
        message_data = {
            'id': message_id,
            'device_id': device_id,
            'target_id': target_id,
            'payload': payload,
            'timestamp': timestamp,
            'created_at': datetime.now().isoformat()
        }
        broadcast_to_sse(message_data)
        
        return jsonify({
            'status': 'saved',
            'message_id': message_id
        }), 202
        
    except Exception as e:
        print(f"[ERROR] /api/publish: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/stream')
def api_stream():
    """SSE: Stream de mensajes en tiempo real"""
    def event_stream():
        while True:
            try:
                # Esperar mensaje con timeout
                message = message_queue.get(timeout=30)
                yield f"data: {json.dumps(message)}\n\n"
            except queue.Empty:
                # Keep-alive
                yield ": keep-alive\n\n"
            except GeneratorExit:
                break
    
    return Response(
        event_stream(),
        mimetype='text/event-stream',
        headers={
            'Cache-Control': 'no-cache',
            'Connection': 'keep-alive',
            'X-Accel-Buffering': 'no'
        }
    )


@app.route('/api/stats')
def api_stats():
    """API: Estadísticas generales"""
    conn = get_db()
    c = conn.cursor()
    
    c.execute('SELECT COUNT(*) FROM messages')
    total_messages = c.fetchone()[0]
    
    c.execute('SELECT COUNT(DISTINCT device_id) FROM messages')
    total_devices = c.fetchone()[0]
    
    c.execute('''
        SELECT device_id, COUNT(*) as count 
        FROM messages 
        GROUP BY device_id 
        ORDER BY count DESC
    ''')
    device_stats = [{'device_id': row[0], 'count': row[1]} for row in c.fetchall()]
    
    conn.close()
    
    return jsonify({
        'total_messages': total_messages,
        'total_devices': total_devices,
        'device_stats': device_stats
    })


@app.route('/api/esp32-status')
def api_esp32_status():
    """API: Proxy para consultar estado del servidor ESP32"""
    try:
        import requests
        response = requests.get('http://127.0.0.1:8000/status', timeout=2)
        return jsonify(response.json())
    except Exception as e:
        return jsonify({
            'status': 'error',
            'active_queues': [],
            'error': str(e)
        }), 500


if __name__ == '__main__':
    init_db()
    print("=" * 60)
    print("ESP32 Chat System - Web Dashboard")
    print("=" * 60)
    print("Servidor corriendo en: http://0.0.0.0:5000")
    print("=" * 60)
    app.run(host='0.0.0.0', port=5000, debug=True, threaded=True)
