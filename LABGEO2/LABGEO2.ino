//
//    FILE: HX_is_ready.ino
//  AUTHOR: Rob Tillaart
// PURPOSE: HX711 demo
//     URL: https://github.com/RobTillaart/HX711

#define MODEL SCDR
#define T_MIN CELDA0
#define T_MAX CELDA1

#define REQ1 				15
#define DATA1       39
#define CLK1 				34

#define REQ2 				25
#define DATA2 		  13
#define CLK2 				14

#define BUSSER 				2

#define DIALA    30
#define DIALB    35
#define DIALC    40
#define DIALD    115

#define CARGAA    45
#define CARGAB    70
#define CARGAC    75
#define CARGAD    125

#define PRESA    80
#define PRESB    85
#define PRESC    110
#define PRESD    120

#include <SCDR.h>
#include <Ethernet.h>
#include "HX711.h"
#include "string.h"
#include "SPIFFS.h"
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>



HX711 scale;
SCDRClass scdr;
Adafruit_ADS1115 ads;
byte mac[] = { 0xDC, 0xA6, 0x32, 0x10, 0x16, 0x2C };
byte ip[] = { 0, 0, 1, 2 };
byte gateway[] = { 192, 168, 1, 1 };
byte server[] = { 192, 168, 1, 1 };
byte subnet[] = { 255, 255, 255, 0 };
EthernetServer serverETH(80);
uint8_t dataPin = 22;
uint8_t clockPin = 21;
char msg[50];

char data[100] = "ll1111";
char monitoreo[150] = "";
char flash[150] = "";
char energia[180] = "";
char modbus[150] = "";
char data_set[100] = "";
EepromRegsSCDR reg;
long lastMsg = 0;

uint8_t corrida1=0;
uint8_t corrida2=0;

uint32_t segundos[20]={0,15,30,60,120,240,480,900,1800,3600,7200,14400,28800,57600,86400,86410};
uint32_t micrometros[70]={50,100,150,200,300,400,500,600,700,800,900,1000,1200,1400,1600,1800,2000,2200,2400,2600,2800,3000,3200,3400,3600,3800,4000,4200,4400,4600,4800,5000,5500,6000,6500,
	7000,7500,8000,8500,9000,9500,10000,10500,11000,11500};
//41
// 0 seg,15seg,30seg,1m,2m,4m,8m,15m,30m,1h,2h,4h,8h,16,24h   

struct Params2
{
	float DIALA2;
  float DIALB2;
  float CARGAA2;
  float CARGAB2;
  float PRESA2;
  float PRESB2;
};
Params2 params;
/*

Pines dial:

NEGRO(ROJO)  GND
AZUL (AMARILLO) MENU
VERDE(VERDE) DATA
MARRON(AZUL) CLK
ROJO(BLANCO) DIRECTO AL TRANSISTOR

*/
uint8_t Out1=0;
uint8_t Out2=0;

void setup()
{
  Serial.begin(115200);
  Serial.println(__FILE__);
  Serial.print("LIBRARY VERSION: ");
  Serial.println(HX711_LIB_VERSION);
  pinMode(BUSSER, OUTPUT);
  sonido();
  Serial.println("OK2222222..........");
  if (!ads.begin(DIR_ADC_DAT)) { Serial.println("Failed to initialize ADS DIR_ADC_DAT"); }
  ads.setGain(GAIN_ONE);
  
  eeprom_start();
  eth_start();

  scale.begin(dataPin, clockPin);
  if (!SPIFFS.begin(true)) {
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }
  // TODO find a nice solution for this calibration..
  // load cell factor 20 KG
  // scale.set_scale(127.15);

  // load cell factor 5 KG
  //scale.set_scale(420.0983);       // TODO you need to calibrate this yourself.
  // reset the scale to zero = 0
  //scale.tare(20); 
  
}


void looprrr()
{
   Serial.println("scale.get_units(1)");
   Serial.println(scale.is_ready());
  if (scale.is_ready())
  {
    Serial.println(scale.get_units(1));
  } 
  else Serial.println("error");
  delay(1000);
}

