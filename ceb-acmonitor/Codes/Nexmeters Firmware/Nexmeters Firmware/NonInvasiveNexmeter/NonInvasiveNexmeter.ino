#include <WiFi.h>
#include <MCP3202.h>
#include <EEPROM.h>
#include <NTPClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "time.h"
#include <Wire.h>
#include <WiFiUdp.h>

#define ADC_CS_PIN  16
#define ADC_CS_PIN2  17
#define ADC_BITS    12
#define ADC_COUNTS  (1 << ADC_BITS)
#define ICAL        20.2    //Valor de calibracao para corrente
#define VCAL        258.8   //Valor de calibracao para tensao
#define PHASECAL    1.7     //Valor de calibracao para fase


MCP3202 adc0 = MCP3202(ADC_CS_PIN); // Instancia ADC externo com pino CS 16
MCP3202 adc1 = MCP3202(ADC_CS_PIN2); // Instancia ADC externo com pino CS 16

IPAddress local_IP(192, 168, 4, 1);
IPAddress gateway(192, 168, 4, 9);
IPAddress subnet(255, 255, 255, 0);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "a.ntp.br", -14400, 100);

AsyncWebServer server(80);

/* Varivaeis para a funcao de calcular tensao e corrente, elas precisam ser globais */
int sampleI0 = 0;                      //Armazena amostra raw do ADC
int sampleI1 = 0;
int sampleV = 0;                      //Armazena amostra raw do ADC
double lastFilteredV, filteredV;      //Filtered_ is the raw analog value minus the DC offset
double filteredI0, filteredI1;
double offsetV = ADC_COUNTS >> 1;     //Low-pass filter output
double offsetI0 = ADC_COUNTS >> 1;     //Low-pass filter output
double offsetI1 = ADC_COUNTS >> 1;     //Low-pass filter output
double phaseShiftedV;                 //Holds the calibrated phase shifted voltage.
double sqV, sumV, sqI1, sqI0 , sumI0, sumI1, instP0, instP1, sumP0, sumP1; //sq = squared, sum = Sum, inst = instantaneous
int startV;                           //Instantaneous voltage at start of sample window.
boolean lastVCross, checkVCross;      //Used to measure number of times threshold is crossed.
double realPower0, realPower1, apparentPower0, apparentPower1, powerFactor, Vrms, Irms0, Irms1;
int SupplyVoltageV = 3300;
int SupplyVoltageI = 5000;

const char* ntpServer = "a.ntp.br";
long  gmtOffset_sec = 0;
const int   daylightOffset_sec = 0;

boolean wifi_flag = 0;
boolean post_flag = false;

static uint8_t taskCoreZero = 0;
static uint8_t taskCoreOne  = 1;

TaskHandle_t Task1;
TaskHandle_t Task2;
const short int postsampling = 60; // amostras por post
const short int powerline = 60;

double power_vector[postsampling];
double power_matrix[powerline][postsampling];
unsigned long time_vector[powerline];

byte power_vector_index = 0;

short int k = 0;
short int j = 0;

char payload[2000];


void setup()
{
  Serial.begin(115200); // Inicializa monitor serial
  EEPROM.begin(128);  // Inicializa EEPROM
  adc0.begin(); // Inicializa ADC externo
  adc1.begin();
  analogReadResolution(12);
  analogSetAttenuation(ADC_0db);

  while (wifiConfig() == 0) { // Aguarda conexão com rede Wi-Fi
    while (wifi_flag == 0) {  // Se não conseguiu, cria AP para configuração
      Serial.write('.');
      delay(500);
    }
  }


  for (int i = 0; i < 25; i++)
  {
    getPower();
  }

  NTPCheck();

  xTaskCreatePinnedToCore(
    TaskPower,   /* Task function. */
    "Task1",     /* name of task. */
    10000,       /* Stack size of task */
    NULL,        /* parameter of the task */
    1,           /* priority of the task */
    &Task1,      /* Task handle to keep track of created task */
    0);          /* pin task to core 0 */
  delay(500);

  //create a task that will be executed in the Task2code() function, with priority 1 and executed on core 1
  xTaskCreatePinnedToCore(
    TaskCommunication,   /* Task function. */
    "Task2",     /* name of task. */
    10000,       /* Stack size of task */
    NULL,        /* parameter of the task */
    1,           /* priority of the task */
    &Task2,      /* Task handle to keep track of created task */
    1);          /* pin task to core 1 */
  delay(500);

}


void loop() {}


void TaskPower( void * pvParameters )
{

  while (true)
  {
    int timeloop = millis();
    ComputePower();
    timeloop = millis() - timeloop;
    Serial.print(timeloop);
    Serial.print(" ");

    if (timeloop < 1000) {
      delay(1000 - timeloop);
    }
  }

}


void TaskCommunication( void * pvParameters )
{

  while (true)
  {
    if (post_flag == true) {

      while (k < j)
      {

        DataToJson();

        if (WiFi.status() == WL_CONNECTED)
        {
          PostToServer();
        }

        if (WiFi.status() != WL_CONNECTED)
        {
         reconnect();
        }

        if (k >= j)
        {
          post_flag = false;
          break;
        }
      }
    }
    delay(100);
  }

}


void NTPCheck()
{
  timeClient.begin();

  while (!timeClient.forceUpdate()) {
    Serial.println("Atualizando NTP");
  }
  Serial.println(timeClient.getFormattedTime());
}


String getTimeStampString(int k)
{
  time_t rawtime = time_vector[k];
  struct tm * ti;
  ti = localtime (&rawtime);

  uint16_t year = ti->tm_year + 1900;
  String yearStr = String(year);

  uint8_t month = ti->tm_mon + 1;
  String monthStr = month < 10 ? "0" + String(month) : String(month);

  uint8_t day = ti->tm_mday;
  String dayStr = day < 10 ? "0" + String(day) : String(day);

  uint8_t hours = ti->tm_hour;
  String hoursStr = hours < 10 ? "0" + String(hours) : String(hours);

  uint8_t minutes = ti->tm_min;
  String minuteStr = minutes < 10 ? "0" + String(minutes) : String(minutes);

  uint8_t seconds = ti->tm_sec;
  String secondStr = seconds < 10 ? "0" + String(seconds) : String(seconds);

  return yearStr + "-" + monthStr + "-" + dayStr + " " +
         hoursStr + ":" + minuteStr + ":" + secondStr;
}