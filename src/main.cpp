#include <Arduino.h>
#include <M5Core2.h>
#include "ColoredWire.h"
#include "GameColor.h"
#include "Direction.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>

#define SERVICE_UUID "53b3448b-4e1e-4d97-bd07-9d11deca4188"
#define RULES_UUID "8af8cd3e-18b1-474d-9393-2b30dca181d1"
#define GAME_STATE_UUID "5ebdfc4f-92b2-4c43-8067-347eff415317"

bool isServer = true; // whether this device is the server or client in a multiplayer game
static bool gameOver = false; // flag to indicate whether the game is running

// client/server vars
bool deviceConnected = false;
static String BROADCAST_NAME = "KABOOM";

// server vars
BLEServer *bleServer;
BLEService *bleService;
BLECharacteristic *rulesCharacteristic;
BLECharacteristic *gameStateCharacteristic;

// client vars
static BLEUUID serviceUUID(SERVICE_UUID);
static BLEUUID rulesUUID(RULES_UUID);
static BLEUUID gameStateUUID(GAME_STATE_UUID);

static BLERemoteCharacteristic* rulesRemoteCharacteristic;
static BLERemoteCharacteristic* gameStateRemoteCharacteristic;
static BLEAdvertisedDevice* bleRemoteServer;

bool doConnect;

/////// lcd vars ////////

int screenWidth;
int screenHeight;

/////// game vars ////////

int timeLimitSeconds = 15; // time limit for the game in seconds
int timeLeft;
double startTime;

// holds the rules of the game in a string representation for display purposes.
const int LOCAL_RULES = 3;
static String rulesAsString[LOCAL_RULES];

// array to hold the wires in the game... we treat wire[0] as the left wire, wire[1] as the middle wire, and wire[2] as the right wire,
// to line up with the Direction enum values.
static ColoredWire wires[3];

// arrays to represent the rules of the game, indexed by their corresponding enum values.
// "should" arrays represent conditions under which a wire should be cut, and "shouldNot" arrays represent conditions under which a wire should not be cut.
// the "shouldNot" arrays override the "should" arrays, so if a condition is true in both, the wire should not be cut.
static bool shouldCutDirection[] = {
    false,   // if true cut left
    false,  // if true cut middle
    false    // if true cut right
}; 

static bool shouldNotCutDirection[] = {
    false,   // if true don't cut left
    false,  // if true don't cut middle
    false    // if true don't cut right
};

static bool shouldCutColor[] = {
    false,   // if true cut red
    false,  // if true cut green
    false    // if true cut blue
};

static bool shouldNotCutColor[] = {
    false,   // if true don't cut red
    false,  // if true don't cut green
    false    // if true don't cut blue
};

// func declarations
void generateRules(int rulesToGenerate);
String generateRuleAsString(int index, int ruleType);
void generateWires();
void printWires();
void drawGameScreen();
void drawSuccessScreen();
void winGame();
void commenceKaboom();
void drawKaboomScreen();
void determineGameState();
bool shouldCutWire(const ColoredWire& wire, Direction dir);
void broadcastBleServer();
bool connectToServer();
void updateRemoteRules(std::string rules);
void updateGameState(std::string gameState);

// BLE Server Callback Methods
class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *pServer) override {
        deviceConnected = true;
        Serial.println("Device connected...");
    }

    void onDisconnect(BLEServer *pServer) override {
        deviceConnected = false;
        Serial.println("Device disconnected...");
    }
};

class MyRulesCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    std::string rules = pCharacteristic->getValue();
    // updateRemoteRules(rules);
  }
};


class MyGameStateCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    std::string gameState = pCharacteristic->getValue();
    // updateGameState(gameState);
  }
};