float celda=0;
uint8_t inter=1;

uint8_t V1=0;
long Tcorrida = 0;

uint32_t  corrida(float dial1,float dial2,float celda,uint8_t corrida1, uint8_t corrida2,long tiempo){
  char flash[50]=" ";
  if(corrida1==1 && (tiempo > (segundos[V1]*1000))){
    
    //aca se puede guardar la informacion.
    if(V1==0) scdr.whiteflash("INICIO DE EL ESTUDIO NUMERO 1");
    sprintf(flash, "%.3f|%.3f|%.3f|%d", dial1, dial2, celda, tiempo / 1000);
    scdr.whiteflash(flash);
    buzzer(50);
    V1=V1+1;
    if(V1==14){V1=0;;Serial.println("estudio1...finalizado");}
    
  }
  else if(corrida2==1 && (uint32_t(1000*dial2) > (micrometros[V1]))){
    
    //aca se puede guardar la informacion.
    if(V1==0) scdr.whiteflash("INICIO DE EL ESTUDIO NUMERO 2");
    sprintf(flash, "%.3f|%.3f|%.3f|%d", dial1, dial2, celda, tiempo / 1000);
    scdr.whiteflash(flash);
    buzzer(50);
    delay(100);
    buzzer(50);
    V1=V1+1;
    if(V1==43){V1=0;;Serial.println("estudio2...finalizado");}
    
  }
  
  if (corrida1 || corrida2) return tiempo / 1000;else {V1=0;return 0;}
}

int lectura(){
  int dato=0;
  float total=0;
  int n=1;
  while(n<=5){
    dato=analogRead(39);
    total=total+dato;
    n=n+1;
  }
  return(total/5);
}

