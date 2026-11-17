// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Per-Simon Saal

#include <Arduino.h>
//#include <U8g2lib.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
#include "DmxInput.h"
#include <EEPROM.h>

#include "FreeSans9pt7b.h"

// === GPIO Button Pins ===
#define PIN_BTN_UP     3//27
#define PIN_BTN_MODE   4//29
#define PIN_BTN_DOWN   5//28

// === GPIO PWM Pins ===
#define LEDRED 12//6
#define LEDGREEN 11//7
#define LEDBLUE 10//8
#define LEDWHITE 9

// === OLED Use Wire1 with custom I2C pins ===
#define I2C_SDA 14
#define I2C_SCL 15

#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels
#define SUBMENU_ROW_START 16
#define SUBMENU_ROW_SPACING 12

// === DMX ===
#define NUM_CHANNELS 4
#define DMX_UNIVERSE_SIZE 512
#define EEPROM_DMX_MAGIC 0xD6

// === LED ===
#define WS2812_PIN 16
#define NUM_PIXELS 1

// === EEPROM ===
#define EEPROM_SIZE 10  // 10 bytes for 10 uint8_t values

// === Adafruit GFX constructor ===
#define OLED_RESET     -1 // Reset pin # (or -1 if sharing Arduino reset pin)
#define SCREEN_ADDRESS 0x3C ///< See datasheet for Address; 0x3D for 128x64, 0x3C for 128x32

// === Constructors ===
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire1, OLED_RESET);
Adafruit_NeoPixel pixels(NUM_PIXELS, WS2812_PIN, NEO_GRB + NEO_KHZ800);
DmxInput dmxInput;

// === Variables ===
volatile uint8_t buffer[DMXINPUT_BUFFER_SIZE(1, DMX_UNIVERSE_SIZE)]; // reads out the complete universe
volatile uint8_t dmxbuf[NUM_CHANNELS];

struct SubItemData;

volatile bool off = 0;

// timing variables
uint32_t actual_time = 0;
uint32_t actual_screen_time = 0;
uint32_t last_time = 0;
uint32_t last_screen_time = 0;
const uint32_t period_time = 100;
const uint32_t period_screen_time = 5000;
const int colorDelay = 10; // Delay between brightness steps (ms)
const uint32_t startupDmxDetectDuration = 1500;
uint32_t startupDmxCheckStarted = 0;
uint32_t startupDmxInitialTimestamp = 0;
bool startupDmxCheckComplete = false;
bool dmxPresentAtStartup = false;

// === Button Handling ===
enum ButtonInput {
  BTN_NONE, BTN_UP, BTN_MODE, BTN_DOWN
};

ButtonInput btn = BTN_NONE;

struct ButtonState {
  uint8_t pin;
  ButtonInput input;
  bool lastReading;
  bool stableReading;
  uint32_t lastChange;
};

const uint32_t buttonDebounceMs = 50;
ButtonState buttonStates[] = {
  {PIN_BTN_UP, BTN_UP, HIGH, HIGH, 0},
  {PIN_BTN_MODE, BTN_MODE, HIGH, HIGH, 0},
  {PIN_BTN_DOWN, BTN_DOWN, HIGH, HIGH, 0}
};

void pollButtons() {
  const uint32_t now = millis();
  for (ButtonState &button : buttonStates) {
    const bool reading = digitalRead(button.pin);
    if (reading != button.lastReading) {
      button.lastReading = reading;
      button.lastChange = now;
    }

    if (now - button.lastChange >= buttonDebounceMs &&
        reading != button.stableReading) {
      button.stableReading = reading;
      if (reading == LOW && btn == BTN_NONE) {
        btn = button.input;
      }
    }
  }
}

// === GUI Function Prototypes ===
void gui_off();
void gui_dmx();
void gui_auto();
void gui_sub_dmx(uint8_t index, uint8_t count);//address, uint8_t channel, uint8_t reso);
void gui_sub_auto(uint8_t index, uint8_t count);

void led_show();
void led_auto();

// === Menu System ===
enum MenuMode {
  MENU_TOP,
  MENU_SUB
};

enum SubParam {
  PARAM_ADDR,
  PARAM_CHAN
};

typedef struct MenuItem {
  const char* label;
  void (*renderFunc)();
  void (*subRenderFunc)(uint8_t, uint8_t);
  uint8_t subCount;
} MenuItem;

