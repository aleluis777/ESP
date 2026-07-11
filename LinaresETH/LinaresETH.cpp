#include "LinaresETH.h"
// #include <EEPROM.h>
// #include <Ethernet.h>
#include "SPIFFS.h"
// #include <Adafruit_ADS1X15.h>
#include "string.h"


// Constructor
LinaresETH::LinaresETH(uint8_t pin_rst, EthernetServer& server): serverETH(server) {
  dato[0] = '\0';       // Inicializa el dato como una cadena vacía
  callback = nullptr;   // Inicialmente sin función asignada
  _pin_rst = pin_rst;    // Asigna el pin de reset
}

uint8_t LinaresETH::begin(uint8_t *mac, uint8_t *ip, uint8_t *dns, uint8_t *gateway, uint8_t *subnet) {
  pinMode(_pin_rst, OUTPUT);
  digitalWrite(_pin_rst, HIGH);
  delay(300);
  digitalWrite(_pin_rst, LOW);
  delay(200);
  digitalWrite(_pin_rst, HIGH);
  delay(300);

  Ethernet.init(17);
  Ethernet.begin(mac, ip, dns, gateway, subnet);
  delay(1000);
  if (Ethernet.hardwareStatus() == EthernetNoHardware) {
    Serial.print("server is at ");
    Serial.println(Ethernet.localIP());
    Serial.println("Ethernet shield was not found. Sorry, can't run without hardware. :(");
    return 0;
  }

  
  if (Ethernet.linkStatus() == LinkOFF) {
    Serial.println("Ethernet cable is not connected.");
    return 0;
  }
  Serial.print("server is at ");
  Serial.println(Ethernet.localIP());
  return 1;
}
// Método para verificar si hay conexión Ethernet
uint8_t LinaresETH::isConnected() {
  return (Ethernet.linkStatus() == LinkON);
}

