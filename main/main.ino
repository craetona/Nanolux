/**@file
 *
 * This file is the main driver of the NanoLux codebase.
 *
**/

#include <ArduinoJson.h>
#include <ArduinoJson.hpp>
#include <FastLED.h>
#include <Arduino.h>
#include "arduinoFFT.h"
#include "patterns.h"
#include "nanolux_types.h"
#include "nanolux_util.h"
#include "core_analysis.h"
#include "ext_analysis.h"
#include "storage.h"
#include "globals.h"
#include "AiEsp32RotaryEncoder.h"


FASTLED_USING_NAMESPACE

#define ENABLE_WEB_SERVER
#ifdef ENABLE_WEB_SERVER
#include "WebServer.h"
#endif

// #define SHOW_TIMINGS

/// Unprocessed output buffer.
CRGB output_buffer[MAX_LEDS]; 

/// Postprocessed output buffer.
CRGB smoothed_output[MAX_LEDS];

/// FFT used for processing audio.
arduinoFFT FFT = arduinoFFT();

/// Contains the current state of the button (true is pressed).
bool button_pressed = false;

/// Contains the peak frequency detected by the FFT.
double peak = 0.;

/// Contains the "base" hue, calculated from the peak frequency.
uint8_t fHue = 0;

/// Contains the current volume detected by the FFT.
double volume = 0.;

/// Contains the "base" brightness value, calculated from the current volume.
uint8_t vbrightness = 0;

/// TODO: Refactor and move.
unsigned long checkTime;

/// TODO: Refactor and move.
double checkVol;

/// Updated to "true" when the web server changes significant pattern settings.
volatile bool pattern_changed = false;

/// The current strip configuration being ran by the device.
Strip_Data loaded_patterns;

/// All patterns and strip configuration currently loaded from storage.
Strip_Data saved_patterns[NUM_SAVES];

/// The currently-loaded device config.
Config_Data config;


/// The pattern ID displayed when manual control is enabled.
uint8_t manual_pattern_idx = 0;

/// Contains if manual control is enabled or not.
volatile bool manual_control_enabled = false;
uint8_t rotary_encoder_pattern_idx = 0;


/// History of all currently-running patterns.
Strip_Buffer histories[PATTERN_LIMIT];

/// The current list of patterns, externed from globals.h.
extern Pattern mainPatterns[];

/**********************************************************
 *
 * We want the API to be included after the globals.
 *
 **********************************************************/
#ifdef ENABLE_WEB_SERVER
#include "api.h"
#endif

AiEsp32RotaryEncoder rotaryEncoder = AiEsp32RotaryEncoder(ROTARY_ENCODER_A_PIN, ROTARY_ENCODER_B_PIN, ROTARY_ENCODER_BUTTON_PIN, ROTARY_ENCODER_VCC_PIN, ROTARY_ENCODER_STEPS);

void setup();
void loop();
void audio_analysis();

/// @brief Sets up various objects needed by the device.
///
/// Initalizes the button pin, serial, pattern index, the FastLED
/// object, and all stored saves.
void setup() {

  pinMode(LED_BUILTIN, OUTPUT);

  // Start USB serial communication
  Serial.begin(115200);  
  while (!Serial) { ; }

  //Initialize rotary encoder
  rotaryEncoder.begin();
  rotaryEncoder.setup(readEncoderISR);
  bool circleValues = true;
  rotaryEncoder.setBoundaries(0, 12, circleValues); //minValue, maxValue, circleValues true|false (when max go to min and vice versa)

  /*Rotary acceleration introduced 25.2.2021.
    in case range to select is huge, for example - select a value between 0 and 1000 and we want 785
    without accelerateion you need long time to get to that number
    Using acceleration, faster you turn, faster will the value raise.
    For fine tuning slow down.
  */
  //rotaryEncoder.disableAcceleration(); //acceleration is now enabled by default - disable if you dont need it
  rotaryEncoder.setAcceleration(250); //or set the value - larger number = more accelearation; 0 or 1 means disabled acceleration

  // Reindex mainPatterns, to make sure it is consistent.
  for (int i = 0; i < NUM_PATTERNS; i++) {
    mainPatterns[i].index = i;
  }

  // attach button interrupt
  pinMode(digitalPinToInterrupt(BUTTON_PIN), INPUT_PULLUP);
  attachInterrupt(BUTTON_PIN, buttonISR, FALLING);

  checkTime = millis();
  checkVol = 0;

  //  initialize up led strip
  FastLED.addLeds<LED_TYPE, DATA_PIN, CLK_PIN, COLOR_ORDER>(smoothed_output, MAX_LEDS).setCorrection(TypicalLEDStrip);

  load_from_nvs();
  verify_saves();
  load_slot(0);

#ifdef ENABLE_WEB_SERVER
  initialize_web_server(apiGetHooks, API_GET_HOOK_COUNT, apiPutHooks, API_PUT_HOOK_COUNT);
#endif
}

