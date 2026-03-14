#include <Arduino.h>
#include <M5Core2.h>
#include "ColoredWire.h"
#include "GameColor.h"
#include "Direction.h"

// holds the rules of the game in a string representation for display purposes.
const int RULES = 4;
static String rulesAsString[RULES];

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
bool shouldCutWire(const ColoredWire& wire, Direction dir);

void setup() {
  M5.begin();
  Serial.begin(115200);

  // generate random rules for the game
  generateRules(RULES);

  // generate randomly colored wires for the game
  generateWires();

  // print the rules to the console for testing purposes
  for (int i = 0; i < RULES; i++) {
    Serial.printf("%d. %s\n", i + 1, rulesAsString[i].c_str());
  }
}

void loop() {
  // TESTING CODE DELETE LATER
  for (int i = 0; i < 3; i++) {
    ColoredWire wire = wires[i];
    String stringRep = wire.toString();
    Serial.printf("Checking wire %d: %c. Should Cut: %d\n", 
                  i, 
                  wire.getCharRepresentation(),
                  shouldCutWire(wire, static_cast<Direction>(i)));
  }
  delay(1000); // delay to avoid spamming the console
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
      ruleString = "Don't cut any " + COLOR_TO_STRING[index] + " wires";
      break;
    case 2: // should cut direction
      ruleString = "Cut all " + DIRECTION_TO_STRING[index] + " wires";
      break;
    case 3: // should not cut direction
      ruleString = "Don't cut any " + DIRECTION_TO_STRING[index] + " wires";
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

// this function determines whether a wire should be cut based on its color and direction, using the rules defined in the arrays.
bool shouldCutWire(const ColoredWire& wire, Direction dir) {
  GameColor color = wire.getColor();

  bool shouldCuts = shouldCutColor[color] || shouldCutDirection[dir];
  bool shouldntCuts = shouldNotCutColor[color] || shouldNotCutDirection[dir];

  return (shouldCuts && !shouldntCuts);
}
