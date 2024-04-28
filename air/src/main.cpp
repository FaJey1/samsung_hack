#include <Arduino.h>
#include <nlohmann/json.hpp>
#include <WiFi.h>
#include <AsyncEventSource.h>
#include <HTTPClient.h>
#include <vector>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>
#include <Barometer.h>
#include <SDS011.h>
#include <TroykaMQ.h>

using json = nlohmann::json;

String HOSTNAME = "samarakosmos";
String WIFI_SIID_SERVER = "ESP32-Access-Point";
String WIFI_PASSWORD_SERVER = "12345678";
String WIFI_SIID_CLIENT = "";
String WIFI_PASSWORD_CLIENT = "";
String DB_IP = "10.0.118.61";
String DB_PORT = "8428";
int SEND_DB_DELAY = 5;

#define NUM_READ 10  // порядок медианы
float P25 = 0.;
float P10 = 0.;
float LUMINANCE = 0.;
float PASCALS = 0.;
float MMHG = 0.;
float HEIGHT = 0.;
float HUMIDITY = 0.;
float TEMPERATURE = 0.;

//SDS011
//-----------------
HardwareSerial* serial_sds = &Serial2;
SDS011 my_sds;
int sds_err;
float p25, p10;
//-----------------
//DHT11
//-----------------
DHT_Unified dht(23, 11);
//Barometer--------
Barometer barometer(0x5C);
MQ135 mq135(16);

AsyncWebServer WebServer(80);
bool CONNECTION_STATUS = false;
const char index_html_configure[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
  <title>ESP Input Form</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  </head><body>
  <form action="/get">
    network_name: <input type="text" name="network_name">
    network_password: <input type="text" name="network_password">
    device_name: <input type="text" name="device_name">
    db_ip: <input type="text" name="db_ip">
    db_port: <input type="text" name="db_port">
    <input type="submit" value="Submit">
  </form>
</body></html>)rawliteral";


void setup_wifi_server() {
  Serial.print("Setting AP (Access Point)…");
  WiFi.softAP(WIFI_SIID_SERVER, WIFI_PASSWORD_SERVER);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println("IP address: ");
  Serial.println(IP);
  Serial.println("Server started");
}

void setup_wifi_client() {
  delay(10);
  int attempts = 0;
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(WIFI_SIID_CLIENT);
  WiFi.setHostname(HOSTNAME.c_str());
  WiFi.begin(WIFI_SIID_CLIENT, WIFI_PASSWORD_CLIENT);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print("connecting... ");
    Serial.println(attempts);
    attempts += 1;
    if (attempts == 30) {
      break;
    }
  }
  if (attempts >= 30) {
    WIFI_SIID_CLIENT = "";
    WIFI_PASSWORD_CLIENT = "";
    HOSTNAME = "";
    Serial.println("");
    Serial.println("WiFi connect faild");
  }
  else {
    CONNECTION_STATUS = true;
    WiFi.softAPdisconnect(true);
    WebServer.end();
    Serial.println("");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
  }
}


void not_found(AsyncWebServerRequest * request) {
  request -> send(404, "text/plain", "Not found");
}


void html_page_configure() {
  WebServer.on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
    request -> send_P(200, "text/html", index_html_configure);
  });
  WebServer.on("/get", HTTP_GET, [](AsyncWebServerRequest * request) {
      String inputParam;
      if (request -> hasParam("network_name")) {
        WIFI_SIID_CLIENT = request -> getParam("network_name") -> value();
        WIFI_PASSWORD_CLIENT = request -> getParam("network_password") -> value();
        HOSTNAME = request -> getParam("device_name") -> value();
        DB_IP = request -> getParam("db_ip") -> value();
        DB_PORT = request -> getParam("db_port") -> value();
        inputParam = "network credentials upload successfull, device access in your network by name: \"" + HOSTNAME + "\"";
      } else {
        inputParam = "none";
      }
      Serial.println(WIFI_SIID_CLIENT + " " + WIFI_PASSWORD_CLIENT);
      request -> send(200, "text/html", "HTTP GET request sent to your ESP on " +
        inputParam + ", with value: " +  WIFI_SIID_CLIENT + " " + WIFI_PASSWORD_CLIENT + " db_ip: " + DB_IP + " db_port: " + DB_PORT +
        "<br><a href=\"/\">Return to Home Page</a>");
    }

  );
  WebServer.onNotFound(not_found);
  WebServer.begin();
}

