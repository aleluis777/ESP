#include <Ethernet.h>
#include <Adafruit_ADS1X15.h>
#include <DHT.h>
#include "RTClib.h"
#include "string.h"
#include "SPIFFS.h"
#include <EEPROM.h>
#include <EthernetUdp.h>
#include "HX711.h"
#include <LinaresETH.h>
#include <ArduinoJson.h>

#define   DIR_ADC_GND   0x48 // direccion ads1115 gnd
#define   DIR_ADC_VCC   0x49 // direccion ads1115 vcc
#define   DIR_ADC_DAT   0x4A // direccion ads1115 data
#define   DIR_ADC_CLK   0x4B // direccion ads1115 clock

#define VERSION 1.1 
#define ETH_RST 5
#define ETH_SS  17
#define dataPin     12
#define clockPin     2

#define REQ1 				13
#define DATA1       15
#define CLK1 				12

#define REQ2 				14
#define DATA2      2
#define CLK2 				27
#define LONGITUD 				14

uint32_t segundos[20]={0,15,30,60,120,240,480,900,1800,3600,7200,14400,28800,57600,86400,86410};
uint32_t micrometros[70]={50,100,150,200,300,400,500,600,700,800,900,1000,1200,1400,1600,1800,2000,2200,2400,2600,2800,3000,3200,3400,3600,3800,4000,4200,4400,4600,4800,5000,5500,6000,6500,
	7000,7500,8000,8500,9000,9500,10000,10500,11000,11500};

uint32_t micrometros2[35] = {
    // 0 a 50 de 5 en 5 (11 valores)
    0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50,
    // 60 a 100 de 10 en 10 (5 valores)
    60, 70, 80, 90, 100,
    // 150 a 300 de 50 en 50 (4 valores)
    150, 200, 250, 300,
    // 400 a 1500 de 100 en 100 (12 valores)
    400, 500, 600, 700, 800, 900, 1000, 1100, 1200, 1300, 1400, 1500
};


HX711 scale;
RTC_DS1307 rtc;
uint8_t corrida1=0;
uint8_t corrida2=0;
float celda=0;
uint32_t tensOfMillisCounter = 0;
long Tcorrida = 0;
uint8_t inter=1;
float k1=0;
float k2=1;
uint8_t V1=0;

// Datos por defecto
uint8_t macArray[6] = { 0xDE, 0xED, 0xBA, 0x2E, 0x85, 0x38 };
uint8_t ipArray[4] = { 192, 168, 1, 91 };
uint8_t gatewayArray[4] = { 192, 168, 1, 1 };
uint8_t subnetArray[4] = { 255, 255, 255, 0 };
EthernetClient ethClient;
EthernetUDP udp;
Adafruit_ADS1115 ads;
Adafruit_ADS1115 ads2;
Adafruit_ADS1115 ads3;
Adafruit_ADS1115 ads4;

EthernetServer serverETH(80);

LinaresETH linaresETH(ETH_RST,serverETH); 


long lastMsg = 0;
long lastMsg2 = 0;
char msg[50];
uint8_t Calibrate=0;
char data[100]="ll1111";
char community[20]="public";
char name_device[30]="Braindlab ColdSmart";
char monitoreo[350]="";
char energia[180]="";
char modbus[150]="";
char data_set[100]="";
uint16_t snmpPort=161;
int value = 0;
//int fail=0;

float hum = 0;
float temp = 0;
float tmax=0;
float tmin=0;

 

