#include <Arduino.h>
#include <M5Core2.h>
#include "ColoredWire.h"
#include "GameColor.h"
#include "Direction.h"
#include "GameStateSignal.h"
#include "Key.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include "Adafruit_seesaw.h"

#define SERVICE_UUID "53b3448b-4e1e-4d97-bd07-9d11deca4188"
#define RULES_UUID "8af8cd3e-18b1-474d-9393-2b30dca181d1"
#define GAME_STATE_UUID "5ebdfc4f-92b2-4c43-8067-347eff415317"

#define BUTTON_B         1
#define BUTTON_SELECT    0
#define BUTTON_START    16
uint32_t buttonMask = (1UL << BUTTON_START) | (1UL << BUTTON_B) | (1UL << BUTTON_SELECT);

String userId = "default";

bool isServer = true; // whether this device is the server or client in a multiplayer game
bool gotRemoteRules = false; // whether this device has received rules from the other core to copy onto itself
static bool doKaboom = false; // flag for game loss
static bool doCheck = false; // flag to check if local wire cuts violate the rules or not
static bool doWin = false; // flag for game win
static bool otherPlayerChecked = false; // flag that is set to true when other player's game state is checked
static bool otherPlayerGood = false; // flag for if other player's cuts didn't violate rules, after checking.
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

int timeLimitSeconds = 30; // time limit for the game in seconds
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

// struct for array and size because you can't get the size of an array from a pointer (which the arrays in ruleArrays are).
struct RuleArray {
  bool* array;
  int size;
};

// used to make it easier to generate and share rules.
static RuleArray ruleArrays[] = {
  {shouldCutColor, sizeof(shouldCutColor) / sizeof(shouldCutColor[0])},
  {shouldNotCutColor, sizeof(shouldNotCutColor) / sizeof(shouldNotCutColor[0])},
  {shouldCutDirection, sizeof(shouldCutDirection) / sizeof(shouldCutDirection[0])},
  {shouldNotCutDirection, sizeof(shouldNotCutDirection) / sizeof(shouldNotCutDirection[0])}
};

int numRuleArrays = sizeof(ruleArrays) / sizeof(ruleArrays[0]);

// func declarations
String inputUserId();
bool isPressed(uint32_t buttonRead, uint8_t button);
void drawKeyboardScreen(Key (&keyboard)[3][10], int selectedKeyRow, int selectedKeyCol, int keyW, int keyH, char userIdArr[]);
void generateRules(int rulesToGenerate);
String generateRuleAsString(int index, int ruleType);
std::string generateRulesAsBits();
void sendRules(std::string rules);
void sendGameStateSignal(GameStateSignal sig);
void generateWires();
void printWires();
void drawGameScreen();
void drawSuccessScreen();
void winGame();
void commenceKaboom();
void drawKaboomScreen();
bool determineGameState();
bool shouldCutWire(const ColoredWire& wire, Direction dir);
void broadcastBleServer();
bool connectToServer();
void updateRemoteRules(std::string rules);
void updateGameState(uint8_t gameState);

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
    updateRemoteRules(rules);
  }
};


class MyGameStateCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    uint8_t* gameState = pCharacteristic->getData();
    updateGameState(*gameState);
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
  updateRemoteRules(rules);
}

static void gameStateNotifyCallback(BLERemoteCharacteristic *pBLERemoteCharacteristic,uint8_t *pData, size_t length, bool isNotify)
{
  updateGameState(*pData);
}

