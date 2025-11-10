// An MP3 player based on the ESP32-S3-WROOM-2 
// that plays music stored in its internal flash.

// Install the library ESP8266Audio by Earle F. Philhower, III
// https://github.com/earlephilhower/ESP8266Audio
// Based in the examples of the library

// The files should be already in the flash.
// Make sure the size of the files is less of the flash memory available
// Use https://github.com/earlephilhower/arduino-littlefs-upload
// to upload files to the partition

// Developed by Fabio Oliveira, 2025 November 08

#include <AudioFileSourceLittleFS.h>  // to read the file (LittleFS, SD, HTTP, etc.)
#include <AudioGeneratorMP3.h>        // decode the format (MP3, WAV, AAC, etc.)
#include <AudioOutputI2S.h>           // to send sound (I2S, DAC, PWM, etc.)

// The pins of the buttons, configured later with internal pull-up resistors
#define NUM_BOTOES 7
#define button_previous         4
#define button_play_pause       5
#define button_next             6
#define button_rec              7 // not implemented
#define button_volume_decrease  15 
#define button_volume_mute      16
#define button_volume_increase  17  

// Pin of the RGB module
#define rgb_module 38

// The pins for I2S
#define BCLK      1
#define LRCLK_WS  2
#define DOUT      8

// The MAX98357A is connected as follow:
// My current setup draws less than 1A, so
// powering from the USB should work fine
// 
// Vin -> 5V 
// GND -> GND
// SD -> Not connected 
//    < 0.16V : Shutdown
//    0.16V ~ 0.77V : (L+R)/2, default
//    0.77 ~ 1.40V : R channel
//    > 1.40V : L channel
// GAIN -> Not connected
//    15 dB : 100k to GND
//    12 dB : GND
//    9 dB : Floating
//    6 dB : Vin
//    3 dB : 100k to Vin
// DIN -> ESP32 I2S DOUT, pin 8
// BCLK -> ESP32 I2S BCLK, pin 1
// LRC -> ESP32 I2S LRCLK/WS, pin 2
// + -> Positive input of the speaker
// - -> Negative input of the speaker

// Volume parameters
#define VOLUME_STEP   1.0     // Increases volume in steps of one
#define VOLUME_MIN    0.0f    // Minimum volume for user is 0.0, f to be treated as a float instead of a double
#define VOLUME_MAX    20.0f   // Maximum volume for user
#define VOLUME_INIT   ((VOLUME_MIN+VOLUME_MAX)/2)    // Starts at
#define VOLUME_FACTOR 3.95     // Scales volume.  The configuration works < 4

#define TRACK_RESTART_THRESHOLD_MS 5000 // When pressing previous, restart or go to previous song

const uint8_t pinsButtons[NUM_BOTOES] = { button_previous, button_play_pause, 
                                          button_next, button_rec, button_volume_decrease, 
                                          button_volume_mute, button_volume_increase };


// Variables shared with the interrupt and associated with the buttons
volatile bool buttonPressed[NUM_BOTOES] = { false };
volatile bool buttonLastState[NUM_BOTOES] = { true };  

// Timer pointer
hw_timer_t *timer = NULL;

// Audio pointers
AudioFileSourceLittleFS *file;
AudioGeneratorMP3 *mp3;
AudioOutputI2S *out;

// "->":" do not forget this is used because in audio objects we are working with pointers

// Variables to be used when manipulating audio
bool inPause = false;             // starts playing when powered on
float volumeLevel = VOLUME_INIT;  // initial volume. This number is passed later to a exponential function, see setVolumeSafe()
float previousVolumeLevel = 0;    // last volume before muting
String currentFile;               // file being reproduced
unsigned long trackStartTime = 0; // the time the track started being reproduced
unsigned long trackElapsedTime = 0; // track was being played this time
uint32_t pausedPos = 0;           // position when the track was paused
bool isMute = false;              // indicates if the system is muted

// -----------------------------------------------------------------------------------------------

// Interrupt routine
// Every timer interrupt, check if there is a button state change and change the flag of that button
// Handling of the flag will be in the loop() section
void IRAM_ATTR onTimer() {
  for (int i = 0; i < NUM_BOTOES; i++) {
    bool state = digitalRead(pinsButtons[i]);
    if (buttonLastState[i] == true && state == false) { 
      buttonPressed[i] = true;
    }
    buttonLastState[i] = state;
  }
}

// -----------------------------------------------------------------------------------------------