void loop() {
  uint16_t respt;
  long now = millis();
  for (int i = 0; i <= 100; i++) {
    loopETH();
    delay(10);
  }



  if(millis()>=272000000) ESP.restart();
  if (now - lastMsg > 1000) {
    lastMsg = now;


    //StatusEth();

    /*switch(inter){
      case 1:
        if (scale.is_ready()){Serial.println(scale.get_units(1));}
        inter=2;
        break;
      case 2:
        Serial.println("dial1");
        Serial.println(dial(REQ1, CLK1, DATA1));
        inter=3;
        break;
      case 3:
        Serial.println("dial2");
        Serial.println(dial(REQ2, CLK2, DATA2));
        inter=1;
        break;
      default:
        inter=1;
    }*/

    // if (scale.is_ready()){Serial.println(scale.get_units(1));}
    // Serial.println(dial(REQ1, CLK1, DATA1));
    char strn[20];
    // sprintf(strn,"%s",dial(REQ1, CLK1, DATA1));
    // sprintf(strn,"%s",dial(REQ2, CLK2, DATA2));
    
    //Serial.println(strn);
    //Serial.println(atof(dial(REQ1, CLK1, DATA1))*1000);
    int in11 = lectura();
    int in22 = analogRead(15);

    Serial.println("in11");
    Serial.println(in11);
    Serial.println(float(in11)*3.3/4095);
    Serial.println("in22");
    Serial.println(float(in22)*3.3/4095);
    Serial.println(params.DIALA2);
    Serial.println(params.DIALB2);
    float dial1= 0;
    float dial2= 0;
    if (scale.is_ready()){
      Serial.println("E---------------------CELDA");
      Serial.println(scale.read_median(5));
      celda=(scale.read_median(5)-params.CARGAA2)/(1000*params.CARGAB2);
      }
    else Serial.println("ERRORRRR CELDA");
    corrida1=Out1;
    corrida2=Out2;
    uint32_t activo=1; 

    //sprintf(MQTTMessage,"{\"i\":{\"i1\":%.2f,\"i2\":%.2f,\"i3\":%.2f,\"i4\":%.2f,\"i5\":%.2f,\"i6\":%.2f,\"i7\":%d,\"i8\":%d,\"i9\":%.2f,\"i10\":%.2f,\"i11\":%.2f,\"i12\":%.2f,\"i13\":%.2f,\"i14\":%.2f},\"o\":{\"o1\":%d,\"o2\":%d,\"o3\":%d,\"o4\":%d},\"t\":%d,\"q\":%d}",T1,T2,T3,T4,temp,hum,at,bp,float(mb[0])/10,float(mb[1])/10,float(mb[2])/10,float(mb[3])/100,float(mb[4])/100,float(mb[5])/100,aa1,aa2,bs,aa1,millis()/1000,q);
    sprintf(monitoreo, "{\"d1\":%.3f,\"d2\":%.3f,\"d3\":%.3f,\"d4\":%d,\"d5\":%d,\"d6\":%d,\"d13\":%d}", ((float(in22)/params.DIALA2)+params.DIALB2), (in22/params.DIALA2), celda, corrida1, corrida2, activo, millis() / 1000);
    
    //  sprintf(monitoreo, "{\"s1\":{\"d1\":1,\"d2\":0,\"d3\":1,\"d4\":1,\"d5\":0},\"s2\":{\"d1\":1,\"d2\":0,\"d3\":1,\"d4\":1,\"d5\":0},\"s3\":{\"d1\":1,\"d2\":0,\"d3\":1,\"d4\":1,\"d5\":0},\"i1\":{\"d1\":1,\"d2\":0,\"d3\":1,\"d4\":1,\"d5\":0,\"d6\":1,\"d7\":0,\"d8\":1,\"d9\":1}}",millis() / 1000);

    //sprintf(energia, "{\"d1\":%.2f,\"d2\":%.2f,\"d3\":%.2f,\"d4\":%.2f,\"d5\":%.2f,\"d6\":%.2f,\"d7\":%.2f,\"d8\":%.2f,\"d9\":%d,\"d10\":%d,\"d11\":%d,\"d12\":%d,\"d13\":%d,\"d14\":%d}", float(mb[0]) / 10, float(mb[1]) / 10, float(mb[2]) / 10, float(mb[3]) / 100, float(mb[4]) / 100, float(mb[5]) / 100, float(mb[6]) / 10, float(mb[7]) / 10, mb[12], mb[13], mb[14], mb[15], inp1, inp2);
    //digitalWrite(LED1, 0);
    //mqttClient.publish(publish, MQTTMessage);
    Serial.println("[APP] Free memory: " + String(esp_get_free_heap_size()) + " bytes");
    Serial.println(monitoreo);
    delay(20);
    //cnt_trap_test++;
    
    //sprintf(flash,"%.2f|%.2f|%d|%d|%d|%d|%d|%d|%d|%d/%d/%d|%d:%d:%d|%s",temp,hum,bs,at,aa1,aa2,aa3,aa4,millis()/1000,now.day(),now.month(),now.year(),now.hour(),now.minute(),now.second(),String(esp_get_free_heap_size()));
    //FlashWork(1,flash);
    
    //scdr.whiteflash(flash);
    //if(cnt_trap_test>2) {cnt_trap_test=0;val=!val;Out2=val;}
    //digitalWrite(LED1, 1);
  }
}

void eth_start() {
  Serial.println("Ethernet start");
  pinMode(ETH_RST, OUTPUT);
  digitalWrite(ETH_RST, HIGH);
  delay(300);
  digitalWrite(ETH_RST, LOW);
  delay(200);
  digitalWrite(ETH_RST, HIGH);
  delay(300);
  Ethernet.init(ETH_SS);  // MKR ETH Shield
  Ethernet.begin(mac, ip, gateway, gateway, subnet);
  delay(1000);
  while (Ethernet.hardwareStatus() == EthernetNoHardware) {
    Serial.println("Ethernet shield was not found.  Sorry, can't run without hardware. :(");
    delay(1000);
  }
  Serial.print("Levantando servidor en IP: ");
  Serial.println(Ethernet.localIP());
  
  serverETH.begin();
}
 
 