// BLE Client callback methods
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks
{
    /**
     * Called for each advertising BLE server.
     */
    void onResult(BLEAdvertisedDevice advertisedDevice)
    {
        // Print device found
        Serial.printf("BLE Advertised Device found with name: %s\n", advertisedDevice.getName().c_str());

        // More debugging print
        Serial.printf("\tAddress: %s\n", advertisedDevice.getAddress().toString().c_str());
        Serial.printf("\tHas a ServiceUUID: %s\n", advertisedDevice.haveServiceUUID() ? "True" : "False");
        for (int i = 0; i < advertisedDevice.getServiceUUIDCount(); i++) {
           Serial.printf("\t\t%s\n", advertisedDevice.getServiceUUID(i).toString().c_str());
        }
        Serial.printf("\tHas our service: %s\n\n", advertisedDevice.isAdvertisingService(serviceUUID) ? "True" : "False");
        
        // We have found a device, let us now see if it contains the service we are looking for.
        if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(serviceUUID) && advertisedDevice.getName() == BROADCAST_NAME.c_str()) {
            BLEDevice::getScan()->stop();
            bleRemoteServer = new BLEAdvertisedDevice(advertisedDevice);
            doConnect = true;
        } 
    }     
};        

class MyClientCallback : public BLEClientCallbacks
{
    void onConnect(BLEClient *pclient)
    {
        deviceConnected = true;
        Serial.println("Client connected.");
    }

    void onDisconnect(BLEClient *pclient)
    {
        deviceConnected = false;
        Serial.println("Client disconnected.");
    }
};

static void rulesNotifyCallback(BLERemoteCharacteristic *pBLERemoteCharacteristic,uint8_t *pData, size_t length, bool isNotify)
{
  std::string rules((char*)pData, length);
  // updateRemoteRules(rules);
}

static void gameStateNotifyCallback(BLERemoteCharacteristic *pBLERemoteCharacteristic,uint8_t *pData, size_t length, bool isNotify)
{
  std::string gameState((char*)pData, length);
  // updateGameState(gameState);
}

void setup() {
  M5.begin();
  Serial.begin(115200);

  screenWidth = M5.Lcd.width();
  screenHeight = M5.Lcd.height();

  if (isServer) {
    BLEDevice::init(BROADCAST_NAME.c_str());
    broadcastBleServer();
  } else {
    BLEDevice::init("");
    BLEScan *pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pBLEScan->setInterval(1349);
    pBLEScan->setWindow(449);
    pBLEScan->setActiveScan(true);
    pBLEScan->start(5, false);

    if (doConnect)
    {
        if (connectToServer())
            Serial.println("We are now connected to the BLE Server.");
        else
            Serial.println("We have failed to connect to the server; there is nothin more we will do.");
        doConnect = false;
    }
  }

  // while (!deviceConnected) {
  //   delay(50);
  // };

  // generate random rules for the game
  generateRules(LOCAL_RULES);

  // generate randomly colored wires for the game
  generateWires();

  // print the rules to the console for testing purposes
  // for (int i = 0; i < RULES; i++) {
  //   Serial.printf("%d. %s\n", i + 1, rulesAsString[i].c_str());
  // }

  // printWires();

  timeLeft = timeLimitSeconds;
  drawGameScreen();

  startTime = millis(); // record the start time of the game for timing purposes
}

void loop() {
  M5.update();

  if (!gameOver) {
    // first check if screen was tapped to end the game, if so check if the player should win or lose depending on if they cut all the correct wires
    int timeElapsed = (millis() - startTime) / 1000; // calculate time elapsed in seconds

    int newTimeLeft = timeLimitSeconds - timeElapsed; // calculate time left
    if (newTimeLeft != timeLeft) { // update the screen if the time left has changed. (not doing every iteration to avoid unnecessary screen redraws)
      timeLeft = newTimeLeft;
      drawGameScreen();
    }

    if (timeLeft > 0) {
      bool btnAWasPressed = M5.BtnA.wasPressed(); // left wire cut 
      bool btnBWasPressed = M5.BtnB.wasPressed(); // middle wire cut
      bool btnCWasPressed = M5.BtnC.wasPressed(); // right wire cut

      if (btnAWasPressed || btnBWasPressed || btnCWasPressed) {
        Direction wireDirection;
        if (btnAWasPressed) {
          wireDirection = Direction::LEFT;
        } else if (btnBWasPressed) {
          wireDirection = Direction::MIDDLE;
        } else if (btnCWasPressed) {
          wireDirection = Direction::RIGHT;
        }

        // cut wire if not already cut
        ColoredWire& wire = wires[wireDirection];
        if (!wire.getIsCut()) {
          if (shouldCutWire(wire, wireDirection)) {
            wire.cut();
            drawGameScreen();
          } else { // game over if you cut the wrong wire
            commenceKaboom();
          }
        }
      } else if (M5.Touch.ispressed()) { // if the user taps the screen and didn't tap a button, check if they win or lose based on whether they cut the correct wires
        determineGameState();
      }
    } else {
      commenceKaboom(); // game over if time runs out
    }
  }

  delay(200);
}

