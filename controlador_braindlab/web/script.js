// Se conecta al WebSocket /ws (ver servidor_web.c) y pinta cada mensaje JSON
// que manda tarea_climatizacion (app_main.c) cada ~2s. Reconecta solo si se
// corta -- no hay estado que mantener del lado del navegador aparte del
// historial de las chispas (sparklines), que es puramente de UI: se pierde
// si se recarga la pagina, no viene del backend.

const NOMBRES_CICLO = {
    0: "Reposo",
    1: "Normal",
    2: "Alta temperatura",
    3: "Bypass",
};

// Rango usado tanto para el arco del gauge como para el eje Y de las
// chispas de temperatura -- mismo rango para que un gauge lleno y una
// chispa "arriba de todo" cuenten la misma historia.
const TEMP_MIN = 0;
const TEMP_MAX = 50;
const VENTANA_CHISPA_MS = 60 * 60 * 1000; // 60 minutos, igual que los ejes -60m..0m

const elEstadoConexion = document.getElementById("estado-conexion");
const elFecha = document.getElementById("fecha-actual");
const elHora = document.getElementById("hora-actual");
const elUptime = document.getElementById("uptime");

let ultimoMensajeEn = null;
let gestorTuvoLecturaBuena = false;

// Gauge simple sin chispa (T. Gestor / H. Gestor) -- mismo dibujo de arco
// que actualizarGauge() pero con rango/unidad propios (humedad es 0-100%,
// no comparte escala con las temperaturas).
function actualizarGaugeSimple(prefijo, valor, min, max, unidad) {
    const pct = Math.max(0, Math.min(100, ((valor - min) / (max - min)) * 100));
    document.getElementById(prefijo + "-arco").setAttribute("stroke-dasharray", `${pct} 100`);

    const cx = 100, cy = 100, r = 80;
    const angulo = 180 - (pct / 100) * 180;
    const rad = (angulo * Math.PI) / 180;
    const mx = cx + r * Math.cos(rad);
    const my = cy - r * Math.sin(rad);
    const marcador = document.getElementById(prefijo + "-marcador");
    marcador.setAttribute("cx", mx.toFixed(1));
    marcador.setAttribute("cy", my.toFixed(1));

    document.getElementById(prefijo + "-valor").textContent = valor.toFixed(1) + unidad;
}

// ------------------------------------------------------------- gauges/chispas

const historiales = { op1: [], op2: [], iny1: [], iny2: [] };

function empujarHistorial(clave, valor) {
    const ahora = Date.now();
    const buf = historiales[clave];
    buf.push({ t: ahora, v: valor });
    const limite = ahora - VENTANA_CHISPA_MS;
    while (buf.length > 0 && buf[0].t < limite) buf.shift();
}

function actualizarGauge(prefijo, valor) {
    const pct = Math.max(0, Math.min(100, ((valor - TEMP_MIN) / (TEMP_MAX - TEMP_MIN)) * 100));
    document.getElementById(prefijo + "-arco").setAttribute("stroke-dasharray", `${pct} 100`);

    const cx = 100, cy = 100, r = 80;
    const angulo = 180 - (pct / 100) * 180;
    const rad = (angulo * Math.PI) / 180;
    const mx = cx + r * Math.cos(rad);
    const my = cy - r * Math.sin(rad);
    const marcador = document.getElementById(prefijo + "-marcador");
    marcador.setAttribute("cx", mx.toFixed(1));
    marcador.setAttribute("cy", my.toFixed(1));

    document.getElementById(prefijo + "-valor").textContent = valor.toFixed(1) + " °C";
}

function actualizarChispaGrande(prefijo, clave) {
    const buf = historiales[clave];
    const linea = document.getElementById(prefijo + "-linea");
    if (buf.length < 2) { linea.setAttribute("points", ""); return; }

    const ahora = Date.now();
    const puntos = buf.map((p) => {
        const x = 300 - ((ahora - p.t) / VENTANA_CHISPA_MS) * 300;
        const y = 60 - ((p.v - TEMP_MIN) / (TEMP_MAX - TEMP_MIN)) * 60;
        return `${x.toFixed(1)},${Math.max(0, Math.min(60, y)).toFixed(1)}`;
    });
    linea.setAttribute("points", puntos.join(" "));
}