//
void loopETH() {
  if (Ethernet.linkStatus() == LinkOFF) {
    Serial.print("*");
  } else {
    Serial.print(".");
    EthernetClient client = serverETH.available();
    //Serial.println(client);
    if (client) {
      Serial.println("new client");
      // una solicitud HTTP termina con una línea en blanco
      bool currentLineIsBlank = true;
      char trama[100] = "";
      char get[30] = "";
      int n = 0;
      bool val = true;
      while (client.connected()) {
        if (client.available()) {
          char c = client.read();
          //Serial.write(c);

          if (val) {
            trama[n] = c;
            n++;
            if (c == '\n' || c == '\r') val = false;
          }
          // si ha llegado al final de la línea (recibió una nueva línea
          // carácter) y la línea está en blanco, la solicitud HTTP ha finalizado,
          // para que puedas enviar una respuesta
          if (c == '\n' && currentLineIsBlank) {
            // send a standard HTTP response header
            //scdr.parse_query(trama,"dato");

            strcpy(get, get_ruta(trama));
            Serial.println(get);
            if (strcmp(get, "/") == 0) {
              scdr.eth_get_sspif(client, "text/html", "/index.html");
            } else if (strcmp(get, "/grafico.html") == 0) {
              scdr.eth_get_sspif(client, "text/html", "/grafico.html");
            } else if (strcmp(get, "/logo.png") == 0) {
              scdr.eth_get_sspif(client, "image/png", "/logo.png");
            } else if (strcmp(get, "/style.css") == 0) {
              scdr.eth_get_sspif(client, "text/css", "/style.css");
            } else if (strcmp(get, "/script.js") == 0) {
              scdr.eth_get_sspif(client, "text/javascript", "/script.js");
            } else if (strcmp(get, "/script2.js") == 0) {
              scdr.eth_get_sspif(client, "text/javascript", "/script2.js");
            } else if (strcmp(get, "/data") == 0) {
              scdr.eth_get_string(client, data);
            } else if (strcmp(get, "/params") == 0) {
              eth_get_data_set(client);
            } else if (strcmp(get, "/history.csv") == 0) {
              scdr.eth_get_history(client);
            } else if (strcmp(get, "/energia") == 0) {
              scdr.eth_get_string(client, energia);
            } else if (strcmp(get, "/monitoreo") == 0) {
              scdr.eth_get_string(client, monitoreo);
            } else if (strcmp(get, "/sag") == 63) {
              eth_get_sag(client, get);
            } else if (strcmp(get, "/sag2") == 63) {
              eth_get_sag2(client, get);
            } else if (strcmp(get, "/sag3") == 63) {
              eth_set_ip(client, get);
            } else if (strcmp(get, "/temp") == 63) {
              eth_set_temp(client, get);
            } else if (strcmp(get, "/modbus") == 63) {
              eth_get_modbus(client, get);
            } else scdr.eth_error(client);
            break;
          }
          if (c == '\n') {
            // estás comenzando una nueva línea
            currentLineIsBlank = true;

          } else if (c != '\r') {
            //  tienes un carácter en la línea actual
            currentLineIsBlank = false;
          }
        }
      }
      //dar tiempo al navegador web para recibir los datos
      delay(1);
      // close the connection:
      client.stop();

      Serial.println("client disconnected");
    }
    //Serial.println("fin lop eth");
  }
}

void eth_get_modbus(EthernetClient client, char *get) {
  uint16_t slave = 0;
  uint8_t reg = 0;
  uint16_t dat = 0;
  Serial.println("modificacion------");
  slave = scdr.parse_query(get, "slave");
  reg = scdr.parse_query(get, "reg");
  dat = scdr.parse_query(get, "dat");
  Serial.println("==============================================");
  Serial.println(slave);
  Serial.println(reg);
  Serial.println(dat);
  //funcionModbusWhiteUpdate(slave, reg, dat);
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Connection: close");  // the connection will be closed after completion of the response
  client.println();
  client.println("");
}


void eth_get_sag2(EthernetClient client, char *get) {
  uint16_t dato = 0;
  uint32_t valor = 0;
  Serial.println("modificacion------");
  dato = scdr.parse_query(get, "dato");
  valor = scdr.parse_query(get, "valor");


  control_tcp2(dato, valor);
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");  // the connection will be closed after completion of the response
  client.println();
  client.println("ok");
}

