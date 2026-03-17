#ifndef COLORED_WIRE_H
#define COLORED_WIRE_H

#include "GameColor.h"
#include "Direction.h"
#include <Arduino.h>

// colored wires have a color, and can be cut or not cut.
// when the wire is cut, it is cut forever.
class ColoredWire {
    private:
        GameColor color;
        bool isCut;
        char charRepresentation;

    public:
        ColoredWire() : color(GAME_RED), isCut(false), charRepresentation('R') {}
        ColoredWire(GameColor c) : color(c), isCut(false), charRepresentation("RGB"[c]) {}

        // irreversibly cut the wire
        void cut() {
            this->isCut = true;
            this->charRepresentation = 'X';
        }

        bool getIsCut() const {
            return this->isCut;
        }

        GameColor getColor() const {
            return this->color;
        }

        String toString() const {
            return "Color: " + String(this->color) + ", Is Cut: " + String(this->isCut) + ", Char Rep: " + String(this->charRepresentation);
        }

        char getCharRepresentation() const {
            return this->charRepresentation;
        }
};

#endif // COLORED_WIRE_H