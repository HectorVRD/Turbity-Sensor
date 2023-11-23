/*
 * Connect the SD card to the following pins:
 *
 * SD Card | ESP32
 *    D2       -
 *    D3       SS
 *    CMD      MOSI
 *    VSS      GND
 *    VDD      3.3V
 *    CLK      SCK
 *    VSS      GND
 *    D0       MISO
 *    D1       -
 */

#include <Arduino.h>            //utilizar esp32S com framework do arduino
//#include <Wire.h>               //Scanner I2C, verifica e retorna o endereço do dispositivo I2C encontrado
#include <LiquidCrystal_I2C.h>  //Biblioteca para utilização do LCD por I2C
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include <WiFi.h>
#include <NTPClient.h> //https://github.com/taranais/NTPClient
#define SD_TIMEOUT 15000 // Tempo entre uma e outra tentativa de conexão (ms) 
#define WIFI_TIMEOUT 15000 // Tempo entre uma e outra tentativa de conexão (ms) 
char ssid[34] = "xxx"; // <----- Inserir nome da rede WI FI no lugar de "xxx"
char password[66] = "xxx"; // <----- Inserir senha da rede WI FI no lugar de "xxx"
WiFiUDP dataOutUDP;
WiFiUDP ntpUDP;
NTPClient ntp(ntpUDP);

LiquidCrystal_I2C lcd(0x27, 16, 2);  //Define o endereço do LCD para 0x3F para um display de 16 caracteres e 2 linhas

// Definição do canal DAC0 (GPIO 25)
#define CANAL_DAC0 25
byte dacOnValue = 100;  // Intensidade do emissor ("0 a 255" e "0 a 3,3V")
#define TOTAL_SAMPLES 30 // Total de amostras
#define TIME_TO_SAMPLE 100 //  Tempo (ms) entre cada amostra
#define TIME_TO_PRINT 1000 // Tempo (ms) entre cada att da Tela
unsigned int adcRead = 0;
unsigned int turbidez = 0;


template <typename t = byte, t s = 17> // 16 + "\0"
class Line{
  char line[s];
  t cursor;  // Armazena onde está o cursor na linha

  public:
  Line(){
    cursor = 0;
    line[0] = '\0'; // Evita lixo de mem
  }

  Line<t, s>& operator<<(char c){
    if(cursor < s - 1){
      line[cursor++] = c; // Continua inserindo os caracteres
      line[cursor] = '\0'; //Define o final da frase
    }
    return *this;
  }
 
  Line<t, s>& operator<<(const char *str){
    for(t i = 0; cursor < s-1 && str[i] != '\0'; i++)
      line[cursor++] = str[i]; // Copia a string recebida pra string da classe
    line[cursor] = '\0'; // Para a string
    return *this;
  }

  Line<t, s>& operator<<(int v){
    char aux[12];
    itoa(v, aux, 10); // conv int em char base 10 
    (*this) << aux;
    return *this;
  }

  Line<t, s>& operator<<(unsigned int v){
    (*this) << (int)v; // conv unsigned int em int
    return *this;
  }

  const char* getStr(){return line;} //retorna o ponteiro da string
  //char* getNonConstStr(){return line;} 
  //t getBufferSize(){return s;} //qts caracteres cabem na string

  void clear(){
    cursor = 0;
    line[0] = '\0';
  }

  void fillExcedent(char c){
    while(cursor < s-1) line[cursor++] = c; // preenche o resto da string com o mesmo char
    line[cursor] = '\0';
  }
};

bool strCompare(const char *str1, const char *str2){
  for(int i = 0; true; i++){
    byte endCheck = (str1[i] == '\0') + (str2[i] == '\0');
    if(endCheck == 1) return false; // Final de uma string, não de ambas
    if(endCheck == 2) return true; // Final de ambas
    if(str1[i] != str2[i]) return false; // Caracteres diferentes
  }
}

void movingScreenHeader(int optional = -1){
  static unsigned int n = 0;
  const int totalHeaders = 2; // Total de telas
  if(optional >= 0) n = optional; // Se não informado, corre normal entre as telas
  if(n >= totalHeaders) n = 0;
  lcd.clear();
  lcd.setCursor(0, 0); // Define a primeira linha na primeira posição
  switch(n){
    case 0:{
      lcd.print("Turbidez liquido");
      printTurbidezLine();
    }break;
    case 1:{
      if(WiFi.status() == WL_CONNECTED){ // Só puxa a hora se WIFI conectado
        Line<> line;
        line << ntp.getHours() << 'h' << ntp.getMinutes() << 'm'; // colocar aqui dd:mm:aa
        lcd.print(line.getStr());

        line.clear();
        line << WiFi.localIP().toString().c_str(); // Converte IP do ESP para string e depois pra char array
        lcd.setCursor(0, 1); 
        lcd.print(line.getStr()); // Printa a string na tela
      }else{
        lcd.print("WiFi disconected");
        printTurbidezLine();
      }
      
    }break;
    default: break;
  }
  n++;
}

void printTurbidezLine(){
  Line<> line;
  line << turbidez << " NTU";
  line.fillExcedent(' ');
  lcd.setCursor(0, 1);
  lcd.print(line.getStr());
}

void setup() { // ------------------------------------ SETUP ----------------------------------------

  lcd.init(); // Inicializa LCD
  lcd.clear();
  lcd.backlight();  // Liga a luz de fundo

  lcd.setCursor(0, 0);  //Define onde o cursor inicia
  lcd.print("Turbidez líquido!");
  SD.begin(); // Inicializa SD
  WiFi.mode(WIFI_STA); // Cliente receptor do wifi (moldem)
  Serial.begin(115200); // Inicializa a porta serial do ESP
}  // fim do setup

