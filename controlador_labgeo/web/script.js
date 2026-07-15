// Se conecta al WebSocket del controlador (/ws) y pinta cada mensaje que
// llega. El formato del JSON lo define app_main.c del lado del firmware:
// {"corrida":N,"activa":true|false,"dial1_mm":F,"dial2_mm":F,"peso_N":F,"tiempo_ms":N}

function fmtTiempo(ms) {
  const totalS = Math.floor(ms / 1000);
  const h = Math.floor(totalS / 3600);
  const m = Math.floor((totalS % 3600) / 60);
  const s = totalS % 60;
  const pad = (n) => String(n).padStart(2, "0");
  return `${pad(h)}:${pad(m)}:${pad(s)}`;
}

// Un solo boton para las dos corridas -- el texto depende del "estado" que
// manda el firmware (ver app_main.c, estado_ensayo_t). La web NUNCA decide
// el estado por su cuenta, solo lo refleja -- asi no se desincroniza si hay
// mas de una pestana abierta.
const ESTADOS_BOTON_ENSAYO = {
  0: "Iniciar Corrida 1",
  1: "Detener Corrida 1",
  2: "Iniciar Corrida 2",
  3: "Detener Corrida 2",
  4: "Reiniciar Todo",
};

function actualizarBotonEnsayo(estado) {
  const btn = document.getElementById("btn-avanzar-ensayo");
  btn.textContent = ESTADOS_BOTON_ENSAYO[estado] ?? "Iniciar Corrida 1";
}

// Pinta solo los campos que vengan en 'd' -- mientras se van sumando
// sensores de a uno (ver app_main.c), el WS todavia no manda el esquema
// completo (por ejemplo, hoy solo llega celda_crudo/peso_n, sin los
// diales), y no tiene sentido tratar eso como un error.
function pintar(d) {
  if (d.activa !== undefined) {
    document.getElementById("corrida").textContent = d.activa
      ? `Corrida ${d.corrida}`
      : "Sin corrida activa";
  }
  if (d.dial1_mm !== undefined) {
    document.getElementById("dial1").textContent = d.dial1_mm.toFixed(4);
  }
  if (d.dial2_mm !== undefined) {
    document.getElementById("dial2").textContent = d.dial2_mm.toFixed(4);
  }
  if (d.peso_N !== undefined) {
    document.getElementById("peso").textContent = d.peso_N.toFixed(1);
  } else if (d.peso_n !== undefined) {
    // Formato del diagnostico actual (solo la celda conectada todavia).
    document.getElementById("peso").textContent = d.peso_n.toFixed(1);
  }
  if (d.tiempo_ms !== undefined) {
    document.getElementById("tiempo").textContent = fmtTiempo(d.tiempo_ms);
  }
  if (d.estado !== undefined) {
    actualizarBotonEnsayo(d.estado);
  }
}

function conectar() {
  const badge = document.getElementById("estado-conexion");
  const ws = new WebSocket(`ws://${location.hostname}/ws`);

  ws.onopen = () => {
    badge.textContent = "Conectado";
    badge.className = "badge badge-on";
  };

  ws.onclose = () => {
    badge.textContent = "Desconectado";
    badge.className = "badge badge-off";
    setTimeout(conectar, 2000); // reintenta solo
  };

  ws.onerror = () => ws.close();

  ws.onmessage = (evt) => {
    try {
      pintar(JSON.parse(evt.data));
    } catch (e) {
      console.error("JSON invalido del WS:", evt.data);
    }
  };
}

conectar();

// ---------------------------------------------------------------------------
// Modal de calibracion de la celda de carga -- habla con los endpoints de
// servidor_web.c: GET /calibracion.json (valores guardados), POST
// /calibrar_cero y POST /calibrar_maximo (ver app_main.c del lado del
// firmware para la logica real de calibracion, esto solo dispara los pedidos).
// ---------------------------------------------------------------------------

function mostrarMensajeCalibracion(texto, esError) {
  const el = document.getElementById("cal-mensaje");
  el.textContent = texto;
  el.className = esError ? "error" : "ok";
}

function cargarSeteoActual() {
  fetch("/calibracion.json")
    .then((r) => {
      if (!r.ok) throw new Error("todavia no se calibro nada");
      return r.json();
    })
    .then((d) => {
      document.getElementById("cal-offset").textContent = d.celda.offset.toFixed(2);
      document.getElementById("cal-pendiente").textContent = d.celda.pendiente.toFixed(4);
    })
    .catch(() => {
      document.getElementById("cal-offset").textContent = "sin calibrar";
      document.getElementById("cal-pendiente").textContent = "sin calibrar";
    });
}

