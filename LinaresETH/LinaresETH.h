#ifndef LinaresETH_h
#define LinaresETH_h
#include "string.h"
#include <Arduino.h>
#include <Ethernet.h>
#include <Update.h>
class LinaresETH {
  public:
    // Constructor
    LinaresETH(uint8_t pin_rst, EthernetServer& server);
    // Método para manejar las conexiones Ethernet
    void loopETH();
    uint8_t actualizarDesdeSPIFFS(char* ruta);
    //void setCallback(void (*func)()); // Método para asignar el puntero a función
    void setCallback(void (*func)(char*, EthernetClient&,char*)); // Puntero a función con 2 parámetros
    // Método para actualizar el dato char
    void setDato(const char* nuevoDato);
    void eth_error(EthernetClient& client, char* err);
    uint16_t parse_query(char *query,char *data);
    void sendData(EthernetClient client,char *data);
    void runLoopETH();
    void eth_get_history(EthernetClient client);
    void  whiteHistory( char* data);
    uint8_t begin(uint8_t *mac, uint8_t *ip, uint8_t *dns, uint8_t *gateway, uint8_t *subnet);  // Método para inicializar
    uint8_t isConnected();  // Método para verificar conexión
  private:
    EthernetServer& serverETH; // Referencia al servidor Ethernet
    char dato[1024]; // Almacena el dato char 
    uint8_t _pin_rst;  // Pin de rese
    // Funciones auxiliares privadas
    void eth_get_sspif(EthernetClient client,char* type,char* name);
    void ReadDataSPIFFS(const char* ruta, EthernetClient& client);
    char* get_ruta(char *cadena);
    char* get_ruta2(char *cadena);
    void (*callback)(char*, EthernetClient& ,char*);
    void processGet(EthernetClient client,char *ruta);
    void processPost(EthernetClient client,char *ruta,char *body);
    void eth_post_config(EthernetClient client,char *post,char *ruta);
    char* extract_filename(const char* input);
    void renameFile( char* oldPath,  char* newPath);
    void eth_get_list_files(EthernetClient client); 
};

#endif