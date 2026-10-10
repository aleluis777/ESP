#!/usr/bin/env python3
"""Genera main/auth_root_fabrica.h con el hash de la contrasena maestra (root)
de fabrica. La contrasena NUNCA queda escrita en el codigo ni en el repo:
solo su hash con salt, igual que lo calcula auth_web.c.

Uso (desde la carpeta controlador_braindlab):
    python tools/generar_hash_root.py

Pide la contrasena dos veces (no se ve al escribirla) y sobreescribe
main/auth_root_fabrica.h. Despues compilar y flashear (o subir por OTA).

OJO: el hash de fabrica solo se usa mientras el equipo no tenga una
contrasena root propia guardada en NVS (es decir, si nunca se cambio desde
root.html, o si se borro la NVS). Un equipo al que ya se le cambio la
maestra sigue con la suya aunque se le cargue un firmware con otro hash.
"""

import getpass
import hashlib
import os
import secrets
import sys

# Tienen que coincidir con auth_web.c.
SALT_LEN = 16
ITERACIONES = 2048
LARGO_MINIMO = 12

SALIDA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "main", "auth_root_fabrica.h")


def calcular_hash(password: bytes, salt: bytes) -> bytes:
    # Vuelta 0: H(salt || password). Resto: H(anterior || salt).
    h = hashlib.sha256(salt + password).digest()
    for _ in range(1, ITERACIONES):
        h = hashlib.sha256(h + salt).digest()
    return h


def como_c(datos: bytes) -> str:
    return ", ".join(f"0x{b:02x}" for b in datos)


def main() -> int:
    pw = getpass.getpass("Contrasena maestra de fabrica: ")
    if len(pw) < LARGO_MINIMO:
        print(f"Muy corta: minimo {LARGO_MINIMO} caracteres (es la llave de todos los equipos).")
        return 1
    if len(pw.encode()) > 64:
        print("Muy larga: maximo 64 bytes.")
        return 1
    if getpass.getpass("Repetir: ") != pw:
        print("No coinciden.")
        return 1

    salt = secrets.token_bytes(SALT_LEN)
    h = calcular_hash(pw.encode(), salt)

    with open(SALIDA, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"""#pragma once

// GENERADO por tools/generar_hash_root.py -- no editar a mano.
// Hash de la contrasena maestra (usuario "root") de FABRICA. La contrasena
// no esta en ningun lado del codigo, solo este hash con salt (ver
// auth_web.c). Para cambiarla: volver a correr el script.

#include <stdint.h>

static const uint8_t AUTH_ROOT_FABRICA_SALT[16] = {{ {como_c(salt)} }};
static const uint8_t AUTH_ROOT_FABRICA_HASH[32] = {{ {como_c(h)} }};
""")
    print(f"Listo: {os.path.normpath(SALIDA)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