void LinaresETH::setCallback(void (*func)(char*, EthernetClient& ,char*)) {
    callback = func; // Guardar la dirección de la función
}
// Método para actualizar el dato char
void LinaresETH::setDato(const char* nuevoDato) {
  strncpy(dato, nuevoDato, sizeof(dato) - 1); // Copia el nuevo dato
  dato[sizeof(dato) - 1] = '\0'; // Asegura que la cadena esté terminada correctamente
} 
void LinaresETH::loopETH(){
  if (Ethernet.linkStatus() == LinkOFF) {
    Serial.print("*");
  }
  else{
    //Serial.print(".");
    EthernetClient client = serverETH.available();
    //Serial.println(client);
    if (client) {
      // Serial.println("new client");
      // una solicitud HTTP termina con una línea en blanco
      
      char filename[30];
      bool currentLineIsBlank = true;
      char trama[1500]="";
      char trama2[100]="";
      char trama3[100]="";
      char trama4[100]="";
      char body[1500]="";
      char ruta[30]="";
      int n=0;bool val=true;
      File uploadFile;
      int salto_linea=0;
      int n2=0;
      int n4=0;
      int boundary=0;
      int n5=0;
      bool streamingFirmware = false;
      bool salto_start=false;
      bool val2=true;
      bool val3=false;
      bool val4=false;
      bool val5=true;
      char http  ='N';
      uint8_t post=0;
      int ContentLength =-1;
      int cnt =0;
      while (client.connected()) {
        if (client.available()) {
          char c = client.read();
          //vSerial.write(c);
          //if(c=='\n'){Serial.write('*');}
          //if(c=='\r'){Serial.write('-');}
          // Lee la trama y la ruta
          if(val){
            trama[n]=c;
            n++;
            if(c=='\n'|| c=='\r') {
              //Serial.println(trama);
              strcpy(ruta, get_ruta(trama)); 
              trama[n]='\0';val=false;
              }
            
          }
          
          // Lee el content length
          if(val2){
            trama2[n2]=c;
            n2++;
            if(trama2[0]=='G' || trama2[0]=='O') {val2=false;}
            // else if(trama2[0]!='P' && trama2[1]!='O') {eth_error(client);return;}

            if((c=='\n'|| c=='\r' ) && strcmp(trama2, "Content-Length")==58){
              //erial.println("Content-Length");
              trama2[n2]='\0';
              val2=false;
              String dat=trama2;
              dat=dat.substring(16, 30);
              ContentLength=dat.toInt();
              //Serial.println(ContentLength);
              if(ContentLength>1000) {
                if(streamingFirmware==false){
                  streamingFirmware = true;
                  if (SPIFFS.exists("/programa.bin")) {
                    SPIFFS.remove("/programa.bin");
                    //Serial.println("Archivo eliminado");
                  }
                  uploadFile = SPIFFS.open("/programa.bin", "w");
                  if (!uploadFile) {
                    client.println("HTTP/1.1 500 Internal Server Error");
                    client.stop();
                    return;
                  }
                  else {}
                }
              }
            }
            else if(c=='\n'|| c=='\r' ) {n2=0;}
          }

          //espera el content type - multipart/form-data; boundary=
          if(val5){
            trama3[n5]=c;
            trama3[n5+1]='\0';
            n5++; 
            // else if(trama2[0]!='P' && trama2[1]!='O') {eth_error(client);return;}
            //if(c=='\n'|| c=='\r' ) Serial.println(strcmp(trama3, "Content-Type: multipart/form-data; boundary="));
            if((c=='\n'|| c=='\r' ) && strcmp(trama3, "Content-Type: multipart/form-data; boundary=")==45){
              //Serial.println("Content-Type: multipart/form-data; boundary=");

              if(streamingFirmware==false){
                Serial.println("Se guardara en el spifss");
                streamingFirmware = true;
                if (SPIFFS.exists("/programa.bin")) {
                  SPIFFS.remove("/programa.bin");
                  //Serial.println("Archivo eliminado");
                }
                uploadFile = SPIFFS.open("/programa.bin", "w");
                if (!uploadFile) {
                  client.println("HTTP/1.1 500 Internal Server Error");
                  client.stop();
                  return;
                }
                else {}
              }

              trama3[n5]='\0';
              val5=false;
              String dat=trama3;
              dat=dat.substring(44,90);
              //Serial.println(n5-45);
              //Serial.println(dat);
              boundary=n5-45+8;// 8 es dos enter ms 4 -
               
            }
            else if(c=='\n'|| c=='\r' ) {n5=0;}
          }

          if (c == '\n' && currentLineIsBlank && (trama[0]=='G' || trama[0]=='O')){processGet(client, ruta);break;}

          if (c == '\n') {currentLineIsBlank = true;}
          else if (c != '\r') {currentLineIsBlank = false;}

          if(val4 && (cnt!=0 || c!='\n') ){

            if(streamingFirmware==false)body[cnt]=c;
            else{
              if(salto_start==true){uploadFile.write(c);}
              else{
                trama4[n4]=c;
                trama4[n4+1]='\0';
                if(c=='\n'|| c=='\r' ) {  

                  const char* filename_key = strstr(trama4, "filename=");
                  if (filename_key != NULL){
                    filename_key += strlen("filename=");
                    if (*filename_key == '"') {filename_key++;}
                
                    const char* end_quote = strchr(filename_key, '"');
                    if (end_quote != NULL) {
                      strncpy(filename, filename_key, end_quote - filename_key);
                      filename[end_quote - filename_key] = '\0';
                      //Serial.println(filename);
                      //sprintf(filename,"/%s\0", filename);
                      //Serial.print("filename=  ");
                      //Serial.println(filename);
                    }
                  }
                  else Serial.println("archivo cacio"); 

                }
                n4++;
                switch (c)
                {
                  case '\r':
                    if(salto_linea==0)salto_linea=1;
                    else if(salto_linea==2)salto_linea=3;
                    else salto_linea=0;
                    break;
                  case '\n': 
                    if(salto_linea==1)salto_linea=2;
                    else if(salto_linea==3){salto_start=true;}
                    else salto_linea=0;
                    break;
                  default:
                    salto_linea=0;
                  break;
                }
              }
            }
            cnt++;
            }
          if(val3){if(c == '\r'){val4=true;val5=false;}val3=false;}
          if (c == '\n'){val3=true;}

          if (cnt==ContentLength-boundary) {
            if(streamingFirmware==true){
              uploadFile.close();
              memmove(filename + 1, filename, 95);//verificad leng+1
              filename[0] = '/';
              //SPIFFS.rename("/programa.bin", filename);

              renameFile("/programa.bin", filename);
              Serial.println("Archivo cerrado");
              Serial.println(strlen(filename));
              Serial.println(filename);
              sendData(client,"ok");
              }
            else processPost(client, ruta, body);
            break;
            }
          }
      }
      //dar tiempo al navegador web para recibir los datos
      delay(1);
      // close the connection:
      client.stop();
      
      // Serial.println("client disconnected");
    }
    //Serial.println("fin lop eth");
  }
}
void LinaresETH::processPost(EthernetClient client,char *ruta,char *body){
  if(strcmp(ruta, "/firmware")==0) {eth_post_config(client,body,"/firmware.bin");}
  if(strcmp(ruta, "/config")==0) {eth_post_config(client,body,"/config.json");} 
  else {
    if (callback) {
      callback(ruta, client,body);
    }
  }
}

