// Comun a todas las paginas (se incluye en el <head>, antes que cualquier
// otro script). Todo corre en el navegador -- el ESP32 solo sirve el archivo.
//
//   1. Tema claro/oscuro: se aplica ANTES de dibujar la pagina (sin parpadeo).
//      Por defecto sigue al sistema; el boton del menu lo fija y se recuerda
//      en localStorage (por navegador, no se guarda nada en el equipo).
//   2. Sesion: cualquier fetch() con 401 manda al login; salir() la cierra;
//      aviso si la contrasena sigue siendo la de fabrica.
//   3. Menu lateral comun: solo en paginas con <body class="con-menu">
//      (index, historico, configurar). files.html y ota.html no lo llevan.
//      Angosto (iconos) por defecto, se abre al pasar el mouse por encima y
//      con el boton de fijar queda abierto. En celular se oculta y se abre
//      con el boton ☰.
//
// Avisa con el evento "temacambiado" en window para que las paginas que
// dibujan en <canvas> (historico) se redibujen con los colores nuevos.

(function () {
    const esLogin = location.pathname === "/login.html";

    // localStorage puede no estar disponible (modo privado, bloqueado) --
    // en ese caso simplemente no se recuerda nada.
    function leer(clave) {
        try { return localStorage.getItem(clave); } catch (e) { return null; }
    }
    function guardar(clave, valor) {
        try { localStorage.setItem(clave, valor); } catch (e) { /* sin memoria */ }
    }

    // ------------------------------------------------------------- tema

    const sistemaClaro = window.matchMedia ? window.matchMedia("(prefers-color-scheme: light)") : null;

    function temaActual() {
        const elegido = leer("tema");
        if (elegido === "claro" || elegido === "oscuro") return elegido;
        return sistemaClaro && sistemaClaro.matches ? "claro" : "oscuro";
    }

    function aplicarTema() {
        document.documentElement.setAttribute("data-tema", temaActual());
        const boton = document.getElementById("btn-tema");
        if (boton) {
            const claro = temaActual() === "claro";
            boton.querySelector(".nav-icono").innerHTML = claro ? "&#9790;" : "&#9728;";
            boton.querySelector(".nav-texto").textContent = claro ? "Tema oscuro" : "Tema claro";
        }
        window.dispatchEvent(new Event("temacambiado"));
    }

    // Ya mismo, con el <head> todavia cargando: la pagina se dibuja
    // directo con el tema correcto.
    document.documentElement.setAttribute("data-tema", temaActual());
    if (sistemaClaro && sistemaClaro.addEventListener) {
        // Si el usuario no eligio a mano, seguir los cambios del sistema.
        sistemaClaro.addEventListener("change", () => { if (!leer("tema")) aplicarTema(); });
    }

    window.cambiarTema = function () {
        guardar("tema", temaActual() === "claro" ? "oscuro" : "claro");
        aplicarTema();
        return false;
    };

    if (esLogin) {
        return; // el login solo necesita el tema
    }

    // ------------------------------------------------------------- sesion

    const fetchOriginal = window.fetch.bind(window);
    window.fetch = function (...args) {
        return fetchOriginal(...args).then((r) => {
            if (r.status === 401) {
                location.href = "/login.html";
            }
            return r;
        });
    };

    window.salir = function () {
        fetchOriginal("/logout", { method: "POST" }).finally(() => {
            location.href = "/login.html";
        });
        return false;
    };

    function avisoPorDefecto() {
        fetch("/api/sesion")
            .then((r) => (r.ok ? r.json() : null))
            .then((datos) => {
                if (!datos || !datos.por_defecto) return;
                const aviso = document.createElement("div");
                aviso.className = "aviso-password";
                aviso.innerHTML =
                    'La contrasena sigue siendo la de fabrica. ' +
                    '<a href="/configurar.html#seguridad">Cambiarla ahora</a>';
                const destino = document.querySelector(".contenido") || document.body;
                destino.prepend(aviso);
            })
            .catch(() => {});
    }

    // ------------------------------------------------------------- menu

    // Paginas del menu. files.html y ota.html quedan afuera a proposito
    // (solo entra quien conoce la ruta).
    const PAGINAS = [
        { href: "/", icono: "&#8962;", texto: "Inicio", rutas: ["/", "/index.html"] },
        { href: "/historico.html", icono: "&#9650;", texto: "Historico", rutas: ["/historico.html"] },
        { href: "/configurar.html", icono: "&#9881;", texto: "Configuracion", rutas: ["/configurar.html"] },
    ];

    function itemMenu(p) {
        const activo = p.rutas.includes(location.pathname) ? ' class="activo"' : "";
        return `<a href="${p.href}"${activo} title="${p.texto}">` +
               `<span class="nav-icono">${p.icono}</span><span class="nav-texto">${p.texto}</span></a>`;
    }

    function boton(id, icono, texto, accion) {
        return `<a href="#" id="${id}" onclick="return ${accion}" title="${texto}">` +
               `<span class="nav-icono">${icono}</span><span class="nav-texto">${texto}</span></a>`;
    }

    // Avisa a las paginas que dibujan segun el ancho (historico) que el
    // contenido cambio de tamanio -- al fijar/soltar el menu la ventana no
    // cambia, solo el margen del contenido.
    function avisarCambioAncho() {
        setTimeout(() => window.dispatchEvent(new Event("resize")), 220);
    }

    function armarMenu() {
        const body = document.body;
        if (!body.classList.contains("con-menu")) return;

        if (leer("menu-fijo") === "1") body.classList.add("menu-fijo");

        const aside = document.createElement("aside");
        aside.className = "sidebar";
        aside.id = "sidebar";
        aside.innerHTML =
            '<div class="marca"><img class="marca-logo" src="/logo.png" alt="BraindLab">' +
            '<img class="marca-icono" src="/logo.png" alt=""></div>' +
            '<nav class="nav-lateral">' + PAGINAS.map(itemMenu).join("") + "</nav>" +
            '<nav class="nav-lateral nav-pie">' +
            boton("btn-tema", "&#9728;", "Tema claro", "cambiarTema()") +
            boton("btn-fijar", "&#187;", "Fijar menu", "fijarMenu()") +
            boton("btn-salir", "&#10162;", "Salir", "salir()") +
            "</nav>";
        body.prepend(aside);

        // Celular: boton ☰ y fondo oscuro para cerrar tocando afuera.
        const hamburguesa = document.createElement("button");
        hamburguesa.className = "btn-hamburguesa";
        hamburguesa.setAttribute("aria-label", "Abrir menu");
        hamburguesa.innerHTML = "&#9776;";
        hamburguesa.addEventListener("click", () => body.classList.add("menu-abierto"));
        body.prepend(hamburguesa);

        const velo = document.createElement("div");
        velo.className = "menu-velo";
        velo.addEventListener("click", () => body.classList.remove("menu-abierto"));
        body.appendChild(velo);

        actualizarBotonFijar();
        aplicarTema(); // pinta el icono correcto del boton de tema
    }

    function actualizarBotonFijar() {
        const btn = document.getElementById("btn-fijar");
        if (!btn) return;
        const fijo = document.body.classList.contains("menu-fijo");
        btn.querySelector(".nav-icono").innerHTML = fijo ? "&#171;" : "&#187;";
        btn.querySelector(".nav-texto").textContent = fijo ? "Soltar menu" : "Fijar menu";
        btn.title = fijo ? "Soltar menu (se cierra solo)" : "Fijar menu abierto";
    }

    window.fijarMenu = function () {
        const fijo = document.body.classList.toggle("menu-fijo");
        guardar("menu-fijo", fijo ? "1" : "0");
        actualizarBotonFijar();
        avisarCambioAncho();
        return false;
    };

    function iniciar() {
        armarMenu();
        avisoPorDefecto();
    }

    if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", iniciar);
    } else {
        iniciar();
    }
})();
