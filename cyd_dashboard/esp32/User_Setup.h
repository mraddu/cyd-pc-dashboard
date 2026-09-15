// User_Setup.h for TFT_eSPI library
// Board: ESP32-2432S028 "Cheap Yellow Display" - CYD2USB revision (ST7789 driver)
//
// HOW TO USE:
// 1. Find where the TFT_eSPI library is installed:
//    Windows Arduino IDE default: Documents\Arduino\libraries\TFT_eSPI
// 2. Open the "User_Setup_Select.h" file in that folder.
// 3. Comment out the line: #include <User_Setup.h>
// 4. Replace this ENTIRE file (TFT_eSPI/User_Setup.h) with the contents below,
//    OR add a new line: #include <User_Setups/Setup_CYD2USB.h> and save this
//    file under that name instead. Easiest: just overwrite the library's
//    default User_Setup.h with this file.

#define USER_SETUP_INFO "CYD2USB_ST7789"

// --- Driver ---
#define ST7789_DRIVER
#define TFT_RGB_ORDER TFT_BGR   // If colors look swapped (blue<->red), flip to TFT_RGB
#define TFT_INVERSION_ON        // ST7789 on this board needs inversion, otherwise colors look wrong

// --- Resolution ---
#define TFT_WIDTH  240
#define TFT_HEIGHT 320

// --- Pin mapping (standard CYD wiring, shared across most 2432S028 batches) ---
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1   // Not connected on most CYD boards, tied to EN
#define TFT_BL   21   // Backlight control
#define TFT_BACKLIGHT_ON HIGH

// --- Touch (XPT2046, separate SPI bus, only needed if you wire touch input) ---
#define TOUCH_CS 33

// --- Fonts ---
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

// --- SPI frequency ---
#define SPI_FREQUENCY       40000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000