// The perceived sound by the human ear is not linear
// So this raises a number to a power pow(a,b) = a^b
// and adjust the volume accordingly
void setVolumeSafe(float &v) {

  v = constrain(v, VOLUME_MIN, VOLUME_MAX); // if it is below or above the predetermined values, updates the variable
  float volume = v / VOLUME_MAX; // scales to 0 ... 1

  volume = pow(volume,2); // volume^2
  volume = volume * VOLUME_FACTOR; // scales the volume

  if (out) {
    out->SetGain(volume);
    Serial.printf("setVolumeSafe(): volume_unor: %.2F | volume_norm: %.2F...\n", v, volume);
  } else {
    Serial.println("setVolumeSafe(): out == nullptr, volume ignored...");
  }
}

// -----------------------------------------------------------------------------------------------

// When returning a file name, if it is in root, we could get "file.mp3"
// when it should be "/file.mp3". This fixes it.
String normalizePath(const String& s) {
  if (s.length() == 0) return s;
  if (s[0] == '/') return s;
  return "/" + s;
}

// -----------------------------------------------------------------------------------------------

// Looks for the first mp3 file in the file system and returns its path/name
String getFirstMP3() {
  File root = LittleFS.open("/");
  if (!root || !root.isDirectory()){
    Serial.println("getFirstMp3(): failing open LittleFS...");
    return "";
  }

  File file = root.openNextFile();
  while (file) {
    String name = normalizePath(file.name());
    name.toLowerCase();
    if (name.endsWith(".mp3")) {
      root.close();
      // c_str() → gives a raw view of the text inside, compatible with plain C functions
      Serial.printf("getFirstMP3(): found %s\n", normalizePath(file.name()).c_str());
      return normalizePath(file.name());
    }
    file = root.openNextFile();
  }

  root.close();
  return ""; // No MP3 found
}

// -----------------------------------------------------------------------------------------------

// Gets the next mp3 file. If there is none after the current one, 
// it starts from the begining of the file system
String getNextMP3(const String &currentFile) {
  String currentLower = currentFile;
  currentLower.toLowerCase();

  File root = LittleFS.open("/");
  if (!root || !root.isDirectory()){
    Serial.println("getNextMp3(): failing open LittleFS...");
    return "";
  }

  File file = root.openNextFile();
  bool foundCurrent = false;

  while (file) {
    String name = normalizePath(file.name());
    name.toLowerCase();

    if (name.endsWith(".mp3")) {
      if (foundCurrent) {
        // Found the next MP3 after the current one
        Serial.printf("getNextMP3(): found %s after %s...\n", normalizePath(file.name()).c_str(), currentFile.c_str());
        root.close();
        return normalizePath(file.name());
      }
      if (name == currentLower) {
        foundCurrent = true;
        Serial.printf("getNextMP3(): found current %s\n", normalizePath(file.name()).c_str());
      }
    }

    file = root.openNextFile();
  }

  // End reached — wrap around and search from start again
  root.close();
  root = LittleFS.open("/");
  file = root.openNextFile();

  while (file) {
    String name = normalizePath(file.name());
    name.toLowerCase();

    if (name.endsWith(".mp3")) {
      // If we find the current file again, stop — nothing else available
      if (name == currentLower) {
        root.close();
        Serial.printf("getNextMP3(): found current again %s\n", normalizePath(file.name()).c_str());
        return "";  // no other MP3 found
      } else {
        root.close();
        Serial.printf("getNextMP3(): from the start found %s\n", normalizePath(file.name()).c_str());
        return normalizePath(file.name()); // found a new one before hitting current again
      }
    }

    file = root.openNextFile();
  }

  root.close();
  return ""; // no MP3 found at all
}

// -----------------------------------------------------------------------------------------------

// Finds the previous mp3 file. If there is none before, 
// goes to the end of the file system to look
String getPreviousMP3(const String &currentFile) {

  String currentLower = currentFile;
  currentLower.toLowerCase();

  File root = LittleFS.open("/");
  if (!root || !root.isDirectory()){
    Serial.println("getPreviousMP3(): failing open LittleFS...");
    return "";
  }

  File file = root.openNextFile();
  String lastMP3 = "";   // keep track of the previous valid MP3
  bool foundCurrent = false;

  while (file) {
    String name = normalizePath(file.name());
    name.toLowerCase();

    if (name.endsWith(".mp3")) {
      if (name == currentLower) {
        foundCurrent = true;
        Serial.printf("getPreviousMP3(): found current %s\n", normalizePath(file.name()).c_str());
        break;  // stop — current file found
      }
      lastMP3 = normalizePath(file.name());  // remember previous valid MP3
    }

    file = root.openNextFile();
  }

  root.close();

  // If we found the current file and there was a previous one
  if (foundCurrent && lastMP3 != ""){
    Serial.printf("getPreviousMP3(): previous mp3 found %s\n", lastMP3.c_str());
    return lastMP3;
  };

  // Otherwise, wrap around to the last MP3 in the directory
  root = LittleFS.open("/");
  file = root.openNextFile();
  String previous = "";

  while (file) {
    String name = normalizePath(file.name());
    name.toLowerCase();
    if (name.endsWith(".mp3")) {
      previous = normalizePath(file.name());  // last valid MP3 found so far
    }
    file = root.openNextFile();
  }

  root.close();

  // If the only MP3 we found when wrapping is the current one → stop
  if (previous == currentFile){
    Serial.printf("getPreviousMP3(): only found the current %s\n", previous.c_str());
    return "";
  } 

  Serial.printf("getPreviousMP3(): previous mp3 found %s\n", previous.c_str());
  return previous;
}