void network_configure() {
  setup_wifi_server();
  html_page_configure();
  while (!CONNECTION_STATUS) {
    while (true) {
      if (WIFI_SIID_CLIENT != "" and WIFI_PASSWORD_CLIENT != "" and HOSTNAME != "" and DB_IP != "" and DB_PORT != "") {
        break;
      }
    }
    setup_wifi_client();
  }
}

void send_to_db(String message){
  HTTPClient http;
  String url = "http://" + DB_IP + ":" + DB_PORT + "/api/v1/import/prometheus";
  Serial.print("DB URL: ");
  Serial.println(url);
  Serial.print("DB MESSAGE: ");
  Serial.println(message);
  http.begin(url);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int httpCode = http.POST(message);
  if (httpCode > 0) {
      String payload = http.getString();
      Serial.println(httpCode);
      Serial.println(payload);
  } else {
      Serial.println("Error on sending POST: " + http.errorToString(httpCode));
  }
  http.end();
}

TaskHandle_t Task1;
void send_metrics(void * pvParameters) {
  while (true) {
    if (WiFi.status() != WL_CONNECTED) {
      network_configure();
    }
    std::vector<String> metrics{"temperature", "humidity", "air_quality_p25", "air_quality_p10", "luminance", "pascals", "mmhg", "height"};
    std::vector<float> metrics_float{TEMPERATURE, HUMIDITY, P25, P10, LUMINANCE, PASCALS, MMHG, HEIGHT};
    for (int metric = 0; metric < metrics.size(); metric++){
      String message = HOSTNAME + "_" + metrics[metric] + "{device_name=" + "\"" + HOSTNAME + "\"" + "} " + metrics_float[metric];
      Serial.println(message);
      send_to_db(message);
      delay(1000);
    }
    delay(SEND_DB_DELAY * 1000);
  }
}

//Мониторинг влажности и температуры
void monitor_dht()
{
  sensors_event_t event;
  dht.temperature().getEvent(&event);
  if (isnan(event.temperature)) {
    Serial.println(F("Error reading temperature!"));
  }
  else {
    TEMPERATURE = event.temperature;
    Serial.print(F("Temperature: "));
    Serial.print(TEMPERATURE);
    Serial.println(F("°C"));
  }
  // Get humidity event and print its value.
  dht.humidity().getEvent(&event);
  if (isnan(event.relative_humidity)) {
    Serial.println(F("Error reading humidity!"));
  }
  else {
    HUMIDITY = event.relative_humidity;
    Serial.print(F("Humidity: "));
    Serial.print(HUMIDITY);
    Serial.println(F("%"));
  }
}

//Мониторинг освещённости
void monitor_lumin()
{
  uint16_t analogValue = analogRead(34);
  LUMINANCE = 100 - ((float)analogValue/4095 * 100); 
  Serial.print("Analog Value = ");
  Serial.print(LUMINANCE);   // the raw analog reading

  // We'll have a few threshholds, qualitatively determined
  if (LUMINANCE < 20) {
  Serial.println(" => Dark");
  } else if (LUMINANCE < 40) {
  Serial.println(" => Dim");
  } else if (LUMINANCE < 60) {
  Serial.println(" => Light");
  } else if (LUMINANCE < 80) {
  Serial.println(" => Bright");
  } else {
  Serial.println(" => Very bright");
  }
}

void monitor_air()
{
  sds_err = my_sds.read(&P25, &P10);
	if (!sds_err) {
		Serial.println("P2.5: " + String(P25) + " мг/м³;  PM 10: " + String(P10) + " мг/м³");
	} 
}

void monitor_pressure()
{
  PASCALS = barometer.readPressurePascals();
  // Создаём переменную для значения атмосферного давления в мм рт.ст.
  MMHG = barometer.readPressureMillimetersHg();
  // Создаём переменную для значения высоты над уровнем море
  HEIGHT = barometer.readAltitude();

  // Вывод данных в Serial-порт
  Serial.print("Pressure: ");
  Serial.print(PASCALS);
  Serial.print(" Pa\t");
  Serial.print(MMHG);
  Serial.print(" mmHg\t");
  Serial.print("Height: ");
  Serial.print(HEIGHT);
  Serial.print(" m \t");
}


