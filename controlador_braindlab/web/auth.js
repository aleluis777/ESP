// Comun a todas las paginas protegidas (se incluye antes que cualquier otro
// script). Hace tres cosas:
//   1. Si cualquier fetch() responde 401 (sesion vencida o reinicio del
//      equipo), manda al login.
//   2. salir(): cierra la sesion -- lo usan los links "Salir".
//   3. Si la contrasena sigue siendo la de fabrica, muestra un aviso arriba.

(function () {
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
                aviso.style.cssText =
                    "background:var(--warning);color:#0f172a;padding:0.5rem 1rem;" +
                    "font-size:0.9rem;text-align:center;font-weight:600";
                aviso.innerHTML =
                    'La contrasena sigue siendo la de fabrica. ' +
                    '<a href="/configurar.html#seguridad" style="color:#0f172a">Cambiarla ahora</a>';
                document.body.prepend(aviso);
            })
            .catch(() => {});
    }

    if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", avisoPorDefecto);
    } else {
        avisoPorDefecto();
    }
})();
