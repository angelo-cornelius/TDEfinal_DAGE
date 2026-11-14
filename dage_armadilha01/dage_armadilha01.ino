// Projeto DAGE - Armadilha 01
// Monitoramento de focos do Aedes aegypti - ES32Lab (ESP32)

#include <WiFi.h>
#include <ArduinoMqttClient.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ES32Lab.h>
#include <Preferences.h>

// rede do celular e IP do notebook
const String WIFI_NOME    = "NOME_DO_HOTSPOT";
const String WIFI_SENHA   = "SENHA_DO_HOTSPOT";
const String MQTT_USUARIO = "dage";
const String MQTT_SENHA   = "SENHA_DO_BROKER";
IPAddress BROKER_IP(192, 168, 0, 100);   // IP do notebook, com virgulas no lugar dos pontos

// sensores e buzzer ligados nos conectores P04, P32 e P26; os LEDs ja vem na placa
const int PINO_DHT     = 4;    // DHT22 (OUT)
const int PINO_AGUA    = 32;   // DS18B20 (fio amarelo)
const int LED_VERDE    = 17;
const int LED_AMARELO  = 16;
const int LED_VERMELHO = 13;
const int LED_AZUL     = 12;
const int BUZZER       = 26;   // buzzer (+)

DHT dht(PINO_DHT, DHT22);
OneWire fio(PINO_AGUA);
DallasTemperature sensorAgua(&fio);
WiFiClient cliente;
MqttClient mqtt(cliente);
ES_TFT tela;
Preferences memoria;   // guarda a configuracao na flash

bool monitorando = true;

// limites (podem ser mudados pelo painel)
float arMin = 26;
float arMax = 30;
float umidAlta = 70;
float aguaLetal = 45;

float tempAr;
float umidade;
float tempAgua;
int indice;
String nivel;
String nivelAnterior = "";   // pro buzzer nao bipar toda hora
unsigned long ultimaTentativa = 0;   // ultima vez que tentou reconectar


void setup() {
  Serial.begin(115200);

  pinMode(LED_VERDE, OUTPUT);
  pinMode(LED_AMARELO, OUTPUT);
  pinMode(LED_VERMELHO, OUTPUT);
  pinMode(LED_AZUL, OUTPUT);

  tela.begin();
  tela.setRotation(3);   // tela deitada
  tela.setTextSize(2);

  dht.begin();
  sensorAgua.begin();

  // carrega o que foi salvo antes de reiniciar (se nao tiver nada, fica o valor padrao)
  memoria.begin("dage", false);
  monitorando = memoria.getBool("monitorando", monitorando);
  arMin = memoria.getFloat("ar_min", arMin);
  arMax = memoria.getFloat("ar_max", arMax);
  umidAlta = memoria.getFloat("umid_alta", umidAlta);
  aguaLetal = memoria.getFloat("agua_letal", aguaLetal);

  // tenta o Wi-Fi por ate 10 s; se nao conseguir, segue sem rede
  // e a placa continua medindo e sinalizando (RNF03)
  WiFi.begin(WIFI_NOME, WIFI_SENHA);
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) Serial.println("Wi-Fi conectado");
  else Serial.println("Sem Wi-Fi, seguindo sem rede");

  mqtt.setId("armadilha01");
  mqtt.setUsernamePassword(MQTT_USUARIO, MQTT_SENHA);
  mqtt.setKeepAliveInterval(10000);   // broker percebe a queda em ~15 s
  mqtt.onMessage(receberComando);     // funcao chamada quando chega comando

  // se a placa cair, o broker publica "desconectada" no status
  mqtt.beginWill("dage/armadilha/01/status", true, 1);
  mqtt.print("desconectada");
  mqtt.endWill();

  conectarMQTT();   // primeira tentativa, se falhar o loop tenta de novo
}


void loop() {
  // tenta reconectar so a cada 10 s, pra nao atrasar o ciclo
  // quando o Wi-Fi ou o broker estiverem fora
  if (!mqtt.connected() && millis() - ultimaTentativa > 10000) {
    ultimaTentativa = millis();
    conectarMQTT();
  }
  mqtt.poll();   // verifica se chegou comando

  if (monitorando) {
    lerSensores();
    calcularRisco();
    atualizarLeds();
    mostrarNaTela();
    enviarDados();
  } else {
    // pausado pelo painel
    apagarLeds();
    tela.fillScreen(TFT_BLACK);
    tela.setCursor(0, 50);
    tela.println("PAUSADO");
    enviar("dage/armadilha/01/nivel", "PAUSADO");
  }

  // espera 3 s ate o proximo ciclo, mas continua ouvindo os comandos do painel
  for (int i = 0; i < 30; i++) {
    mqtt.poll();
    delay(100);
  }
}


void lerSensores() {
  tempAr = dht.readTemperature();
  umidade = dht.readHumidity();
  sensorAgua.requestTemperatures();
  tempAgua = sensorAgua.getTempCByIndex(0);

  Serial.print(tempAr);
  Serial.print(" C  ");
  Serial.print(umidade);
  Serial.print(" %  ");
  Serial.print(tempAgua);
  Serial.println(" C");
}


