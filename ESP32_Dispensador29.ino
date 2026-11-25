#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>
#include <ESPmDNS.h>
const char *SSID = "TU_WIFI";
const char *PASSWORD = "TU_PASSWORD";
WebServer server(80);
Preferences prefs;
struct Porcion {
  int minuto;
  int gramos;
};
struct Plan {
  int n;
  Porcion p[4];
  int ultimo[4];
};
Plan plan = {1, {{420, 50}, {840, 50}, {1200, 50}, {1320, 50}}, {0, 0, 0, 0}};
struct ConfigMotor {
  int pasos, abrir, cerrar, sentido;
};
ConfigMotor motor = {1024, 10, 15, -1};
bool motorValido(const ConfigMotor &c) {
  return c.pasos >= 1 && c.pasos <= 4096 &&
         c.abrir >= 10 && c.abrir <= 15 &&
         c.cerrar >= 10 && c.cerrar <= 15 &&
         (c.sentido == -1 || c.sentido == 1);
}
String linea, nivel = "ERROR", mensaje = "Esperando Arduino";
float peso = 0, distancia = -1;
bool ocupado = false, bascula = false, referencia = false, pendiente = false;
unsigned long ultimoDato = 0, ultimoEnvio = 0, ultimoCheck = 0;
bool recibido = false;
const char HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="es">

<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Dispensador</title>
  <style>
    :root {
      --bg: #f1f5f9;
      --fg: #172b3b;
      --card-bg: #ffffff;
      --card-border: #dce4ea;
      --muted: #526575;
      --input-border: #bdcbd5;
      --btn-bg: #146b70;
      --nav-btn-bg: #334155;
      --warn: #a34b00;
      color-scheme: light dark
    }

    @media (prefers-color-scheme: dark) {
      :root {
        --bg: #11171d;
        --fg: #e6edf3;
        --card-bg: #1a232b;
        --card-border: #2c3a44;
        --muted: #9fb0bd;
        --input-border: #3a4b57;
        --btn-bg: #1f8a90;
        --nav-btn-bg: #41566a;
        --warn: #e8a559
      }
    }

    * {
      box-sizing: border-box
    }

    body {
      margin: 0;
      background: var(--bg);
      color: var(--fg);
      font: 16px system-ui
    }

    main {
      max-width: 850px;
      margin: auto;
      padding: 20px
    }

    h1 {
      font-size: clamp(24px, 5vw, 34px)
    }

    section,
    article {
      background: var(--card-bg);
      border: 1px solid var(--card-border);
      border-radius: 14px;
      padding: 18px;
      margin: 12px 0
    }

    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
      gap: 12px
    }

    .grid article {
      margin: 0
    }

    label {
      display: block;
      margin: 10px 0 4px
    }

    input,
    select,
    button {
      font: inherit;
      padding: 12px;
      border-radius: 8px;
      border: 1px solid var(--input-border);
      width: 100%;
      background: var(--card-bg);
      color: var(--fg)
    }

    button {
      background: var(--btn-bg);
      color: white;
      border: 0;
      cursor: pointer;
      margin: 5px 0
    }

    button:disabled {
      opacity: .5;
      cursor: not-allowed
    }

    nav {
      display: flex;
      flex-wrap: wrap;
      gap: 8px
    }

    nav button {
      background: var(--nav-btn-bg)
    }

    .big {
      font-size: 28px;
      font-weight: 700
    }

    small {
      color: var(--muted)
    }

    #msg {
      overflow-wrap: anywhere
    }

    .warn {
      color: var(--warn)
    }

    [hidden] {
      display: none !important
    }
  </style>
</head>