function actualizarChispaChica(prefijo, clave) {
    const buf = historiales[clave];
    const linea = document.getElementById(prefijo + "-linea");
    if (buf.length < 2) { linea.setAttribute("points", ""); return; }

    const ahora = Date.now();
    const puntos = buf.map((p) => {
        const x = 200 - ((ahora - p.t) / VENTANA_CHISPA_MS) * 200;
        const y = 40 - ((p.v - TEMP_MIN) / (TEMP_MAX - TEMP_MIN)) * 40;
        return `${x.toFixed(1)},${Math.max(0, Math.min(40, y)).toFixed(1)}`;
    });
    linea.setAttribute("points", puntos.join(" "));
}

// ------------------------------------------------------------- actuadores/diagrama

function pintarActuador(id, encendido, manual = false) {
    const el = document.getElementById(id);
    el.classList.toggle("activo", encendido);
    el.querySelector("em").textContent = encendido ? "ON" : "OFF";

    let etiqueta = el.querySelector(".etiqueta-manual");
    if (manual) {
        if (!etiqueta) {
            etiqueta = document.createElement("small");
            etiqueta.className = "etiqueta-manual";
            el.querySelector("em").after(etiqueta);
        }
        etiqueta.textContent = "(manual)";
    } else if (etiqueta) {
        etiqueta.remove();
    }
}

// POST /control_aire o /control_bypass -- pide forzar a mano el valor
// OPUESTO al que se esta mostrando ahora mismo (toggle). Persiste hasta que
// la automatica cruce a otro ciclo (ver app_main.c) o hasta "Volver a
// automatico". No espera la respuesta para actualizar la UI -- el proximo
// mensaje del WS (dentro de 2s) trae el estado real aplicado.
function enviarControl(ruta, cuerpo) {
    fetch(ruta, { method: "POST", body: JSON.stringify(cuerpo) }).catch((err) => {
        console.error("control: fallo el POST a", ruta, err);
    });
}

function engancharControlesEditables() {
    document.querySelectorAll(".actuador.editable").forEach((el) => {
        el.addEventListener("click", () => {
            const encendidoActual = el.classList.contains("activo");
            if (el.dataset.tipo === "aire") {
                enviarControl("/control_aire", { indice: Number(el.dataset.indice), encendido: !encendidoActual });
            } else if (el.dataset.tipo === "bypass") {
                enviarControl("/control_bypass", { solicitado: !encendidoActual });
            } else if (el.dataset.tipo === "at") {
                enviarControl("/control_at", { activa: !encendidoActual });
            }
        });
    });

    document.getElementById("btn-volver-automatico").addEventListener("click", () => {
        enviarControl("/control_automatico", {});
    });
}

// BYPASS/AT del diagrama -- el color (amarillo/rojo) lo pone el CSS via la
// clase "activo" (.nodo-bypass.activo / .nodo-at.activo), esta funcion solo
// prende/apaga esa clase y el texto.
function pintarNodoEstado(id, encendido) {
    const el = document.getElementById(id);
    el.classList.toggle("activo", encendido);
    el.querySelector("span").textContent = encendido ? "ON" : "OFF";
}

// AA1-4 del diagrama -- si el bypass esta activo el A/C queda conectado
// directo y el estado individual de cada rele deja de importar, se muestra
// "BLOQUEADO" en vez de ON/OFF (ver conversacion).
function pintarNodoAire(id, encendido, bloqueadoPorBypass) {
    const el = document.getElementById(id);
    el.classList.toggle("bloqueado", bloqueadoPorBypass);
    el.querySelector("span").textContent = bloqueadoPorBypass ? "BLOQUEADO" : (encendido ? "ON" : "OFF");
}

function actualizarSensoresDiagrama(temperaturas) {
    for (let i = 0; i < 4; i++) {
        const el = document.getElementById("d-t" + (i + 1));
        el.textContent = typeof temperaturas[i] === "number" ? temperaturas[i].toFixed(1) + "°C" : "--";
    }
}