// -----------------------------------------------------------------------------------------------

// Plays a file
void startTrack(const String &filename) {
  if (filename == ""){
    Serial.println("startTrack(): empty filename...");
    return; // No file to play
  } 

  if (mp3) { 
    if (mp3->isRunning()) mp3->stop();
    delete mp3; mp3 = nullptr;
  }
  if (file) { delete file; file = nullptr; }
  if (out)  { delete out;  out = nullptr; }

  Serial.println("startTrack(): old audio objects deleted...");

  // Define the new audio objects
  file = new AudioFileSourceLittleFS(filename.c_str()); 
  out  = new AudioOutputI2S();

  // Define pins I2S (BCLK, LRCLK/WS, DOUT)
  out->SetPinout(BCLK, LRCLK_WS, DOUT); 
  out->SetBuffers(4, 1024);  // 4 buffers of 1024 bytes each, helps avoid pops, underruns, or distortions
  setVolumeSafe(volumeLevel);

  mp3  = new AudioGeneratorMP3();
  mp3->begin(file, out);

  trackStartTime = millis(); // Start timer
  trackElapsedTime = 0;
  Serial.printf("Now playing: %s\n", filename.c_str());
  inPause = false;
}

// -----------------------------------------------------------------------------------------------

// Replays the file from the point it was paused
void resumeTrack() {
  if (!mp3 || !out) {
    Serial.println("resumeTrack(): audio objects not initialized...");
    return;
  }

  // Close previous file source if it exists
  if (file) {
    delete file;
    file = nullptr;
  }

  // Reopen file fresh
  file = new AudioFileSourceLittleFS(currentFile.c_str());
  if (!file) {
    Serial.println("resumeTrack(): failed to reopen file");
    return;
  }

  // Seek to stored position
  if (pausedPos > 0 && !file->seek(pausedPos, SEEK_SET)) {
    Serial.println("resumeTrack(): seek failed, starting from 0");
    file->seek(0, SEEK_SET);
    pausedPos = 0;
  }

  // Restart decoder
  if (!mp3->begin(file, out)) {
    Serial.println("resumeTrack(): mp3 begin failed");
  } else {
    Serial.printf("Resumed from %lu bytes\n", pausedPos);
    trackStartTime = millis(); // Restart timer
    inPause = false;
  }
}

// -----------------------------------------------------------------------------------------------

// Pauses the track being played
void pauseTrack() {
  if (!out || !mp3) {
    Serial.println("pauseTrack(): audio objects not initialized...");
      return;
  }

  if (mp3->isRunning()) {
    pausedPos = file->getPos();
    mp3->stop();
    Serial.printf("Paused at %lu bytes\n", pausedPos);

    // Keeps track of for how long it was/is being played in total
    unsigned long now = millis();
    trackElapsedTime = trackElapsedTime + now - trackStartTime;
    trackStartTime = now; // Register the new start time
    Serial.printf("It was played during %lu seconds...\n", trackElapsedTime/1000);
    inPause = true;
  }
}

// -----------------------------------------------------------------------------------------------

