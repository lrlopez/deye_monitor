# ⚡ Deye Monitor

Monitor de instalación solar fotovoltaica con inversor híbrido **Deye SUN-6K-SG05** en tiempo real, con pantalla táctil, historial de hasta 2 años, servidor web integrado, notificaciones Telegram, acceso por nombre mDNS (`inversor.local`) y actualización OTA.

Compatible con **ESP32-S3** (pantalla 480×272 px) y **ESP32-P4** (pantalla Guition JC1060P470, 1024×600 px). Desarrollado con **LVGL 9** y **PlatformIO**.

---

## 📋 Tabla de contenidos

- [Características](#-características)
- [Hardware requerido](#-hardware-requerido)
- [Arquitectura del software](#-arquitectura-del-software)
- [Estructura del proyecto](#-estructura-del-proyecto)
- [Pantallas](#-pantallas)
- [Servidor web](#-servidor-web)
- [Panel de administración web](#-panel-de-administración-web)
- [Bot de Telegram](#-bot-de-telegram)
- [Almacenamiento](#-almacenamiento)
- [Instalación y configuración](#-instalación-y-configuración)
- [Primer arranque](#-primer-arranque)
- [Actualización OTA](#-actualización-ota)
- [Compilación CI/CD](#-compilación-cicd)
- [API REST](#-api-rest)
- [Resolución de problemas](#-resolución-de-problemas)
- [Estructura de ficheros](#-estructura-de-ficheros)

---

## ✨ Características

### Monitorización en tiempo real
- Lectura de datos del inversor cada **5 segundos** vía protocolo **SolarmanV5** sobre TCP
- Potencia PV (PV1 + PV2), red eléctrica, batería y carga
- SOC de la batería con barra de progreso
- Indicador de autoconsumo instantáneo con código de colores
- Reloj en tiempo real sincronizado por NTP

### Historial y estadísticas
- Registro de medidas cada **5 minutos** alineado a intervalos exactos (XX:00, XX:05…)
- Agregación horaria pre-calculada para generación instantánea de gráficas
- Totales diarios con % de autoconsumo y autosuficiencia
- Historial de hasta **730 días (2 años)** en flash LittleFS
- Caché en **PSRAM** de toda la historia horaria y diaria para acceso sin latencia

### Interfaz táctil (LVGL 9)
- 4 pantallas deslizables horizontalmente con indicador de posición
- Dashboard de tiempo real con 4 tarjetas
- Estadísticas diarias con donuts de consumo/producción, navegable día a día
- Gráfica diaria con líneas temporales de 5 variables
- Pantalla de configuración con scroll y teclado virtual
- Calendario mensual para selección directa de fecha
- Modo nocturno con brillo configurable y horario automático
- Pantalla de inicio (splash) con progreso de inicialización

### Servidor web integrado
- Dashboard HTML con actualización por AJAX cada 5 segundos (sin recargar la página)
- Donuts SVG animados de autoconsumo/producción
- Gráfica interactiva diaria con **Chart.js** y actualización incremental
- Navegación día a día en el navegador
- API REST con soporte de granularidad 5min/horario/diario
- Actualización de firmware OTA vía navegador (`/update`)
- **Panel de administración** en `/admin`: configura inversor, gráfica, Telegram, pantalla y nombre mDNS desde el navegador
- Protección por contraseña del panel de administración y de OTA, configurable desde la pantalla táctil
- **Acceso por nombre mDNS**: el dispositivo es accesible en la red local como `http://inversor.local` (nombre configurable)
- **PWA (Progressive Web App)**: el dashboard y la gráfica se pueden instalar como aplicación en el móvil con "Añadir a pantalla de inicio"

### Notificaciones Telegram
- **Dos niveles de alerta de batería**: aviso (umbral configurable, por defecto 25 %) y crítico (por defecto 20 %), con mensajes diferenciados y recuperación con histéresis
- Alertas proactivas: solar arranca/para, corte de red, fallo logger
- Comandos del bot para consultar datos desde cualquier lugar
- Silenciado temporal de alertas
- Cambio de umbrales en caliente sin reiniciar

### Otras características
- Configuración WiFi, IP del logger, nombre mDNS y demás parámetros por pantalla táctil, guardados en **NVS**
- Detección y selección de redes WiFi disponibles
- Reconexión WiFi automática sin bloquear la interfaz
- Recuperación de datos en gaps por corte de alimentación
- CI/CD con GitHub Actions: genera `.bin` por entorno en cada tag

---

## 🔧 Hardware requerido

### Placas soportadas

| Entorno PlatformIO | Placa | MCU | Pantalla |
|---|---|---|---|
| `sunton_4827s043` | Sunton 4827S043 | ESP32-S3 @ 240 MHz, 8 MB PSRAM | 480×272 px RGB, touch GT911 |
| `guition_jc1060p470` | Guition JC1060P470 | ESP32-P4 @ 360 MHz, 8 MB PSRAM | 1024×600 px MIPI DSI, touch GT911 |

### Componentes comunes

| Componente | Especificación |
|---|---|
| Inversor | Deye SUN-6K-SG05 (compatible con variantes SG03/SG04) |
| Datalogger | Stick WiFi LSW3 incluido con el inversor (SolarmanV5) |
| Red | WiFi 2.4 GHz compartida entre la placa y el datalogger |

### Conexiones

El datalogger y la placa solo necesitan estar en la **misma red WiFi**. No se requiere ninguna conexión física adicional entre ellos. El protocolo de comunicación es SolarmanV5 sobre TCP puerto **8899**.

### Identificar datos del datalogger

- **IP:** Asignada por el router DHCP al stick WiFi (recomendable fijar por MAC)
- **Número de serie:** Etiqueta adhesiva del stick WiFi o en la app SolarmanPV → Dispositivo → S/N (en decimal)

---

## 🏗️ Arquitectura del software

```
┌─────────────────────────────────────────────────────────────┐
│                        Core 1 (loop)                        │
│  lv_timer_handler() → dashboard_tick() → backlight_tick()   │
│  summary_screen_tick() → chart_screen_tick()                │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│                        Core 0                               │
│  ┌─────────────┐  ┌─────────────┐  ┌───────────────────┐    │
│  │solarmanTask │  │webserverTask│  │telegram_bot task  │    │
│  │             │  │             │  │                   │    │
│  │ Polling 5s  │  │ HTTP server │  │ Polling + alertas │    │
│  │ Grabación   │  │ /api/data   │  │ Comandos bot      │    │
│  │ 5min/hrly/  │  │ /api/history│  │                   │    │
│  │ daily       │  │ /api/latest │  │                   │    │
│  │             │  │ /api/status │  │                   │    │
│  │             │  │ /chart      │  │                   │    │
│  └──────┬──────┘  └─────┬───────┘  └─────────┬─────────┘    │
│         │               │                    │              │
│  ┌──────▼───────────────▼────────────────────▼──────────┐   │
│  │               Mutex g_mutex                          │   │
│  │         g_energy   g_daily   g_cfg                   │   │
│  └──────────────────────┬───────────────────────────────┘   │
└─────────────────────────┼───────────────────────────────────┘
                          │
         ┌────────────────▼──────────────────┐
         │         Capa de datos             │
         │                                   │
         │  DataStore (LittleFS)             │
         │  ├── /raw.bin   (5 min, 16B)      │
         │  ├── /hrly.bin  (horario, 32B)    │
         │  └── /day.bin   (diario, 32B)     │
         │                                   │
         │  PsramCache (PSRAM 8MB)           │
         │  ├── Raw cache   (90 días, 405KB) │
         │  ├── Hourly cache (730d, 548KB)   │
         │  └── Daily cache (730d, 23KB)     │
         └───────────────────────────────────┘
```

### Tareas FreeRTOS

| Tarea | Core | Stack | Prioridad | Función |
|---|---|---|---|---|
| `loop()` | 1 | Sistema | — | Render LVGL, UI updates |
| `solarmanTask` | 0 | 8 KB | 1 | Polling inversor, grabación |
| `webserver_task` | 0 | 8 KB | 1 | Servidor HTTP |
| `telegram_bot` | 0 | 16 KB | 1 | Bot Telegram + alertas |
| `cache_bg` | 0 | 4 KB | 0 | Carga background de caché |

---

## 📁 Estructura del proyecto

```
deye-monitor/
├── .github/
│   ├── workflows/
│   │   └── build_release.yml      # CI/CD: build y release automático
│   └── scripts/
│       └── get_envs.py            # Detecta entornos de platformio.ini
├── src/
│   ├── main.cpp                   # Setup, loop, solarmanTask
│   ├── config.h                   # Constantes de compilación
│   ├── ui_constants.h             # Geometría y fuentes escalables
│   │
│   ├── solarman.h / .cpp          # Protocolo SolarmanV5 + Modbus
│   ├── storage.h / .cpp           # NVS: config, sesión, backlight
│   ├── data_store.h / .cpp        # LittleFS: buffer circular triple
│   ├── psram_cache.h / .cpp       # Caché en PSRAM de toda la historia
│   ├── psram_alloc.h / .cpp       # Allocator LVGL → PSRAM
│   │
│   ├── dashboard.h / .cpp         # Pantalla 0: tiempo real
│   ├── stats_screen.h / .cpp      # Pantalla 1: estadísticas diarias
│   ├── chart_screen.h / .cpp      # Pantalla 2: gráfica diaria
│   ├── config_screen.h / .cpp     # Pantalla 3: configuración
│   ├── splash_screen.h / .cpp     # Pantalla de inicio
│   ├── calendar_popup.h / .cpp    # Calendario modal
│   ├── pagination_dots.h / .cpp   # Indicador de posición
│   ├── backlight.h / .cpp         # Control PWM de brillo
│   │
│   ├── web_server.h / .cpp        # Servidor HTTP + OTA
│   └── telegram.h / .cpp         # Bot Telegram (UniversalTelegramBot)
│
├── partitions.csv                 # Particionado: NVS+OTA+App+LittleFS
├── platformio.ini                 # Configuración PlatformIO
└── lv_conf.h                      # Configuración LVGL 9
```

---

## 📱 Pantallas

### Pantalla 0 — Dashboard

Vista principal con datos en tiempo real actualizados cada 5 segundos.

```
┌──────────────────────────────────────────────────────┐
│ 10 May · 14:32:07              Autoconsumo 87% 🟢    │
├─────────────────────┬────────────────────────────────┤
│  ⚡ SOLAR           │  📶 RED                        │
│                     │                                │
│   2.340 W           │   -914 W                       │
│ PV1:2340  PV2:0     │   Exportando a la red          │
├─────────────────────┼────────────────────────────────┤
│  🔋 BATERIA         │  🏠 CARGA                      │
│                     │                                │
│   99%               │   Consumo actual               │
│  ████████████░░     │                                │
│  -37 W · Cargando   │   360 W                        │
└─────────────────────┴────────────────────────────────┘
                    ● ○ ○ ○ ○
```

### Pantalla 1 — Estadísticas diarias

Donuts de distribución de energía con navegación día a día y selector de calendario.

- **CONSUMO:** Solar directo | Descarga batería | Importación
- **PRODUCCIÓN:** Autoconsumo | Carga batería | Exportación
- Tap en el título de fecha → volver a hoy
- Botón 📅 → calendario mensual con días coloreados según disponibilidad de datos

### Pantalla 2 — Gráfica diaria

Gráfica de líneas con 5 series temporales por hora:

| Serie | Color | Descripción |
|---|---|---|
| PV | Amarillo | Producción solar media |
| Red | Azul | Intercambio con la red (+import/-export) |
| Batería | Verde | Potencia de la batería (+desc/-carga) |
| Carga | Violeta | Consumo del hogar |
| SOC | Azul oscuro | Estado de carga batería (%) |

- Tap en la gráfica → popup con valores de esa hora
- Línea vertical indicadora
- Autoescalado o escala fija configurable
- Refresco automático al llegar las 00:00

### Pantalla 3 — Configuración

Formulario scrollable con teclado virtual:

| Sección | Parámetros |
|---|---|
| RED WiFi | SSID (con escáner de redes) + contraseña |
| INVERSOR | IP del datalogger + número de serie |
| GRÁFICA | Autoescalado / máximo kW |
| PANTALLA | Brillo normal/reducido, inactividad, horario nocturno |
| TELEGRAM | Bot token, chat ID, umbral de aviso (amarillo) y crítico (rojo) de batería, tipos de alerta |
| ESTADO RED | IP del ESP32, señal WiFi, nombre mDNS |
| ACCESO WEB | Contraseña para el panel de administración web y OTA |

Los cambios de WiFi/logger requieren reinicio. Los de brillo y Telegram se aplican en caliente.

---

## 🌐 Servidor web

Accesible en `http://<ip-del-esp32>/` o, si mDNS está activo, en `http://inversor.local/` (el nombre es configurable).

### Páginas

| Ruta | Descripción |
|---|---|
| `/` | Dashboard con valores live y donuts SVG animados |
| `/chart` | Gráfica diaria interactiva con Chart.js, navegable |
| `/update` | Actualización de firmware OTA (protegida por contraseña si está configurada) |
| `/admin` | Panel de administración: inversor, gráfica, Telegram, pantalla (protegido por contraseña) |

### API REST

| Ruta | Descripción |
|---|---|
| `/api/data` | Valores en tiempo real (live + totales del día) |
| `/api/history` | Histórico: 5min, horario o diario con filtros de fecha |
| `/api/latest_date` | Último día con datos de 5 min persistidos en flash |
| `/api/status` | Contadores de almacenamiento, PSRAM libre, IP y RSSI |

### Actualización dinámica

- Los valores instantáneos se refrescan **sin recargar la página** vía `fetch('/api/data')` cada 5 segundos
- La gráfica de `/chart` usa actualizaciones **incrementales**: solo se piden los registros nuevos desde el último timestamp recibido (`from_ts`)
- Al cargar `/chart`, se consulta `/api/latest_date` para navegar directamente al último día con datos, evitando mostrar una gráfica vacía tras un reinicio
- Al llegar las 00:00 se cambia automáticamente al nuevo día

---

## 🔐 Panel de administración web

Accesible en `http://<ip-del-esp32>/admin` o `http://inversor.local/admin`.

Permite configurar todos los parámetros del sistema desde el navegador, sin necesidad de acceder a la pantalla táctil. La WiFi **no es modificable** desde aquí por seguridad: solo se muestra el nombre de la red activa como campo de solo lectura.

### Secciones del panel

| Sección | Parámetros configurables |
|---|---|
| **Red** | SSID (solo lectura), nombre mDNS |
| **Inversor** | IP del datalogger, número de serie |
| **Gráfica** | Autoescalado, máximo kW |
| **Telegram** | Token del bot, chat ID, umbral de aviso (amarillo) y crítico (rojo) de batería, tipos de alerta |
| **Pantalla** | Brillo normal y reducido, tiempo de inactividad, horario nocturno |

El panel incluye un botón **Guardar** que aplica todos los cambios y redirige de vuelta al panel, y un botón **Reiniciar** para aplicar cambios que requieren reinicio (como la IP del datalogger o el nombre mDNS).

### Protección por contraseña

El panel `/admin` y la página `/update` de OTA están protegidos mediante **HTTP Basic Auth** cuando hay una contraseña configurada.

**La contraseña solo puede establecerse desde la pantalla táctil** (sección ACCESO WEB de la pantalla de configuración), nunca desde la web. Esto evita que un atacante que acceda a la red pueda bloquearte el acceso.

Flujo de configuración:

1. En la pantalla táctil, desplázate hasta la sección **ACCESO WEB**
2. Escribe la contraseña deseada en el campo y pulsa **Guardar**
3. A partir de ese momento, el navegador solicitará usuario `admin` y la contraseña elegida al acceder a `/admin` o `/update`

> Si no se ha configurado ninguna contraseña, ambas páginas son accesibles sin autenticación (comportamiento predeterminado para facilitar el primer acceso).

---

## 🤖 Bot de Telegram

Configura el bot en la pantalla de configuración con el token de @BotFather y tu chat ID.

### Comandos disponibles

| Comando | Descripción | Ejemplo respuesta |
|---|---|---|
| `/estado` | Valores instantáneos completos | ☀️ 2340W 🔌 -914W 🔋 99% 🏠 360W |
| `/bateria` | SOC, potencia y estimación de autonomía | 🔋 99% · Cargando · ~2.1h para completa |
| `/hoy` | Totales del día con % autoconsumo/autosuficiencia | ☀️ 28.4 kWh · Autoconsumo 71% |
| `/dia YYYYMMDD` | Totales de cualquier fecha histórica | `/dia 20260508` |
| `/semana` | Resumen de los últimos 7 días | Tabla con PV, consumo, balance |
| `/sistema` | IP, WiFi, uptime, memoria, registros | IP: 192.168.1.34 · 6h uptime |
| `/umbral N` | Cambiar umbral de alerta batería | `/umbral 20` |
| `/silenciar` | Silenciar alertas durante 1 hora | 🔕 Alertas silenciadas |
| `/activar` | Reactivar alertas | 🔔 Alertas activas |
| `/ayuda` | Lista de comandos | — |

### Alertas proactivas

Las alertas usan máquinas de estado: una vez disparada, no se repite hasta que la condición se recupera.

| Evento | Condición de disparo | Condición de recuperación |
|---|---|---|
| ⚠️ Aviso batería | SOC < umbral de aviso (5–95 %, default 25 %) | SOC ≥ umbral aviso + 5 % |
| 🔴 Batería crítica | SOC < umbral crítico (5–50 %, default 20 %) | SOC ≥ umbral aviso + 5 % |
| ✅ Batería recuperada | — | (cualquiera de las dos anteriores) |
| Solar arranca | PV > 50 W en 3 lecturas consecutivas (15 s) | PV < 20 W en 3 lecturas consecutivas |
| Solar para | PV < 20 W en 3 lecturas consecutivas (15 s) | PV > 50 W en 3 lecturas consecutivas |
| Fallo logger | 5 fallos TCP seguidos (~25 s) | Siguiente lectura exitosa |
| Corte de red | `\|grid_w\|` = 0 W durante 15 s (solo tras haber visto red activa) | `\|grid_w\|` Distinto de 0 W en 1 lectura |
| Red restaurada | — | (ver fila anterior) |

---

## 💾 Almacenamiento

### Particionado flash (16 MB)

```
nvs        20 KB   Configuración, sesión, credenciales
otadata     8 KB   Control de particiones OTA
app0        4 MB   Firmware activo
app1        4 MB   Firmware OTA pendiente
spiffs      8 MB   LittleFS: histórico de medidas
coredump   64 KB   Volcado de excepciones
```

### Estructura de datos en LittleFS

Tres buffers circulares independientes, todos con el mismo número de días:

| Fichero | Registro | Tamaño | Días | Uso |
|---|---|---|---|---|
| `/raw.bin` | `Record5Min` | 16 B | 730 | Potencias cada 5 min |
| `/hrly.bin` | `HourlyRecord` | 32 B | 730 | Medias horarias pre-calculadas |
| `/day.bin` | `DailyRecord` | 32 B | 730 | Totales diarios |

**Capacidad total:** 730 días (~2 años). Los ficheros crecen on-demand; el espacio se reserva progresivamente conforme se escriben datos, no en el primer arranque.

```
raw.bin:   730 × 288 × 16 B = 3,363 KB
hrly.bin:  730 ×  24 × 32 B =   560 KB
day.bin:   730 ×   1 × 32 B =    23 KB
─────────────────────────────────────────
Total máx:                     3,946 KB < 8,192 KB (LittleFS) ✓
Espacio libre mín:             4,246 KB (>50 %)
```

### Caché PSRAM (8 MB)

| Caché | Contenido | Tamaño |
|---|---|---|
| Raw (90 días) | Registros recientes de 5 min | 405 KB |
| Hourly (730 días) | **Toda** la historia horaria | 548 KB |
| Daily (730 días) | **Toda** la historia diaria | 23 KB |
| LVGL heap | Objetos de UI | 256 KB |
| Canvas gráfica | Buffer de píxeles | 184 KB |

La historia horaria completa reside en PSRAM, por lo que **la generación de gráficas no toca flash** independientemente del día seleccionado.

### NVS (20 KB)

| Namespace | Clave | Contenido |
|---|---|---|
| `cfg` | `ssid`, `pass`, `lip`, `lserial` | WiFi e inversor |
| `cfg` | `mdns_host` | Nombre mDNS (default: `inversor`) |
| `cfg` | `ch_auto`, `ch_kw` | Configuración de la gráfica |
| `cfg` | `bl_norm`, `bl_red`, `bl_inact`, `bl_isecs`, `bl_night`, `bl_nstart`, `bl_nend` | Brillo y horario nocturno |
| `cfg` | `tg_token`, `tg_chatid`, `tg_batt`, `tg_bwarn`, `tg_solar`, `tg_grid`, `tg_logger` | Telegram |
| `cfg` | `web_pass` | Contraseña del panel de administración web |
| `cfg` | `session` | Epoch del último registro (recuperación tras corte) |

---

## ⚙️ Instalación y configuración

### 1. Prerrequisitos

- [PlatformIO](https://platformio.org/) (extensión VS Code o CLI)
- Python 3.8+

### 2. Clonar el repositorio

```bash
git clone https://github.com/lrlopez/deye-monitor.git
cd deye-monitor
```

### 3. Elegir entorno en `platformio.ini`

El repositorio incluye dos entornos listos para usar. Selecciona el que corresponda a tu hardware:

**ESP32-S3 (Sunton 4827S043 u otro, 480×272 px):**
```bash
pio run -e esp32s3box --target upload
```

**Guition JC1060P470 (ESP32-P4, 1024×600 px MIPI DSI):**
```bash
pio run -e guition_jc1060p470 --target upload
```

### 4. Configurar `lv_conf.h`

```c
// Fuentes necesarias
#define LV_FONT_MONTSERRAT_12  1
#define LV_FONT_MONTSERRAT_14  1
#define LV_FONT_MONTSERRAT_28  1

// Heap en PSRAM
#define LV_USE_STDLIB_MALLOC   LV_STDLIB_CUSTOM
#define LV_MEM_CUSTOM_INCLUDE  "psram_alloc.h"
#define LV_MEM_CUSTOM_ALLOC    psram_malloc
#define LV_MEM_CUSTOM_FREE     psram_free
#define LV_MEM_CUSTOM_REALLOC  psram_realloc
```

### 5. Añadir tu driver de pantalla

En `main.cpp`, en la zona marcada del `setup()`:

```cpp
// ── Tu código de display/touch aquí ──────────────────────────────
lv_init();
setup_display();     // tu función de inicialización del display
setup_touch();       // tu función de inicialización del táctil
lv_tick_set_cb([]() -> uint32_t { return millis(); });
// ─────────────────────────────────────────────────────────────────
```

### 6. Compilar y subir

```bash
# Compilar
pio run

# Subir firmware
pio run --target upload

# Subir filesystem (primera vez obligatorio)
pio run --target uploadfs

# Monitor serie
pio device monitor
```

---

## 🚀 Primer arranque

Al iniciar por primera vez, la pantalla de splash mostrará el progreso de inicialización. Con el NVS vacío, el dispositivo usará los valores por defecto de `config.h`.

Para configurar WiFi y el datalogger:

1. Desliza hasta la **pantalla de configuración** (última, a la derecha)
2. Introduce el **SSID** de tu red (o pulsa 🔍 para escanear)
3. Introduce la **contraseña WiFi**
4. Introduce la **IP del datalogger** (ver etiqueta del stick WiFi o app SolarmanPV)
5. Introduce el **número de serie** del datalogger (decimal, etiqueta del stick)
6. Pulsa **Guardar** — el dispositivo se reiniciará y conectará

### Verificar funcionamiento

En el monitor serie deberías ver:

```
[WiFi] Conectado: 192.168.1.34
[NTP] Sincronizado
[mDNS] Activo: inversor.local
[Solarman] Conectando a 192.168.1.214:8899
[Live] PV:705W Grid:-914W Bat:-37W(99%) Load:360W
[Record] Startup ok: slot=92345 h=14 d=10
```

---

## 🔄 Actualización OTA

### Desde el navegador

1. Accede a `http://<ip-del-esp32>/update`
2. Si hay contraseña configurada, el navegador pedirá usuario `admin` y la contraseña (ver [Panel de administración web](#-panel-de-administración-web))
3. Selecciona el fichero `.bin` de la release
4. Pulsa **Actualizar** — la barra de progreso mostrará el avance
5. El ESP32 se reiniciará automáticamente al terminar

> **Nota:** El fichero `.bin` para OTA es solo la partición de aplicación (no incluye el filesystem). Se genera en `.pio/build/<entorno>/firmware.bin`.

### Desde PlatformIO

```bash
pio run --target upload --upload-port 192.168.1.34
```

---

## 🤖 Compilación CI/CD

Cada vez que se crea un tag con formato `v*`, GitHub Actions compila automáticamente un firmware por entorno y crea un release.

### Crear un release

```bash
git tag v1.0.0
git push origin v1.0.0
```

### Releases de prueba (pre-release)

```bash
git tag v1.1.0-beta
git push origin v1.1.0-beta
```

Los tags con guión se marcan automáticamente como pre-release en GitHub.

### Saltar un entorno en CI

```ini
[env:esp32s3_debug]
; ...
ci_skip = true   # Este entorno no se compilará en GitHub Actions
```

---

## 🌐 API REST

### Resumen de endpoints

| Método | Ruta | Descripción |
|---|---|---|
| `GET` | `/api/data` | Valores en tiempo real |
| `GET` | `/api/history` | Histórico (5min / horario / diario) |
| `GET` | `/api/latest_date` | Último día con datos de 5 min en flash |
| `GET` | `/api/status` | Estado del almacenamiento y sistema |
| `POST` | `/api/restart` | Reinicia el ESP32 (usado desde el panel de administración) |

---

#### `GET /api/data`
Valores actuales en tiempo real.

```json
{
  "live": {
    "pv_w": 2340, "pv1_w": 2340, "pv2_w": 0,
    "grid_w": -914, "batt_w": -37, "batt_soc": 99,
    "load_w": 360
  },
  "daily": {
    "pv_kwh": 28.4, "export_kwh": 8.3, "import_kwh": 0.4,
    "load_kwh": 12.6, "batt_charge_kwh": 8.2,
    "batt_discharge_kwh": 1.1, "valid": true
  }
}
```

**Signo de `batt_w`:** positivo = descargando, negativo = cargando.  
**Signo de `grid_w`:** positivo = importando, negativo = exportando.

---

#### `GET /api/history`
Histórico con múltiples granularidades.

| Parámetro | Valores | Descripción |
|---|---|---|
| `date` | `YYYYMMDD` | Día concreto (por defecto: hoy) |
| `granularity` | `5min`, `hourly`, `daily` | Resolución temporal |
| `from_ts` | epoch Unix | Solo registros más recientes (5min) |
| `from` | `YYYYMMDD` | Inicio del rango (daily) |
| `to` | `YYYYMMDD` | Fin del rango (daily) |

**Ejemplos:**

```bash
# Registros de 5 min de hoy
GET /api/history?granularity=5min

# Solo los nuevos desde el último recibido (actualización incremental)
GET /api/history?granularity=5min&from_ts=1746870300

# Datos horarios del 8 de mayo
GET /api/history?date=20260508&granularity=hourly

# Totales diarios de un rango
GET /api/history?granularity=daily&from=20260501&to=20260510
```

---

#### `GET /api/latest_date`
Devuelve el último día para el que existen registros de 5 minutos en flash. La página `/chart` lo consulta al cargar para navegar directamente al día con datos más reciente en lugar de mostrar siempre el día de hoy (que puede estar vacío tras un reinicio).

```json
{ "date": "20260519" }
```

Si no hay ningún registro persistido aún:

```json
{ "date": null }
```

---

#### `GET /api/status`
Estado del almacenamiento y del sistema.

```json
{
  "raw":    {"count": 12456, "capacity": 210240},
  "hourly": {"count": 1320,  "capacity": 17520},
  "daily":  {"count": 55,    "capacity": 730},
  "psram_free_kb": 6163,
  "wifi_rssi": -62,
  "ip": "192.168.1.34"
}
```

---

## 🔍 Resolución de problemas

### El inversor no responde

```
[Solarman] Timeout conectando al datalogger
```

- Verifica que el stick WiFi esté conectado a la misma red que el ESP32
- Comprueba la IP en la app SolarmanPV → Dispositivo → Dirección IP
- Verifica que el número de serie sea **decimal** (no hexadecimal)
- Asegúrate de que el puerto **8899** no esté bloqueado por el router

### Los datos son incorrectos (balance no cuadra)

Activa el dump de registros temporalmente:

```cpp
// En solarman.cpp, fetchDailyStats():
DBGSERIAL.println("[RAW Daily] Dump registros 70-108:");
for (int i = 0; i < REG_DAILY_COUNT; i++)
    DBGSERIAL.printf("  Reg %03d: %5u  (%.1f kWh)\n",
                  REG_DAILY_BASE + i, regs[i], regs[i] * 0.1f);
```

Compara los valores con los que muestra la app SolarmanPV.

### Sin datos en la web / donuts vacíos

Verifica en el monitor serie:

```
[API] daily valid=0 pv=0.00 load=0.00
```

Si `valid=0`, la cache PSRAM aún no tiene datos del día. Espera al primer ciclo completo de `fetchDailyStats` (cada 60 segundos).

### Pantalla congelada al abrir teclado

Causa: el teclado se creó sobre el splash screen que fue eliminado. Solución: asegurar que el teclado se crea sobre `lv_layer_top()`:

```cpp
kb = lv_keyboard_create(lv_layer_top());
```

### PSRAM no detectada

```
[Cache] ERROR: PSRAM insuficiente
```

Verifica en `platformio.ini`:

```ini
board_build.arduino.memory_type = qio_opi   ; ESP32-S3 con OPI PSRAM
```

Y en `build_flags`:

```ini
-DBOARD_HAS_PSRAM
-DCONFIG_SPIRAM_USE_CAPS_ALLOC=1
```

### Alertas Telegram no se envían

Error de TLS habitual:

```
[-32512] SSL - Memory allocation failed
```

Causas y soluciones:

1. `ledcAttach` + driver de pantalla pueden tocar el mismo pin → llamar `Backlight.begin()` **después** de `setup_display()`
2. Heap fragmentado → aumentar stack de la tarea Telegram a 16 KB
3. Buffers SSL reducidos: `client.setBufferSizes(4096, 1024)`

---

## 📐 Soporte multi-resolución

El proyecto soporta cualquier resolución ≥ 480×270 px mediante defines de compilación:

```ini
; ESP32-S3 — 480×272 px (entorno esp32s3box)
-DSCREEN_WIDTH=480  -DSCREEN_HEIGHT=272
-DFONT_SMALL_SIZE=12  -DFONT_SMALL=lv_font_montserrat_12
-DFONT_NORMAL_SIZE=14 -DFONT_NORMAL=lv_font_montserrat_14
-DFONT_LARGE_SIZE=28  -DFONT_LARGE=lv_font_montserrat_28

; ESP32-P4 — 1024×600 px (entorno guition_jc1060p470)
-DSCREEN_WIDTH=1024 -DSCREEN_HEIGHT=600
-DFONT_SMALL_SIZE=14  -DFONT_SMALL=lv_font_montserrat_14
-DFONT_NORMAL_SIZE=18 -DFONT_NORMAL=lv_font_montserrat_18
-DFONT_LARGE_SIZE=36  -DFONT_LARGE=lv_font_montserrat_36
```

Toda la geometría de la UI se escala automáticamente mediante las macros `SX()`, `SY()` y `SS()` de `ui_constants.h`.

---

## 📄 Licencia

MIT License — ver [LICENSE](LICENSE) para detalles.

---

## 🙏 Créditos

- **LVGL** — Biblioteca de UI para microcontroladores ([lvgl.io](https://lvgl.io))
- **UniversalTelegramBot** — Brian Lough ([GitHub](https://github.com/witnessmenow/Universal-Arduino-Telegram-Bot))
- **ArduinoJson** — Benoît Blanchon ([arduinojson.org](https://arduinojson.org))
- **Chart.js** — Gráficas web ([chartjs.org](https://www.chartjs.org))
- **Comunidad Deye** — Mapa de registros Modbus ([deye-inverter-mqtt](https://github.com/kbialek/deye-inverter-mqtt))
- **PlatformIO** — Entorno de desarrollo ([platformio.org](https://platformio.org))