// ------------------------------------------------------------- estado del sistema

function actualizarEstadoSistema(datos) {
    const titulo = document.getElementById("estado-titulo");
    const escudo = document.getElementById("estado-escudo");
    const detalle = document.getElementById("estado-detalle");

    let clase = "ok", texto = "Operacion normal", nota = "Todos los sistemas funcionando correctamente.", icono = "✓";

    if (datos.alarma_at) {
        clase = "critical"; texto = "Alta temperatura"; icono = "⚠";
        nota = "Alarma de alta temperatura activa.";
    } else if (datos.ciclo === 3) {
        clase = "warning"; texto = "Bypass activo"; icono = "⇄";
        nota = "El equipo de A/A esta puenteado.";
    }

    titulo.textContent = texto;
    titulo.className = "estado-titulo " + clase;
    escudo.className = "estado-escudo " + clase;
    escudo.innerHTML = icono;
    detalle.textContent = nota;
}

// ------------------------------------------------------------- eventos y alertas

function segundosATexto(s) {
    if (s < 60) return `hace ${s}s`;
    if (s < 3600) return `hace ${Math.floor(s / 60)}min`;
    return `hace ${Math.floor(s / 3600)}h`;
}

function actualizarEventos(datos) {
    const lista = document.getElementById("lista-eventos");
    const eventos = [];

    if (datos.alarma_at) {
        eventos.push({ clase: "critical", icono: "⚠", titulo: "Alarma de alta temperatura activa", detalle: "Revisar el equipo -- se supero temp_at." });
    }
    if (datos.bypass_activo) {
        eventos.push({ clase: "warning", icono: "⇄", titulo: "Bypass fisico activo", detalle: "El A/A esta conectado directo, sin control." });
    }
    if (typeof datos.sd_estado === "number" && datos.sd_estado !== 0) {
        const e = SD_ESTADOS[datos.sd_estado] || { valor: "Desconocido", nota: "" };
        eventos.push({ clase: "warning", icono: "💾", titulo: "Memoria SD: " + e.valor, detalle: e.nota });
    }
    // Resto de los modulos (la SD ya se agrego arriba con su detalle).
    for (const m of MODULOS) {
        if (m.falla && m.ok(datos) === false) {
            const detalle = typeof m.falla === "function" ? m.falla(datos) : m.falla;
            eventos.push({ clase: "warning", icono: "⚡", titulo: "Falla: " + m.nombre, detalle });
        }
    }
    if (eventos.length === 0) {
        eventos.push({ clase: "ok", icono: "✓", titulo: "Sin fallas criticas", detalle: "El sistema opera dentro de parametros normales." });
    }
    eventos.push({ clase: "info", icono: "ℹ", titulo: "Ultima sincronizacion", detalle: "Datos actualizados correctamente.", tiempo: true });

    lista.innerHTML = "";
    for (const ev of eventos) {
        const li = document.createElement("li");
        li.className = ev.clase;
        const tiempoTxt = ev.tiempo && ultimoMensajeEn
            ? segundosATexto(Math.round((Date.now() - ultimoMensajeEn) / 1000))
            : "Ahora";
        li.innerHTML = `<span class="marca-evento">${ev.icono}</span><div><strong>${ev.titulo}</strong><span class="detalle-evento">${ev.detalle} &middot; ${tiempoTxt}</span></div>`;
        lista.appendChild(li);
    }
}

// ------------------------------------------------------------- pintar mensaje completo

function marcarConexion(conectado) {
    elEstadoConexion.innerHTML = `<i class="punto"></i> ${conectado ? "Sistema en linea" : "Sistema fuera de linea"}`;
    elEstadoConexion.className = "pill-estado " + (conectado ? "conectado" : "desconectado");
}

function formatearUptime(segundos) {
    const h = Math.floor(segundos / 3600);
    const m = Math.floor((segundos % 3600) / 60);
    const s = Math.floor(segundos % 60);
    return `Tiempo de actividad: ${h}h ${m}m ${s}s`;
}