MenuItem menu[] = {
  {"DMX",    gui_dmx,    gui_sub_dmx,      255},
  {"AUTO",   gui_auto,   gui_sub_auto,     3}
};

#define MENU_COUNT (sizeof(menu)/sizeof(MenuItem))

struct MenuState {
  uint8_t selected = 0;
  uint8_t subSelected = 0;
  uint8_t fieldIndex = PARAM_ADDR;
  MenuMode mode = MENU_TOP;
};

MenuState menuState;

enum AutoEffect {
  AUTO_EFFECT_NONE,
  AUTO_EFFECT_LED_AUTO,
  AUTO_EFFECT_ONLY_WHITE,
  AUTO_EFFECT_BLUE_GREEN
};

AutoEffect activeAutoEffect = AUTO_EFFECT_NONE;
uint16_t autoEffectStep = 0;
uint8_t autoEffectPhase = 0;
uint32_t lastAutoEffectStepTime = 0;
uint32_t autoEffectWaitStart = 0;

struct SubItemData {
  uint16_t address = 1;
  uint8_t channel = NUM_CHANNELS;
};

SubItemData dmxData;
SubItemData autoData[3];

static uint16_t maxDmxStartAddress(uint8_t channelCount) {
  return DMX_UNIVERSE_SIZE - channelCount + 1;
}

static void saveDmxSettings() {
  EEPROM.write(0, EEPROM_DMX_MAGIC);
  EEPROM.write(1, dmxData.address & 0xFF);
  EEPROM.write(2, dmxData.address >> 8);
  EEPROM.write(3, dmxData.channel);
  EEPROM.commit();
}

static SubItemData* getSubDataForCurrentMenu() {
  const MenuItem &m = menu[menuState.selected];

  if (m.subRenderFunc == gui_sub_dmx) return &dmxData;
  if (m.subRenderFunc == gui_sub_auto) return autoData;
  return nullptr;
}

static uint8_t getSubCountForCurrentMenu() {
  const MenuItem &m = menu[menuState.selected];

  if (m.subRenderFunc == gui_sub_dmx) return 1;
  if (m.subRenderFunc == gui_sub_auto) return 3;
  return 0;
}


// === Menu Logic ===
void menu_handleInput(ButtonInput btn) {
  if (btn == BTN_NONE) return;

  if (menuState.mode == MENU_TOP) {
    if (btn == BTN_UP) {
      menuState.selected = (menuState.selected + MENU_COUNT - 1) % MENU_COUNT;
    }
    else if (btn == BTN_DOWN) {
      menuState.selected = (menuState.selected + 1) % MENU_COUNT;
    }
    else if (btn == BTN_MODE) {
      const MenuItem &m = menu[menuState.selected];
      if (m.subRenderFunc) {
        menuState.mode = MENU_SUB;
        menuState.subSelected = 0;
        menuState.fieldIndex = PARAM_ADDR;
      }
    }
    return;
  }

  if (menuState.selected == 1) {
    if (btn == BTN_UP) {
      menuState.subSelected = (menuState.subSelected + 2) % 3;
    }
    else if (btn == BTN_DOWN) {
      menuState.subSelected = (menuState.subSelected + 1) % 3;
    }
    else if (btn == BTN_MODE) {
      activeAutoEffect = (AutoEffect)(menuState.subSelected + 1);
      autoEffectStep = 0;
      autoEffectPhase = 0;
      lastAutoEffectStepTime = 0;
      autoEffectWaitStart = 0;
      menuState.mode = MENU_TOP;
      if (activeAutoEffect == AUTO_EFFECT_ONLY_WHITE) {
        only_white();
      } else {
        dmxbuf[3] = 0;
      }
    }
    return;
  }

  SubItemData *data = getSubDataForCurrentMenu();
  if (!data) return;

  SubItemData &item = data[menuState.subSelected];

  if (btn == BTN_MODE) {
    if (menuState.fieldIndex == PARAM_ADDR) {
      menuState.fieldIndex = PARAM_CHAN;
    } else {
      menuState.fieldIndex = PARAM_ADDR;
      menuState.mode = MENU_TOP;
      if (menuState.selected == 0) {
        saveDmxSettings();
      }
    }
  }
  else if (btn == BTN_UP || btn == BTN_DOWN) {
    int dir = (btn == BTN_UP) ? -1 : 1;

    switch (menuState.fieldIndex) {
      case PARAM_ADDR:
        if (menuState.selected == 0 && dir < 0 && item.address == 1) {
          item.address = maxDmxStartAddress(item.channel);
        } else if (menuState.selected == 0 && dir > 0 &&
                   item.address == maxDmxStartAddress(item.channel)) {
          item.address = 1;
        } else {
          const uint16_t maxAddress = menuState.selected == 0
              ? maxDmxStartAddress(item.channel)
              : maxDmxStartAddress(NUM_CHANNELS);
          item.address = constrain((int)item.address + dir, 1, maxAddress);
        }
        break;
      case PARAM_CHAN:
        item.channel = constrain((int)item.channel + dir, 1, NUM_CHANNELS);
        if (menuState.selected == 0 &&
            item.address > maxDmxStartAddress(item.channel)) {
          item.address = maxDmxStartAddress(item.channel);
        }
        break;
    }
  }
}