void loop() { // ------------------------------------- LOOP ----------------------------------------
  static unsigned long timeToPrint = 0, timeToSample = 0, wifiReconnectionTime = 0, sdReconnectionTime = 0;
  static byte sampleCount = 0;
  static bool wifiConnected = false, sdConnected = false, shouldTransmitUDP = false;

  if(!sdConnected){
    if(SD.cardType() != CARD_NONE){
      sdConnected = true;
    }else if(millis() > sdReconnectionTime){
      SD.begin();
      sdReconnectionTime = millis() + SD_TIMEOUT;
    }
  }

  if(!wifiConnected){ // Se não estiver conectado
    if(WiFi.status() == WL_CONNECTED){ // Se conectar
      wifiConnected = true; // Inicializa WIFI
      dataOutUDP.begin(82); // Inicia o servidor udp na porta 82
      ntp.begin(); // Inicializa o cliente que recebe a hora da rede que está conectado
      ntp.update();
    }else if(millis() > wifiReconnectionTime){ // Se o tempo limite for atingido
      if(SD.cardType() != CARD_NONE){ // Se detec SD, tenta carregar id e senha do WIFI 
        File file = SD.open("/wifi/credentials.txt", FILE_READ);
        if(file){
          file.readBytesUntil('\n', ssid, sizeof(ssid));
          file.readBytesUntil('\n', password, sizeof(password));
          file.close();
        }
      }
      WiFi.begin(ssid, password); // Tentativa de conexão
      wifiReconnectionTime = millis() + WIFI_TIMEOUT; // Define novo tempo limite
    }
  }else if(WiFi.status() != WL_CONNECTED){
    wifiConnected = false;
    shouldTransmitUDP = false; 
  }else{
    unsigned int packetSize = dataOutUDP.parsePacket(); // Obter o tamanho da mensagem do udp
    if(packetSize){ // Se maior que 0 bytes 
      char buffer[10];
      int len = dataOutUDP.read(buffer, 10); // Lê pacote para buffer
      buffer[len] = '\0'; // Sabendo do comprimendo, define o "\0" para indicar final da string
      if(strCompare(buffer, "start")){ // Se "start"
        shouldTransmitUDP = true; // Amostras devem ser enviadas periodicamente
        Line<byte, 64> line; // Cria uma linha para a mensagem a seguir:
        line << "Transmitindo uma média de "<<TOTAL_SAMPLES<<" amostras a cada "<<TIME_TO_SAMPLE*TOTAL_SAMPLES<<" ms\n";
        dataOutUDP.beginPacket(dataOutUDP.remoteIP(), dataOutUDP.remotePort()); // Inicia a transmissão para o local de origem da mensagem
        dataOutUDP.print(line.getStr());                                        
        dataOutUDP.endPacket(); // Termina de enviar mensagem
      }else if(strCompare(buffer, "stop")) shouldTransmitUDP = false; // O Comando "stop" deve interromper a transmissão
    }
  }

  if(sampleCount < TOTAL_SAMPLES){
    if(timeToSample <= millis()){ //Igual ou maior ao tempo atual
      dacWrite(CANAL_DAC0, 65);
      delay(10); // Garante emissor ligado
      adcRead += analogRead(35); // Soma a amostra lida na porta analogica p poder tirar a media
      dacWrite(CANAL_DAC0, 0);
      timeToSample = millis() + TIME_TO_SAMPLE; // Tpo atual mais tempo da próx amostra
      sampleCount++;
    }
  }else{
    adcRead = adcRead / TOTAL_SAMPLES; // Média das leituras
    turbidez = ((0.2098*(adcRead))+28.31); // <<<<----------------------- Fazer a conversão aqui -----------
    //turbidez = adcRead; // <<<<----------------------- Fazer a conversão aqui -----------
    adcRead = 0; // Após realizar a leitura, define como 0 para não interferir em novas leituras
    sampleCount = 0; // Reseta as amostras 
    Line<> line; // usar, por ex.: <byte, 32> para mudar o tamanho da linha para 32
    line << turbidez << " \n"; // <<---------------------------- terminar a formatação ----------------

    Serial.print(line.getStr()); // Imprime a linha na porta serial (monitor)

    if(shouldTransmitUDP){ // Se pretende fazer transmissões periódicas, transmita a linha
      dataOutUDP.beginPacket(dataOutUDP.remoteIP(), dataOutUDP.remotePort());
      dataOutUDP.print(line.getStr());
      dataOutUDP.endPacket();
    }

    lcd.setCursor(0, 0); // A próxima será a mensagem no topo (nova tela)
    if(SD.cardType() == CARD_NONE){
      lcd.println("#No SD card     ");
      printTurbidezLine();
      timeToPrint = millis() + TIME_TO_PRINT;
      //SD.begin();
    }else{
      File file = SD.open("/turbidez.txt", FILE_APPEND, true);
      if(file){ // Se o arquivo estiver aberto
        if(!file.print(line.getStr())){ // Se não der pra imprimir no SD
          lcd.print("#File error     ");
          printTurbidezLine();
          timeToPrint = millis() + TIME_TO_PRINT;
        }
      }else{
        lcd.print("#File not open  ");
        printTurbidezLine();
        timeToPrint = millis() + TIME_TO_PRINT;
      }
      file.close();
    }
  }

  if(timeToPrint <= millis()){ // Tpo previsto para atualizar o display
    movingScreenHeader(); //Alterna a tela
    timeToPrint = millis() + TIME_TO_PRINT;
  }
}