void eth_set_ip(EthernetClient client, char *get) {
  uint16_t dato = 0;
  uint8_t ip1 = 0;
  uint8_t ip2 = 0;
  uint8_t ip3 = 0;
  uint8_t ip4 = 0;
  Serial.println("modificacion------");
  dato = scdr.parse_query(get, "dato");
  ip1 = scdr.parse_query(get, "ip1");
  ip2 = scdr.parse_query(get, "ip2");
  ip3 = scdr.parse_query(get, "ip3");
  ip4 = scdr.parse_query(get, "ip4");
  Serial.println("==============================================");
  Serial.println(ip1);
  Serial.println(ip2);
  Serial.println(ip3);
  Serial.println(ip4);

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");  // the connection will be closed after completion of the response
  client.println();
  client.println("ok");

  if (dato == 5) {
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.write(IP_1, ip1);
    EEPROM.write(IP_2, ip2);
    EEPROM.write(IP_3, ip3);
    EEPROM.write(IP_4, ip4);
    EEPROM.commit();
    EEPROM.end();
  } else if (dato == 6) {
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.write(GW_1, ip1);
    EEPROM.write(GW_2, ip2);
    EEPROM.write(GW_3, ip3);
    EEPROM.write(GW_4, ip4);
    EEPROM.commit();
    EEPROM.end();
  } else if (dato == 7) {
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.write(S_1, ip1);
    EEPROM.write(S_2, ip2);
    EEPROM.write(S_3, ip3);
    EEPROM.write(S_4, ip4);
    EEPROM.commit();
    EEPROM.end();
  } else if (dato == 8) {
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.write(SUB_1, ip1);
    EEPROM.write(SUB_2, ip2);
    EEPROM.write(SUB_3, ip3);
    EEPROM.write(SUB_4, ip4);
    EEPROM.commit();
    EEPROM.end();
  }
}


void eth_set_temp(EthernetClient client, char *get) {
  uint16_t dato = 0;
  float valor = 0;
  Serial.println("modificacion------");
  dato = scdr.parse_query(get, "dato");
  valor = scdr.parse_query(get, "valor");


  control_tcp3(dato, valor);
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");  // the connection will be closed after completion of the response
  client.println();
  client.println("ok");
}

void control_tcp(uint16_t dato, uint16_t valor) {
  if (dato == 9) Out1 = valor;
  else if (dato == 10) Out2 = valor;
  sonido();
  /*if (dato == 7) OutBPS = valor;
  else if (dato == 8) {
    OutAT = valor;
    //Out1=valor;
    //Out2=valor;
    //Out3=valor;
    //Out4=valor;
    //OutBPS=!valor;
  }

  else if (dato == 9) Out1 = valor;
  else if (dato == 10) Out2 = valor;
  else if (dato == 11) Out3 = valor;
  else if (dato == 12) Out4 = valor;*/
}


void control_tcp2(uint16_t dato, uint32_t valor) {
  if (dato == 1) WhiteEepromFloat(T_MAX, (valor / 100));
  else if (dato == 2) WhiteEepromFloat(T_MIN, (valor / 100));
  else if (dato == 10) {
    ESP.restart();
  } else if(dato==11) {scdr.clearflash();}
}
void eth_get_data_set(EthernetClient client) {
  char data[150] = "";
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Connection: close");  // the connection will be closed after completion of the response
  client.println();

  sprintf(data, "{\"i1\":%.2f,\"i2\":%.2f,\"i3\":%.2f,\"i4\":%.2f,\"i5\":\"%d.%d.%d.%d\",\"i6\":\"%d.%d.%d.%d\",\"i7\":\"%d.%d.%d.%d\",\"i8\":\"%d.%d.%d.%d\",\"i9\":%d}", reg.Tmax, reg.Tmin, reg.Tbps, reg.Tat, ip[0], ip[1], ip[2], ip[3], gateway[0], gateway[1], gateway[2], gateway[3], server[0], server[1], server[2], server[3], subnet[0], subnet[1], subnet[2], subnet[3], reg.Cant_aa);
  Serial.println(data);
  client.println(data);
}