// Configurations
void setup() {

  // LED module turns red
  rgbLedWrite(rgb_module, 50, 0, 0);  delay(1);

  // Initialize the serial port
  Serial.begin(115200);
  // Wait briefly for serial connection (only if a host is connected)
  unsigned long start = millis();
  while (!Serial && millis() - start < 2000) {
    delay(10);
  }

  // Initializes the buttons as inputs and with pullup resistors
  for (int i = 0; i < NUM_BOTOES; i++) {
    pinMode(pinsButtons[i], INPUT_PULLUP);
  }

  // Mounts the LittleFS file system
  Serial.println("Mounting LittleFS...");
  if(!LittleFS.begin()){
    Serial.println("Failed mounting LittleFS...");
    while(1){
      rgbLedWrite(rgb_module, 0, 0, 50);  delay(100);
      rgbLedWrite(rgb_module, 0, 0, 0);  delay(100);
    };
  };

  Serial.println("Selecting file to be read...");

  // Gets the first mp3 file in memory
  currentFile = getFirstMP3();
  if (currentFile == "") {
    Serial.println("setup(): No MP3 files found at the begining of the program...");
  } else {
    startTrack(currentFile);
  }

  // Configure timer0 of group0 — safe with Wi-Fi/Bluetooth
  // Set prescaler
  timer = timerBegin(1000000); // 80 MHz / 1000000 = 80 Hz -> increments every 12.5 ms
  // Attach onTimer function to our timer.
  timerAttachInterrupt(timer, &onTimer);
  // Set alarm to call onTimer function every 10 ms (value in microseconds).
  // Repeat the alarm (third parameter) with unlimited count = 0 (fourth parameter).
  timerAlarm(timer, 10000, true, 0);

  // LED module turns green
  rgbLedWrite(rgb_module, 0, 50, 0);  delay(1);
}

// -----------------------------------------------------------------------------------------------

// Main program
void loop() {

  if (mp3 && mp3->isRunning()) {
    if (!inPause) {
      // Does the audio loop only if not in pause and if there is an mp3 object created
      // If the loop() ended, the track reached its end
      if (!mp3->loop()) {
        mp3->stop();
        // Next track
        String next = getNextMP3(currentFile);
        if (next == "") {
          Serial.println("End of playlist reached. Replaying current track because it is the only one...");
          startTrack(currentFile);
        } else {
          // Plays next mp3 file
          currentFile = next;
          startTrack(currentFile);
        }
      }
    }
  }

  // Button Previous
  if (buttonPressed[0]) {
    buttonPressed[0] = false;
    Serial.println("Button Previous pressed...");

    // Stores for how long it has been playing
    // in order to decide if it goes to the beginning of
    // the track or for the previous track
    unsigned long elapsed = trackElapsedTime + millis() - trackStartTime;

    // If the button was pressed in the beginning of the song
    if (elapsed < TRACK_RESTART_THRESHOLD_MS ) {
      Serial.println("Button was pressed before the timewindow...");
      // Less than 5s → go to previous track
      String prev = getPreviousMP3(currentFile);
      if (prev != "") {
        currentFile = prev;
        startTrack(currentFile);
      } else {
        Serial.println("No previous track. Does nothing...");
      }
    } else {
      Serial.println("Button was pressed after the timewindow...");
      // Restart current track
      startTrack(currentFile);
    }
  }

  // Button Play/Pause
  else if (buttonPressed[1]) {
    buttonPressed[1] = false;

    // If it was paused
    if(inPause){

      Serial.println("Button Play/Pause pressed. Pause state detected...");
      resumeTrack();
    } 
    else{

      Serial.println("Button Play/Pause pressed. Play state detected...");
      pauseTrack();
    } 
  }

  // Button Next
  else if (buttonPressed[2]) {
    buttonPressed[2] = false;
    Serial.println("Button Next pressed...");

    String next = getNextMP3(currentFile);
    if (next == ""){
      Serial.println("No MP3 files found...");
    }
    else{
      currentFile = next;
      startTrack(currentFile); 
    }
  }

  // Button REC
  else if (buttonPressed[3]) {
    buttonPressed[3] = false;
    Serial.println("Button REC pressed...");
    // To be implemented...
  }

  // Button Volume Decrease
  else if (buttonPressed[4]) {
    buttonPressed[4] = false;

    if(isMute){
      volumeLevel = previousVolumeLevel;
      isMute = false;
    }
    // Increase volume
    volumeLevel = volumeLevel - VOLUME_STEP;

    setVolumeSafe(volumeLevel);
    Serial.println("Button Volume Decrease pressed...");
  }

  // Button Volume Mute
  else if (buttonPressed[5]) {
    buttonPressed[5] = false;

    // If it was muted
    if(isMute){
      volumeLevel = previousVolumeLevel;
      isMute = false;
    }
    else{
      previousVolumeLevel = volumeLevel;
      volumeLevel = 0.0;
      isMute = true;
    }
    setVolumeSafe(volumeLevel);
    Serial.println("Button Volume Mute pressed...");
  }

  // Button Volume Increase
  else if (buttonPressed[6]) {
    buttonPressed[6] = false;

    if(isMute){
      volumeLevel = previousVolumeLevel;
      isMute = false;
    }
    // Decrease volume
    volumeLevel = volumeLevel + VOLUME_STEP;
    setVolumeSafe(volumeLevel);
    Serial.println("Button Volume Increase pressed...");
  }
}