// Medidor JSY-MK-333G por RS-485 (modbus_braindlab.c). Si el medidor no
// responde (energia_ok=false) las tarjetas vuelven a N/D en vez de dejar
// congelado el ultimo valor.
function pintarEnergia(datos) {
    const ok = !!datos.energia_ok;
    const v = datos.voltajes || [];
    const c = datos.corrientes || [];
    const valores = [
        ["v-r", v[0], 1, " V"],
        ["v-s", v[1], 1, " V"],
        ["v-t", v[2], 1, " V"],
        ["i-r", c[0], 2, " A"],
        ["i-s", c[1], 2, " A"],
        ["i-t", c[2], 2, " A"],
    ];
    for (const [id, valor, decimales, unidad] of valores) {
        const hay = ok && typeof valor === "number";
        document.getElementById(id + "-valor").textContent = hay ? valor.toFixed(decimales) + unidad : "N/D";
        document.getElementById(id + "-nota").textContent = hay ? "" : "Medidor RS-485 sin respuesta";
        document.getElementById(id + "-tarjeta").classList.toggle("nd", !hay);
    }
}

// Estado de la MicroSD del controlador (registro_sd.h): 0=OK, 1=sin
// tarjeta, 2=sin formato, 3=error de escritura.
const SD_ESTADOS = {
    0: { valor: "OK", nota: "Guardando un registro por minuto" },
    1: { valor: "Sin tarjeta", nota: "No hay tarjeta o no responde -- se reintenta cada minuto" },
    2: { valor: "Sin formato", nota: "La tarjeta no es FAT32 -- formatearla en la PC" },
    3: { valor: "Error escritura", nota: "Fallo al guardar (llena, dañada o se saco)" },
    4: { valor: "Formateando", nota: "Formateo en curso -- no sacar la tarjeta" },
};

function pintarSd(datos) {
    if (typeof datos.sd_estado !== "number") return;
    const e = SD_ESTADOS[datos.sd_estado] || { valor: "Desconocido", nota: "" };
    document.getElementById("sd-valor").textContent = e.valor;
    document.getElementById("sd-nota").textContent = e.nota;
    document.getElementById("sd-tarjeta").classList.toggle("nd", datos.sd_estado !== 0);
}

// Estado en tiempo real de cada modulo del controlador. 'ok' recibe el
// JSON del WS y devuelve true/false, o null si el campo no vino (firmware
// viejo) -- en ese caso el punto queda gris.
const MODULOS = [
    { nombre: "RTC", ok: (d) => (typeof d.rtc_ok === "boolean" ? d.rtc_ok && !d.rtc_detenido : undefined),
      falla: (d) => d.rtc_detenido
          ? "El reloj responde pero NO avanza -- oscilador detenido (revisar 5 V, pila/VBAT y cristal del DS1307)"
          : "Reloj (DS1307) sin respuesta -- registros a la SD detenidos" },
    { nombre: "ADC (T1-T4)", ok: (d) => d.adc_ok, falla: "ADS1115 sin respuesta por I2C -- temperaturas no validas" },
    // Solo si el ADS responde: sin NTC la entrada queda en ~3.3 V y el
    // firmware la rechaza -- es un sensor desconectado, no falla del chip.
    { nombre: "Sensores NTC",
      ok: (d) => (Array.isArray(d.ntc_ok) && d.adc_ok ? d.ntc_ok.every(Boolean) : undefined),
      falla: (d) => "Sin sensor (desconectado o en corto): " +
          d.ntc_ok.map((ok, i) => (ok ? null : "T" + (i + 1))).filter(Boolean).join(", ") },
    { nombre: "Sensor HR", ok: (d) => d.gestor_ok, falla: "AM2301A fallo la ultima lectura" },
    { nombre: "Ethernet", ok: (d) => d.eth_conectado, falla: "Cable Ethernet sin link" },
    { nombre: "Medidor", ok: (d) => d.energia_ok, falla: "Medidor RS-485 sin respuesta" },
    { nombre: "Memoria SD", ok: (d) => (typeof d.sd_estado === "number" ? d.sd_estado === 0 : undefined), falla: null },
];