// creates and broadcasts BLE server
void broadcastBleServer() {
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new MyServerCallbacks());
  bleService = bleServer->createService(SERVICE_UUID);
  // creating characteristics
  rulesCharacteristic = bleService->createCharacteristic(RULES_UUID,
                                                         BLECharacteristic::PROPERTY_READ |
                                                         BLECharacteristic::PROPERTY_WRITE |
                                                         BLECharacteristic::PROPERTY_NOTIFY |
                                                         BLECharacteristic::PROPERTY_INDICATE
  );
  rulesCharacteristic->setCallbacks(new MyRulesCallbacks());

  gameStateCharacteristic = bleService->createCharacteristic(GAME_STATE_UUID,
                                                             BLECharacteristic::PROPERTY_READ |
                                                             BLECharacteristic::PROPERTY_WRITE |
                                                             BLECharacteristic::PROPERTY_NOTIFY |
                                                             BLECharacteristic::PROPERTY_INDICATE
  );
  gameStateCharacteristic->setCallbacks(new MyGameStateCallbacks());

  // starting service after creating characteristics
  bleService->start();

   // Start broadcasting (advertising) BLE service
  BLEAdvertising *bleAdvertising = BLEDevice::getAdvertising();
  bleAdvertising->addServiceUUID(SERVICE_UUID);
  bleAdvertising->setScanResponse(true);
  bleAdvertising->setMinPreferred(0x12); // Use this value most of the time
  // bleAdvertising->setMinPreferred(0x06); // Functions that help w/ iPhone connection issues
  // bleAdvertising->setMinPreferred(0x00); // Set value to 0x00 to not advertise this parameter
  BLEDevice::startAdvertising();
  Serial.println("Characteristic defined...you can connect with your BOMB! I mean... your other M5.");
}

// connects to ble server
bool connectToServer()
{
    // Create the client
    Serial.printf("Forming a connection to %s\n", bleRemoteServer->getAddress().toString().c_str());
    BLEClient *bleClient = BLEDevice::createClient();
    bleClient->setClientCallbacks(new MyClientCallback());

    // Connect to the remove BLE Server. 
    if (!bleClient->connect(bleRemoteServer)) {
        Serial.printf("the heinz big bean has escaped. may god have mercy on our souls (failed to connect to server %s)\n", 
            bleRemoteServer->getName().c_str());
    }
    Serial.printf("Connected to server %s\n", bleRemoteServer->getName().c_str());

    // Obtain a reference to the service we are after in the remote BLE server.
    BLERemoteService *bleRemoteService = bleClient->getService(serviceUUID);
    if (bleRemoteService == nullptr) {
        Serial.printf("the heinz big bean has ascended to the heavens. they have left us (failed to find our service UUID: %s)\n", serviceUUID.toString().c_str()); 
        bleClient->disconnect();
        return false;
    }
    Serial.printf("\tFound our service UUID: %s\n", serviceUUID.toString().c_str());

    // Obtain references to the characteristics in the service of the remote BLE server.
    rulesRemoteCharacteristic = bleRemoteService->getCharacteristic(rulesUUID);
    if (rulesRemoteCharacteristic == nullptr) {
        Serial.printf("the heinz big bean has transformed into an unrecognizable abomination (failed to find our characteristic UUID: %s)\n", rulesUUID.toString().c_str());
        bleClient->disconnect();
        return false;
    }

    gameStateRemoteCharacteristic = bleRemoteService->getCharacteristic(gameStateUUID);
    if (gameStateRemoteCharacteristic == nullptr) {
        Serial.printf("the heinz big bean has transformed into an unrecognizable abomination (failed to find our characteristic UUID: %s)\n", gameStateUUID.toString().c_str());
        bleClient->disconnect();
        return false;
    }

    Serial.printf("\tFound our characteristic UUID: %s\n", rulesUUID.toString().c_str());
    Serial.printf("\tFound our characteristic UUID: %s\n", gameStateUUID.toString().c_str());

     // Read the value of the characteristic.
    // if (bleRemoteCharacteristicServerPos->canRead()) {
    //     std::string value = bleRemoteCharacteristicServerPos->readValue();
    //     Serial.printf("The characteristic value was: %s\n", value.c_str());
    //     updateGameScreen();
    // }

    // Check if server's characteristics can notify client of changes and register to listen if so
    if (rulesRemoteCharacteristic->canNotify())
        rulesRemoteCharacteristic->registerForNotify(rulesNotifyCallback);

    if (gameStateRemoteCharacteristic->canNotify())
        rulesRemoteCharacteristic->registerForNotify(gameStateNotifyCallback);

    deviceConnected = true;
    return deviceConnected;
}