<body>
  <main>
    <h1>Dispensador automático</h1>
    <p id="reloj">Sincronizando reloj...</p>
    <div class="grid">
      <article><small>Tolva</small>
        <div class="big" id="nivel">--</div><span id="dist">--</span>
      </article>
      <article><small>Peso actual</small>
        <div class="big" id="peso">--</div><span id="estado">--</span>
      </article>
    </div>
    <p id="msg" role="status"></p>
    <nav><button onclick="vista(false)">Programación</button><button onclick="vista(true)">Panel de control</button></nav>
    <section id="programa">
      <h2>Porciones diarias</h2><label>Número de porciones</label><select id="n" onchange="mostrar()">
        <option>1</option>
        <option>2</option>
        <option>3</option>
        <option>4</option>
      </select>
      <div id="porciones"></div><button onclick="guardar()">Guardar programación</button><small>Los horarios se guardan incluso si se reinicia el ESP32. No se recuperan automáticamente porciones perdidas.</small>
    </section>
    <section id="panel" hidden>
      <h2>Motor</h2><label>Medios pasos (1–4096)</label><input id="pasos" type="number" min="1" max="4096" value="5"><label>Velocidad (10–15 RPM)</label><input id="rpm" type="number" min="10" max="15" value="12">
      <div class="grid"><button class="control" onclick="mover(-1)">← Izquierda</button><button class="control" onclick="mover(1)">Derecha →</button></div>
      <p class="warn">Ajusta físicamente la compuerta hasta que quede cerrada y confirma la referencia antes de dispensar.</p><button class="control" onclick="cmd('ref')">Confirmar posición cerrada</button>
      <h2>Configuración automática del motor</h2>
      <label>Medios pasos de apertura</label><input id="apertura" type="number" min="1" max="4096" value="1024">
      <label>Velocidad de apertura (RPM)</label><input id="rpmAbrir" type="number" min="10" max="15" value="10">
      <label>Velocidad de cierre (RPM)</label><input id="rpmCerrar" type="number" min="10" max="15" value="15">
      <label>Sentido de apertura</label><select id="sentido"><option value="-1">Izquierda</option><option value="1">Derecha</option></select>
      <button onclick="guardarMotor()">Guardar configuración del motor</button>
      <small>La prueba manual usa los valores visibles; las dosis programadas usan los guardados. No modifica las flechas de ajuste.</small>
      <p id="msgMotor" role="status"></p>
      <h2>Báscula</h2>
      <p>Retira el contenido del plato y espera que el peso se estabilice.</p><button class="control" onclick="cmd('tara')">Realizar tara</button><label>Peso conocido (g)</label><input id="conocido" type="number" min="1" max="5000" value="100"><button class="control" onclick="cmd('cal',{g:el('conocido').value})">Calibrar con peso conocido</button>
      <h2>Prueba manual</h2><label>Cantidad (g)</label><input id="manual" type="number" min="5" max="1000" value="20"><button class="control" onclick="pruebaManual()">Dispensar prueba</button>
    </section>
  </main>
  <script>
    const el = id => document.getElementById(id);

    function vista(p) {
      el('panel').hidden = !p;
      el('programa').hidden = p
    }
    for (let i = 0; i < 4; i++) {
      el('porciones').insertAdjacentHTML('beforeend', `<article id="p${i}"><b>Porción ${i+1}</b><div class="grid"><div><label>Hora</label><input type="time" id="h${i}" value="07:00"></div><div><label>Gramos</label><input type="number" id="g${i}" min="5" max="1000" value="50"></div></div></article>`)
    }

    function mostrar() {
      for (let i = 0; i < 4; i++) el('p' + i).hidden = i >= +el('n').value
    }
    async function request(url, data) {
      let r = await fetch(url, data ? {
        method: 'POST',
        body: new URLSearchParams(data)
      } : {
        cache: 'no-store'
      });
      if (!r.ok) throw Error(await r.text());
      return r
    }
    async function cargar() {
      try {
        let p = await (await request('/plan')).json();
        el('n').value = p.n;
        p.p.forEach((v, i) => {
          el('h' + i).value = v.h;
          el('g' + i).value = v.g
        });
        mostrar()
      } catch (e) {
        el('msg').textContent = e.message
      }
    }
    async function cargarMotor() {
      try {
        let m = await (await request('/motor')).json();
        el('apertura').value = m.pasos;
        el('rpmAbrir').value = m.abrir;
        el('rpmCerrar').value = m.cerrar;
        el('sentido').value = m.sentido;
      } catch (e) { el('msgMotor').textContent = e.message; }
    }
    async function guardarMotor() {
      try {
        el('msgMotor').textContent = await (await request('/motor', {
          pasos: el('apertura').value, abrir: el('rpmAbrir').value,
          cerrar: el('rpmCerrar').value, sentido: el('sentido').value
        })).text();
      } catch (e) { el('msgMotor').textContent = e.message; }
    }
    async function guardar() {
      let d = {
        n: el('n').value
      };
      for (let i = 0; i < +d.n; i++) {
        d['h' + i] = el('h' + i).value;
        d['g' + i] = el('g' + i).value
      }
      try {
        el('msg').textContent = await (await request('/plan', d)).text()
      } catch (e) {
        el('msg').textContent = e.message
      }
    }
    function pruebaManual() {
      cmd('prueba', {
        g: el('manual').value,
        pasos: el('apertura').value,
        abrir: el('rpmAbrir').value,
        cerrar: el('rpmCerrar').value,
        sentido: el('sentido').value
      });
    }
    async function cmd(a, d = {}) {
      try {
        el('msg').textContent = await (await request('/cmd', {
          a,
          ...d
        })).text()
      } catch (e) {
        el('msg').textContent = e.message
      }
    }

    function mover(s) {
      cmd('mover', {
        pasos: s * +el('pasos').value,
        rpm: el('rpm').value
      })
    }
    async function estado() {
      try {
        let s = await (await request('/estado')).json();
        el('reloj').textContent = s.hora;
        el('nivel').textContent = s.online ? s.nivel : 'SIN CONEXIÓN';
        el('nivel').style.color = ({
          LLENO: '#198344',
          MEDIO: '#bd6900',
          BAJO: '#bd6900',
          VACIO: '#bd2222',
          ERROR: '#bd2222'
        })[s.nivel] || '#555';
        el('dist').textContent = s.online && s.dist >= 0 ? s.dist.toFixed(1) + ' cm' : '--';
        el('peso').textContent = s.online && s.bascula ? s.peso.toFixed(2) + ' g' : '--';
        el('estado').textContent = !s.online ? 'Arduino desconectado' : s.ocupado ? 'En movimiento / dispensando' : !s.ref ? 'Falta referencia cerrada' : !s.bascula ? 'Falta tara o sensor' : 'Disponible';
        if (s.msg !== window.ultimo) {
          el('msg').textContent = s.msg;
          window.ultimo = s.msg
        }
        document.querySelectorAll('.control').forEach(b => b.disabled = !s.online || s.ocupado)
      } catch (e) {
        el('estado').textContent = 'Sin conexión al ESP32';
        document.querySelectorAll('.control').forEach(b => b.disabled = true)
      } finally {
        setTimeout(estado, 700)
      }
    }
    mostrar();
    cargar();
    cargarMotor();
    estado();
  </script>