function pintarModulos(datos) {
    const lista = document.getElementById("lista-modulos");
    lista.innerHTML = "";
    for (const m of MODULOS) {
        const v = m.ok(datos);
        const li = document.createElement("li");
        li.textContent = m.nombre;
        if (typeof v === "boolean") li.className = v ? "ok" : "falla";
        lista.appendChild(li);
    }
}

function pintar(datos) {
    ultimoMensajeEn = Date.now();
    pintarHoraRtc(datos.fecha_hora);

    // Canal sin NTC (o ADS sin respuesta) -> null: no se grafica el ultimo
    // valor viejo como si fuera real. Firmware viejo sin ntc_ok: se usa tal cual.
    const t = (datos.temperaturas || []).map((v, i) =>
        datos.adc_ok === false || (Array.isArray(datos.ntc_ok) && datos.ntc_ok[i] === false) ? null : v);
    const claves = ["op1", "op2", "iny1", "iny2"];
    claves.forEach((clave, i) => {
        if (typeof t[i] === "number") {
            empujarHistorial(clave, t[i]);
        } else {
            document.getElementById(clave + "-valor").textContent = "Sin sensor";
        }
    });
    if (typeof t[2] !== "number") document.getElementById("iny1-mini-valor").textContent = "-- °C";
    if (typeof t[3] !== "number") document.getElementById("iny2-mini-valor").textContent = "-- °C";

    if (typeof t[0] === "number") { actualizarGauge("op1", t[0]); actualizarChispaGrande("op1", "op1"); }
    if (typeof t[1] === "number") { actualizarGauge("op2", t[1]); actualizarChispaGrande("op2", "op2"); }
    if (typeof t[2] === "number") {
        actualizarGauge("iny1", t[2]);
        actualizarChispaGrande("iny1", "iny1");
        document.getElementById("iny1-mini-valor").textContent = t[2].toFixed(1) + " °C";
        actualizarChispaChica("iny1-mini", "iny1");
    }
    if (typeof t[3] === "number") {
        actualizarGauge("iny2", t[3]);
        actualizarChispaGrande("iny2", "iny2");
        document.getElementById("iny2-mini-valor").textContent = t[3].toFixed(1) + " °C";
        actualizarChispaChica("iny2-mini", "iny2");
    }
    actualizarSensoresDiagrama(t);

    if (datos.gestor_ok) {
        gestorTuvoLecturaBuena = true;
    }
    if (gestorTuvoLecturaBuena && typeof datos.temp_gestor === "number" && typeof datos.humedad_gestor === "number") {
        actualizarGaugeSimple("gestor-t", datos.temp_gestor, 0, 50, " °C");
        actualizarGaugeSimple("gestor-h", datos.humedad_gestor, 0, 100, " %");
        document.getElementById("d-gestor-t").textContent = datos.temp_gestor.toFixed(1) + "°C";
        document.getElementById("d-gestor-h").textContent = datos.humedad_gestor.toFixed(1) + "%";
    }
    const notaGestor = datos.gestor_ok ? "" : "Ultima lectura del AM2301A fallo -- mostrando el ultimo valor bueno";
    document.getElementById("gestor-t-nota").textContent = gestorTuvoLecturaBuena ? notaGestor : "Esperando lectura del AM2301A...";
    document.getElementById("gestor-h-nota").textContent = gestorTuvoLecturaBuena ? notaGestor : "Esperando lectura del AM2301A...";

    const aa = datos.salida_aire || [];
    const aaManual = datos.aire_manual || [];
    pintarActuador("act-aa1", !!aa[0], !!aaManual[0]);
    pintarActuador("act-aa2", !!aa[1], !!aaManual[1]);
    pintarActuador("act-aa3", !!aa[2], !!aaManual[2]);
    pintarActuador("act-aa4", !!aa[3], !!aaManual[3]);
    pintarActuador("act-at", !!datos.alarma_at, !!datos.alarma_at_manual);
    pintarActuador("act-bypass", !!datos.bypass_solicitado, !!datos.bypass_manual);

    const bypassActivo = !!datos.bypass_solicitado;
    pintarNodoAire("d-aa1", !!aa[0], bypassActivo);
    pintarNodoAire("d-aa2", !!aa[1], bypassActivo);
    pintarNodoAire("d-aa3", !!aa[2], bypassActivo);
    pintarNodoAire("d-aa4", !!aa[3], bypassActivo);
    pintarNodoEstado("d-at", !!datos.alarma_at);
    pintarNodoEstado("d-bypass", bypassActivo);

    pintarEnergia(datos);
    pintarSd(datos);
    pintarModulos(datos);
    actualizarEstadoSistema(datos);
    actualizarEventos(datos);

    if (typeof datos.uptime_s === "number") {
        elUptime.textContent = formatearUptime(datos.uptime_s);
    }
}