// === Menu Rendering with Adafruit GFX ===
void menu_render() {
  display.clearDisplay();
  display.setTextColor(1);
  display.setTextWrap(false);
  display.setFont();
  display.setCursor(0, 10);

  if (menuState.mode == MENU_TOP) {
    for (uint8_t i = 0; i < MENU_COUNT; i++) {
      display.print(i == menuState.selected ? "> " : "  ");
      display.println(menu[i].label);
    }

    if (menu[menuState.selected].renderFunc)
      menu[menuState.selected].renderFunc();
  } else {
    const MenuItem &m = menu[menuState.selected];
    if (m.subRenderFunc)
      m.subRenderFunc(menuState.subSelected, getSubCountForCurrentMenu());
  }

  display.display();
}

static bool dmxSignalAvailable() {
  return (millis() - dmxInput.latest_packet_timestamp()) < 1000;
}

static void updateDmxChannels() {
  for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
    dmxbuf[i] = i < dmxData.channel
        ? buffer[dmxData.address + i]
        : 0;
  }
}

// === DMX Setup ===
void setup_dmx(){
  for (uint i = 0; i < sizeof(buffer); i++) {
    buffer[i] = 0;
  }
  dmxInput.begin(0, 1, 512);
  dmxInput.read_async(buffer);
}

// === Setup ===
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Pico ready");

  pinMode(PIN_BTN_UP, INPUT_PULLUP);
  pinMode(PIN_BTN_MODE, INPUT_PULLUP);
  pinMode(PIN_BTN_DOWN, INPUT_PULLUP);

  // Setup the LEDs
  pinMode(LEDRED, OUTPUT);
  pinMode(LEDGREEN, OUTPUT);
  pinMode(LEDBLUE, OUTPUT);
  pinMode(LEDWHITE, OUTPUT);
  analogWriteFreq(500);

  // Setup Wire1 for OLED
  Wire1.setSDA(I2C_SDA);
  Wire1.setSCL(I2C_SCL);
  Wire1.begin();

  // SSD1306_SWITCHCAPVCC = generate display voltage from 3.3V internally
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    for(;;); // Don't proceed, loop forever
  }
  Serial.println("Display OK.");

  // Clear the buffer
  display.clearDisplay();
  display.display();

  // Initialize EEPROM with 10 bytes and read
  EEPROM.begin(EEPROM_SIZE);
  if (EEPROM.read(0) == EEPROM_DMX_MAGIC) {
    uint8_t savedChannel = EEPROM.read(3);
    if (savedChannel >= 1 && savedChannel <= NUM_CHANNELS) {
      dmxData.channel = savedChannel;
    }
    uint16_t savedAddress = EEPROM.read(1) | ((uint16_t)EEPROM.read(2) << 8);
    if (savedAddress >= 1 && savedAddress <= maxDmxStartAddress(dmxData.channel)) {
      dmxData.address = savedAddress;
    }
  }

  // Setup DMX Input to read
  setup_dmx();
  startupDmxCheckStarted = millis();
  startupDmxInitialTimestamp = dmxInput.latest_packet_timestamp();

  // Setup Pixel LED
  pixels.begin();
  pixels.show(); // Initialize all pixels to 'off'

  // show the menu first time, ready to go!
  menu_render();
}

// === Main Loop ===
unsigned long lastInputCheck = 0;
unsigned long lastRender = 0;
ButtonInput lastBtn = BTN_NONE;

