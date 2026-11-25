// Arduino Uno ATmega328P, 16 MHz. Sin librerias externas.
// UART D0/D1; HX711 D2/D3; RGB catodo comun D4/D5/D6;
// ULN2003 D8-D11; HC-SR04 TRIG D12, ECHO D13.
#include <Arduino.h>
#include <avr/interrupt.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

const float NIVEL_LLENO = 3.0, NIVEL_MEDIO = 8.0, NIVEL_BAJO = 10.0;
const float ANTICIPO = 3.0;
const uint8_t seq[] = {1, 3, 2, 6, 4, 12, 8, 9};
// Configuracion de la dosis activa: se recibe del ESP32.
int aperturaDosis = 1024, sentidoAbrir = -1;
int rpmAbrir = 10, rpmCerrar = 15;
volatile uint32_t ticks = 0;
volatile uint16_t restantes = 0, intervalo = 15, cuenta = 0;
volatile int8_t sentido = 1;
volatile uint8_t fase = 0;
volatile uint8_t rojo = 0, verde = 0, azul = 0, pwm = 0;
volatile uint16_t inicioEco = 0, anchoEco = 0;
volatile bool ecoListo = false, esperandoEco = false, ecoSubio = false;
volatile uint8_t pulsoTrig = 0;
float factor = 26.43043857, peso = 0, distancia = -1, filtrado = 0;
int32_t cero = 0;
bool bascula = false, tarada = false;
uint32_t ultimoHX = 0, ultimoEco = 0, ping = 0, reporte = 0;
uint32_t inicioDosis = 0, ultimoFlujo = 0, cierreDesde = 0;
float objetivo = 0, pesoInicial = 0, referenciaFlujo = 0;
uint8_t estado = 0; // 0 reposo, 1 apertura, 2 dosificando, 3 cierre, 4 estabilizacion
bool requiereReferencia = true;
const char *resultado = "INICIO";
volatile uint8_t cola[128], cabeza = 0, colaPos = 0;
char rx[80];
uint8_t nr = 0;
bool descartar = false;