function conectarWs() {
    const ws = new WebSocket(`ws://${location.host}/ws`);

    ws.onopen = () => marcarConexion(true);
    ws.onclose = () => {
        marcarConexion(false);
        // Si el WS se cerro por sesion vencida (o el equipo reinicio y se
        // perdieron las sesiones), este fetch da 401 y auth.js manda al login.
        fetch("/api/sesion").catch(() => {});
        setTimeout(conectarWs, 2000); // reintenta -- el controlador puede reiniciar o el link caerse
    };
    ws.onerror = () => ws.close();

    ws.onmessage = (evento) => {
        try {
            pintar(JSON.parse(evento.data));
        } catch (err) {
            console.error("WS: mensaje invalido", err, evento.data);
        }
    };
}

// ------------------------------------------------------------- reloj


// Reloj del panel: muestra EXACTAMENTE la hora que manda el RTC (DS1307) en
// cada mensaje del WS -- sin interpolar, sin usar la hora de la PC/celular.
// Se actualiza cuando llega un mensaje (~cada 2 s), por eso los segundos
// avanzan de a saltos: es la hora real del equipo, tal cual.
// Se formatea a mano desde el texto "AAAA-MM-DD HH:MM:SS" (sin pasar por
// Date) para que ninguna zona horaria del navegador la pueda correr.
const DIAS_SEMANA = ["domingo", "lunes", "martes", "miercoles", "jueves", "viernes", "sabado"];
const MESES_NOMBRE = ["enero", "febrero", "marzo", "abril", "mayo", "junio", "julio",
                      "agosto", "septiembre", "octubre", "noviembre", "diciembre"];

function pintarHoraRtc(fechaHoraTexto) {
    const m = /^(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2})$/.exec(fechaHoraTexto || "");
    if (!m) {
        // "----" = RTC sin respuesta: no se inventa ninguna hora.
        elHora.textContent = "--:--:--";
        elFecha.textContent = "RTC sin respuesta";
        return;
    }
    const [, anio, mes, dia, hh, mm, ss] = m;
    const h = parseInt(hh, 10);
    const h12 = h % 12 === 0 ? 12 : h % 12;
    elHora.textContent = `${h12}:${mm}:${ss} ${h < 12 ? "a. m." : "p. m."}`;
    // Dia de la semana: calculo de calendario puro (UTC), no depende de la
    // zona horaria del navegador.
    const dsem = new Date(Date.UTC(+anio, +mes - 1, +dia)).getUTCDay();
    elFecha.textContent = `${DIAS_SEMANA[dsem]}, ${parseInt(dia, 10)} de ${MESES_NOMBRE[+mes - 1]} de ${anio}`;
}

function actualizarReloj() {
    // Re-renderiza la lista de eventos solo para refrescar el "hace Xs" de
    // la ultima sincronizacion -- no vuelve a pedir nada al backend.
    if (ultimoMensajeEn) {
        const li = document.querySelector("#lista-eventos li.info span.detalle-evento");
        if (li) {
            const seg = Math.round((Date.now() - ultimoMensajeEn) / 1000);
            li.textContent = `Datos actualizados correctamente. · ${segundosATexto(seg)}`;
        }
    }
}

marcarConexion(false);
actualizarReloj();
setInterval(actualizarReloj, 1000);
engancharControlesEditables();
conectarWs();
