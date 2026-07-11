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

function pintar(d) {
  document.getElementById("corrida").textContent = d.activa
    ? `Corrida ${d.corrida}`
    : "Sin corrida activa";
  document.getElementById("dial1").textContent = d.dial1_mm.toFixed(3);
  document.getElementById("dial2").textContent = d.dial2_mm.toFixed(3);
  document.getElementById("peso").textContent = d.peso_N.toFixed(1);
  document.getElementById("tiempo").textContent = fmtTiempo(d.tiempo_ms);
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