// randomly generates the rules for the game by filling the rule arrays with true values
void generateRules(int rulesToGenerate) {
  // struct for array and size because you can't get the size of an array from a pointer (which the arrays in ruleArrays are).
  struct RuleArray {
    bool* array;
    int size;
  };
  
  static RuleArray ruleArrays[] = {
    {shouldCutColor, sizeof(shouldCutColor) / sizeof(shouldCutColor[0])},
    {shouldNotCutColor, sizeof(shouldNotCutColor) / sizeof(shouldNotCutColor[0])},
    {shouldCutDirection, sizeof(shouldCutDirection) / sizeof(shouldCutDirection[0])},
    {shouldNotCutDirection, sizeof(shouldNotCutDirection) / sizeof(shouldNotCutDirection[0])}
  };

  int numRuleArrays = sizeof(ruleArrays) / sizeof(ruleArrays[0]);

  int8_t rulesSoFar = 0;

  while (rulesSoFar < rulesToGenerate) {
    // on each iteration we randomly select one of the rule arrays, and then randomly select an index within that array
    int arrayIndex = random(0, numRuleArrays);
    RuleArray selectedArray = ruleArrays[arrayIndex];
    int selectedArraySize = selectedArray.size;

    int indexWithinSelectedArray = random(0, selectedArraySize);

    // if the selected rule is not already set to true, set it to true, make a string representation of the rule, and increment the rulesSoFar counter
    if (!selectedArray.array[indexWithinSelectedArray]) {
      selectedArray.array[indexWithinSelectedArray] = true;
      rulesAsString[rulesSoFar] = generateRuleAsString(indexWithinSelectedArray, arrayIndex);
      rulesSoFar++;
    }
  }
}

// generates a string representation of a rule 
String generateRuleAsString(int index, int ruleType) {
  // defining these arrays within the function to avoid polluting the global namespace, we don't need them anywhere else.
  static const String COLOR_TO_STRING[] = {
      "red",
      "green",
      "blue"
  };

  static const String DIRECTION_TO_STRING[] = {
      "left",
      "middle",
      "right"
  };

  String ruleString;

  switch (ruleType) {
    case 0: // should cut color
      ruleString = "Cut all " + COLOR_TO_STRING[index] + " wires";
      break;
    case 1: // should not cut color
      ruleString = "Don't cut " + COLOR_TO_STRING[index] + " wires";
      break;
    case 2: // should cut direction
      ruleString = "Cut all " + DIRECTION_TO_STRING[index] + " wires";
      break;
    case 3: // should not cut direction
      ruleString = "Don't cut " + DIRECTION_TO_STRING[index] + " wires";
      break;
  }

  return ruleString;
}

// generates random wires for the game by filling the wires array with randomly colored wires.
void generateWires() {
  for (int i = 0; i < 3; i++) {
    wires[i] = ColoredWire(static_cast<GameColor>(random(0, 3)));
  }
}