void LinaresETH::processGet(EthernetClient client,char *ruta){
  if (strcmp(ruta, "/") == 0) {eth_get_sspif(client, "text/html", "/index.html");} 
  else if (strcmp(ruta, "/index.html") == 0) {eth_get_sspif(client, "text/html", "/index.html");}
  else if (strcmp(ruta, "/lista.html") == 0) {eth_get_sspif(client, "text/html", "/lista.html");}
  else if (strcmp(ruta, "/lista") == 0) {eth_get_sspif(client, "text/html", "/lista.html");}
  else if (strcmp(ruta, "/logo.png") == 0) {eth_get_sspif(client, "image/png", "/logo.png");}
  else if (strcmp(ruta, "/fimware.html") == 0) {eth_get_sspif(client, "text/html", "/fimware.html");}
  else if (strcmp(ruta, "/fimware") == 0) {eth_get_sspif(client, "text/html", "/fimware.html");}
  else if (strcmp(ruta, "/test") == 0) {eth_get_sspif(client, "text/html", "/programa.bin");}
  else if (strcmp(ruta, "/style.css") == 0) {eth_get_sspif(client, "text/css", "/style.css");}
  else if (strcmp(ruta, "/alerts") == 0) {eth_get_sspif(client, "text/html", "/alerts.html");}
  else if (strcmp(ruta, "/script.js") == 0) {eth_get_sspif(client, "text/javascript", "/script.js");}
  else if (strcmp(ruta, "/config.json") == 0) {eth_get_sspif(client, "text/javascript", "/config.json");}
  else if (strcmp(ruta, "/config2.json") == 0) {eth_get_sspif(client, "text/javascript", "/config2.json");}
  else if (strcmp(ruta, "/script2.js") == 0) {eth_get_sspif(client, "text/javascript", "/script2.js");}
  else if (strcmp(ruta, "/log.csv") == 0) {eth_get_sspif(client, "text/javascript", "/log.csv");}
  else if (strcmp(ruta, "/calibrations.json") == 0) {eth_get_sspif(client, "text/javascript", "/calibrations.json");}
  else if (strcmp(ruta, "/log") == 0) {eth_get_sspif(client, "text/html", "/log.html");}
  else if (strcmp(ruta, "/history.csv") == 0) {eth_get_history(client);}
  else if (strcmp(ruta, "/history") == 0) {eth_get_sspif(client, "text/html", "/history.html");}
  else if (strcmp(ruta, "/history.html") == 0) {eth_get_sspif(client, "text/html", "/history.html");}
  else if (strcmp(ruta, "/list.json") == 0) {eth_get_list_files(client);}
  else {
    if (callback) {
      callback(ruta, client, "");
    }
  }
}


void LinaresETH::eth_post_config(EthernetClient client,char *post,char *ruta){
  File uploadFile;

  if (SPIFFS.exists(ruta)) {
    SPIFFS.remove(ruta);
    //Serial.println("Archivo eliminado");
  }
  
  uploadFile = SPIFFS.open(ruta, "w");
  if (!uploadFile) {eth_error(client,"error");return;}

  uploadFile.print(post);
  uploadFile.close();
  sendData(client,"OK");
  return;
} 

void LinaresETH::runLoopETH() {
  for (int i = 0; i < 100; i++) {
    loopETH();
    yield();     // Permite que el sistema realice otras tareas
    delay(10);  // Espera el tiempo especificado
  }
}

// Función para manejar la ruta sspif
void LinaresETH::eth_get_sspif(EthernetClient client,char* type,char* name){//image/png
  client.println("HTTP/1.1 200 OK");
  client.println("Access-Control-Allow-Origin: *");
  client.println("Access-Control-Allow-Methods: GET, POST, OPTIONS");
  client.println("Access-Control-Allow-Headers: Content-Type, Authorization");
  client.print("Content-Type: ");
  client.println(type);
  client.println("Connection: close");  // the connection will be closed after completion of the response
  client.println();
  ReadDataSPIFFS(name,client);
}