void loop() {
  actual_screen_time = millis();
  if (!startupDmxCheckComplete &&
      actual_screen_time - startupDmxCheckStarted >= startupDmxDetectDuration) {
    dmxPresentAtStartup =
        dmxInput.latest_packet_timestamp() != startupDmxInitialTimestamp;
    startupDmxCheckComplete = true;
  }
  pollButtons();

  if (btn != BTN_NONE) {
    if (off) {
      off = false;
      last_screen_time = actual_screen_time;
      menu_render();
    } else {
      menu_handleInput(btn);
      menu_render();
      last_screen_time = actual_screen_time;
    }
    btn = BTN_NONE;
  }

  if ((actual_screen_time - last_screen_time) > period_screen_time) {
    last_screen_time = actual_screen_time;
    if (!off) {
      saveDmxSettings();
      menuState.selected = 0;
      menuState.subSelected = 0;
      menuState.fieldIndex = PARAM_ADDR;
      menuState.mode = MENU_TOP;
      gui_off();
      off = true;
    }
  }

  if (!off && menuState.mode == MENU_TOP && menuState.selected <= 1 &&
      millis() - lastRender >= 250) {
    lastRender = millis();
    menu_render();
  }

  const bool dmxActive = dmxSignalAvailable();
  static bool dmxWasActive = false;
  static uint32_t dmxActiveSince = 0;

  if (dmxActive) {
    if (!dmxWasActive) {
      dmxWasActive = true;
      dmxActiveSince = millis();
    }
    updateDmxChannels();
    led_show();
    return;
  }

  if (dmxWasActive) {
    const uint32_t dmxDuration = millis() - dmxActiveSince;
    lastAutoEffectStepTime += dmxDuration;
    if (autoEffectPhase == 1 || autoEffectPhase == 3) {
      autoEffectWaitStart += dmxDuration;
    }
    if (activeAutoEffect == AUTO_EFFECT_ONLY_WHITE) {
      only_white();
    }
    dmxWasActive = false;
  }

  if (activeAutoEffect == AUTO_EFFECT_LED_AUTO) {
    led_auto();
    return;
  }
  if (activeAutoEffect == AUTO_EFFECT_ONLY_WHITE) {
    return;
  }
  if (activeAutoEffect == AUTO_EFFECT_BLUE_GREEN) {
    blue_green();
    return;
  }

  if (!startupDmxCheckComplete || dmxPresentAtStartup) {
    return;
  }

  // DMX input handler: do not block the loop, otherwise buttons and display stop responding.
  if (millis() > 1000 + dmxInput.latest_packet_timestamp()) {
    static uint8_t autoStep = 0;
    static uint8_t autoPhase = 0;
    static uint32_t lastAutoStepTime = 0;

    if (millis() - lastAutoStepTime >= colorDelay) {
      lastAutoStepTime = millis();

      switch (autoPhase) {
        case 0: // Red -> Green
          dmxbuf[0] = 255 - autoStep;
          dmxbuf[1] = autoStep;
          dmxbuf[2] = 0;
          break;
        case 1: // Green -> Blue
          dmxbuf[0] = 0;
          dmxbuf[1] = 255 - autoStep;
          dmxbuf[2] = autoStep;
          break;
        case 2: // Blue -> Red
          dmxbuf[0] = autoStep;
          dmxbuf[1] = 0;
          dmxbuf[2] = 255 - autoStep;
          break;
      }

      if (autoStep >= 255) {
        autoStep = 0;
        autoPhase = (autoPhase + 1) % 3;
      } else {
        autoStep++;
      }

      led_show();
    }
    return;
  }
  // Print the DMX channels
  /*Serial.print("Received packet: ");
  for (uint i = 0; i < sizeof(buffer); i++)
  {
      Serial.print(buffer[i]);
      Serial.print(", ");
  }
  Serial.println("");*/

  updateDmxChannels();
  
  led_show();
}

// === GUI Dummy Implementations ===
// Replace with real drawing code
void gui_off(){
  display.clearDisplay();
  display.display();
}

void gui_dmx() {
  display.setCursor(0, 56);
  display.print(dmxSignalAvailable() ? "DMX: OK" : "DMX: NO SIGNAL");
}