void setup() {
  M5.begin();
  Serial.begin(115200);

  screenWidth = M5.Lcd.width();
  screenHeight = M5.Lcd.height();

  userId = inputUserId();

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

  // generate random rules for the game. these rules are locally displayed on the m5.
  generateRules(LOCAL_RULES);

  std::string rulesAsBits = generateRulesAsBits();
  Serial.printf("Local rules as bits: %s\n", rulesAsBits.c_str());

  // wait for another device to connect
  while (!deviceConnected) {
    delay(50);
  };

  if (isServer) {
    delay(500);
  } else {
    delay(1000);
  }

  // when both devices are connected, they exchange the random rules they generated. 
  // Only the locally generated rules are visible on each m5, but all rules are active across both devices.
  sendRules(rulesAsBits);

  // wait for both devices to acknowledge processing of other device's rules.
  while (!gotRemoteRules) {
    delay(50);
  }

  // debug
  rulesAsBits = generateRulesAsBits();
  Serial.printf("Global rules as bits: %s\n", rulesAsBits.c_str());

  // generate randomly colored wires for the game
  generateWires();

  timeLeft = timeLimitSeconds;
  drawGameScreen();

  startTime = millis(); // record the start time of the game for timing purposes
}

void loop() {
  M5.update();

  if (!gameOver) {
    if (doKaboom) {
      commenceKaboom();
    } else if (doWin) {
      winGame();
    } else if (doCheck) {
      // check if local wires were cut correctly according to local rules
      bool gameState = determineGameState();

      // if they were, tell other player to check.
      if (gameState) {
        sendGameStateSignal(GameStateSignal::GOOD);

        // wait until other player sends signal back.
        while (!otherPlayerChecked) {
          delay(10);
        }; 

        // if it was a good signal then set doWin to true as we already know we're good. otherwise it was a bad signal so we will both explode on next loop.
        if (otherPlayerGood) {
          doWin = true;
        }
      } else {
        // kaboom if we didn't cut a wire that we were supposed to.
        doKaboom = true;
        sendGameStateSignal(GameStateSignal::DO_KABOOM);
      }
    } else {
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
              doKaboom = true;
              sendGameStateSignal(GameStateSignal::DO_KABOOM);
            }
          }
          // if the user taps the screen and didn't tap a button, check if they win or lose based on whether they cut the correct wires
          // we only check if the press point wasn't near the very bottom of the screen, as in that case we assume it was accidental from pressing the buttons.
        } else if (M5.Touch.ispressed()) { 
          Point touchCoord = M5.Touch.getPressPoint();
          if (touchCoord.y < screenHeight - 50) {
            doCheck = true;
          }
        }
      } else {
        doKaboom = true; // game over if time runs out
      }
    }
  }

  delay(50);
}