function iniciarCalibracion() {
  const modal = document.getElementById("modal-calibracion");
  const btnAbrir = document.getElementById("btn-abrir-calibracion");
  const btnCerrar = document.getElementById("btn-cerrar-calibracion");
  const btnCero = document.getElementById("btn-calibrar-cero");
  const btnPeso = document.getElementById("btn-calibrar-peso");

  btnAbrir.addEventListener("click", () => {
    document.getElementById("cal-mensaje").textContent = "";
    modal.classList.remove("oculto");
    cargarSeteoActual();
  });

  btnCerrar.addEventListener("click", () => modal.classList.add("oculto"));

  btnCero.addEventListener("click", () => {
    fetch("/calibrar_cero", { method: "POST" })
      .then((r) => {
        if (!r.ok) throw new Error();
        mostrarMensajeCalibracion("Cero calibrado.", false);
        cargarSeteoActual();
      })
      .catch(() => mostrarMensajeCalibracion("No se pudo calibrar el cero.", true));
  });

  btnPeso.addEventListener("click", () => {
    const pesoN = parseFloat(document.getElementById("input-peso-n").value);
    if (isNaN(pesoN) || pesoN === 0) {
      mostrarMensajeCalibracion("Ingresa un peso de referencia valido (distinto de cero).", true);
      return;
    }

    fetch("/calibrar_maximo", {
      method: "POST",
      body: JSON.stringify({ peso_n: pesoN }),
    })
      .then((r) => {
        if (!r.ok) throw new Error();
        mostrarMensajeCalibracion("Pendiente calibrada.", false);
        cargarSeteoActual();
      })
      .catch(() => mostrarMensajeCalibracion("No se pudo calibrar con ese peso.", true));
  });
}

iniciarCalibracion();

// ---------------------------------------------------------------------------
// Modal de configuracion de red -- GET /sistema.json (seccion "red") y
// POST /configurar_red. Por ahora solo GUARDA los valores (ver app_main.c,
// on_configurar_red) -- todavia no se aplican al W5500, eso es un paso
// pendiente aparte.
// ---------------------------------------------------------------------------

function mostrarMensajeRed(texto, esError) {
  const el = document.getElementById("red-mensaje");
  el.textContent = texto;
  el.className = esError ? "error" : "ok";
}

function cargarRedActual() {
  fetch("/sistema.json")
    .then((r) => {
      if (!r.ok) throw new Error("todavia no hay sistema.json");
      return r.json();
    })
    .then((d) => {
      if (!d.red) return;
      document.getElementById("input-red-ip").value = d.red.ip || "";
      document.getElementById("input-red-gateway").value = d.red.gateway || "";
      document.getElementById("input-red-mascara").value = d.red.mascara || "";
    })
    .catch(() => {
      // Sin config.json todavia -- se dejan los placeholders con los
      // valores por defecto, no es un error real.
    });
}

function iniciarConfigRed() {
  const modal = document.getElementById("modal-red");
  const btnAbrir = document.getElementById("btn-abrir-red");
  const btnCerrar = document.getElementById("btn-cerrar-red");
  const btnGuardar = document.getElementById("btn-guardar-red");

  btnAbrir.addEventListener("click", () => {
    document.getElementById("red-mensaje").textContent = "";
    modal.classList.remove("oculto");
    cargarRedActual();
  });

  btnCerrar.addEventListener("click", () => modal.classList.add("oculto"));

  btnGuardar.addEventListener("click", () => {
    const ip = document.getElementById("input-red-ip").value.trim();
    const gateway = document.getElementById("input-red-gateway").value.trim();
    const mascara = document.getElementById("input-red-mascara").value.trim();

    if (!ip || !gateway || !mascara) {
      mostrarMensajeRed("Completa los tres campos (IP, gateway y mascara).", true);
      return;
    }

    fetch("/configurar_red", {
      method: "POST",
      body: JSON.stringify({ ip, gateway, mascara }),
    })
      .then((r) => {
        if (!r.ok) throw new Error();
        mostrarMensajeRed("Configuracion de red guardada (todavia no aplicada).", false);
      })
      .catch(() => mostrarMensajeRed("No se pudo guardar la configuracion de red.", true));
  });
}

iniciarConfigRed();

// ---------------------------------------------------------------------------
// Boton unico de la corrida -- solo dispara POST /avanzar_ensayo. El texto
// del boton lo actualiza actualizarBotonEnsayo() cuando llega el "estado"
// por WS (ver pintar()); este handler no cambia el texto el mismo, para no
// mostrar algo que despues el firmware podria contradecir.
// ---------------------------------------------------------------------------

function iniciarBotonEnsayo() {
  document.getElementById("btn-avanzar-ensayo").addEventListener("click", () => {
    fetch("/avanzar_ensayo", { method: "POST" }).catch(() => {
      // Si falla el POST, el boton se queda como estaba -- el proximo
      // mensaje de WS (~200ms) va a confirmar el estado real de todos modos.
    });
  });
}

iniciarBotonEnsayo();