/// @brief Layers two CRGB arrays together and places the result in another array.
/// @param a      The first array to layer.
/// @param b      The second array to layer.
/// @param out    The array to output to.
/// @param length The number of pixels to layer.
/// @param blur   The ratio between the first and second array in the output. 0-255.
void calculate_layering(CRGB *a, CRGB *b, CRGB *out, int length, uint8_t blur) {
  for (int i = 0; i < length; i++) {
    out[i] = blend(a[i], b[i], blur);
  }
}

/// @brief Checks if the web server has new data,
/// and marks settings as dirty if required.
/// 
/// Some settings are updated inside a more constrained context
/// without enough stack space for a "disk" operation so we
/// defer the save to here.
void update_web_server() {
#ifdef ENABLE_WEB_SERVER
  if (dirty_settings) {
    save_settings();
    dirty_settings = false;
  }
#endif
}

/// @brief Scales all LEDs in a CRGB array by (factor)/255
/// @param arr    The array to scale
/// @param len    The number of LEDs to scale
/// @param factor The factor to scale by. Divided by 255.
void scale_crgb_array(CRGB *arr, uint8_t len, uint8_t factor) {
  float_t scale_factor = ((float_t)factor) / 255;
  for (int i = 0; i < len; i++) {
    arr[i].r *= scale_factor;
    arr[i].g *= scale_factor;
    arr[i].b *= scale_factor;
  }
}

/// @brief Reverses the LED buffer supplied given a length.
/// @param buf The LED buffer to reverse.
/// @param len The length of the buffer to reverse.
void reverse_buffer(CRGB* buf, uint8_t len){

    CRGB temp[MAX_LEDS];
    memcpy(temp, buf, len * sizeof(CRGB));

    for (int i = 0; i < len; i++) {
      memcpy(&buf[i], &temp[len - 1 - i], sizeof(CRGB));
    }
}

/// @brief Unfolds the buffer by mirroring it across the right side.
/// @param buf The buffer to unfold.
/// @param len The length to unfold.
/// @param even If the LED strip length is even/odd. When odd, duplicates
/// the middle pixel a second time to fill space.
void unfold_buffer(CRGB* buf, uint8_t len, bool even){

  uint8_t offset = 0;

  // If this is not an even case, increase our array offset by 1 and
  // make the pixel at len equal to the pixel at len-1
  if(!even){
    buf[len] = buf[len-1];
    offset = 1;
  }

  // Unfold the array following the set offset.
  for(int i = 0; i < len; i++){
    buf[len + i + offset] = buf[len-i-1];
  }
}

void process_pattern(uint8_t idx, uint8_t len){

  // Pull the current postprocessing effects from the struct integer.
  uint8_t pp_mode = loaded_patterns.pattern[idx].postprocessing_mode;
  bool is_reversed = (pp_mode == 1) || (pp_mode == 3);
  bool is_mirrored = (pp_mode == 2) || (pp_mode == 3);

  getFhue(loaded_patterns.pattern[idx].minhue, loaded_patterns.pattern[idx].maxhue);
  getVbrightness();
  // Calculate the length to process
  uint8_t processed_len = (is_mirrored) ? len/2 : len;

  // Reverse the buffer to make it normal if it is reversed.
  if(is_reversed) reverse_buffer(histories[idx].leds, processed_len);

  // Process the pattern.
  mainPatterns[loaded_patterns.pattern[idx].idx].pattern_handler(
      &histories[idx],
      processed_len,
      &loaded_patterns.pattern[idx]);
  
  // Re-invert the buffer if we need the output to be reversed.
  if(is_reversed) reverse_buffer(histories[idx].leds, processed_len);

  // Unfold the buffer if needed.
  if(is_mirrored) 
    unfold_buffer(histories[idx].leds, processed_len, (len == processed_len * 2));
}


/// @brief  Runs the strip splitting LED strip mode
///
/// This function allocates a number of LEDs per pattern, then
/// moves LED data from the pattern's history buffer to the
/// unsmoothed output buffer.
///
/// Once the data is in the unsmoothed buffer, the buffer is
/// "smoothed" into the output smoothed buffer.
void run_strip_splitting() {

  // Defines the length of an LED strip segment
  uint8_t section_length = config.length / loaded_patterns.pattern_count;

  // Repeat for each pattern
  for (int i = 0; i < loaded_patterns.pattern_count; i++) {

    // Run the pattern handler for pattern i using history i
    process_pattern(i, section_length);

    // Copy the processed segment to the output buffer
    memcpy(
      &output_buffer[section_length * i],
      histories[i].leds,
      sizeof(CRGB) * section_length);

    // Scale the LED strip light output based on brightness
    scale_crgb_array(
      &output_buffer[section_length * i],
      section_length,
      loaded_patterns.pattern[i].brightness);

    

    

    // Smooth the brightness-adjusted output and put it
    // into the main output buffer.
    calculate_layering(
      &smoothed_output[section_length * i],
      &output_buffer[section_length * i],
      &smoothed_output[section_length * i],
      section_length,
      255 - loaded_patterns.pattern[i].smoothing);
  }
}

