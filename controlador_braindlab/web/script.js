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
    if (!datos.eth_conectado) {
        eventos.push({ clase: "warning", icono: "⚡", titulo: "Cable Ethernet desconectado", detalle: "Ultimo estado de link conocido: sin conexion." });
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

function pintar(datos) {
    ultimoMensajeEn = Date.now();
    actualizarDesfaseServidor(datos.fecha_hora);

    const t = datos.temperaturas || [];
    empujarHistorial("op1", t[0]);
    empujarHistorial("op2", t[1]);
    empujarHistorial("iny1", t[2]);
    empujarHistorial("iny2", t[3]);

    if (typeof t[0] === "number") { actualizarGauge("op1", t[0]); actualizarChispaGrande("op1", "op1"); }
    if (typeof t[1] === "number") { actualizarGauge("op2", t[1]); actualizarChispaGrande("op2", "op2"); }
    if (typeof t[2] === "number") { document.getElementById("iny1-valor").textContent = t[2].toFixed(1) + " °C"; actualizarChispaChica("iny1", "iny1"); }
    if (typeof t[3] === "number") { document.getElementById("iny2-valor").textContent = t[3].toFixed(1) + " °C"; actualizarChispaChica("iny2", "iny2"); }
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

// ------------------------------------------------------------- reloj y sidebar

const formateadorFecha = new Intl.DateTimeFormat("es-PE", { weekday: "long", year: "numeric", month: "long", day: "numeric" });
const formateadorHora = new Intl.DateTimeFormat("es-PE", { hour: "numeric", minute: "2-digit", second: "2-digit", hour12: true });

// Desfase entre la hora del RTC (DS1307, ver rtc_braindlab.c) y el reloj del
// navegador -- se recalcula cada vez que llega un mensaje de WS con
// fecha_hora valida. Entre mensajes (el WS manda cada ~2s), actualizarReloj()
// sigue tickeando cada 1s sumando este desfase al reloj local, para no
// depender de la hora de la PC/celular que abre la pagina.
let desfaseServidorMs = null;

function actualizarDesfaseServidor(fechaHoraTexto) {
    if (!fechaHoraTexto || fechaHoraTexto === "----") return; // RTC no disponible todavia
    const fecha = new Date(fechaHoraTexto.replace(" ", "T"));
    if (isNaN(fecha.getTime())) return;
    desfaseServidorMs = fecha.getTime() - Date.now();
}

function actualizarReloj() {
    const ahora = desfaseServidorMs !== null ? new Date(Date.now() + desfaseServidorMs) : new Date();
    elFecha.textContent = formateadorFecha.format(ahora);
    elHora.textContent = formateadorHora.format(ahora);

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

const sidebar = document.getElementById("sidebar");
document.getElementById("btn-colapsar").addEventListener("click", () => {
    sidebar.classList.toggle("colapsado");
});

marcarConexion(false);
actualizarReloj();
setInterval(actualizarReloj, 1000);
engancharControlesEditables();
conectarWs();