// draws the game screen on the LCD, showing the rules and the wires with their colors and cut status
void drawGameScreen() {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(WHITE);

  int tapWhenDoneXOffset = 10;
  int tapWhenDoneYOffset = 40;

  // print message at top informing user to tap the screen when done cutting wires
  M5.Lcd.setCursor(tapWhenDoneXOffset, tapWhenDoneYOffset);
  M5.Lcd.print("Tap screen to end game");


  M5.Lcd.setTextColor(timeLeft > 5 ? GREEN : RED); // red if 5 or less seconds left, green otherwise
  int timeLeftXOffset = screenWidth - 30;
  int timeLeftYOffset = 20;

  // print the time left in the top right corner of the screen
  M5.Lcd.setCursor(timeLeftXOffset, timeLeftYOffset);
  M5.Lcd.printf("%d", timeLeft);

  int ruleXOffset = 10;
  int ruleYOffset = 80;
  int ruleInBetweenSpacing = 30;

  M5.Lcd.setCursor(ruleXOffset, ruleYOffset);
  M5.Lcd.setTextColor(YELLOW);
  M5.Lcd.setTextSize(2);

  // print the rules
  for (int i = 0; i < LOCAL_RULES; i++) {
    M5.Lcd.print(rulesAsString[i]);
    M5.Lcd.setCursor(ruleXOffset, ruleYOffset + (i + 1) * ruleInBetweenSpacing); // move cursor down for next rule
  }

  static const int WIRE_X_OFFSET = 50;
  static const int WIRE_Y_OFFSET = 30;
  static const int WIRE_SPACING = 100;

  // converts GameColor enum values to their corresponding TFT color values for setting the text color when printing the wires on the LCD.
  static const uint16_t GAME_COLOR_TO_TFT_COLOR[] = {
    RED, 
    GREEN, 
    BLUE
  };

  M5.Lcd.setCursor(WIRE_X_OFFSET, screenHeight - WIRE_Y_OFFSET); 

  // print the wire char representations at the bottom of the screen above the buttons, in its corresponding color
  for (int i = 0; i < 3; i++) {
    ColoredWire &wire = wires[i];
    M5.Lcd.setTextColor(GAME_COLOR_TO_TFT_COLOR[wire.getColor()]);
    M5.Lcd.print(wire.getCharRepresentation());
    M5.Lcd.setCursor(WIRE_X_OFFSET + (i + 1) * WIRE_SPACING, screenHeight - WIRE_Y_OFFSET); // move cursor to the right for the next wire
  }
}

// draws game over screen and sets gameOver to true to prevent further input processing
void commenceKaboom() {
  drawKaboomScreen();
  gameOver = true;
}

// draws success screen and sets gameOver to true to prevent further input processing
void winGame() {
  drawSuccessScreen();
  gameOver = true;
}

// draws the success screen, with a green background and "DEFUSED" in big black letters in the middle of the screen
void drawSuccessScreen() {
  M5.Lcd.fillScreen(GREEN);
  M5.Lcd.setTextSize(5);
  M5.Lcd.setTextColor(BLACK);
  M5.Lcd.drawString("DEFUSED", 60, 100);
}

// draws a glorious kaboom screen that fills the entire LCD with red and "KABOOM" in big black letters 
void drawKaboomScreen() {
  int xOffset = 0;
  int yOffset = 0;

  M5.Lcd.fillScreen(RED);
  M5.Lcd.setCursor(xOffset, yOffset);
  M5.Lcd.setTextSize(5);
  M5.Lcd.setTextColor(BLACK);
  M5.Lcd.print("KABOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOM"); 
}

// print the wires
void printWires() {
  for (int i = 0; i < 3; i++) {
    ColoredWire wire = wires[i];
    Serial.printf("Wire %d: %c.\n", 
                  i, 
                  wire.getCharRepresentation());
  }
}

// kabooms if player failed to cut a wire they should have cut, wins the game if they cut all the wires they should have cut
void determineGameState() {
  for (int i = 0; i < 3; i++) { 
    ColoredWire &wire = wires[i];
    Direction dir = static_cast<Direction>(i);
    if (shouldCutWire(wire, dir) && !wire.getIsCut()) {
      commenceKaboom(); // if the player failed to cut a wire they should have cut, kaboom
      return;
    }
  }

  winGame(); // if we get to the end of the loop and haven't kaboomed, the player wins!
}

// this function determines whether a wire should be cut based on its color and direction, using the rules defined in the arrays.
bool shouldCutWire(const ColoredWire& wire, Direction dir) {
  GameColor color = wire.getColor();

  bool shouldCuts = shouldCutColor[color] || shouldCutDirection[dir];
  bool shouldntCuts = shouldNotCutColor[color] || shouldNotCutDirection[dir];

  return (shouldCuts && !shouldntCuts); // true if the wire s
}