void eth_get_sag(EthernetClient client, char *get) {
  uint16_t dato = 0;
  uint16_t valor = 0;
  Serial.println("modificacion------");
  dato = scdr.parse_query(get, "dato");
  valor = scdr.parse_query(get, "valor");
  control_tcp(dato, valor);
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  
}

float funciona( float m1,float m2, float n1, float n2){
  return(float(m1-n1)/float(m2-n2));
}

float funcionb(float m1,float m2, float n1, float n2){
  float a= (float(m1-n1)/float(m2-n2));
  return (n2-(n1/a));
}

void control_tcp3(uint16_t dato, float valor) {
  if (dato == 3){WhiteEepromFloat(T_MIN,scale.read_median(5));ESP.restart();}
  else if (dato == 4) {WhiteEepromFloat(T_MAX,(scale.read_median(5)-reg.Tmin)/(valor));ESP.restart();}

  else if (dato == 1) {
    WhiteEepromFloat(DIALC,(analogRead(15)));
    WhiteEepromFloat(DIALD,valor);
    Serial.println("m1");
    Serial.println(analogRead(15));
    Serial.println("M2");
    Serial.println(valor);

    ESP.restart();
  }

  else if (dato == 2) {
    EEPROM.begin(EEPROM_SIZE);
    float m1=EEPROM.readFloat(DIALC);
    float m2=EEPROM.readFloat(DIALD);
    float n1=analogRead(15);
    float n2=valor;
    float a=funciona(m1,m2,n1,n2);
    float b=funcionb(m1,m2,n1,n2);

    WhiteEepromFloat(DIALA,a);
    WhiteEepromFloat(DIALB,b);

    Serial.println("m1");
    Serial.println(m1);
    Serial.println("m2");
    Serial.println(m2);
    Serial.println(n1);
    Serial.println(n2);
    Serial.println(a);
    Serial.println(b);
    EEPROM.commit();
    ESP.restart();
  }

  else if (dato == 5) {
    WhiteEepromFloat(DIALC,(-1*lectura()));
    WhiteEepromFloat(DIALD,valor);
    Serial.println("m1");
    Serial.println(analogRead(15));
    Serial.println("M2");
    Serial.println(valor);

    ESP.restart();
  }

  else if (dato == 6) {
    EEPROM.begin(EEPROM_SIZE);
    float m1=EEPROM.readFloat(DIALC);
    float m2=EEPROM.readFloat(DIALD);
    float n1=analogRead(15);
    float n2=valor;
    float a=funciona(m1,m2,n1,n2);
    float b=funcionb(m1,m2,n1,n2);

    WhiteEepromFloat(DIALA,a);
    WhiteEepromFloat(DIALB,b);

    Serial.println("m1");
    Serial.println(m1);
    Serial.println("m2");
    Serial.println(m2);
    Serial.println(n1);
    Serial.println(n2);
    Serial.println(a);
    Serial.println(b);
    EEPROM.commit();
    ESP.restart();
  }
}



void WhiteEepromFloat(uint16_t REG_SET, float val) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.writeFloat(REG_SET, val);
  EEPROM.commit();
  EEPROM.end();
  //ESP.restart();
}

void WhiteEeprom(uint16_t REG_SET, uint8_t val) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(REG_SET, val);
  EEPROM.commit();
  EEPROM.end();
  //ESP.restart();
}

const char *get_ruta(char *cadena) {
  uint8_t n = 0;
  char delimitador[2] = " ";
  static char data[40] = "";
  char *token = strtok(cadena, delimitador);
  while (token != NULL) {
    if (n == 1) { return token; }
    n = n + 1;
    token = strtok(NULL, delimitador);
  }
  return "";
}



