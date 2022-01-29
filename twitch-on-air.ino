/*
 * On-Air using the twitch API OAuth client credentials flow to light on-air signage when the user stream is live
 *    
 *    Author: Riley Kenyon
 */

#include <ESP8266WiFi.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>

#define DEBUG                     // Additional debug info
#define INVALID           (-3)
#define VALID             (0)

#define HTTP_TIMEOUT      (5000)  // milliseconds
#define POLLING_INTERVAL  (10000) // milliseconds
#define PIXEL_DELAY       (50)    // milliseconds
#define PIXEL_ERR_DELAY   (500)   // milliseconds
#define INTENSITY         (255)

#define START_ADDR        (0x00)  // EEPROM start address
#define TWITCH_TOKEN_LEN  (30)    // characters

#define LED_PIN           (12)
#define LED_COUNT         (52)

// WiFi credentials
const char* ssid      = "#########";    // FILL THIS IN
const char* password  = "#########";    // FILL THIS IN

// Authorization tokens
const char* accessToken;
char tempAccessToken[TWITCH_TOKEN_LEN + 1];
unsigned long accessTokenExpiration;

// Application credentials https://dev.twitch.tv/console/apps/create
const char* clientId      = "##############################";         // FILL THIS IN
const char* clientSecret  = "##############################";         // FILL THIS IN

// URL for the Twitch API and Oauth token authorization and validation
//https://dev.twitch.tv/docs/authentication/getting-tokens-oauth/#oauth-client-credentials-flow
const char* apiHost       = "api.twitch.tv";
const char* host          = "id.twitch.tv";
const char* twitchURL     = "/helix/streams?user_login=";
const char* authURL       = "/oauth2/token";
const char* validateURL   = "/oauth2/validate";
const char* username      = "###########";                            // FILL THIS IN

// Construct the JSON buffer for access keys
// TODO: Figure out a more robust way to allocate the size of the JSON objects
const int capacity = JSON_OBJECT_SIZE(10); // Originally was 6, ran into NoMemory errors when deserializing
StaticJsonDocument<capacity> authResponse;
StaticJsonDocument<capacity> validationResponse;

// Construct JSON buffer for api response
StaticJsonDocument<JSON_OBJECT_SIZE(60)> apiResponse;
StaticJsonDocument<JSON_OBJECT_SIZE(60)> dataJson;

// Instantiate WiFi Client
WiFiClientSecure client;
const int httpsPort = 443;

// Declare NeoPixel strip object
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// --------------------------------------------------------------------------------
// Setup
// --------------------------------------------------------------------------------
void setup() {
  // Setup serial communication
  Serial.begin(9600);
  delay(100);

  // Setup EEPROM
  EEPROM.begin(TWITCH_TOKEN_LEN);

  // Start Neopixel
  strip.begin();              // Initialize
  strip.show();               // Turns OFF all pixels ASAP
  strip.setBrightness(100);   // Max 255

  // Connect to Wifi Network
  WiFi.begin(ssid, password);
  while (WL_CONNECTED != WiFi.status())
  {
    // Set waiting pattern
    unsigned long currTime = millis();
    while ( millis() - currTime < 500)
    {
      set_waiting_led_pattern();
    }
    Serial.print(".");
  }
  Serial.println();
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
  delay(500);

  // TODO: Add in cipher suite for authentication
  // ECDHE-RSA-AES128-GCM-SHA256  
  client.setInsecure();

  if (!client.connect(host, httpsPort))
  {
    Serial.println("connection failed");
    return;
  }

  delay(500);

  // Read from EEPROM to see if there is an access token
  // https://forum.arduino.cc/t/esp12e-save-data-into-flash-memory/472136
  // https://github.com/esp8266/Arduino/blob/master/doc/libraries.rst#eeprom  
  for (int idx = START_ADDR; idx < TWITCH_TOKEN_LEN + START_ADDR; idx++)
  {
    char tokenChar = (char)EEPROM.read(idx);
    tempAccessToken[idx] = tokenChar;
  }
  accessToken = tempAccessToken;
  
  // Check if EEPROM token is valid
  get_valid_oauth_access_token();

  Serial.print("Token: ");
  Serial.println(String(accessToken));
  Serial.print("Expiration: ");
  Serial.println(accessTokenExpiration);
  Serial.println();
}