</body>

</html>
)HTML";
bool online() { return recibido && millis() - ultimoDato < 2000; }
bool listo() { return online() && !ocupado && !pendiente; }
String hora(int m) {
  char b[8];
  snprintf(b, sizeof(b), "%02d:%02d", m / 60, m % 60);
  return b;
}
String reloj() {
  struct tm t;
  time_t now = time(nullptr);
  if (now < 1700000000) return "Esperando NTP";
  localtime_r(&now, &t);
  char b[40];
  strftime(b, sizeof(b), "%d/%m/%Y %H:%M:%S", &t);
  return b;
}
bool entero(String s, int minV, int maxV, int &v) {
  if (!s.length()) return false;
  char *end;
  long x = strtol(s.c_str(), &end, 10);
  if (*end || x < minV || x > maxV) return false;
  v = x;
  return true;
}
bool parseHora(String s, int &m) {
  int h, k;
  if (s.length() != 5 || s[2] != ':' || !entero(s.substring(0, 2), 0, 23, h) || !entero(s.substring(3), 0, 59, k))
    return false;
  m = h * 60 + k;
  return true;
}
void enviar(String s) {
  Serial2.println(s);
  pendiente = true;
  ultimoEnvio = millis();
  mensaje = "Comando enviado";
}
void enviarDosis(int g) {
  enviar("DOSIS," + String(g) + "," + String(motor.pasos) + "," +
         String(motor.abrir) + "," + String(motor.cerrar) + "," + String(motor.sentido));
}
void leerArduino() {
  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n') {
      linea.trim();
      if (linea.startsWith("DAT,")) {
        float w, d;
        char niv[16];
        int b, h, r;
        if (sscanf(linea.c_str(), "DAT,%f,%f,%15[^,],%d,%d,%d", &w, &d, niv, &b, &h, &r) == 6) {
          peso = w;
          distancia = d;
          nivel = niv;
          ocupado = b;
          bascula = h;
          referencia = r;
          ultimoDato = millis();
          recibido = true;
        }
      } else if (linea.startsWith("FACTOR,")) {
        float f = linea.substring(7).toFloat();
        if (isfinite(f) && fabs(f) > 0.01) prefs.putFloat("factor", f);
        mensaje = "Calibración: " + linea;
        pendiente = false;
      } else if (linea.startsWith("OK,ARRANQUE")) {
        Serial2.println("FACTOR," + String(prefs.getFloat("factor", 26.43043857f), 6));
        mensaje = "Arduino reiniciado: realizar tara y referencia";
      } else if (linea.startsWith("OK,") || linea.startsWith("ERR,") || linea.startsWith("FIN,")) {
        mensaje = linea;
        pendiente = false;
        if (linea == "OK,MOVER" || linea == "OK,DOSIS") ocupado = true;
      }
      linea = "";
    } else if (c != '\r') {
      if (linea.length() < 180)
        linea += c;
      else
        linea = "";
    }
  }
  if (pendiente && millis() - ultimoEnvio > 3000) {
    pendiente = false;
    mensaje = "Sin confirmación del Arduino; verificar antes de repetir";
  }
}
void guardarPlan() { prefs.putBytes("plan", &plan, sizeof(plan)); }
void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, 16, 17);
  prefs.begin("dispensador", false);
  ConfigMotor guardado;
  if (prefs.getBytesLength("motor") == sizeof(guardado) &&
      prefs.getBytes("motor", &guardado, sizeof(guardado)) == sizeof(guardado) &&
      motorValido(guardado)) motor = guardado;
  if (prefs.getBytesLength("plan") == sizeof(plan)) prefs.getBytes("plan", &plan, sizeof(plan));
  if (plan.n < 1 || plan.n > 4) plan.n = 1;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(SSID, PASSWORD);
  configTime(-7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html; charset=utf-8", HTML); });
  server.on("/plan", HTTP_GET, [] {
    String s = "{\"n\":" + String(plan.n) + ",\"p\":[";
    for (int i = 0; i < 4; i++) {
      if (i) s += ",";
      s += "{\"h\":\"" + hora(plan.p[i].minuto) + "\",\"g\":" + String(plan.p[i].gramos) + "}";
    }
    server.send(200, "application/json", s + "]}");
  });
  server.on("/plan", HTTP_POST, [] {
    Plan nuevo = plan;
    int n;
    if (!entero(server.arg("n"), 1, 4, n)) {
      server.send(400, "text/plain", "Número inválido");
      return;
    }
    nuevo.n = n;
    for (int i = 0; i < n; i++) {
      if (!parseHora(server.arg("h" + String(i)), nuevo.p[i].minuto) ||
          !entero(server.arg("g" + String(i)), 5, 1000, nuevo.p[i].gramos)) {
        server.send(400, "text/plain", "Hora o gramos inválidos");
        return;
      }
      for (int j = 0; j < i; j++)
        if (nuevo.p[j].minuto == nuevo.p[i].minuto) {
          server.send(400, "text/plain", "No repitas horarios");
          return;
        }
    }
    plan = nuevo;
    guardarPlan();
    server.send(200, "text/plain", "Programación guardada");
  });
  server.on("/motor", HTTP_GET, [] {
    String s = "{\"pasos\":" + String(motor.pasos) + ",\"abrir\":" + String(motor.abrir) +
               ",\"cerrar\":" + String(motor.cerrar) + ",\"sentido\":" + String(motor.sentido) + "}";
    server.send(200, "application/json", s);
  });
  server.on("/motor", HTTP_POST, [] {
    ConfigMotor nuevo;
    if (!entero(server.arg("pasos"), 1, 4096, nuevo.pasos) ||
        !entero(server.arg("abrir"), 10, 15, nuevo.abrir) ||
        !entero(server.arg("cerrar"), 10, 15, nuevo.cerrar) ||
        !entero(server.arg("sentido"), -1, 1, nuevo.sentido) || !motorValido(nuevo)) {
      server.send(400, "text/plain", "Configuración inválida"); return;
    }
    if (prefs.putBytes("motor", &nuevo, sizeof(nuevo)) != sizeof(nuevo)) {
      server.send(500, "text/plain", "No se pudo guardar"); return;
    }
    motor = nuevo;
    server.send(200, "text/plain", "Configuración guardada para la próxima dosis");
  });
  server.on("/estado", HTTP_GET, [] {
    String s = "{\"hora\":\"" + reloj() + "\",\"peso\":" + String(peso, 2) + ",\"dist\":" + String(distancia, 2) +
               ",\"nivel\":\"" + nivel + "\",\"online\":" + String(online() ? "true" : "false") +
               ",\"ocupado\":" + String((ocupado || pendiente) ? "true" : "false") +
               ",\"bascula\":" + String(bascula ? "true" : "false") +
               ",\"ref\":" + String(referencia ? "true" : "false") + ",\"msg\":\"" + mensaje + "\"}";
    server.send(200, "application/json", s);
  });
  server.on("/cmd", HTTP_POST, [] {
    if (!listo()) {
      server.send(409, "text/plain", "Arduino ocupado o desconectado");
      return;
    }
    String a = server.arg("a");
    int g, p, r;
    if (a == "tara")
      enviar("TARA");
    else if (a == "ref")
      enviar("REFERENCIA");
    else if (a == "cal" && entero(server.arg("g"), 1, 5000, g))
      enviar("CAL," + String(g));
    else if (a == "mover" && entero(server.arg("pasos"), -4096, 4096, p) && p && entero(server.arg("rpm"), 10, 15, r))
      enviar("MOVER," + String(p) + "," + String(r));
    else if (a == "dosis" && entero(server.arg("g"), 5, 1000, g))
      enviarDosis(g);
    else if (a == "prueba") {
      int pasos, abrir, cerrar, sentido;
      if (!entero(server.arg("g"), 5, 1000, g) ||
          !entero(server.arg("pasos"), 1, 4096, pasos) ||
          !entero(server.arg("abrir"), 10, 15, abrir) ||
          !entero(server.arg("cerrar"), 10, 15, cerrar) ||
          !entero(server.arg("sentido"), -1, 1, sentido) || sentido == 0) {
        server.send(400, "text/plain", "Parametros invalidos");
        return;
      }
      enviar("DOSIS," + String(g) + "," + String(pasos) + "," +
             String(abrir) + "," + String(cerrar) + "," + String(sentido));
    }
    else {
      server.send(400, "text/plain", "Comando o valores inválidos");
      return;
    }
    server.send(200, "text/plain", "Comando enviado; esperando confirmación");
  });
  server.begin();
}
void loop() {
  leerArduino();
  server.handleClient();
  static bool conectado = false;
  if (WiFi.status() == WL_CONNECTED && !conectado) {
    conectado = true;
    Serial.println(WiFi.localIP());
    if (MDNS.begin("dispensador")) {
      MDNS.addService("http", "tcp", 80);
      Serial.println("mDNS activo: http://dispensador.local");
    } else {
      Serial.println("No se pudo iniciar mDNS");
    }
  } else if (WiFi.status() != WL_CONNECTED)
    conectado = false;
  if (millis() - ultimoCheck >= 500) {
    ultimoCheck = millis();
    time_t now = time(nullptr);
    if (now < 1700000000) return;
    struct tm t;
    localtime_r(&now, &t);
    int dia = (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
    int m = t.tm_hour * 60 + t.tm_min;
    for (int i = 0; i < plan.n; i++) {
      if (plan.p[i].minuto == m && plan.ultimo[i] != dia && listo() && bascula && referencia && distancia >= 0 &&
          distancia <= 10) {
        plan.ultimo[i] = dia;
        guardarPlan();
        enviarDosis(plan.p[i].gramos);
        break;
      }
    }
  }
}