/// @brief Runs the pattern layering output mode
///
/// This function layers two patterns onto each other,
/// covering the entire length of the LED strip.
///
/// Each pattern buffer is moved into a temp buffer, which
/// scales the brightness of each pattern according to user
/// settings.
///
/// Then, the patterns are merged into the unsmoothed buffer,
/// which then is combined with the smoothed buffer.
void run_pattern_layering() {

  // Create two CRGB buffers for holding LED strip data
  // that has been light-adjusted.
  static CRGB temps[2][MAX_LEDS];

  // If there is one pattern pattern, run strip splitting,
  // which will output the single pattern.
  if (loaded_patterns.pattern_count < 2) {
    run_strip_splitting();
    return;
  }

  // If there are more than two patterns, use the first two
  // for pattern layering.
  uint8_t section_length = config.length;
  for (uint8_t i = 0; i < 2; i++) {
    // Run the pattern handler for pattern i using history i
    process_pattern(i, section_length);

    // Copy the processed segment to the temp buffer
    memcpy(
      temps[i],
      histories[i].leds,
      sizeof(CRGB) * config.length);

    // Scale the temp buffer light output based on brightness
    scale_crgb_array(
      temps[i],
      config.length,
      loaded_patterns.pattern[i].brightness);
  }

  // Combine the two temp arrays.
  calculate_layering(
    temps[0],
    temps[1],
    output_buffer,
    config.length,
    loaded_patterns.alpha);

  // Smooth the combined layered output buffer.
  calculate_layering(
    smoothed_output,
    output_buffer,
    smoothed_output,
    config.length,
    255 - loaded_patterns.pattern[0].smoothing);
}

/// @brief Prints a buffer to serial.
/// @param buf  The CRGB buffer to print.
/// @param len  The number of RGB values to print.
void print_buffer(CRGB *buf, uint8_t len) {
  for (int i = 0; i < len; i++) {
    Serial.print(String(buf[i].r) + "," + String(buf[i].g) + "," + String(buf[i].b) + " ");
  }
  Serial.print("\n");
}

/// @brief Runs the main program loop.
///
/// Carries out functions related to timing and updating the
/// LED strip.
void loop() {
  begin_loop_timer(config.loop_ms);  // Begin timing this loop
  rotary_loop();
  delay(50); 

  // Reset buffers if pattern settings were changed since
  // last program loop.
  if (pattern_changed) {
    pattern_changed = false;
    memset(smoothed_output, 0, sizeof(CRGB) * MAX_LEDS);
    memset(output_buffer, 0, sizeof(CRGB) * MAX_LEDS);
    histories[0] = Strip_Buffer();
    histories[1] = Strip_Buffer();
    histories[2] = Strip_Buffer();
    histories[3] = Strip_Buffer();
  }

  reset_button_state();  // Check for user button input

  process_reset_button();  // Manage resetting saves if button held

  audio_analysis();  // Run the audio analysis pipeline

  // fHue = remap(
  //   log(peak) / log(2),
  //   log(MIN_FREQUENCY) / log(2),
  //   log(MAX_FREQUENCY) / log(2),
  //   10, 240);

  // vbrightness = remap(
  //   volume,
  //   MIN_VOLUME,
  //   MAX_VOLUME,
  //   0,
  //   MAX_BRIGHTNESS);


  switch (loaded_patterns.mode) {

      case STRIP_SPLITTING:
        run_strip_splitting();
        break;

      case Z_LAYERING:
        run_pattern_layering();
        break;

      default:
        0;
    
  }

  FastLED.show();  // Push changes from the smoothed buffer to the LED strip

  // Print the LED strip buffer if the simulator is enabled.
  if (config.debug_mode == 2)
    print_buffer(smoothed_output, config.length);

  // Update the web server while waiting for the current
  // frame to complete.
  do {
    update_web_server();
  } while (timer_overrun() == 0);

  update_web_server();
}

/// @brief Performs audio analysis by running audio_analysis.cpp's
/// audio processing functions.
///
/// If the macro SHOW_TIMINGS is defined, it will print out the amount
/// of time audio processing takes via serial.
void audio_analysis() {
#ifdef SHOW_TIMINGS
  const int start = micros();
#endif

  sample_audio();

  update_peak();

  update_volume();

  update_max_delta();

  update_formants();

  update_noise();

  update_drums();

  noise_gate(loaded_patterns.noise_thresh);

  Serial.println(loaded_patterns.pattern[0].postprocessing_mode);

#ifdef SHOW_TIMINGS
  const int end = micros();
  Serial.printf("Audio analysis: %d ms\n", (end - start) / 1000);
#endif
}