// медиана на N значений со своим буфером, ускоренный вариант
float findMedianN_optim(float newVal) {
  static float buffer[NUM_READ];  // статический буфер
  static byte count = 0;
  buffer[count] = newVal;
  if ((count < NUM_READ - 1) and (buffer[count] > buffer[count + 1])) {
    for (int i = count; i < NUM_READ - 1; i++) {
      if (buffer[i] > buffer[i + 1]) {
        float buff = buffer[i];
        buffer[i] = buffer[i + 1];
        buffer[i + 1] = buff;
      }
    }
  } else {
    if ((count > 0) and (buffer[count - 1] > buffer[count])) {
      for (int i = count; i > 0; i--) {
        if (buffer[i] < buffer[i - 1]) {
          float buff = buffer[i];
          buffer[i] = buffer[i - 1];
          buffer[i - 1] = buff;
        }
      }
    }
  }
  if (++count >= NUM_READ) count = 0;
  return buffer[(int)NUM_READ / 2];
}

float get_median(float arr[NUM_READ])
{
  float median;
  for(uint8_t metric = 0; metric < NUM_READ; metric++)
  {
    median = findMedianN_optim(arr[metric]);
  }

  return median;
}

void setup() {
  Serial.begin(115200);
  network_configure();
  xTaskCreatePinnedToCore(
  send_metrics,   /* Функция задачи. */
  "Task1",     /* Ее имя. */
  10000,       /* Размер стека функции */
  NULL,        /* Параметры */
  1,           /* Приоритет */
  &Task1,      /* Дескриптор задачи для отслеживания */
  1);          /* Указываем пин для данного ядра */
  barometer.begin();
  my_sds.begin(serial_sds);
  dht.begin();
}

void loop() {
  float MEDIAN_AIR_P25[NUM_READ];
  float MEDIAN_AIR_P10[NUM_READ];
  float MEDIAN_DHT_HUMIDITY[NUM_READ];
  float MEDIAN_DHT_TEMPERATURE[NUM_READ];
  float MEDIAN_LUMIN[NUM_READ];
  float MEDIAN_PRESSURE_PA[NUM_READ];
  float MEDIAN_PRESSURE_MMHG[NUM_READ];
  float MEDIAN_PRESSURE_HEIGHT[NUM_READ];
  for(uint8_t metric = 0; metric < NUM_READ; metric++)
  {
    monitor_air();
    monitor_dht();
    monitor_lumin();
    monitor_pressure();
    Serial.println();
    MEDIAN_AIR_P25[metric] = P25;
    MEDIAN_AIR_P10[metric] = P10;
    MEDIAN_DHT_HUMIDITY[metric] = HUMIDITY;
    MEDIAN_DHT_TEMPERATURE[metric] = TEMPERATURE;
    MEDIAN_LUMIN[metric] = LUMINANCE;
    MEDIAN_PRESSURE_PA[metric] = PASCALS;
    MEDIAN_PRESSURE_MMHG[metric] = MMHG;
    MEDIAN_PRESSURE_HEIGHT[metric] = HEIGHT;
    delay(300);
  }
  HUMIDITY = get_median(MEDIAN_DHT_HUMIDITY);
  TEMPERATURE = get_median(MEDIAN_DHT_TEMPERATURE);
  P25 = get_median(MEDIAN_AIR_P10);
  P10 = get_median(MEDIAN_AIR_P25);
  LUMINANCE = get_median(MEDIAN_LUMIN);
  PASCALS = get_median(MEDIAN_PRESSURE_PA);
  MMHG = get_median(MEDIAN_PRESSURE_MMHG);
  HEIGHT = get_median(MEDIAN_PRESSURE_HEIGHT);
  Serial.println("-----MEDIANS-----");
  Serial.println(HUMIDITY);
  Serial.println(TEMPERATURE);
  Serial.println(P25);
  Serial.println(P10);
  Serial.println(LUMINANCE);
  Serial.println(PASCALS);
  Serial.println(MMHG);
  Serial.println(HEIGHT);
  Serial.println("-----------------");
}
