#include <SPI.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <stdlib.h>
#include <fs.h>
#include <MiLightClient.h>
#include <WebServer.h>
#include <IntParsing.h>
#include <Settings.h>
#include <MiLightUdpServer.h>

MiLightClient* milightClient;
MiLightUdpServer** udpServers;
int numUdpServers = 0;
WiFiManager wifiManager;
WebServer *server = new WebServer(80);
File updateFile;
Settings settings;

void handleUpdateGateway(const UrlTokenBindings* urlBindings) {
  DynamicJsonBuffer buffer;
  JsonObject& request = buffer.parse(server->arg("plain"));
  
  const uint16_t deviceId = parseInt<uint16_t>(urlBindings->get("device_id"));
  
  if (request.containsKey("status")) {
    if (request["status"] == "on") {
      milightClient->allOn(deviceId);
    } else if (request["status"] == "off") {
      milightClient->allOff(deviceId);
    }
  }
  
  server->send(200, "application/json", "true");
}

void handleUpdateGroup(const UrlTokenBindings* urlBindings) {
  DynamicJsonBuffer buffer;
  JsonObject& request = buffer.parse(server->arg("plain"));
  
  if (!request.success()) {
    server->send(400, "text/plain", "Invalid JSON");
    return;
  }
  
  const uint16_t deviceId = parseInt<uint16_t>(urlBindings->get("device_id"));
  const uint8_t groupId = urlBindings->get("group_id").toInt();
  
  if (request.containsKey("status")) {
    const String& statusStr = request.get<String>("status");
    MiLightStatus status = (statusStr == "on" || statusStr == "true") ? ON : OFF;
    milightClient->updateStatus(deviceId, groupId, status);
  }
  
  if (request.containsKey("hue")) {
    milightClient->updateHue(deviceId, groupId, request["hue"]);
  }
  
  if (request.containsKey("level")) {
    milightClient->updateBrightness(deviceId, groupId, request["level"]);
  }
  
  if (request.containsKey("command")) {
    if (request["command"] == "set_white") {
      milightClient->updateColorWhite(deviceId, groupId);
    }
    
    if (request["command"] == "all_on") {
      milightClient->allOn(deviceId);
    }
    
    if (request["command"] == "all_off") {
      milightClient->allOff(deviceId);
    }
    
    if (request["command"] == "unpair") {
      milightClient->unpair(deviceId, groupId);
    }
    
    if (request["command"] == "pair") {
      milightClient->pair(deviceId, groupId);
    }
  }
  
  server->send(200, "application/json", "true");
}

void handleListenGateway() {
  while (!milightClient->available()) {
    if (!server->clientConnected()) {
      return;
    }
    
    yield();
  }
  
  MiLightPacket packet;
  milightClient->read(packet);
  
  String response = "Packet received (";
  response += String(sizeof(packet)) + " bytes)";
  response += ":\n";
  response += "Request type : " + String(packet.deviceType, HEX) + "\n";
  response += "Device ID    : " + String(packet.deviceId, HEX) + "\n";
  response += "Color        : " + String(packet.color, HEX) + "\n";
  response += "Brightness   : " + String(packet.brightness, HEX) + "\n";
  response += "Group ID     : " + String(packet.groupId, HEX) + "\n";
  response += "Button       : " + String(packet.button, HEX) + "\n";
  response += "Sequence Num : " + String(packet.sequenceNum, HEX) + "\n";
  response += "\n\n";
  
  server->send(200, "text/plain", response);
}

bool serveFile(const char* file, const char* contentType = "text/html") {
  if (SPIFFS.exists(file)) {
    File f = SPIFFS.open(file, "r");
    server->send(200, contentType, f.readString());
    f.close();
    return true;
  }
  
  return false;
}

ESP8266WebServer::THandlerFunction handleServeFile(const char* filename, 
  const char* contentType, 
  const char* defaultText = NULL) {
    
  return [filename, contentType, defaultText]() {
    if (!serveFile(filename)) {
      if (defaultText) {
        server->send(200, contentType, defaultText);
      } else {
        server->send(404);
      }
    }
  };
}