void LinaresETH::eth_get_list_files(EthernetClient client){//image/png
  char lista[200]="";
  File root = SPIFFS.open("/");
  File archivo = root.openNextFile();

  client.println("HTTP/1.1 200 OK");
  client.println("Access-Control-Allow-Origin: *");
  client.println("Access-Control-Allow-Methods: GET, POST, OPTIONS");
  client.println("Access-Control-Allow-Headers: Content-Type, Authorization");
  client.println("Content-Type: text/javascript");
  client.println("Connection: close");  // the connection will be closed after completion of the response
  client.println();
  client.print("["); 
  while (archivo) {  
    sprintf(lista,"{\"name\":\"%s\",\"size\":%d}",archivo.name(),archivo.size());
    client.print(lista); 
    archivo = root.openNextFile();
    if(archivo) client.print(",");
  }
  client.println("]"); 
}
 

// Función para manejar errores
void LinaresETH::eth_error(EthernetClient& client, char* err) {
  client.println("HTTP/1.1 400");
  client.println("Access-Control-Allow-Origin: *");
  client.println("Access-Control-Allow-Methods: GET, POST, OPTIONS");
  client.println("Access-Control-Allow-Headers: Content-Type, Authorization");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();
  client.println(err);
}
 
void LinaresETH::ReadDataSPIFFS(const char* ruta, EthernetClient& client) {
  File file = SPIFFS.open(ruta, "r"); // Abrir el archivo en modo lectura
  if (!file) {
    client.println("Error interno");
    return;
  }

  // Buffer para leer el archivo en trozos
  const size_t bufferSize = 2000; // Tamaño del buffer
  char buffer[bufferSize];

  // Leer y enviar el archivo en trozos
  while (file.available()) {
    // Verificar si el cliente sigue conectado
    if (!client.connected()) {
      Serial.println("Cliente desconectado");
      file.close();
      return;
    }

    // Leer un trozo del archivo
    size_t bytesRead = file.readBytes(buffer, bufferSize);

    // Enviar el trozo al cliente
    client.write(buffer, bytesRead);
  }

  file.close(); // Cerrar el archivo
}

char* LinaresETH::get_ruta(char *cadena){
    uint8_t n=0;
    char delimitador[2]=" ";
    static char  data[40] = "";
    char *token = strtok(cadena,delimitador);
        while(token != NULL){
            if(n==1){return token;}
            n=n+1;
            token = strtok(NULL, delimitador);
        }
    return "";

}

char* LinaresETH::get_ruta2( char* cadena) {
    // Verificar que la cadena no sea NULL
    if (cadena == NULL) {
        return "";
    }

    // Buffer para almacenar la ruta
    static char data[40] = "";

    // Copiar la cadena para no modificar la original
    char copiaCadena[100]; // Ajusta el tamaño según tus necesidades
    strncpy(copiaCadena, cadena, sizeof(copiaCadena) - 1);
    copiaCadena[sizeof(copiaCadena) - 1] = '\0'; // Asegurar terminación nula

    // Tokenizar la cadena usando el delimitador de espacio
    char* token = strtok(copiaCadena, " ");
    uint8_t n = 0;

    while (token != NULL) {
        if (n == 1) {
            // Copiar la ruta al buffer estático
            strncpy(data, token, sizeof(data) - 1);
            data[sizeof(data) - 1] = '\0'; // Asegurar terminación nula
            return data;
        }
        n++;
        token = strtok(NULL, " ");
    }

    // Si no se encuentra la ruta, devolver una cadena vacía
    return "";
}

void LinaresETH::sendData(EthernetClient client,char *data){
  client.println("HTTP/1.1 200 OK");
  client.println("Access-Control-Allow-Origin: *");
  client.println("Access-Control-Allow-Methods: GET, POST, OPTIONS");
  client.println("Access-Control-Allow-Headers: Content-Type, Authorization");
  client.println("Content-Type: application/json");
  client.println("Connection: close");  // the connection will be closed after completion of the response
  client.println();
  client.println(data);
}
//parse_query("GET /sag?Id=12&senros=2&dato3=14&Point=40&e=12 HTTP/1.1","e=");
uint16_t LinaresETH::parse_query(char *query,char *data) {
  //Serial.println(query);
  //Serial.println(data);
  char *ret;
  char *dato;
  ret = strstr(query, data);
  dato = strdup(ret);
  strtok(dato, "&");//salida e=12 HTTP/1.1
  //Serial.println(dato);
  strtok(dato, " ");//salida e=12
  //Serial.println(dato);
  char *token = strtok(dato, "=");//salida e
  //Serial.println(token);
  token = strtok(NULL, "=");//salida 12
  Serial.println(token);
  return(atoi(token));
}