int Out[8] ={0,0,0,0,0,1};
float In[18] ={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
float SetIn[18] ={100,100,100,100,100,100,100,100,100,100,100,100,100,100,100,100,100,100};
float setpointArray[4]={25,28,30,32};


uint8_t failsArray[4]={0,0,0,0};

//Paraetros lectusa SNMP 



uint8_t ciclo=0;
int time_sag = 0;
uint8_t lead = 1;
 
 
void setup() {
  Serial.begin(115200);
  k2=(9830-240)/(352/1.2);
  k1=240;
  ///scdr.Restore();
  
  if(!SPIFFS.begin(true)){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }
  listarArchivos();
  SpisspsbBegin2("/config.json");
  SpisspsbBeginCalibrations();

  if (!ads.begin(DIR_ADC_VCC)) { Serial.println("Failed to initialize DIR_ADC_VCC."); }
  ads.setGain(GAIN_ONE);
  scale.begin(dataPin, clockPin);
  // GPIO_Config_dial();

  linaresETH.setCallback(miFuncionDesdeSketch);
  // Inicializar Ethernet con los objetos ipArrayAddress
  if (linaresETH.begin(macArray, ipArray, gatewayArray, gatewayArray, subnetArray)) {
    Serial.println("Ethernet inicializado correctamente.");
  } else {
    Serial.println("Error al inicializar Ethernet.");
  }

  if (! rtc.begin()) {
    Serial.println("Couldn't find RTC");
    delay(1000);
  }

  if (!rtc.isrunning()) {
    Serial.println("RTC is NOT running");
    rtc.adjust(DateTime(2023, 1, 21, 3, 0, 0));
  }
  Serial.println("RTC corriendo");
  delay(1000);

  ///digitalWrite(BPS_S, Out[5]);
  //digitalWrite(BPS_S, !Out[5]);
  serverETH.begin();
  // linaresSensors.busser(BUZZER);
  flashLog("baja|Encendido del equipo");
  
}
 
uint16_t mb[25] = {0};
char buff[40];




bool humedad=false;
uint val=0;
uint8_t cnt_trap_test=0;
uint8_t semana = 0;
uint8_t semana_last = 0;

void listarArchivos() {
  char lista[200]="";
  File root = SPIFFS.open("/");
  File archivo = root.openNextFile();
  Serial.print("["); 
  while (archivo) {  
    sprintf(lista,"{\"name\":\"%s\",\"size\":%d}",archivo.name(),archivo.size());
    Serial.print(lista); 
    archivo = root.openNextFile();
    if(archivo) Serial.print(",");
  }
  Serial.print("]"); 
}

void GPIO_Config_dial(void){

  pinMode(REQ1, OUTPUT);
  pinMode(DATA1, INPUT_PULLUP);
  pinMode(CLK1, INPUT_PULLUP);

  pinMode(REQ2, OUTPUT);
  pinMode(DATA2, INPUT_PULLUP);
  pinMode(CLK2, INPUT_PULLUP);
 

  ///pinMode(AA1, OUTPUT);
}
const char* dial(uint8_t REQ, uint8_t CLK, uint8_t DATA) {
	uint32_t timeout=0;
	int i = 0;int j = 0;int k = 0;
	static char distancia[20];
	char mydata[14];

	

	digitalWrite(REQ,1);
	//digitalWrite(LED3,1);
	
	for( i = 0; i < 13; i++ ){
    //Serial.println("for1");
		k = 0;
		for (j = 0; j < 4; j++){
      //Serial.println("for2"); 
			timeout=100000;
			while(digitalRead(CLK) == 0) {
        //Serial.println("CLK0"); 
        delayMicroseconds(3);
				if ((timeout--) == 0){
					digitalWrite(REQ, 0);
          //digitalWrite(LED3, 0);
          //Serial.println("ERROR1"); 
					return "ERROR!";
				};
			}
			timeout=100000;
			while(digitalRead(CLK) == 1) {
        //Serial.println("CLK1"); 
        delayMicroseconds(3);
				if ((timeout--) == 0){
					
					digitalWrite(REQ,0);
          //Serial.println("ERROR2"); 
					//digitalWrite(LED3,0);
					return "ERROR!";
				};
			}
			timeout=2000;
			while ((timeout--) != 0);
			if(digitalRead(DATA)==1 && j==0)k=1;
			else if(digitalRead(DATA)==1 && j==1)k=2+k;
			else if(digitalRead(DATA)==1 && j==2)k=4+k;
			else if(digitalRead(DATA)==1 && j==3)k=8+k;
			if(k==15)k=0;
		}

		char c = k+'0';
		mydata[i] = c;
		
		
	}
	sprintf(distancia,"%c%c.%c%c%c",mydata[6],mydata[7],mydata[8],mydata[9],mydata[10]);
  Serial.println(distancia);
	digitalWrite(REQ,0);
	//digitalWrite(LED3,0);
	return distancia;
}

void loo3p(){
  digitalWrite(REQ1,1);
  delay(150);
  digitalWrite(REQ1,0);
  delay(150);
  digitalWrite(REQ1,1);
  delay(150);
  digitalWrite(REQ1,0);
  delay(150);
  digitalWrite(REQ1,1);
  Serial.print("DATA1: ");
  Serial.println(digitalRead(DATA1));
  Serial.print("CLK1: ");
  Serial.println(digitalRead(CLK1));
   
}

float dial_tara=0;
float desplazamiento=0;

uint32_t Tiempo_corrida(float dial, float presion, float celda, uint8_t corrida1,long tiempo){
  
  char flash[50]=" ";
  
  //LONGITUD
  if(corrida1==1 && (dial >= (float(desplazamiento*setpointArray[0])/1000))){
    if(desplazamiento=0) linaresETH.whiteHistory("INICIO DE EL ESTUDIO NUMERO 1");
    if(desplazamiento >= 0 && desplazamiento< 100) {desplazamiento+1;}
    if (desplazamiento>100 && desplazamiento< 1500){ desplazamiento=desplazamiento+30;}
    //Serial.println("aca se puede guardar la informacion.");
    
    sprintf(flash, "%.3f|%.3f|%.3f|%d", dial, presion, celda, tiempo / 1000);
    linaresETH.whiteHistory(flash);
    // buzzer(50);
    //V1=V1+1;
    if (desplazamiento>1500){ V1=0;Serial.println("estudio1...finalizado");}
  
  
  }
  if (corrida1) return tiempo / 1000;else {V1=0;return 0;}
}


uint32_t  corridaxx(float dial,float dial2,float celda,uint8_t corrida1, uint8_t corrida2,long tiempo){
  Serial.println("corrida...............");
  char flash[50]=" ";
  Serial.println(corrida1);
  Serial.println(segundos[V1]*1000);
  Serial.println(tiempo);
  if(corrida1==1 && (tiempo > (segundos[V1]*1000))){
    
    Serial.println("aca se puede guardar la informacion.");
    if(V1==0) linaresETH.whiteHistory("INICIO DE EL ESTUDIO NUMERO 1");
    sprintf(flash, "%.3f|%.3f|%.3f|%d", dial, dial2, celda, tiempo / 1000);
    linaresETH.whiteHistory(flash);
    buzzer(50);
    V1=V1+1;
    if(V1==14){V1=0;;Serial.println("estudio1...finalizado");}
    
  }
  else if(corrida2==1 && (uint32_t(1000*dial2) > (micrometros[V1]))){
    
    //aca se puede guardar la informacion.
    if(V1==0) linaresETH.whiteHistory("INICIO DE EL ESTUDIO NUMERO 2");
    sprintf(flash, "%.3f|%.3f|%.3f|%d", dial, dial2, celda, tiempo / 1000);
    linaresETH.whiteHistory(flash);
    buzzer(50);
    delay(100);
    buzzer(50);
    V1=V1+1;
    if(V1==43){V1=0;;Serial.println("estudio2...finalizado");}
    
  }
  
  if (corrida1 || corrida2) return tiempo / 1000;else {V1=0;return 0;}
}
void loop22(){
  Serial.println(ads.readADC_SingleEnded(1));
  Serial.println(ads.readADC_SingleEnded(3));
  Serial.println(ads.readADC_SingleEnded(1));
  Serial.println(ads.readADC_SingleEnded(3));

  delay(1000);
}
void loop() {
  
  long now = millis();
  linaresETH.runLoopETH();

  if (now - lastMsg > 500) {
    
    lastMsg = now;
    uint8_t q = 10;
    DateTime now2 = rtc.now();
    DateTime future(now2 + TimeSpan(-18000)); 
    //Serial.println(dial(REQ1, CLK1, DATA1));
    if (scale.is_ready()){
      Serial.print("celda:   ");
      Serial.print(scale.read_median(5));
      celda=(scale.read_median(5)-SetIn[0])/(SetIn[1]*100);
      }
    else  Serial.println("FALLA EN LECTURA DE CELDA.........");
    delay(10);
    // celda=(scale.read_median(5)-SetIn[0])/(SetIn[1]/100);
    // float dial= ads.readADC_SingleEnded(1);
    float presion= getPressure(SetIn[2],(SetIn[3]/100));
    float dial= getDial(SetIn[4],(SetIn[5]/10));
    Serial.print("presion:   ");
    Serial.print(presion);
    Serial.print("    dial:   ");
    Serial.println(dial);
    Serial.println(ads.readADC_SingleEnded(1));
    Serial.println(ads.readADC_SingleEnded(3));
    uint32_t activo=0;
    activo=Tiempo_corrida(dial, presion, celda, corrida1,(millis()-Tcorrida));
    //presion=averageReading;
    
    sprintf(monitoreo,"{\"o1\":%d,\"o2\":%d,\"i1\":%.3f,\"i2\":%.2f,\"i3\":%.3f,\"i4\":%d,\"t\":%d,\"d\":%d}",corrida1,corrida2,dial, presion, celda,activo,millis()/1000,now2.unixtime());
    Serial.println(monitoreo);
    if(Calibrate==1){SetCalibrate();Calibrate=0;}
    delay(10);  
  }
}
void flashLog(char* message){
  DateTime now2 = rtc.now();
  DateTime future(now2 + TimeSpan(-18000)); 
  char d[130]= "";
  sprintf(d,"%s|%d",message,now2.unixtime());
  // linaresSensors.whiteflash(d,"/log.csv");
}

float getPressure(float k1, float k2){
  float currentReading;
  float currentReading2;
  float sumReadings=0;
  float sumReadings2=0;
  uint8_t NUM_READINGS=10;
  for (int i = 0; i < NUM_READINGS; i++) {
    currentReading = ads.readADC_SingleEnded(1);
    //Serial.println(currentReading);
    currentReading2=(currentReading-k1)/k2;
    sumReadings += currentReading2;
    sumReadings2+= currentReading;
    //delay(4);
  }
  //Serial.println(sumReadings2/NUM_READINGS);
  float averageReading = (float)sumReadings / NUM_READINGS;
  return(averageReading);
}

float getDial(float k1, float k2){
  float currentReading;
  float currentReading2;
  float sumReadings=0;
  float sumReadings2=0;
  uint8_t NUM_READINGS=10;
  for (int i = 0; i < NUM_READINGS; i++) {
    currentReading = ads.readADC_SingleEnded(3);

    currentReading2=(currentReading-k1)/k2;
    sumReadings += currentReading2;
    sumReadings2+= currentReading;
    //delay(4);
  }
  Serial.println(sumReadings2/NUM_READINGS);
  float averageReading = (float)sumReadings / NUM_READINGS;
  return(averageReading);
}

void miFuncionDesdeSketch(char* mensaje, EthernetClient& client, char* body) {

  //POSTrtc.adjust(DateTime(valor));
  if ((strlen(body)>0) && strcmp(mensaje, "/api/restart") == 0) {
    Serial.println("reinicio"); 
    Serial.println(body);
    //{"button":"2","data":0
    StaticJsonDocument<100> doc2;
    DeserializationError error = deserializeJson(doc2, body);
    if (error) {linaresETH.eth_error(client,"error");Serial.println("error en serializacion");return;}
     

    String button = doc2["button"];
    uint8_t data = doc2["data"];
    char val[5];
    sprintf(val,"%s",button); 
    if(val[0]=='0'){Serial.print("reiniciando");linaresETH.sendData(client,"OK");flashLog("media|Reiniciando modulo braindlab");ESP.restart();}
    else if(val[0]=='1'){
      Serial.print("actualizando fimware");
      uint8_t ok=linaresETH.actualizarDesdeSPIFFS("/compilado.bin");
      char response[30];
      switch (ok) {
        case 0:
            sprintf(response,"[OK] Firmware verificado correctamente");
            break;
        case 1:
            sprintf(response,"[ERROR] No se pudo abrir el archivo");
            break;

        case 2:
            sprintf(response,"[ERROR] Archivo de firmware vacío o corrupto");
            break;

        case 3:
            sprintf(response,"[ERROR] Escritura incompleta");
            break;
        case 4:
            sprintf(response,"[ERROR] Fallo al finalizar actualización");
            break;
        case 5:
            sprintf(response,"[ERROR] No hay espacio suficiente para la actualización");
            break;
        

        default:
            sprintf(response,"[ERROR] desconocido");
            break;
        }
      ok==0? linaresETH.sendData(client,response):linaresETH.eth_error(client,response);
    } 
    if(val[0]=='2'){for (int i = 0; i < 4; i++) {failsArray[i] = 0;Calibrate=1;};linaresETH.sendData(client,"OK");flashLog("media|Limpiando fallas");}
    else linaresETH.sendData(client,"OK");
    
    //Restart
    return;

  }
  else if ((strlen(body)>0) && strcmp(mensaje, "/api/data") == 0) {
    Serial.println("aqui prendo y apago");
    Serial.println(body);
    //{"button":"2","data":0
    StaticJsonDocument<100> doc;
    DeserializationError error = deserializeJson(doc, body);
    if (error) {linaresETH.eth_error(client,"error");Serial.println("error en serializacion");return;}
    uint8_t  cnt=0;

    String button = doc["button"];
    uint8_t data = doc["data"];

    char val[5];
    sprintf(val,"%s",button);
    Serial.println("button");
    Serial.println(val[0]);
    Serial.println("data");
    Serial.println(data);
    char message[100]= "Activacion de EQUIPO METROLOGIA - modo remoto ";
    if(val[0]=='1') {corrida1=data; sprintf(message,"corrida 1 |%s de equipo mtrologia - modo remoto ",data?"Encendido":"Apagado");}
    if(val[0]=='2') {corrida2=data; sprintf(message,"corrida 1 |%s de equipo mtrologia- modo remoto ",data?"Encendido":"Apagado");}
    sonido();
    flashLog(message);

    linaresETH.sendData(client,"OK");
    return;

  }
  else if ((strlen(body)>0) && strcmp(mensaje, "/api/set") == 0) {
    Serial.println("RTC");
    Serial.println(body);
    //{"button":"2","data":0

    StaticJsonDocument<100> doc;
    DeserializationError error = deserializeJson(doc, body);
    if (error) {linaresETH.eth_error(client,"error");Serial.println("error en serializacion");return;}

    String button = doc["button"];
    uint32_t data = doc["data"];

    char b[5];
    sprintf(b,"%s",button);
    unsigned int btn = (unsigned int)atoi(b);
    //(btn==20) rtc.adjust(DateTime(data));
    if(btn==0){ SetIn[0] =scale.read_median(5);Serial.println(SetIn[0]);Calibrate=1;}
    else if(btn==1){ SetIn[1] =((scale.read_median(5)-SetIn[0])*100)/data;Serial.println(SetIn[1]);Calibrate=1;}
    else if(btn==2){ SetIn[2] =getPressure(0,1);Serial.println(SetIn[2]);Calibrate=1;}
    else if(btn==3){ SetIn[3] =(100*getPressure(SetIn[2],1))/data;Serial.println(SetIn[3]);Calibrate=1;}
    else if(btn==4){
      SetIn[4] =getDial(0,1);
      Serial.println("calibracion del cero dial");
      Serial.println(SetIn[4]);
      Calibrate=1;
      }
    else if(btn==5){
      SetIn[5] =(10000*getDial(SetIn[4],1))/data;
      Serial.println("calibracion del maximo dial");
      Serial.println(SetIn[5]);
      Calibrate=1;}

    //else if(btn==6){ SetIn[6] =data/uint16_t(linaresSensors.getPeak(0, 1, 100));Serial.println(SetIn[6]);Calibrate=1;}
    //else if(btn==7){ SetIn[7] =data/uint16_t(linaresSensors.getPeak(1, 1, 100));Serial.println(SetIn[7]);Calibrate=1;}
    //else if(btn==8){ SetIn[8] =data/uint16_t(linaresSensors.getPeak(2, 1, 100));Serial.println(SetIn[8]);Calibrate=1;}
    //else if(btn==9){ SetIn[9] =data/uint16_t(linaresSensors.getPeak(0, 2, 100));Serial.println(SetIn[9]);Calibrate=1;}
    //else if(btn==10) {SetIn[10] =data/uint16_t(linaresSensors.getPeak(1, 2, 100));Serial.println(SetIn[10]);Calibrate=1;}
    //else if(btn==11) {SetIn[11] =data/uint16_t(linaresSensors.getPeak(2, 2, 100));Serial.println(SetIn[11]);Calibrate=1;}
    else {linaresETH.eth_error(client,"error");return;}
    linaresETH.sendData(client,"OK");
    return;

  }
  else if ((strlen(body)>0) && strcmp(mensaje, "/delete/file") == 0) { 
    Serial.println(body);
    StaticJsonDocument<100> doc;
    DeserializationError error = deserializeJson(doc, body);
    if (error) {linaresETH.eth_error(client,"error");Serial.println("error en serializacion");return;}
    uint8_t  cnt=0;

    String name = doc["name"]; 

    char val[30];
    sprintf(val,"/%s",name);  
    if(strcmp(val, "/fimware.html") == 0){
      linaresETH.eth_error(client,"no se puede borrar este archivo");
    }
    else if (SPIFFS.exists(val)) {
      SPIFFS.remove(val); 
      linaresETH.sendData(client,"OK");
    } 
    else linaresETH.eth_error(client,"no existe archivo");
    
    return;

  }
  //GET
  else if (strcmp(mensaje, "/monitoreo") == 0) {
    linaresETH.sendData(client,monitoreo);
  }
  else if (strcmp(mensaje, "/update-fimware") == 0) {
    uint8_t ok=linaresETH.actualizarDesdeSPIFFS("/programa.bin");
    ok==1? linaresETH.sendData(client,"OK"):linaresETH.eth_error(client,"error");
  }
  else if (strcmp(mensaje, "/energia") == 0) {
    linaresETH.sendData(client,energia);
  }
  else {
    Serial.println("error");
    linaresETH.eth_error(client,"error");
  }

}

uint8_t SpisspsbBegin2(const char *ruta) {
    File file = SPIFFS.open(ruta);
    if (!file) {
        Serial.println("FALLO ABRIENDO");
        return 0;
    }

    String json = file.readString();
    file.close();
    Serial.println(json);

    StaticJsonDocument<2500> doc;
    DeserializationError error = deserializeJson(doc, json);
    if (error) {
        Serial.println("error...");
        return 0;
    } 
    JsonArray inputs = doc["inputs"];
    JsonArray fails = doc["fails"];
    JsonArray setpoints = doc["setpoints"];
    JsonArray ip = doc["ip"];
    JsonArray gateway = doc["gateway"];
    JsonArray subnet = doc["subnet"];
    JsonArray mac = doc["mac"];
    uint16_t port = doc["port"];
    uint32_t baudrate = doc["baudrate"];
    uint8_t LeadJson = doc["lead"];
    uint16_t snmpport = doc["snmpport"];
    const char* snmpcommunity = doc["snmpcommunity"];
    uint8_t snmpversion = doc["snmpversion"];
    const char* device = doc["device"];
    const char* type = doc["type"];
    const char* password = doc["password"];
    const char* user = doc["user"];

    lead=LeadJson;
    if (!doc["ip"].isNull() && !doc["gateway"].isNull() && !doc["subnet"].isNull() && !doc["mac"].isNull()) {
      for (int i = 0; i < 4; i++) {ipArray[i] = ip[i];} 
      for (int i = 0; i < 4; i++) {gatewayArray[i] = gateway[i];}
      for (int i = 0; i < 4; i++) {subnetArray[i] = subnet[i];}
      for (int i = 0; i < 6; i++) {macArray[i] = mac[i];}
    }else{Serial.print("error configuracion RED");}
    
    for (int i = 0; i < 4; i++) {setpointArray[i] = setpoints[i];}

    if (!doc["snmpport"].isNull() && !doc["snmpcommunity"].isNull() && !doc["device"].isNull()) {
      strncpy(community, snmpcommunity, sizeof(community) - 1);community[sizeof(community) - 1] = '\0'; // Asegura terminación nula
      strncpy(name_device, device, sizeof(name_device) - 1);name_device[sizeof(name_device) - 1] = '\0'; // Asegura terminación nula
      snmpPort=snmpport;
    }

    return 1;
}

uint8_t SpisspsbBeginCalibrations() {
    File file = SPIFFS.open("/calibrations.json");
    if (!file) {
        Serial.println("FALLO ABRIENDO");
        return 0;
    }
    String json = file.readString();
    file.close();
    Serial.println(json);

    StaticJsonDocument<2500> doc;
    DeserializationError error = deserializeJson(doc, json);
    if (error) {
        Serial.println("error...");
        return 0;
    }
    JsonArray inputs = doc["calibration"];
    JsonArray failArray = doc["fails"];

    
    for (int i = 0; i < 18; i++) {SetIn[i] = inputs[i];Serial.print(SetIn[i]);} 

    for (int i = 0; i < 4; i++) {failsArray[i] = failArray[i];;Serial.print(failsArray[i]);}


    return 1;
}



uint8_t SetCalibrate() {
  int inputsSize = sizeof(SetIn) / sizeof(SetIn[0]);
  int failsSize = sizeof(failsArray) / sizeof(failsArray[0]);
  Serial.println(inputsSize);
  Serial.println(failsSize);
  String bufferJson; // Ajusta el tamaño según tus necesidades

 
 //StaticJsonDocument doc;}
 StaticJsonDocument<4500> doc;
  

  JsonArray calibration = doc["calibration"].to<JsonArray>();
  for (int i = 0; i < inputsSize; i++) {
    calibration.add(SetIn[i]);
    Serial.println(SetIn[i]);
  }

  JsonArray fails = doc["fails"].to<JsonArray>();
  for (int i = 0; i < failsSize; i++) {
    fails.add(failsArray[i]);
    Serial.println(failsArray[i]);
  }

  serializeJson(doc, bufferJson);
  Serial.println(bufferJson);

  SPIFFS.remove("/file.json");
  File file2 = SPIFFS.open("/file.json", "a");
  if (!file2) {Serial.println("FALLO ABRIENDO");return 0;}
  file2.print(bufferJson);
  file2.print("\n");
  file2.close();

  SPIFFS.remove("/calibrations.json");
  SPIFFS.rename("/file.json", "/calibrations.json");

  return 1;
}


uint8_t SpisspsWhite(const char *ruta, uint8_t lead) {
    Serial.println("lead nuevo ");
    Serial.println(lead);
    File file = SPIFFS.open(ruta);
    if (!file) {
        Serial.println("FALLO ABRIENDO");return 0;
    }

    String json = file.readString();
    String json2;
    file.close();
    Serial.println(json);

    StaticJsonDocument<2500> doc;
    DeserializationError error = deserializeJson(doc, json);
    if (error) {
        Serial.println("error...");
        return 0;
    }
    doc["lead"] = lead;
    serializeJson(doc, json2);
    Serial.println(json2);

    SPIFFS.remove("/file.json");
    File file2 = SPIFFS.open("/file.json", "a");
    if (!file2) {Serial.println("FALLO ABRIENDO");return 0;}
    file2.print(json2);
    file2.print("\n");
    file2.close();

    SPIFFS.remove("/config.json");
    SPIFFS.rename("/file.json", "/config.json");
    delay(2);

    return 1;
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
  //digitalWrite(BUSSER, HIGH);
  delay(time);
  //digitalWrite(BUSSER, LOW);
}
 