// creates a keyboard on the m5core2 screen, which the user can operate with seesaw to input their userId.
String inputUserId() {
  // init seesaw
  Adafruit_seesaw ss;

  if (!ss.begin(0x50)) {
      Serial.println("Seesaw not found");
      while (1); // infinite loop
  }

  // settings for seesaw
  ss.pinModeBulk(buttonMask, INPUT_PULLUP); 
  ss.setGPIOInterrupts(buttonMask, true);

  // keyboard spec
  int keyW = 30;
  int keyH = 30;
  int keyStartX = 10;
  int keyStartY = screenHeight / 2;

  char layout[3][10] = {
    {'q','w','e','r','t','y','u','i','o','p'},
    {'a','s','d','f','g','h','j','k','l','\0'},
    {'z','x','c','v','b','n','m','\0','\0','\0'}
  };

  Key keyboard[3][10];
  int selectedKeyRow = 0;
  int selectedKeyCol = 0;

  // initializes keyboard array
  for (int row = 0; row < 3; row++) {
    for (int col = 0; col < 10; col++) {
      // if (layout[row][col] != '\0') {
        keyboard[row][col].ch = layout[row][col];
        keyboard[row][col].x = keyStartX + col * keyW + row * 5; // indent per row
        keyboard[row][col].y = keyStartY + row * (keyH + 5); // spacing between rows
      // }
    }
  }

  // default values
  uint32_t buttonRead = buttonMask;
  bool bPressed = false;
  bool startPressed = false;
  bool selectPressed = false;
  bool leftKeyExists = false;
  bool rightKeyExists = false;
  bool aboveKeyExists = false;
  bool belowKeyExists = false;
  int analogXRead = 1023 / 2;
  int analogYRead = 1023 / 2;

  char userIdArr[16] = {0}; // full of '\0' initially. userIdArr[15] will always be '\0'
  int userIdArrPos = 0;

  bool doDrawKeyboardScreen = true;

  do {
    M5.update();

    // backspace
    if (bPressed && userIdArrPos > 0) {
      userIdArr[--userIdArrPos] = '\0';
      doDrawKeyboardScreen = true;
    }

    // like pressing the key you have selected.
    if (selectPressed && userIdArrPos < 15) {
      userIdArr[userIdArrPos++] = keyboard[selectedKeyRow][selectedKeyCol].ch;
      userIdArr[userIdArrPos] = '\0'; // just for safety. not necessary if our logic is right
      doDrawKeyboardScreen = true;
    }
    
    if (rightKeyExists && analogXRead > 700) { // right input
      selectedKeyCol++;
      doDrawKeyboardScreen = true;
    } else if (leftKeyExists && analogXRead < 300) { // left input
      selectedKeyCol--;
      doDrawKeyboardScreen = true;
    } else if (aboveKeyExists && analogYRead > 700) { // up input
      selectedKeyRow--;
      doDrawKeyboardScreen = true;
    } else if (belowKeyExists && analogYRead < 300) { // down input
      selectedKeyRow++;
      doDrawKeyboardScreen = true;
    }

    if (doDrawKeyboardScreen) {
      drawKeyboardScreen(keyboard, selectedKeyRow, selectedKeyCol, keyW, keyH, userIdArr);
    }

    buttonRead = ss.digitalReadBulk(buttonMask);
    bPressed = isPressed(buttonRead, BUTTON_B);
    startPressed = isPressed(buttonRead, BUTTON_START);
    selectPressed = isPressed(buttonRead, BUTTON_SELECT);
    analogXRead = 1023 - ss.analogRead(14);
    analogYRead = 1023 - ss.analogRead(15);
    
    char rightKey = selectedKeyCol < 9 ? keyboard[selectedKeyRow][selectedKeyCol + 1].ch : '\0';
    rightKeyExists = rightKey != '\0';

    char leftKey = selectedKeyCol > 0 ? keyboard[selectedKeyRow][selectedKeyCol - 1].ch : '\0';
    leftKeyExists = leftKey != '\0';

    char aboveKey = selectedKeyRow > 0 ? keyboard[selectedKeyRow - 1][selectedKeyCol].ch : '\0';
    aboveKeyExists = aboveKey != '\0';

    char belowKey = selectedKeyRow < 2 ? keyboard[selectedKeyRow + 1][selectedKeyCol].ch : '\0';
    belowKeyExists = belowKey != '\0';

    doDrawKeyboardScreen = false;

    delay(125);
  } while (!startPressed);

  return String(userIdArr);
}

// returns true if the button is pressed, false otherwise.
bool isPressed(uint32_t buttonRead, uint8_t button) {
    return (buttonRead & (1UL << button)) == 0;
}

void drawKeyboardScreen(Key (&keyboard)[3][10], int selectedKeyRow, int selectedKeyCol, int keyW, int keyH, char userIdArr[]) {
  M5.Lcd.setTextSize(3);
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.setTextColor(TFT_WHITE);

  // Prompt to enter name
  M5.Lcd.setCursor(10,10);
  M5.Lcd.println("Enter user id:");

  M5.Lcd.setCursor(M5.Lcd.getCursorX() + 10, M5.Lcd.getCursorY());
  M5.Lcd.printf(userIdArr);
  // draw each valid key as a character

  for (int row = 0; row < 3; row++) {
    for (int col = 0; col < 10; col++) {
      if (keyboard[row][col].ch == '\0') {
        continue;
      }

      if (row == selectedKeyRow && col == selectedKeyCol) {
        M5.Lcd.drawRect(keyboard[row][col].x, keyboard[row][col].y, keyW, keyH, TFT_WHITE);
      }

      M5.Lcd.setCursor(keyboard[row][col].x, keyboard[row][col].y);
      M5.Lcd.print(keyboard[row][col].ch);
    }
  }
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
    else
      Serial.println("Rules characteristic can't notify.");

    if (gameStateRemoteCharacteristic->canNotify())
        gameStateRemoteCharacteristic->registerForNotify(gameStateNotifyCallback);
    else
      Serial.println("Gamestate characteristic can't notify.");

    deviceConnected = true;
    return deviceConnected;
}