int value = 0;


// --------------------------------------------------------------------------------
// Main loop
// --------------------------------------------------------------------------------
void loop() {
  delay(500);
  ++value;
  // Connect to Twitch API endpoint
  if(!client.connect(apiHost, httpsPort))
  {
    Serial.println("connection failed");
    return;
  }
  
  // Get user data
  int retval = send_and_receive_user_data();
  // Error handling
  if (0 > retval)
  {
    set_error_led_pattern();
    return;
  }

  // Check if online status changed 
  static boolean wasLive = false;
  void (*patternPtr) (void) = set_waiting_led_pattern;
  boolean userOnline = get_user_online_status();

  #ifdef DEBUG
    Serial.print("User was live: ");
    Serial.println(wasLive);
    Serial.print("User is currently online: ");
    Serial.println(userOnline);
  #endif
  
  if (false == wasLive && true == userOnline)
  {
    // Startup led patttern - user has just come online
    wasLive = true;
    patternPtr = &set_startup_led_pattern;
  }
  else if (true == userOnline)
  {
    // Continue online pattern
    patternPtr = &set_active_led_pattern;
  }
  else
  {
    // user offline or was online and went offline
    patternPtr = &set_offline_led_pattern;
  }

  // Display led pattern for polling interval
  unsigned long currTime = millis();
  while (millis() - currTime < POLLING_INTERVAL)
  {
    // Display the active led pattern
    patternPtr();
  }
  Serial.println();
}

// --------------------------------------------------------------------------------
// Helper functions
// --------------------------------------------------------------------------------
String get_data_request_header()
{
  // Header using OAuth tokens
  return (String("GET ") + twitchURL + username + " HTTP/1.1\r\n" +
          "Host: " + apiHost + "\r\n" +
          "Client-ID: " + clientId + "\r\n" +
          "Authorization: Bearer " + accessToken + "\r\n");
}

String get_auth_request_body()
{
  return (String("client_id=") + clientId + 
          "&client_secret=" + clientSecret + 
          "&grant_type=client_credentials");
}

String get_auth_request_header(unsigned int len)
{
  return (String("POST ") + authURL + " HTTP/1.1\r\n" +
          "Host: " + host + "\r\n" +
          "Content-Type: application/x-www-form-urlencoded\r\n" +
          "Content-Length: " + len + "\r\n"); 
}

String get_auth_validation_header()
{
  return(String("GET ") + validateURL + " HTTP/1.1\r\n" +
         "Host: " + host + "\r\n" +
         "Authorization: Bearer " + accessToken + "\r\n");
}

String get_server_raw_response()
{
  // Read all the lines of the reply from server
  long startTime = millis();
  String line = "";
  while (millis() - startTime < HTTP_TIMEOUT)
  {
    while(client.available())
    {
      String tempLine = client.readStringUntil('\r');

      // Post process line - looking for json payload with access token
      tempLine.trim();
      if (tempLine.startsWith("{"))
      { 
        line = tempLine;
        return line;
      }
    }
  }
  return line;
}

int send_and_receive_user_data()
{
  // Send request for online/offline
  Serial.println("Sending request for online status ...");
  String dataRequestHeader = get_data_request_header();
  client.println(dataRequestHeader);

  // Wait for server response
  Serial.println("Waiting for response...");
  String line = get_server_raw_response();
  
  #ifdef DEBUG
    Serial.print("Raw response: ");
    Serial.println(line);
  #endif
  
  // deserialize the JSON data
  Serial.println("Parsing Data...");
  DeserializationError err = deserializeJson(apiResponse,line.c_str());
  if (err)
  {
    Serial.println(err.f_str());
    return -1;
  }
   return 0;
}

