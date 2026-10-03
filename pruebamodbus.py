# Espía RS-485: muestra en hexadecimal todo lo que llega al puerto.
# Uso:  py espia_rs485.py COM10 9600
import sys, time
import serial  # pip install pyserial
 
PUERTO = sys.argv[1] if len(sys.argv) > 1 else "COM10"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 9600
PAUSA = 0.02  # silencio (s) que separa una trama de otra
 
 
def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc
 
 
def crc_ok(frame):
    if len(frame) < 4:
        return False
    return crc16(frame[:-2]) == (frame[-2] | (frame[-1] << 8))
 
 
s = serial.Serial(PUERTO, BAUD, bytesize=8, parity="N", stopbits=1, timeout=0.005)
print(f"Escuchando {PUERTO} a {BAUD} 8N1... (Ctrl+C para salir)")
 
buf = b""
ultimo = time.time()
try:
    while True:
        data = s.read(256)
        ahora = time.time()
        if data:
            buf += data
            ultimo = ahora
        elif buf and ahora - ultimo > PAUSA:
            estado = "CRC OK" if crc_ok(buf) else "CRC MAL / basura"
            print(f"{time.strftime('%H:%M:%S')}  [{len(buf):3d} bytes]  "
                  f"{buf.hex(' ').upper()}   <- {estado}")
            buf = b""
except KeyboardInterrupt:
    s.close()
 