ESP8266WebServer::THandlerFunction handleUpdateFile(const char* filename) {
  return [filename]() {
    HTTPUpload& upload = server->upload();
    
    if (upload.status == UPLOAD_FILE_START) {
      updateFile = SPIFFS.open(filename, "w");
    } else if(upload.status == UPLOAD_FILE_WRITE){
      if (updateFile.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Serial.println("Error updating web file");
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      updateFile.close();
    }
  };
  yield();
}

void onWebUpdated() {
  server->send(200, "text/plain", "success");
}

void initMilightUdpServers() {
  if (udpServers) {
    for (int i = 0; i < numUdpServers; i++) {
      if (udpServers[i]) {
        delete udpServers[i];
      }
    }
    
    delete udpServers;
  }
  
  udpServers = new MiLightUdpServer*[settings.numGatewayConfigs];
  numUdpServers = settings.numGatewayConfigs;
  
  for (size_t i = 0; i < settings.numGatewayConfigs; i++) {
    GatewayConfig* config = settings.gatewayConfigs[i];
    
    if (config->protocolVersion == 0) {
      udpServers[i] = new MiLightUdpServer(milightClient, config->port, config->deviceId);
      udpServers[i]->begin();
    } else {
      Serial.print("Error initializing milight UDP server - Unsupported protocolVersion: ");
      Serial.println(config->protocolVersion);
    }
  }
}

void initMilightClient() {
  if (milightClient) {
    delete milightClient;
  }
  
  milightClient = new MiLightClient(settings.cePin, settings.csnPin);
  milightClient->begin();
}

void handleUpdateSettings() {
  DynamicJsonBuffer buffer;
  const String& rawSettings = server->arg("plain");
  JsonObject& parsedSettings = buffer.parse(rawSettings);
  
  if (parsedSettings.success()) {
    settings.patch(parsedSettings);
    settings.save();
    initMilightClient();
    initMilightUdpServers();
    
    if (server->authenticationRequired() && !settings.hasAuthSettings()) {
      server->disableAuthentication();
    } else {
      server->requireAuthentication(settings.adminUsername, settings.adminPassword);
    }
    
    server->send(200, "application/json", "true");
  } else {
    server->send(400, "application/json", "\"Invalid JSON\"");
  }
}

void setup() {
  Serial.begin(9600);
  wifiManager.autoConnect();
  SPIFFS.begin();
  Settings::load(settings);
  initMilightClient();
  initMilightUdpServers();
  
  server->on("/", HTTP_GET, handleServeFile(WEB_INDEX_FILENAME, "text/html"));
  server->on("/settings", HTTP_GET, handleServeFile(SETTINGS_FILE, "application/json"));
  server->on("/settings", HTTP_PUT, handleUpdateSettings);
  server->on("/gateway_traffic", HTTP_GET, handleListenGateway);
  server->onPattern("/gateways/:device_id/:group_id", HTTP_PUT, handleUpdateGroup);
  server->onPattern("/gateways/:device_id", HTTP_PUT, handleUpdateGateway);
  server->on("/web", HTTP_POST, onWebUpdated, handleUpdateFile(WEB_INDEX_FILENAME));
  server->on("/firmware", HTTP_POST, 
    [](){
      server->sendHeader("Connection", "close");
      server->sendHeader("Access-Control-Allow-Origin", "*");
      server->send(200, "text/plain", (Update.hasError())?"FAIL":"OK");
      ESP.restart();
    },
    [](){
      HTTPUpload& upload = server->upload();
      if(upload.status == UPLOAD_FILE_START){
        WiFiUDP::stopAll();
        uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        if(!Update.begin(maxSketchSpace)){//start with max available size
          Update.printError(Serial);
        }
      } else if(upload.status == UPLOAD_FILE_WRITE){
        if(Update.write(upload.buf, upload.currentSize) != upload.currentSize){
          Update.printError(Serial);
        }
      } else if(upload.status == UPLOAD_FILE_END){
        if(Update.end(true)){ //true to set the size to the current progress
        } else {
          Update.printError(Serial);
        }
      }
      yield();
    }
  );
  
  if (settings.adminUsername.length() > 0 && settings.adminPassword.length() > 0) {
    server->requireAuthentication(settings.adminUsername, settings.adminPassword);
  }
  
  server->begin();
}

void loop() {
  server->handleClient();
  
  if (udpServers) {
    for (size_t i = 0; i < settings.numGatewayConfigs; i++) {
      yield();
      udpServers[i]->handleClient();
    }
  }
}
