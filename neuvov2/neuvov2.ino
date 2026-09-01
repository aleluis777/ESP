#include <LinaresSNMP.h>
#include <Ethernet.h>
#include <DHT.h>
#include "RTClib.h"
#include "string.h"
#include "SPIFFS.h"
#include <EthernetUdp.h>
#include <SNMP_Agent.h>
#include <LinaresETH.h>
#include <LinaresSensors.h>
#include <ArduinoJson.h>

#define VERSION 3.4
#define DHT_PIN 27            //what pin we're connected to
#define DHT_TYPE DHT21       //DHT 21  (AM2301
#define ETH_RST 5
#define ETH_SS  17

RTC_DS1307 rtc;

//ModbusRTU mbMaster;

SNMPAgent snmp;
 
//SNMPTrap* BpsTrap = nullptr; // Puntero para la trampa BPS
//SNMPTrap* AtTrap = nullptr;  // Puntero para la trampa AT
//SNMPTrap* RSTTrap = nullptr; // Puntero para la trampa RST


//INFORMACION PARA TRAPS
// SNMPTrap* BpsTrap = new SNMPTrap("public", SNMP_VERSION_2C);
// SNMPTrap* AtTrap = new SNMPTrap("public", SNMP_VERSION_2C);
// SNMPTrap* RSTTrap = new SNMPTrap("public", SNMP_VERSION_2C);

TimestampCallback* timestampCallback;
uint32_t tensOfMillisCounter = 0;


// Datos por defecto
uint8_t macArray[6] = { 0xDE, 0xED, 0xBA, 0x2E, 0x85, 0x38 };
uint8_t ipArray[4] = { 192, 168, 18, 91 };
uint8_t gatewayArray[4] = { 192, 168, 18, 1 };
uint8_t subnetArray[4] = { 255, 255, 255, 0 };
EthernetClient ethClient;
EthernetUDP udp;

EthernetServer serverETH(80);

LinaresETH linaresETH(ETH_RST,serverETH); 
LinaresSensors linaresSensors(DHT_PIN, DHT_TYPE);

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
uint16_t SetIn[18] ={100,100,100,100,100,100,100,100,100,100,100,100,100,100,100,100,100,100};
float setpointArray[4]={25,28,30,32};


uint8_t failsArray[4]={0,0,0,0};

//Paraetros lectusa SNMP

int SNMP_H = 0;
int SNMP_T = 0;
int SNMP_T1 = 0;
int SNMP_T2 = 0;
int SNMP_T3 = 0;
int SNMP_T4 = 0;

int SNMP_VRS = 0;
int SNMP_VST = 0;
int SNMP_VRT = 0;
int SNMP_IR = 0;
int SNMP_IS = 0;
int SNMP_IT = 0;



uint8_t ciclo=0;
int time_sag = 0;
uint8_t lead = 1;
 

void setupSNMPTrap(SNMPTrap* &trap, const char* community) {
    trap = new SNMPTrap(community, SNMP_VERSION_2C); // Crea el objeto SNMPTrap
}
 