void eeprom_start(void) {
  EEPROM.begin(EEPROM_SIZE);
  
  //EEPROM.writeFloat(DIALA, 1);
  //EEPROM.writeFloat(DIALB, 0);
  //EEPROM.writeFloat(DIALC, 0);
  //EEPROM.writeFloat(DIALD, 0);

  //EEPROM.writeFloat(CARGAA, 1);
  //EEPROM.writeFloat(CARGAB, 0);
  //EEPROM.writeFloat(CARGAC, 0);
  //EEPROM.writeFloat(CARGAD, 0);

  //EEPROM.writeFloat(PRESA, 1);
  //EEPROM.writeFloat(PRESB, 0);
  //EEPROM.writeFloat(PRESC, 0);
  //EEPROM.writeFloat(PRESD, 0);

  ip[0] = uint8_t(EEPROM.read(IP_1));
  ip[1] = uint8_t(EEPROM.read(IP_2));
  ip[2] = uint8_t(EEPROM.read(IP_3));
  ip[3] = uint8_t(EEPROM.read(IP_4));

  gateway[0] = uint8_t(EEPROM.read(GW_1));
  gateway[1] = uint8_t(EEPROM.read(GW_2));
  gateway[2] = uint8_t(EEPROM.read(GW_3));
  gateway[3] = uint8_t(EEPROM.read(GW_4));

  server[0] = uint8_t(EEPROM.read(S_1));
  server[1] = uint8_t(EEPROM.read(S_2));
  server[2] = uint8_t(EEPROM.read(S_3));
  server[3] = uint8_t(EEPROM.read(S_4));

  subnet[0] = uint8_t(EEPROM.read(SUB_1));
  subnet[1] = uint8_t(EEPROM.read(SUB_2));
  subnet[2] = uint8_t(EEPROM.read(SUB_3));
  subnet[3] = uint8_t(EEPROM.read(SUB_4));
/*
  reg.Lead = uint8_t(EEPROM.read(LEAD));
  reg.Cant_aa = uint8_t(EEPROM.read(CANT_AA));

  reg.Fail_aa1 = uint8_t(EEPROM.read(FAIL_1));
  reg.Fail_aa2 = uint8_t(EEPROM.read(FAIL_2));
  reg.Fail_aa3 = uint8_t(EEPROM.read(FAIL_3));
  reg.Fail_aa4 = uint8_t(EEPROM.read(FAIL_4));

  reg.RefS1 = float(EEPROM.readFloat(REF_1));
  reg.RefS2 = float(EEPROM.readFloat(REF_2));
  reg.RefS3 = float(EEPROM.readFloat(REF_3));
  reg.RefS4 = float(EEPROM.readFloat(REF_4));

  reg.AlphaS1 = float(EEPROM.readFloat(ALFA_1));
  reg.AlphaS2 = float(EEPROM.readFloat(ALFA_2));
  reg.AlphaS3 = float(EEPROM.readFloat(ALFA_3));
  reg.AlphaS4 = float(EEPROM.readFloat(ALFA_4));

  reg.Tmax = float(EEPROM.readFloat(T_MAX));
  reg.Tmin = float(EEPROM.readFloat(T_MIN));
  reg.Tbps = float(EEPROM.readFloat(T_BPS));
  reg.Tat = float(EEPROM.readFloat(T_AT));

  reg.Tmax2 = float(EEPROM.readFloat(T_MAX2));
  reg.Tmin2 = float(EEPROM.readFloat(T_MIN2));
  reg.Tbps2 = float(EEPROM.readFloat(T_BPS2));
  reg.Tat2 = float(EEPROM.readFloat(T_AT2));

  reg.baud = uint8_t(EEPROM.read(MODBUS_BAUD));

  reg.DatYear = uint16_t(EEPROM.readShort(D_YEAR));
  reg.DatMon = uint8_t(EEPROM.read(D_MON));
  reg.DatDay = uint8_t(EEPROM.read(D_DAY));
  reg.DatHr = uint8_t(EEPROM.read(D_HR));
  reg.DatMin = uint8_t(EEPROM.read(D_MIN));
  reg.DatSec = uint8_t(EEPROM.read(D_SEC));
  */
  
  params.DIALA2=float(EEPROM.readFloat(DIALA));
  params.DIALB2=float(EEPROM.readFloat(DIALB));

  params.CARGAA2=float(EEPROM.readFloat(CARGAA));
  params.CARGAB2=float(EEPROM.readFloat(CARGAB));

  params.PRESA2=float(EEPROM.readFloat(PRESA));
  params.PRESB2=float(EEPROM.readFloat(PRESB));

  

  Serial.println(params.DIALA2);
  Serial.println(params.DIALB2);
  Serial.println("m1");
  Serial.println(EEPROM.readFloat(DIALC));
  Serial.println("m2");
  Serial.println(EEPROM.readFloat(DIALD));
  EEPROM.end();
  Log_Eeprom();
}

