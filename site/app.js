"use strict";
/* Cadence — site : onglets + branchement à la vitrine HTTP de l'invité QNX.
   Aucune dépendance externe. Le site fonctionne aussi sans JS (contenu statique) ;
   le panneau « live » est le seul élément dynamique. */

(function () {
  const $ = (id) => document.getElementById(id);

  /* ——— onglets ——— */
  const nav = $("nav");
  if (nav) {
    nav.addEventListener("click", (e) => {
      const b = e.target.closest("button[data-tab]");
      if (!b) return;
      nav.querySelectorAll("button").forEach((x) => x.classList.toggle("active", x === b));
      document.querySelectorAll("main .tab").forEach((s) => {
        s.classList.toggle("active", s.id === b.dataset.tab);
      });
    });
  }

  /* ——— la vitrine de l'invité QNX ———
     La vitrine est un petit serveur HTTP (stdlib Python) branché sur le moniteur
     QEMU de l'invité : /screen.png (screendump), /console (journal série),
     /key?k=… (sendkey). Elle tourne sur le poste qui héberge l'invité, donc en
     http:// local — jamais publiée sur Pages.

     VAR_LIVE_BASE : URL de base de la vitrine. Vide par défaut => panneau d'aide.
     On la lit depuis l'URL du site (?live=http://127.0.0.1:8080) pour pouvoir
     brancher une vitrine sans reconstruire le site. */
  const DEFAULT_LIVE = "";
  const params = new URLSearchParams(location.search);
  const BASE = (params.get("live") || DEFAULT_LIVE).replace(/\/+$/, "");

  const screen = $("liveScreen");
  const consoleEl = $("liveConsole");
  const note = $("liveNote");
  const age = $("liveAge");
  if (!screen || !consoleEl) return;

  function helpMode(why) {
    screen.setAttribute("src", "about:blank");
    consoleEl.textContent = "vitrine hors ligne — " + why;
    if (note) note.classList.remove("ok");
  }

  if (!BASE) {
    helpMode("aucune URL de vitrine configurée");
    return;
  }

  /* Sonde : la vitrine répond-elle ? (CORS : elle pose Access-Control-Allow-Origin: *) */
  async function probe() {
    const r = await fetch(BASE + "/console", { cache: "no-store", mode: "cors" });
    if (!r.ok) throw new Error("statut " + r.status);
    return r.text();
  }

  let live = false;
  async function tick() {
    try {
      const txt = await probe();
      if (!live) {
        live = true;
        screen.setAttribute("src", BASE + "/");
        if (note) {
          note.classList.add("ok");
          note.innerHTML =
            "<h4>Branchement actif</h4><p>L'écran et la console viennent de l'invité QNX 8.0 " +
            "réel, via le moniteur QEMU. Rafraîchissement toutes les 2 s.</p>";
        }
      }
      consoleEl.textContent = txt;
      consoleEl.scrollTop = consoleEl.scrollHeight;
      if (age) age.textContent = "console fraîche · " + BASE;
    } catch (e) {
      live = false;
      helpMode(BASE + " injoignable (" + e.message + ")");
    }
  }
  tick();
  setInterval(tick, 2000);

  /* ——— injection de touches ——— */
  const keys = $("liveKeys");
  if (keys) {
    keys.addEventListener("click", (e) => {
      const b = e.target.closest("button[data-k]");
      if (!b || !BASE) return;
      fetch(BASE + "/key?k=" + encodeURIComponent(b.dataset.k), { mode: "cors" }).catch(() => {});
    });
  }
})();