void setup() {
  Serial.begin(115200);
  
  ///scdr.Restore();
  pinMode(AA1, OUTPUT);
  pinMode(AA2, OUTPUT);
  pinMode(AA3, OUTPUT);
  pinMode(AA4, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  pinMode(AT, OUTPUT);
  pinMode(BPS_S, OUTPUT);
  pinMode(BPS_H, INPUT); 

  if(!SPIFFS.begin(true)){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }
  listarArchivos();
  SpisspsbBegin2("/config.json");
  SpisspsbBeginCalibrations();


  Serial.println("Inicializacion MODBUS");
  //Inicializacion MODBUS
  Serial1.begin(9600, SERIAL_8N1, RXD1, TXD1);
	//mbMaster.begin(&Serial1,S_485);
  //mbMaster.master(); 

  linaresETH.setCallback(miFuncionDesdeSketch);
  // Inicializar Ethernet con los objetos ipArrayAddress
  if (linaresETH.begin(macArray, ipArray, gatewayArray, gatewayArray, subnetArray)) {
    Serial.println("Ethernet inicializado correctamente.");
  } else {
    Serial.println("Error al inicializar Ethernet.");
  }

  if (!linaresSensors.beginTemperature(DIR_ADC_DAT, DIR_ADC_GND)) {
    Serial.println("Failed to initialize ADS1115. Check connections!");
    // while (1);  // Detener el programa si no se inicializan los ADS1115
  }
  if (!linaresSensors.beginEnergy(DIR_ADC_CLK, DIR_ADC_VCC)) {
    Serial.println("Failed to initialize ADS1115. Check connections!");
    // while (1);  // Detener el programa si no se inicializan los ADS1115
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
  snmp = SNMPAgent(community, community);

  //setupSNMPTrap(BpsTrap, "sag"); // Configura BpsTrap
  //setupSNMPTrap(AtTrap, "sag");   // Configura AtTrap
  //setupSNMPTrap(RSTTrap, "sag"); // Configura RSTTrap
  Serial.println(snmpPort);
  udp.begin(snmpPort);
  snmp.setUDP(&udp);

  snmp.begin();
  snmp.addReadOnlyStaticStringHandler(OID_DESCR, "INFORMACION DEL EQUIPO");
  snmp.addReadOnlyStaticStringHandler(OID_OBJECTID, "");
  snmp.addTimestampHandler(OID_UPTIME, &tensOfMillisCounter);
  snmp.addReadOnlyStaticStringHandler(OID_NAME, name_device);
  snmp.addReadOnlyStaticStringHandler(OID_LOCATION, "braindtic.com");
  snmp.addReadOnlyStaticStringHandler(OID_SERVICES, "90");

  snmp.addIntegerHandler(OID_SAA1, &Out[0], true);
  snmp.addIntegerHandler(OID_SAA2, &Out[1], true);
  snmp.addIntegerHandler(OID_SAA3, &Out[2], true);
  snmp.addIntegerHandler(OID_SAA4, &Out[3], true);
  snmp.addIntegerHandler(OID_AT, &Out[4], true);
  snmp.addIntegerHandler(OID_BPS, &Out[5], true);

  snmp.addIntegerHandler(OID_HUM, &SNMP_H);  
  snmp.addIntegerHandler(OID_T, &SNMP_T);  
  snmp.addIntegerHandler(OID_T1, &SNMP_T1);  
  snmp.addIntegerHandler(OID_T2, &SNMP_T2);  
  snmp.addIntegerHandler(OID_T3, &SNMP_T3);  
  snmp.addIntegerHandler(OID_T4, &SNMP_T4);

  snmp.addIntegerHandler(OID_VRS, &SNMP_VRS); 
  snmp.addIntegerHandler(OID_VST, &SNMP_VST); 
  snmp.addIntegerHandler(OID_VRT, &SNMP_VRT); 
  snmp.addIntegerHandler(OID_IR, &SNMP_IR); 
  snmp.addIntegerHandler(OID_IS, &SNMP_IS); 
  snmp.addIntegerHandler(OID_IT, &SNMP_IS); 
  //failsArray[lead-1]<4
  (failsArray[0]>3 || failsArray[1]>3)?Out[5]=0:Out[5]=1; //fails para AA1 Y AA2
  ///digitalWrite(BPS_S, Out[5]);
  //digitalWrite(BPS_S, !Out[5]);
  serverETH.begin();
  // linaresSensors.busser(BUZZER);
  flashLog("baja|Encendido del equipo");
  
}
 
uint16_t mb[25] = {0};
char buff[40];


void ControlAA(float *input,float *setpoint,uint8_t num,uint8_t lead){
  switch(ciclo){
    case 0:
      if(input[0]>setpoint[1] || input[1]>setpoint[1]|| input[2]>setpoint[1]|| input[3]>setpoint[1]){
        flashLog( "baja|Activacion de Aires Acondicionados - modo normal");
        Out[lead-1] = 1;
        ciclo=1;
        //Out[4]=0;
        //Out[5]=1;
      }
      break;
    case 1:
      if((input[0]<setpoint[0]) &&(input[1]<setpoint[0]) &&(input[2]<setpoint[0]) &&(input[3]<setpoint[0])){
        flashLog( "baja|Desactivacion de Aires Acondicionados - modo normal");
        Out[0]=0;Out[1]=0;Out[2]=0;Out[3]=0;
        //Out[4]=0;
        ciclo=0;
      }
      else if(input[0]>setpoint[2] || input[1]>setpoint[2] || input[2]>setpoint[2] || input[3]>setpoint[2]){
        if(lead==1) flashLog( "media| Activacion de Aire Acondicionado # 2  - modo AT");
        else if(lead==2) flashLog( "media| Activacion de Aire Acondicionado # 1  - modo AT");
        Out[0] = 1;Out[1] = 1;Out[2] = 1;
        Out[3] = 1;Out[4]=1;

        //Fails
        failsArray[lead-1]=failsArray[lead-1]+1;;
        //SetCalibrate();
        // reg.Fail_aa1=reg.Fail_aa1+1;
        // WhiteEeprom(FAIL_1,reg.Fail_aa1);
        ciclo=2;
      }
      break;
    case 2:
      if(input[0]<setpoint[0] &&  input[1]<setpoint[0] &&  input[2]<setpoint[0] && input[3]<setpoint[0]){
        flashLog( "baja|Desactivacion de Aires Acondicionados - luego de  AT");
        Out[0]=0;Out[1]=0;Out[2]=0;Out[3]=0;
        Out[4]=0;
        if(failsArray[lead-1]>3)Out[5]=0;
        //if(reg.Fail_aa1<4) Out[5]=1;
        //else Out[5]=0;
        
        ciclo=0;
      }
      else if(input[0]>setpoint[3] || input[1]>setpoint[3] || input[2]>setpoint[3] || input[3]>setpoint[3]){
        flashLog( "alta|Activacion modo Bypass");
        Out[5]=0;
        Out[4]=1;
        ciclo=3;
      }
      break;
    case 3:
      if(input[0]<setpoint[0] && input[1]<setpoint[0] && input[2]<setpoint[0] && input[3]<setpoint[0]){
        flashLog( "baja|Desactivacion de Aires Acondicionados - luego de  Bypass");
        Out[0]=0;Out[1]=0;Out[2]=0;Out[3]=0;
        Out[4]=0;
        
        if(failsArray[lead-1]>3)Out[5]=0;
        else Out[5]=1;
        //if(reg.Fail_aa1<4) Out[5]=1;
        //else Out[5]=0;
        ciclo=0;
      }
      break;
    default: break;

  }
}

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

void loop() {
  snmp.loop();
  long now = millis();
  linaresETH.runLoopETH();
  
  snmp.sortHandlers();

  digitalWrite(AA1, Out[0]);
  digitalWrite(AA2, Out[1]);
  digitalWrite(AA3, Out[2]);
  digitalWrite(AA4, Out[3]);
  digitalWrite(AT, Out[4]);
  digitalWrite(BPS_S, !Out[5]);

  if (now - lastMsg > 2000) {
    
    lastMsg = now;
    // DateTime now = rtc.now(); 
    
    uint8_t q = 10;

        //Serial.println("LEER HJUMEDAD");
    In[4] = linaresSensors.readHumidity();
    In[5]= linaresSensors.readTemperature(); 
        //Serial.println("LEER SENSORES");
    In[0] = linaresSensors.readNTC(0, 1,SetIn[0]);//GetPT100(2, reg.AlphaS2,reg.RefS2);//    float T2 = linaresSensors.readNTC(0, 1,SetIn[0]);//GetPT100(2, reg.AlphaS2,reg.RefS2);//
    In[1] = linaresSensors.readNTC(1, 1,SetIn[1]);//GetPT100(3, reg.AlphaS3,reg.RefS3);//    float T3 = linaresSensors.readNTC(0, 1,SetIn[0]);//GetPT100(3, reg.AlphaS3,reg.RefS3);//
    In[2] = linaresSensors.readNTC(2, 1,SetIn[2]);//GetPT100(0, reg.AlphaS1,reg.RefS1);//    float T1 = linaresSensors.readNTC(0, 1,SetIn[0]);//GetPT100(0, reg.AlphaS1,reg.RefS1);//
    In[3] = linaresSensors.readNTC(3, 1,SetIn[3]);//GetPT100(1, reg.AlphaS4,reg.RefS4);//    float T4 = linaresSensors.readNTC(0, 1,SetIn[0]);//GetPT100(1, reg.AlphaS4,reg.RefS4);//



    //Serial.println("LEER ENERGIA");
    In[6]= linaresSensors.getPeak(0, 1,SetIn[6]);
    In[7]= linaresSensors.getPeak(1, 1,SetIn[7]);
    In[8]= linaresSensors.getPeak(2, 1,SetIn[8]);
    In[9]= linaresSensors.getPeak(0, 2,SetIn[9]);
    In[10]= linaresSensors.getPeak(1, 2,SetIn[10]);
    In[11] = linaresSensors.getPeak(2, 2,SetIn[11]);

    
    uint8_t bp =digitalRead(BPS_H); 
    uint8_t bs =digitalRead(BPS_S); 


    SNMP_T1 =(int)(In[0]*100);
    SNMP_T2 =(int)(In[1]*100);
    SNMP_T3 =(int)(In[2]*100);
    SNMP_T4 =(int)(In[3]*100);
    SNMP_H = (int)(In[4]*100);
    SNMP_T =(int)(In[5]*100);
    SNMP_VRS =(int)(In[6]);
    SNMP_VST =(int)(In[7]);
    SNMP_VRT =(int)(In[8]);
    SNMP_IR =(int)(In[9]);
    SNMP_IS =(int)(In[10]);
    SNMP_IT =(int)(In[11]);

    DateTime now2 = rtc.now();
    DateTime future(now2 + TimeSpan(-18000)); 

    // Cambio de lead 
    semana = future.dayOfTheWeek();
    //Serial.println(semana_last);

    if ((semana_last == 1 && semana == 2)) {
      //if(reg.Lead==1) reg.Lead=2;
      if (lead == 1)lead = lead + 1;
      else lead = 1;
      SpisspsWhite("/config.json",lead);
      flashLog( "baja|Reinicio por cambio de secuenciado");
      ESP.restart();

    }
    semana_last = semana;

    ControlAA(In, setpointArray,2,lead);



    if (now - lastMsg2 > 120000) {
      lastMsg2 = now;
      sprintf(monitoreo,"%d|%d|%d|%d|%.2f|%.2f|%.2f|%.2f|%.2f|%.2f|%d|%d|%d|%.2f|%.2f|%.2f|%.2f|%.2f|%.2f|%d|%d",Out[0],Out[1],!Out[5],Out[4],In[0],In[1],In[2],In[3],In[4],In[5],Out[4],bp,Out[2],In[6],In[7],In[8],In[9],In[10],In[11],VERSION,millis()/1000,now2.unixtime());
      linaresETH.whiteHistory(monitoreo);
    }

    sprintf(monitoreo,"{\"o1\":%d,\"o2\":%d,\"o5\":%d,\"o4\":%d,\"i1\":%.2f,\"i2\":%.2f,\"i3\":%.2f,\"i4\":%.2f,\"i5\":%.2f,\"i6\":%.2f,\"i7\":%d,\"i8\":%d,\"i9\":%d,\"i10\":%.2f,\"i11\":%.2f,\"i12\":%.2f,\"i13\":%.2f,\"i14\":%.2f,\"i15\":%.2f,\"v\":%.1f,\"t\":%d,\"d\":%d}",Out[0],Out[1],!Out[5],Out[4],In[0],In[1],In[2],In[3],In[4],In[5],Out[4],bp,lead,In[6],In[7],In[8],In[9],In[10],In[11],VERSION,millis()/1000,now2.unixtime());
    

    Serial.println("memoria libre: " + String(esp_get_free_heap_size()) + " bytes");
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
  linaresSensors.whiteflash(d,"/log.csv");
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
    char message[100]= "Activacion de Aires Acondicionados - modo remoto ";
    if(val[0]=='1') {Out[0]=data; sprintf(message,"media|%s de Aire Acondicionado %u- modo remoto ",data?"Encendido":"Apagado",val[0]);}
    if(val[0]=='2') {Out[1]=data; sprintf(message,"media|%s de Aire Acondicionado %u- modo remoto ",data?"Encendido":"Apagado",val[0]);}
    if(val[0]=='5') {Out[5]=!data; sprintf(message,"alta|%s de Alarma Bypass- modo remoto ",data?"Encendido":"Apagado");}
    if(val[0]=='4') {Out[4]=data; sprintf(message,"media|%s de Alarma AT- modo remoto ",data?"Encendido":"Apagado");}
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
    if(btn==20) rtc.adjust(DateTime(data));
    else if(btn==0){ SetIn[0] =data/uint16_t(linaresSensors.readNTC(0, 1, 100));Serial.println(SetIn[0]);Calibrate=1;}
    else if(btn==1){ SetIn[1] =data/uint16_t(linaresSensors.readNTC(1, 1, 100));Serial.println(SetIn[1]);Calibrate=1;}
    else if(btn==2){ SetIn[2] =data/uint16_t(linaresSensors.readNTC(2, 1, 100));Serial.println(SetIn[2]);Calibrate=1;}
    else if(btn==3){ SetIn[3] =data/uint16_t(linaresSensors.readNTC(3, 1, 100));Serial.println(SetIn[3]);Calibrate=1;}

    else if(btn==6){ SetIn[6] =data/uint16_t(linaresSensors.getPeak(0, 1, 100));Serial.println(SetIn[6]);Calibrate=1;}
    else if(btn==7){ SetIn[7] =data/uint16_t(linaresSensors.getPeak(1, 1, 100));Serial.println(SetIn[7]);Calibrate=1;}
    else if(btn==8){ SetIn[8] =data/uint16_t(linaresSensors.getPeak(2, 1, 100));Serial.println(SetIn[8]);Calibrate=1;}
    else if(btn==9){ SetIn[9] =data/uint16_t(linaresSensors.getPeak(0, 2, 100));Serial.println(SetIn[9]);Calibrate=1;}
    else if(btn==10) {SetIn[10] =data/uint16_t(linaresSensors.getPeak(1, 2, 100));Serial.println(SetIn[10]);Calibrate=1;}
    else if(btn==11) {SetIn[11] =data/uint16_t(linaresSensors.getPeak(2, 2, 100));Serial.println(SetIn[11]);Calibrate=1;}
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
 
 
 