void Log_Eeprom() {
  char message[250];

  sprintf(message, "DIALA2: %f   %f", params.DIALA2,params.DIALB2);
  Serial.println(message);


  sprintf(message, "IHHHHHHH: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  Serial.println(message);

  sprintf(message, "Gateway: %d.%d.%d.%d", gateway[0], gateway[1], gateway[2], gateway[3]);
  Serial.println(message);

  sprintf(message, "Servidoror: %d.%d.%d.%d", server[0], server[1], server[2], server[3]);
  Serial.println(message);

  sprintf(message, "Subnet: %d.%d.%d.%d", subnet[0], subnet[1], subnet[2], subnet[3]);
  Serial.println(message);

  sprintf(message, "Cantidad de AA: %d", reg.Cant_aa);
  Serial.println(message);

  sprintf(message, "Secuenciador status de AA: %d", reg.Lead);
  Serial.println(message);

  sprintf(message, "FAIL en AA1: %d", reg.Fail_aa1);
  Serial.println(message);

  sprintf(message, "FAIL en AA2: %d", reg.Fail_aa2);
  Serial.println(message);

  sprintf(message, "FAIL en AA3: %d", reg.Fail_aa3);
  Serial.println(message);

  sprintf(message, "FAIL en AA4: %d", reg.Fail_aa4);
  Serial.println(message);

  sprintf(message, "Cte ref PT1: %f", reg.RefS1);
  Serial.println(message);

  sprintf(message, "Cte ref PT2: %f", reg.RefS2);
  Serial.println(message);

  sprintf(message, "Cte ref PT3: %f", reg.RefS3);
  Serial.println(message);

  sprintf(message, "Cte ref PT4: %f", reg.RefS4);
  Serial.println(message);

  sprintf(message, "Cte ref PT1: %f", reg.AlphaS1);
  Serial.println(message);

  sprintf(message, "Cte ref PT2: %f", reg.AlphaS2);
  Serial.println(message);

  sprintf(message, "Cte ref PT3: %f", reg.AlphaS3);
  Serial.println(message);

  sprintf(message, "Cte ref PT4: %f", reg.AlphaS4);
  Serial.println(message);

  sprintf(message, "Setpoints: %f   %f   %f    %f", reg.Tmin, reg.Tmax, reg.Tbps, reg.Tat);
  Serial.println(message);

  sprintf(message, "Setpoints2: %f   %f   %f    %f", reg.Tmin2, reg.Tmax2, reg.Tbps2, reg.Tat2);
  Serial.println(message);

  sprintf(message, "Velocidad modbus tipo (0:240 1:4800 2:9600 3:19200): %d", reg.baud);
  Serial.println(message);
}

void UpdateSensor(float valor, uint8_t adc) {
  float alfa = scdr.CalPT100(adc, valor, 0);
  //Serial.println("============================");
  //Serial.println(alfa);
  if (adc == 0) WhiteEepromFloat(ALFA_1, alfa);
  else if (adc == 1) WhiteEepromFloat(ALFA_4, alfa);
  else if (adc == 2) WhiteEepromFloat(ALFA_2, alfa);
  else if (adc == 3) WhiteEepromFloat(ALFA_3, alfa);
}

void sonido(void){
  buzzer(100);
  delay(500);
  buzzer(100);
  delay(500);
  buzzer(100);
  Tcorrida=millis();
}

void buzzer(uint32_t time){
  digitalWrite(BUSSER, HIGH);
  delay(time);
  digitalWrite(BUSSER, LOW);
}