uint8_t LinaresETH::actualizarDesdeSPIFFS(char* ruta) {
  Serial.println("\n[OTA] Iniciando proceso de actualización desde SPIFFS");
  
  File firmware = SPIFFS.open(ruta, "r");
  if (!firmware) {return 1;//"[ERROR] No se pudo abrir el archivo"
  }

  size_t firmwareSize = firmware.size();
  Serial.printf("[INFO] Tamaño del firmware: %zu bytes\n", firmwareSize);

  if (firmwareSize == 0) {firmware.close();return 2;//"[ERROR] Archivo de firmware vacío o corrupto"
    }

  if (Update.begin(firmwareSize)) {
      size_t written = Update.writeStream(firmware);
      Serial.printf("[INFO] Bytes escritos: %zu/%zu (%.1f%%)\n",written, firmwareSize, (written * 100.0) / firmwareSize);
      firmware.close();
      if (written != firmwareSize) {
          return 3;//"[ERROR] Escritura incompleta"
           
      }
      if (Update.end(true)) { // true para validar el firmware escrito
          if (SPIFFS.exists(ruta)) {
            SPIFFS.remove(ruta);
            Serial.println("Archivo eliminado");
          }
          else Serial.println("Archivo no encontrado");
          return 0;//[OK] Firmware verificado correctamente"
      } else {
          return 4;//"[ERROR] Fallo al finalizar actualización"
      }
  } else {
      firmware.close();
      return 5;//[ERROR] No hay espacio suficiente para la actualización
  }
}

char* LinaresETH::extract_filename(const char* input) {

  const char* filename_key = strstr(input, "filename=");
  if (filename_key == NULL) {return strdup("");}

  // Avanzar hasta la comilla que sigue a "filename="
  filename_key += strlen("filename=");if (*filename_key == '"') {filename_key++;}

  // Buscar la comilla de cierre
  const char* end_quote = strchr(filename_key, '"');if (end_quote == NULL) {return strdup("");}

  // Calcular la longitud del nombre del archivo
  size_t filename_length = end_quote - filename_key;

  // Asignar memoria para el nombre del archivo
  char* filename = (char*)malloc(filename_length + 1); // +1 para el carácter nulo
  if (filename == NULL) {
      // Si falla la asignación de memoria, retornar cadena vacía
      return strdup("");
  }

  // Copiar el nombre del archivo
  strncpy(filename, filename_key, filename_length);
  filename[filename_length] = '\0'; // Asegurar que la cadena termine correctamente

  return filename;
}

void LinaresETH::renameFile( char* oldPath,  char* newPath) {
  // Verificar si el archivo original existe
  if (!SPIFFS.exists(oldPath)) {
    Serial.printf("Archivo original '%s' no existe\n\r", oldPath);
    return;
  }

  // Verificar si el nuevo nombre ya existe (opcional)
  if (SPIFFS.exists(newPath)) {
    Serial.printf("Advertencia: '%s' ya existe. Se sobrescribirá.\n\r", newPath);
    SPIFFS.remove(newPath);

  }

  // Renombrar archivo
  if (SPIFFS.rename(oldPath, newPath)) {
    Serial.printf("Archivo renombrado: '%s' -> '%s'\n\r", oldPath, newPath);
  } else {
    Serial.println("¡Error al renombrar!");
  }
}

void LinaresETH::eth_get_history(EthernetClient client){
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/text");

  client.println("Access-Control-Allow-Origin: *");
  client.println("Access-Control-Allow-Methods: GET, POST, OPTIONS");
  client.println("Access-Control-Allow-Headers: Content-Type, Authorization");
  
  client.println("Connection: close");
  client.println();

  ReadDataSPIFFS("/history3.csv",client);
  ReadDataSPIFFS("/history2.csv",client);
  ReadDataSPIFFS("/history.csv",client);
}


void  LinaresETH::whiteHistory( char* data){
  File file = SPIFFS.open("/history.csv", "a");
  if (!file) {Serial.println("FALLO ABRIENDO");return;}
  
  if(file.size()>100000){
    
    SPIFFS.remove("/history3.csv");SPIFFS.rename("/history2.csv","/history3.csv");
    SPIFFS.remove("/history2.csv");SPIFFS.rename("/history.csv","/history2.csv");
    SPIFFS.remove("/history.csv");

    file.close();
    File file2 = SPIFFS.open("/history.csv", "a");
    if (!file2) {Serial.println("FALLO ABRIENDO");return;}

    file2.print(data);
    file2.print("\n");
    file2.close();
    delay(2);
  }
  else{
    file.print(data);
    file.print("\n");
    file.close();
    delay(2);
  }  
}