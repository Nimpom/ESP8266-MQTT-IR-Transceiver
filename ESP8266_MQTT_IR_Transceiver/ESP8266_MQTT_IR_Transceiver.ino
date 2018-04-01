#include <SECRETS.h>

#include <ESP8266WiFi.h>
#include <PubSubClient.h>

#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>

/************************* WiFi Access Point *********************************/

#define WLAN_SSID       SECRET_SSID
#define WLAN_PASS       SECRET_WLAN_PSW

/************************* MQTT Setup *********************************/

#define MQTT_SERVER           SECRET_MQTT_URL
#define MQTT_SERVERPORT       SECRET_MQTT_PORT
#define MQTT_USERNAME         SECRET_MQTT_USERNAME
#define MQTT_KEY              SECRET_MQTT_KEY
#define MQTT_MAX_PACKET_SIZE  512
#define MQTT_PUB_TOPIC        "hass/IR/sovrum/recived/state"
#define MQTT_SUB_TOPIC        "hass/IR/sovrum/send/state"

/************************* IR Setup *********************************/

#define RECV_PIN D5
#define IR_SEND_PIN D2
#define DELAY_BETWEEN_COMMANDS 1000
#define CAPTURE_BUFFER_SIZE 1024
#define TIMEOUT 15U
#define MIN_UNKNOWN_SIZE 12

bool justSentIR;

// Use turn on the save buffer feature for more complete capture coverage.
IRrecv irrecv(RECV_PIN, CAPTURE_BUFFER_SIZE, TIMEOUT, true);
IRsend irsend(IR_SEND_PIN);

decode_results results;  // Somewhere to store the results

/************ Global State (you don't need to change this!) ******************/

// Create an ESP8266 WiFiClient class to connect to the MQTT server.
WiFiClient espClient;
PubSubClient client(espClient);

/*************************** Sketch Code ************************************/

// Bug workaround for Arduino 1.6.6, it seems to need a function declaration
// for some reason (only affects ESP8266, likely an arduino-builder bug).
void MQTT_connect();

String IRcode;
//#############################################################################################################

void setup() {
  Serial.begin(115200);
  delay(10);

  // Connect to WiFi access point.
  Serial.print("Connecting to: ");
  Serial.println(WLAN_SSID);

  WiFi.begin(WLAN_SSID, WLAN_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  Serial.println("WiFi connected");
  Serial.println("IP address: "); Serial.println(WiFi.localIP());

  client.setServer(MQTT_SERVER, MQTT_SERVERPORT);
  client.setCallback(callback);
  
  // Start IR reciver and sender
  irrecv.enableIRIn();
  irsend.begin();
}
//#############################################################################################################

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Recived IR code from MQTT broker: ");
  String str = "";
  for (int i = 0; i < length; i++) {
    str += (char)payload[i];
  }
  Serial.println(str);
  Serial.println(length);
  if(length > 10){
    Serial.println("Pronto");
    parseStringAndSendPronto(str.c_str(), 3);
  }else{
    Serial.println("NEC");
    uint8_t first  = atoi (str.substring(1, 3).c_str ()); 
    irsend.sendNEC(first, 32);  
  }
  delay(1000);
  justSentIR = true;
}
//#############################################################################################################

void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (client.connect("ESP8266Client", MQTT_USERNAME, MQTT_KEY)) {
      Serial.println("connected");
      client.subscribe(MQTT_SUB_TOPIC);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}
//#############################################################################################################

void loop() {

  if (!client.connected()) {
    reconnect();
  }

  client.loop();
  
  // this is our 'wait for incoming subscription packets' busy subloop
  // try to spend your time here

  if (irrecv.decode(&results) && !justSentIR) {
    if (results.overflow)
      Serial.printf("WARNING: IR code is too big for buffer (>= %d). "
                    "This result shouldn't be trusted until this is resolved. "
                    "Edit & increase CAPTURE_BUFFER_SIZE.\n",
                    CAPTURE_BUFFER_SIZE);

    IRcode = uint64ToString(results.value, HEX);
    if(IRcode != "FFFFFFFFFFFFFFFF"){
      IRcode = "{\"IRcode\": \"" + IRcode + "\"}";
      //Serial.println(resultToSourceCode(&results));
      
      Serial.print(F("\nSending IR val to MQTT broker: "));
      Serial.println(IRcode);
      client.publish(MQTT_PUB_TOPIC, IRcode.c_str());
    }
  }
  justSentIR = false;
}

//#############################################################################################################

uint16_t countValuesInStr(const String str, char sep) {
  int16_t index = -1;
  uint16_t count = 1;
  do {
    index = str.indexOf(sep, index + 1);
    count++;
  } while (index != -1);
  return count;
}

uint16_t * newCodeArray(const uint16_t size) {
  uint16_t *result;

  result = reinterpret_cast<uint16_t*>(malloc(size * sizeof(uint16_t)));
  // Check we malloc'ed successfully.
  if (result == NULL) {  // malloc failed, so give up.
    Serial.printf("\nCan't allocate %d bytes. (%d bytes free)\n",
                  size * sizeof(uint16_t), ESP.getFreeHeap());
    Serial.println("Giving up & forcing a reboot.");
    ESP.restart();  // Reboot.
    delay(500);  // Wait for the restart to happen.
    return result;  // Should never get here, but just in case.
  }
  return result;
}

void parseStringAndSendPronto(const String str, uint16_t repeats) {
  uint16_t count;
  uint16_t *code_array;
  int16_t index = -1;
  uint16_t start_from = 0;

  // Find out how many items there are in the string.
  count = countValuesInStr(str, ',');

  // Check if we have the optional embedded repeats value in the code string.
  if (str.startsWith("R") || str.startsWith("r")) {
    // Grab the first value from the string, as it is the nr. of repeats.
    index = str.indexOf(',', start_from);
    repeats = str.substring(start_from + 1, index).toInt();  // Skip the 'R'.
    start_from = index + 1;
    count--;  // We don't count the repeats value as part of the code array.
  }

  // We need at least PRONTO_MIN_LENGTH values for the code part.
  if (count < PRONTO_MIN_LENGTH) return;

  // Now we know how many there are, allocate the memory to store them all.
  code_array = newCodeArray(count);

  // Rest of the string are values for the code array.
  // Now convert the hex strings to integers and place them in code_array.
  count = 0;
  do {
    index = str.indexOf(',', start_from);
    // Convert the hexadecimal value string to an unsigned integer.
    code_array[count] = strtoul(str.substring(start_from, index).c_str(),
                                NULL, 16);
    start_from = index + 1;
    count++;
  } while (index != -1);

  irsend.sendPronto(code_array, count, repeats);  // All done. Send it.
  free(code_array);  // Free up the memory allocated.
}
