// ---------------------------------------------------------------------------
// Prueba de diagnostico en Arduino (framework Arduino para ESP32, NO
// ESP-IDF) -- mismos pines fisicos que la prueba en controlador_labgeo:
//   REQ  = GPIO13 (OUT-AT)
//   CLK  = GPIO25 (P2)
//   DATA = GPIO27 (HUM)
//
// Objetivo: descartar que el problema sea de ESP-IDF (timing, configuracion
// de GPIO, etc.) probando el mismo protocolo con el framework Arduino, que
// es el que ya se sabe que funcionaba en el hardware viejo (LABGEO2.ino).
// Si esto tampoco lee el calibre, el problema es 100% de cableado/
// alimentacion del calibre, no de software/framework.
//
// Instrucciones:
//   1. Abrir esta carpeta con Arduino IDE (necesita el paquete de placas
//      "esp32" de Espressif instalado).
//   2. Placa: cualquier variante "ESP32 Dev Module" (el chip es el mismo
//      esp32 clasico que usa controlador_labgeo).
//   3. Subir y abrir el Monitor Serie a 115200 baud.
// ---------------------------------------------------------------------------

#define REQ  13  // OUT-AT
#define CLK  25  // P2
#define DATA 27  // HUM

// Copia casi literal de la funcion dial() de LABGEO2.ino/labgeo2025.ino.
// Unica diferencia real: se agregaron Serial.print de diagnostico (digito
// por digito y timeouts) que el original no tenia (estaban comentados ahi).
const char* dial(uint8_t pinREQ, uint8_t pinCLK, uint8_t pinDATA) {
  uint32_t timeout = 0;
  int i = 0, j = 0, k = 0;
  static char distancia[20];
  char mydata[14];

  Serial.println("dial(): REQ=1, esperando tren de 13 digitos...");
  digitalWrite(pinREQ, 1);

  for (i = 0; i < 13; i++) {
    k = 0;
    for (j = 0; j < 4; j++) {
      timeout = 100000;
      while (digitalRead(pinCLK) == 0) {
        delayMicroseconds(3);
        if ((timeout--) == 0) {
          Serial.print("timeout esperando CLK=1 (digito ");
          Serial.print(i);
          Serial.print(", bit ");
          Serial.print(j);
          Serial.println(")");
          digitalWrite(pinREQ, 0);
          return "ERROR!";
        }
      }
      timeout = 100000;
      while (digitalRead(pinCLK) == 1) {
        delayMicroseconds(3);
        if ((timeout--) == 0) {
          Serial.print("timeout esperando CLK=0 (digito ");
          Serial.print(i);
          Serial.print(", bit ");
          Serial.print(j);
          Serial.println(")");
          digitalWrite(pinREQ, 0);
          return "ERROR!";
        }
      }

      timeout = 2000;
      while ((timeout--) != 0)
        ;

      if (digitalRead(pinDATA) == 1 && j == 0) k = 1;
      else if (digitalRead(pinDATA) == 1 && j == 1) k = 2 + k;
      else if (digitalRead(pinDATA) == 1 && j == 2) k = 4 + k;
      else if (digitalRead(pinDATA) == 1 && j == 3) k = 8 + k;
      if (k == 15) k = 0;
    }

    char c = k + '0';
    mydata[i] = c;
    Serial.print("digito ");
    Serial.print(i);
    Serial.print(" = ");
    Serial.println(c);
  }

  sprintf(distancia, "%c%c.%c%c%c", mydata[6], mydata[7], mydata[8], mydata[9], mydata[10]);
  Serial.print("dial(): REQ=0, tren completo -> \"");
  Serial.print(distancia);
  Serial.println("\"");
  digitalWrite(pinREQ, 0);
  return distancia;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(REQ, OUTPUT);
  digitalWrite(REQ, 0);

  pinMode(CLK, INPUT);
  pinMode(DATA, INPUT);

  Serial.println();
  Serial.printf("Dial listo: REQ=GPIO%d CLK=GPIO%d DATA=GPIO%d\n", REQ, CLK, DATA);
}

void loop() {
  const char* resultado = dial(REQ, CLK, DATA);
  Serial.print("resultado = ");
  Serial.println(resultado);
  Serial.println("---");
  delay(2000);
}



// const char* dial(uint8_t REQ, uint8_t CLK, uint8_t DATA) {
// 	uint32_t timeout=0;
// 	int i = 0;int j = 0;int k = 0;
// 	static char distancia[20];
// 	char mydata[14];

	

// 	digitalWrite(REQ,1);
// 	//digitalWrite(LED3,1);
	
// 	for( i = 0; i < 13; i++ ){
// 		k = 0;
// 		for (j = 0; j < 4; j++){
// 			timeout=100000;
// 			while(digitalRead(CLK) == 0) {
//         delayMicroseconds(10);
// 				if ((timeout--) == 0){
// 					digitalWrite(REQ, 0);
//           //digitalWrite(LED3, 0);
// 					return "ERROR!";
// 				};
// 			}
// 			timeout=100000;
// 			while(digitalRead(CLK) == 1) {
//         delayMicroseconds(10);
// 				if ((timeout--) == 0){
					
// 					digitalWrite(REQ,0);
// 					//digitalWrite(LED3,0);
// 					return "ERROR!";
// 				};
// 			}
// 			timeout=2000;
// 			while ((timeout--) != 0);
// 			if(digitalRead(DATA)==1 && j==0)k=1;
// 			else if(digitalRead(DATA)==1 && j==1)k=2+k;
// 			else if(digitalRead(DATA)==1 && j==2)k=4+k;
// 			else if(digitalRead(DATA)==1 && j==3)k=8+k;
// 			if(k==15)k=0;
// 		}

// 		char c = k+'0';
// 		mydata[i] = c;
		
		
// 	}
// 	sprintf(distancia,"%c%c.%c%c%c",mydata[6],mydata[7],mydata[8],mydata[9],mydata[10]);
// 	digitalWrite(REQ,0);
// 	//digitalWrite(LED3,0);
// 	return distancia;
// }