void calcularRisco() {
  indice = 0;

  // se o sensor falhar ele manda um valor fora dessas faixas (ex: -127)
  bool sensorOk = tempAr > -40 && tempAr < 80 && umidade >= 0 && umidade <= 100 && tempAgua > -100 && tempAgua < 85;

  if (!sensorOk) {
    nivel = "ERRO";
  } else if (tempAgua > aguaLetal) {
    nivel = "LETAL";   // agua muito quente mata as larvas
  } else {
    // soma os pontos (maximo 7)
    if (tempAr >= arMin && tempAr <= arMax) indice += 2;
    else if (tempAr >= 18 && tempAr <= 34) indice += 1;

    if (umidade > umidAlta) indice += 2;
    else if (umidade > 60) indice += 1;

    if (tempAgua >= 22 && tempAgua <= 34) indice += 3;
    else if (tempAgua >= 15 && tempAgua <= 36) indice += 1;

    if (indice <= 2) nivel = "BAIXO";
    else if (indice <= 5) nivel = "MEDIO";
    else nivel = "ALTO";
  }
}


void apagarLeds() {
  digitalWrite(LED_VERDE, LOW);
  digitalWrite(LED_AMARELO, LOW);
  digitalWrite(LED_VERMELHO, LOW);
  digitalWrite(LED_AZUL, LOW);
}


void atualizarLeds() {
  apagarLeds();
  if (nivel == "BAIXO") digitalWrite(LED_VERDE, HIGH);
  if (nivel == "MEDIO") digitalWrite(LED_AMARELO, HIGH);
  if (nivel == "ALTO" || nivel == "LETAL") digitalWrite(LED_VERMELHO, HIGH);
  if (nivel == "ERRO") digitalWrite(LED_AZUL, HIGH);

  // bipa so quando o risco muda pra alto
  if (nivel == "ALTO" && nivelAnterior != "ALTO") {
    tone(BUZZER, 2000, 300);
  }
  nivelAnterior = nivel;
}


void mostrarNaTela() {
  tela.fillScreen(TFT_BLACK);
  tela.setCursor(0, 0);
  tela.println("Armadilha 01");
  tela.print("Ar: ");
  tela.println(tempAr, 1);   // 1 casa decimal
  tela.print("Umid: ");
  tela.println(umidade, 0);
  tela.print("Agua: ");
  tela.println(tempAgua, 1);
  tela.print("Indice: ");
  tela.println(indice);
  tela.println(nivel);
}


void conectarMQTT() {
  // sem Wi-Fi nao adianta tentar o broker
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Sem Wi-Fi, tentando reconectar");
    WiFi.reconnect();
    return;
  }

  if (mqtt.connect(BROKER_IP, 1883)) {
    // o true no beginMessage pede pro broker guardar a mensagem
    mqtt.beginMessage("dage/armadilha/01/status", true);
    mqtt.print("conectada");
    mqtt.endMessage();

    mqtt.subscribe("dage/armadilha/01/cmd/#");   // # = todos os comandos
    Serial.println("MQTT conectado");
    enviarConfig();
  } else {
    Serial.print("Erro MQTT: ");
    Serial.println(mqtt.connectError());
  }
}


void enviar(String topico, String valor) {
  if (!mqtt.connected()) return;   // sem conexao so nao envia
  mqtt.beginMessage(topico);
  mqtt.print(valor);
  mqtt.endMessage();
}


// igual ao enviar, mas o broker guarda a ultima mensagem
void enviarRetido(String topico, String valor) {
  if (!mqtt.connected()) return;
  mqtt.beginMessage(topico, true);
  mqtt.print(valor);
  mqtt.endMessage();
}


// devolve pro painel o estado e os limites em uso (secao 11.2)
// se um comando for recusado, o painel recebe de volta o valor antigo
void enviarConfig() {
  enviarRetido("dage/armadilha/01/config/monitoramento", monitorando ? "1" : "0");
  enviarRetido("dage/armadilha/01/config/ar_min", String(arMin, 1));
  enviarRetido("dage/armadilha/01/config/ar_max", String(arMax, 1));
  enviarRetido("dage/armadilha/01/config/umid_alta", String(umidAlta, 1));
  enviarRetido("dage/armadilha/01/config/agua_letal", String(aguaLetal, 1));
}


void enviarDados() {
  if (nivel != "ERRO") {
    enviar("dage/armadilha/01/ar", String(tempAr, 1));
    enviar("dage/armadilha/01/umidade", String(umidade, 1));
    enviar("dage/armadilha/01/agua", String(tempAgua, 1));
  }
  enviar("dage/armadilha/01/indice", String(indice));
  enviar("dage/armadilha/01/nivel", nivel);
}


void receberComando(int tamanho) {
  String t = mqtt.messageTopic();
  String texto = mqtt.readString();
  float valor = texto.toFloat();

  Serial.println(t + " = " + texto);

  // so aceita valores que fazem sentido
  if (t == "dage/armadilha/01/cmd/monitoramento" && texto == "1") monitorando = true;
  if (t == "dage/armadilha/01/cmd/monitoramento" && texto == "0") monitorando = false;
  if (t == "dage/armadilha/01/cmd/ar_min" && valor >= 15 && valor < arMax) arMin = valor;
  if (t == "dage/armadilha/01/cmd/ar_max" && valor > arMin && valor <= 40) arMax = valor;
  if (t == "dage/armadilha/01/cmd/umid_alta" && valor > 60 && valor <= 100) umidAlta = valor;
  if (t == "dage/armadilha/01/cmd/agua_letal" && valor > 36 && valor <= 60) aguaLetal = valor;

  salvarConfig();
  enviarConfig();
}


// salva o estado e os limites pra voltarem depois de reiniciar
void salvarConfig() {
  memoria.putBool("monitorando", monitorando);
  memoria.putFloat("ar_min", arMin);
  memoria.putFloat("ar_max", arMax);
  memoria.putFloat("umid_alta", umidAlta);
  memoria.putFloat("agua_letal", aguaLetal);
}