boolean get_user_online_status()
{
  boolean online = false;
  const char* userLiveStatus = "offline";

  // Convert data array to JSON
  String data = String(apiResponse["data"]);
  String substringData = data.substring(1, data.length() - 1);  // Remove brackets
  if (substringData == NULL)
  {
    // User may or may not exist - offline and not exist return same response
    online = false;
  }
  else
  {
    // User has data - make sure the user is online
    DeserializationError err2 = deserializeJson(dataJson, substringData.c_str());
    if (err2) {
      Serial.println(err2.f_str());
      return -2;
    }
    
    // Get online status
    const char* liveStr = "live";
    const char* userLiveStatus = dataJson["type"];
    if (userLiveStatus != NULL) {
      online = (0 == strcmp(userLiveStatus, liveStr));  
    }
  }
  #ifdef DEBUG
    Serial.print("Status: ");
    Serial.println(String(userLiveStatus));
  #endif
  return online;
}

// --------------------------------------------------------------------------------
// OAuth handling functions
// --------------------------------------------------------------------------------
int get_oauth_access_token()
{
  int statusCode;
  if (client.connected())
  {
    // Send the OAuth token request 
    Serial.println("Sending OAuth token request ...");
    send_oauth_request();

    delay(1000);

    // Get OAuth Response
    Serial.println("Waiting for response...");
    String authDataRaw = get_server_raw_response();

    // Decode into JSON structure
    DeserializationError err = deserializeJson(authResponse,authDataRaw.c_str());

    // Error handling
    if (err)
    {
      Serial.println(err.f_str());
      statusCode = -2;
    }
    else
    {
      // Assign access token information
      accessToken = authResponse["access_token"].as<const char*>();
      accessTokenExpiration = authResponse["expires_in"].as<unsigned long>();
      statusCode = 0;
          
      #ifdef DEBUG
          Serial.print("Authorization raw: ");
          Serial.println(authDataRaw);
          Serial.println(authDataRaw.c_str());
          Serial.print("Authorization token: ");
          Serial.println(accessToken);
          Serial.print("Authroization Expiration: ");
          Serial.println(accessTokenExpiration);
          Serial.println();
      #endif
    }
  } 
  else 
  {
    // error, client is not connected
    statusCode = -1;
  }
  return statusCode;
}

int get_oauth_validation()
{
  // Check if the current oauth token is still valid
  int statusCode;
  if (client.connected())
  {
    // Send the request for validation
    Serial.println("Sending OAuth validation request... ");
    send_oauth_validation();
  
    delay(1000);
  
    // Get Validation response, check for "expires_in" field
    Serial.println("Waiting for response...");
    String validationDataRaw = get_server_raw_response();
  
    // Decode into JSON
    DeserializationError err = deserializeJson(validationResponse, validationDataRaw.c_str());
  
    // Error handling
    if (err)
    {
      Serial.println(err.f_str());
      statusCode = -2;
    } 
    else
    {
      // Check for the "expires_in" field
      accessTokenExpiration = validationResponse["expires_in"].as<unsigned long>();
      if (0 < accessTokenExpiration)
      {
        statusCode = 0;
      }
      else
      {
        // Could also check if "status" field is 401 - invalid access token
        statusCode = -3;
      }
      
      #ifdef DEBUG
        Serial.print("Time remaining: ");
        Serial.println(accessTokenExpiration);
        Serial.print("Full response: ");
        Serial.println(validationDataRaw.c_str());
      #endif
    }
  }
  else
  {
    // error, client is not connected
    statusCode = -1;
  }
  return statusCode;
}

int get_valid_oauth_access_token()
{
  int statusCode;

  // Check existing access token
  statusCode = get_oauth_validation();
  if (INVALID == statusCode)
  {
    // Access code is invalid - request another one
    statusCode = get_oauth_access_token();

    if (0 > statusCode)
    {
      // Error - do not write to EEPROM
      set_error_led_pattern();
    }
    else
    {
      // Write to 'EEPROM' - on ESP8266 this is Flash
      Serial.println("Writing new token to EEPROM...");
      for (int idx = START_ADDR; idx < TWITCH_TOKEN_LEN + START_ADDR; idx++)
      {
        byte value = (byte)accessToken[idx]; // Could also use unsigned char or uint8_t
        EEPROM.write(idx, value);
      }
      
      // Save changes
      if (EEPROM.commit())
      {
        Serial.println("EEPROM successfully commited");
      }
      else
      {
        Serial.println("ERROR! EEPROM commit failed");
      }
    }
  }
  return statusCode;
}