ISR(USART_RX_vect) {
  uint8_t c = UDR0;
  uint8_t n = (cabeza + 1) & 127;
  if (n != colaPos) { cola[cabeza] = c; cabeza = n; }
}
uint32_t ahora() {
  uint8_t s = SREG; cli();
  uint32_t x = ticks;
  SREG = s;
  return x / 10;
}
bool motorOcupado() {
  uint8_t s = SREG; cli();
  bool b = restantes != 0;
  SREG = s;
  return b;
}
void mover(int pasos, int rpm) {
  if (!pasos) return;
  uint8_t s = SREG; cli();
  sentido = pasos > 0 ? 1 : -1;
  restantes = abs(pasos);
  intervalo = (600000UL + 2048UL * rpm) / (4096UL * rpm);
  cuenta = 0;
  SREG = s;
}
ISR(TIMER1_COMPA_vect) {
  OCR1A += 200; // 100 us con prescaler 8
  ticks++;
  if (pulsoTrig && !--pulsoTrig) PORTB &= ~_BV(PB4);
  if (restantes && ++cuenta >= intervalo) {
    cuenta = 0;
    fase = (fase + sentido + 8) & 7;
    PORTB = (PORTB & 0xF0) | seq[fase];
    restantes--;
  } else if (!restantes) {
    PORTB &= 0xF0; // Desenergizar bobinas al terminar
  }
  pwm = (pwm + 1) & 31;
  if (pwm < rojo) PORTD |= _BV(PD4); else PORTD &= ~_BV(PD4);
  if (pwm < verde) PORTD |= _BV(PD5); else PORTD &= ~_BV(PD5);
  if (pwm < azul) PORTD |= _BV(PD6); else PORTD &= ~_BV(PD6);
}
ISR(PCINT0_vect) { // ECHO D13 = PB5
  if (!esperandoEco) return;
  uint16_t t = TCNT1;
  if (PINB & _BV(PB5)) { inicioEco = t; ecoSubio = true; }
  else if (ecoSubio) {
    anchoEco = t - inicioEco;
    ecoListo = true;
    esperandoEco = false;
  }
}
void tx(const char *s) {
  while (*s) {
    while (!(UCSR0A & _BV(UDRE0))) {}
    UDR0 = *s++;
  }
}
void num(float x) { char b[24]; dtostrf(x, 1, 2, b); tx(b); }
bool leerHX(int32_t &v) {
  if (PIND & _BV(PD2)) return false;
  uint32_t u = 0;
  uint8_t s = SREG; cli();
  for (uint8_t i = 0; i < 24; i++) {
    PORTD |= _BV(PD3);
    asm volatile("nop\n nop\n nop\n nop\n");
    u = (u << 1) | ((PIND & _BV(PD2)) ? 1 : 0);
    PORTD &= ~_BV(PD3);
    asm volatile("nop\n nop\n nop\n nop\n");
  }
  PORTD |= _BV(PD3);
  asm volatile("nop\n nop\n nop\n nop\n");
  PORTD &= ~_BV(PD3);
  SREG = s;
  if (u & 0x800000UL) u |= 0xFF000000UL;
  v = (int32_t)u;
  return true;
}
const char *nivel() {
  if (distancia < 0) return "ERROR";
  if (distancia <= NIVEL_LLENO) return "LLENO";
  if (distancia <= NIVEL_MEDIO) return "MEDIO";
  if (distancia <= NIVEL_BAJO) return "BAJO";
  return "VACIO";
}
void led(uint32_t t) {
  rojo = verde = azul = 0;
  if (distancia < 0) rojo = (t / 350) % 2 ? 32 : 0;
  else if (distancia <= NIVEL_LLENO) verde = 32;
  else if (distancia <= NIVEL_BAJO) { rojo = 32; verde = 7; }
  else rojo = 32;
}
void cerrar(bool error, const char *motivo) {
  resultado = motivo;
  estado = 3;
  // Usa exactamente la configuracion guardada al iniciar esta dosis.
  mover(-sentidoAbrir * aperturaDosis, rpmCerrar);
}
// Leer un entero delimitado por coma (o fin de linea).
bool campoEntero(char *&p, long &v, bool ultimo) {
  char *fin;
  v = strtol(p, &fin, 10);
  if (fin == p || *fin != (ultimo ? '\0' : ',')) return false;
  p = ultimo ? fin : fin + 1;
  return true;
}
void comando(char *s) {
  if (!strcmp(s, "ESTADO")) return;
  if (estado || motorOcupado()) { tx("ERR,OCUPADO\n"); return; }
  if (!strcmp(s, "REFERENCIA")) {
    requiereReferencia = false; tx("OK,REFERENCIA\n"); return;
  }
  if (!strcmp(s, "TARA")) {
    if (!bascula) { tx("ERR,BASCULA\n"); return; }
    cero = (int32_t)filtrado; tarada = true; peso = 0;
    tx("OK,TARA\n"); return;
  }
  if (!strncmp(s, "FACTOR,", 7)) {
    float f = atof(s + 7);
    if (isfinite(f) && fabs(f) > 0.01 && fabs(f) < 1000000) {
      factor = f; tx("OK,FACTOR\n");
    } else tx("ERR,FACTOR\n");
    return;
  }
  if (!strncmp(s, "CAL,", 4)) {
    float g = atof(s + 4);
    if (!bascula || !tarada || !isfinite(g) || g < 1 || g > 5000) {
      tx("ERR,CALIBRACION\n"); return;
    }
    float f = (filtrado - cero) / g;
    if (!isfinite(f) || fabs(f) < 0.01 || fabs(f) >= 1000000) {
      tx("ERR,CALIBRACION\n"); return;
    }
    factor = f; tx("FACTOR,"); num(factor); tx("\n"); return;
  }
  if (!strncmp(s, "MOVER,", 6)) {
    char *p = s + 6;
    long pasos, rpm;
    if (!campoEntero(p, pasos, false) || !campoEntero(p, rpm, true) ||
        !pasos || pasos > 4096 || pasos < -4096 || rpm < 10 || rpm > 15) {
      tx("ERR,RANGO\n"); return;
    }
    requiereReferencia = true;
    mover((int)pasos, (int)rpm); tx("OK,MOVER\n"); return;
  }
  // Nuevo formato: DOSIS,gramos,pasos,rpmAbrir,rpmCerrar,sentido
  if (!strncmp(s, "DOSIS,", 6)) {
    char *p = s + 6;
    long g, pasos, ra, rc, dir;
    if (!campoEntero(p, g, false) || !campoEntero(p, pasos, false) ||
        !campoEntero(p, ra, false) || !campoEntero(p, rc, false) ||
        !campoEntero(p, dir, true)) { tx("ERR,FORMATO_DOSIS\n"); return; }
    if (g < 5 || g > 1000 || pasos < 1 || pasos > 4096 ||
        ra < 10 || ra > 15 || rc < 10 || rc > 15 || (dir != -1 && dir != 1)) {
      tx("ERR,RANGO_DOSIS\n"); return;
    }
    if (!bascula || !tarada || requiereReferencia || distancia < 0 ||
        distancia > NIVEL_BAJO || peso + g > 5000) {
      tx("ERR,DOSIS_NO_SEGURA\n"); return;
    }
    aperturaDosis = (int)pasos;
    sentidoAbrir = (int)dir;
    rpmAbrir = (int)ra;
    rpmCerrar = (int)rc;
    pesoInicial = peso; objetivo = g; referenciaFlujo = peso;
    inicioDosis = ultimoFlujo = ahora();
    resultado = "EN_PROCESO"; estado = 1;
    mover(sentidoAbrir * aperturaDosis, rpmAbrir);
    tx("OK,DOSIS\n"); return;
  }
  tx("ERR,COMANDO\n");
}
void setup() {
  cli(); TIMSK0 = 0;
  DDRD |= _BV(PD1) | _BV(PD3) | _BV(PD4) | _BV(PD5) | _BV(PD6);
  DDRD &= ~(_BV(PD0) | _BV(PD2));
  PORTD &= ~(_BV(PD3) | _BV(PD4) | _BV(PD5) | _BV(PD6));
  DDRB |= 0x1F; DDRB &= ~_BV(PB5); PORTB &= ~0x3F;
  UBRR0H = 0; UBRR0L = 103;
  UCSR0A = 0; UCSR0B = _BV(RXEN0) | _BV(TXEN0) | _BV(RXCIE0);
  UCSR0C = _BV(UCSZ01) | _BV(UCSZ00);
  TCCR1A = 0; TCCR1B = 0; TCNT1 = 0; OCR1A = 200;
  TIFR1 = _BV(OCF1A); TIMSK1 = _BV(OCIE1A); TCCR1B = _BV(CS11);
  PCICR |= _BV(PCIE0); PCMSK0 |= _BV(PCINT5);
  sei(); tx("OK,ARRANQUE\n");
}
void loop() {
  uint32_t t = ahora();
  while (colaPos != cabeza) {
    char c = cola[colaPos]; colaPos = (colaPos + 1) & 127;
    if (c == '\n') {
      if (!descartar) { rx[nr] = 0; comando(rx); }
      nr = 0; descartar = false;
    } else if (c != '\r') {
      if (nr < sizeof(rx) - 1 && !descartar) rx[nr++] = c;
      else descartar = true;
    }
  }
  int32_t v;
  if (leerHX(v)) {
    if (!bascula) filtrado = v;
    else filtrado = 0.7f * filtrado + 0.3f * v;
    bascula = true; ultimoHX = t; peso = (filtrado - cero) / factor;
  }
  if (t - ultimoHX > 700) bascula = false;
  if (t - ping >= 80) {
    ping = t;
    uint8_t s = SREG; cli();
    esperandoEco = true; ecoSubio = false;
    PORTB |= _BV(PB4); pulsoTrig = 2;
    SREG = s;
  }
  if (ecoListo) {
    uint8_t s = SREG; cli();
    uint16_t w = anchoEco; ecoListo = false; SREG = s;
    float d = w * 0.008575f;
    if (d >= 2 && d <= 400) { distancia = d; ultimoEco = t; }
  }
  if (t - ultimoEco > 400) distancia = -1;
  led(t);
  if (estado == 1 && !motorOcupado()) {
    estado = 2; ultimoFlujo = t; referenciaFlujo = peso;
  }
  if (estado == 2) {
    if (!bascula || distancia < 0 || distancia > NIVEL_BAJO)
      cerrar(true, "SENSOR_O_TOLVA");
    else if (peso - pesoInicial >= max(2.0f, objetivo - ANTICIPO))
      cerrar(false, "OK");
    else {
      if (peso - referenciaFlujo >= 2) { referenciaFlujo = peso; ultimoFlujo = t; }
      if (t - ultimoFlujo > 5000 || t - inicioDosis > 30000)
        cerrar(true, "SIN_FLUJO_O_TIMEOUT");
    }
  }
  if (estado == 3 && !motorOcupado()) { estado = 4; cierreDesde = t; }
  if (estado == 4 && t - cierreDesde >= 1500) {
    estado = 0; tx("FIN,"); tx(resultado); tx(","); num(peso - pesoInicial); tx("\n");
  }
  if (t - reporte >= 500) {
    reporte = t; tx("DAT,");
    num(bascula && tarada ? (fabs(peso) < 3 ? 0 : peso) : 0);
    tx(","); num(distancia); tx(","); tx(nivel()); tx(",");
    tx(estado || motorOcupado() ? "1" : "0"); tx(",");
    tx(bascula && tarada ? "1" : "0"); tx(",");
    tx(requiereReferencia ? "0" : "1"); tx("\n");
  }
}