// randomly generates the rules for the game by filling the rule arrays with true values
void generateRules(int rulesToGenerate) {
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

// iterates over ruleArrays and generates a string representing the truth values in each array.
// each array is represented by a substring of binary numbers within the larger returned string, 
// where, for some j, the jth character in the set = 1 means array[j] is true, and = 0 means false.
// the substrings of binary are seperated by spaces, such that, for some i, the ith substirng of binary represents truth values in the ith array
std::string generateRulesAsBits() {
  std::string res = "";

  for (int i = 0; i < numRuleArrays; i++) {
    RuleArray currentArray = ruleArrays[i];
    
    for (int j = 0; j < currentArray.size; j++) {
      res += currentArray.array[j] ? '1' : '0';
    }

    // append space on every iteration except the last
    if (i < numRuleArrays - 1) {
      res += " ";
    }
  }

  return res;
}

void sendRules(std::string rules) {
  if (isServer) {
    // server writes and notifies client
    rulesCharacteristic->setValue(rules);
    rulesCharacteristic->notify();
  } else {
    // client writes and server is automatically notified
    rulesRemoteCharacteristic->writeValue(rules);
  }
}

void sendGameStateSignal(GameStateSignal sig) {
  uint8_t sigInt = static_cast<uint8_t>(sig);

  if (isServer) {
    // server writes and notifies client
    gameStateCharacteristic->setValue(&sigInt, 1);
    gameStateCharacteristic->notify();
  } else {
    // client writes and server is automatically notified
    gameStateRemoteCharacteristic->writeValue(&sigInt, 1);
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

// copies over the rules from the other device by parsing the generated rules bit string.
// for the ith binary substring and the jth character within that substring, ruleArrays[i].array[j] becomes true if that jth character is '1'.  
void updateRemoteRules(std::string rules) {
  int ruleArrayIndex = 0;
  int innerArrayIndex = 0;

  for (int i = 0; i < rules.length(); i++) {
    char currentChar = rules[i];

    if (currentChar == ' ') {
      ruleArrayIndex++;
      innerArrayIndex = 0;
    } else {
      RuleArray currentArray = ruleArrays[ruleArrayIndex];
      bool currentValue = currentArray.array[innerArrayIndex];
      
      currentArray.array[innerArrayIndex] = currentValue || rules[i] == '1';
      innerArrayIndex++;
    }
  }

  gotRemoteRules = true;
}

// called within callback methods when one core tells the other to take game-state related action.
void updateGameState(uint8_t gameState) {
  GameStateSignal gameStateAsEnumVal = static_cast<GameStateSignal>(gameState);
  otherPlayerChecked = true;

  switch (gameStateAsEnumVal) {
    case GameStateSignal::DO_KABOOM: // other player lost, so both players lose.
      doKaboom = true;
      break;
    case GameStateSignal::GOOD: // other player checked their wire cuts and didn't violate the rules
      doCheck = true; // prompting local check if it hasn't happened yet.
      otherPlayerGood = true;
      break;
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
bool determineGameState() {
  for (int i = 0; i < 3; i++) { 
    ColoredWire &wire = wires[i];
    Direction dir = static_cast<Direction>(i);
    if (shouldCutWire(wire, dir) && !wire.getIsCut()) {
      // if the player failed to cut a wire they should have cut, kaboom
      return false;
    }
  }

  return true; // if we get to the end of the loop and haven't kaboomed, the player wins!
}

// this function determines whether a wire should be cut based on its color and direction, using the rules defined in the arrays.
bool shouldCutWire(const ColoredWire& wire, Direction dir) {
  GameColor color = wire.getColor();

  bool shouldCuts = shouldCutColor[color] || shouldCutDirection[dir];
  bool shouldntCuts = shouldNotCutColor[color] || shouldNotCutDirection[dir];

  return (shouldCuts && !shouldntCuts); // true if the wire s
}