void send_oauth_request()
{
    String authRequestBody = get_auth_request_body();
    String authRequestHeader = get_auth_request_header(authRequestBody.length());
    client.println(authRequestHeader);
    client.println(authRequestBody);
    return;
}

void send_oauth_validation()
{
  String authValidHeader = get_auth_validation_header();
  client.println(authValidHeader);
  return;
}

// --------------------------------------------------------------------------------
// Neopixel animation functions
// --------------------------------------------------------------------------------
// Theater-marquee-style chasing lights. Pass in a color (32-bit value,
// a la strip.Color(r,g,b) as mentioned above), and a delay time (in ms)
// between frames.
void theaterChase(uint32_t color, int wait) {
  for(int a=0; a<10; a++) {  // Repeat 10 times...
    for(int b=0; b<3; b++) { //  'b' counts from 0 to 2...
      strip.clear();         //   Set all pixels in RAM to 0 (off)
      // 'c' counts up from 'b' to end of strip in steps of 3...
      for(int c=b; c<strip.numPixels(); c += 3) {
        strip.setPixelColor(c, color); // Set pixel 'c' to value 'color'
      }
      strip.show(); // Update strip with new contents
      delay(wait);  // Pause for a moment
    }
  }
}

// Rainbow-enhanced theater marquee. Pass delay time (in ms) between frames.
void theaterChaseRainbow(int wait) {
  int firstPixelHue = 0;     // First pixel starts at red (hue 0)
  for(int a=0; a<30; a++) {  // Repeat 30 times...
    for(int b=0; b<3; b++) { //  'b' counts from 0 to 2...
      strip.clear();         //   Set all pixels in RAM to 0 (off)
      // 'c' counts up from 'b' to end of strip in increments of 3...
      for(int c=b; c<strip.numPixels(); c += 3) {
        // hue of pixel 'c' is offset by an amount to make one full
        // revolution of the color wheel (range 65536) along the length
        // of the strip (strip.numPixels() steps):
        int      hue   = firstPixelHue + c * 65536L / strip.numPixels();
        uint32_t color = strip.gamma32(strip.ColorHSV(hue)); // hue -> RGB
        strip.setPixelColor(c, color); // Set pixel 'c' to value 'color'
      }
      strip.show();                // Update strip with new contents
      delay(wait);                 // Pause for a moment
      firstPixelHue += 65536 / 90; // One cycle of color wheel over 90 frames
    }
  }
}

// --------------------------------------------------------------------------------
// Neopixel status functions
// --------------------------------------------------------------------------------
void set_waiting_led_pattern()
{
  theaterChase(strip.Color(INTENSITY,INTENSITY,0),PIXEL_DELAY);
}

void set_startup_led_pattern()
{
  theaterChase(strip.Color(0,INTENSITY,0),PIXEL_DELAY);
}

void set_error_led_pattern()
{
  for (int idx = 0; idx < 5; idx++)
  {
    strip.clear();
    strip.show();
    delay(PIXEL_ERR_DELAY);
    uint32_t color = strip.Color(INTENSITY,0,0);
    for (int pixId = 0; pixId < strip.numPixels(); pixId++)
    {
      strip.setPixelColor(pixId, color);
    }
    strip.show(); // Update strip with all red LEDs
    delay(PIXEL_ERR_DELAY);
  }
}

void set_active_led_pattern()
{
  // Solid green
  strip.clear();
  uint32_t color = strip.Color(INTENSITY,0,INTENSITY);
  for (int pixId = 0; pixId < strip.numPixels(); pixId++)
  {
    strip.setPixelColor(pixId, color);
  }
  strip.show(); // Update strip with all green LEDs
  delay(PIXEL_DELAY);
}

void set_offline_led_pattern()
{
  // Solid red
  strip.clear();
  uint32_t color = strip.Color(INTENSITY,0,0);
  for (int pixId = 0; pixId < strip.numPixels(); pixId++)
  {
    strip.setPixelColor(pixId, color);
  }
  strip.show(); // Update strip with all red LEDs
  delay(PIXEL_DELAY);
}