void gui_auto() {
  display.setCursor(0, 56);
  if (activeAutoEffect != AUTO_EFFECT_NONE && dmxSignalAvailable()) {
    display.print("DMX override");
    return;
  }
  switch (activeAutoEffect) {
    case AUTO_EFFECT_LED_AUTO:
      display.print("Playing: LED Auto");
      break;
    case AUTO_EFFECT_ONLY_WHITE:
      display.print("Playing: Only White");
      break;
    case AUTO_EFFECT_BLUE_GREEN:
      display.print("Playing: Blue-Green");
      break;
    default:
      display.print("Auto Mode");
      break;
  }
}

void gui_sub_dmx(uint8_t index, uint8_t count) {
  (void)count;
  SubItemData &item = dmxData;

  display.setCursor(0, SUBMENU_ROW_START);
  display.printf("%s Start Addr: %d", menuState.fieldIndex == PARAM_ADDR ? ">" : " ", item.address);
  display.setCursor(0, SUBMENU_ROW_START + SUBMENU_ROW_SPACING);
  display.printf("%s Num Channel: %d", menuState.fieldIndex == PARAM_CHAN ? ">" : " ", item.channel);
}

void gui_sub_auto(uint8_t index, uint8_t count) {
  const char* effects[] = {"LED Auto", "Only White", "Blue-Green"};
  display.setCursor(0, 0);
  display.print("AUTO EFFECT");

  for (uint8_t i = 0; i < count; i++) {
    display.setCursor(0, SUBMENU_ROW_START + i * SUBMENU_ROW_SPACING);
    display.print(i == index ? "> " : "  ");
    display.print(effects[i]);
  }

  display.setCursor(0, 56);
  display.print("MODE: Play");
}

void led_show(){
  // Set LEDs
  // buffer[0] seems to be the break byte
  analogWrite(LEDRED, dmxbuf[0]);
  analogWrite(LEDGREEN, dmxbuf[1]);
  analogWrite(LEDBLUE, dmxbuf[2]);
  analogWrite(LEDWHITE, dmxbuf[3]);
  
  // Apply to WS2812 LED
  pixels.setPixelColor(0, pixels.Color(dmxbuf[0], dmxbuf[1], dmxbuf[2]));
  //pixels.setPixelColor(0, pixels.Color(255,255,0));
  pixels.show();
}

void led_auto(){
  if (millis() - lastAutoEffectStepTime < colorDelay) return;
  lastAutoEffectStepTime = millis();

  switch (autoEffectPhase) {
    case 0:
      dmxbuf[0] = 255 - autoEffectStep;
      dmxbuf[1] = autoEffectStep;
      dmxbuf[2] = 0;
      break;
    case 1:
      dmxbuf[0] = 0;
      dmxbuf[1] = 255 - autoEffectStep;
      dmxbuf[2] = autoEffectStep;
      break;
    case 2:
      dmxbuf[0] = autoEffectStep;
      dmxbuf[1] = 0;
      dmxbuf[2] = 255 - autoEffectStep;
      break;
  }

  led_show();

  if (autoEffectStep >= 255) {
    autoEffectStep = 0;
    autoEffectPhase = (autoEffectPhase + 1) % 3;
  } else {
    autoEffectStep++;
  }
}

void only_white(){
  dmxbuf[0] = 0;
  dmxbuf[1] = 0;
  dmxbuf[2] = 0;
  dmxbuf[3] = 255;
  led_show();
}

void blue_green(){
  if (autoEffectPhase == 1) {
    if (millis() - autoEffectWaitStart >= 5000) {
      autoEffectPhase = 2;
      autoEffectStep = 0;
      lastAutoEffectStepTime = millis();
    }
    return;
  }
  if (autoEffectPhase == 3) {
    if (millis() - autoEffectWaitStart >= 5000) {
      autoEffectPhase = 0;
      autoEffectStep = 0;
      lastAutoEffectStepTime = millis();
    }
    return;
  }

  if (millis() - lastAutoEffectStepTime < colorDelay) return;
  lastAutoEffectStepTime = millis();

  dmxbuf[0] = 0;
  if (autoEffectPhase == 0) {
    dmxbuf[1] = 255 - autoEffectStep;
    dmxbuf[2] = autoEffectStep;
  } else {
    dmxbuf[1] = autoEffectStep;
    dmxbuf[2] = 255 - autoEffectStep;
  }
  led_show();

  if (autoEffectStep >= 255) {
    autoEffectStep = 0;
    autoEffectWaitStart = millis();
    autoEffectPhase = (autoEffectPhase == 0) ? 1 : 3;
  } else {
    autoEffectStep++;
  }
}
