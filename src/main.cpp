/*

//////////////////////// ESP32-Paxcounter \\\\\\\\\\\\\\\\\\\\\\\\\\

Copyright  2018-2020 Oliver Brandmueller <ob@sysadm.in>
Copyright  2018-2020 Klaus Wilting <verkehrsrot@arcor.de>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

NOTE:
Parts of the source files in this repository are made available under different
licenses. Refer to LICENSE.txt file in repository for more details.

//////////////////////// ESP32-Paxcounter \\\\\\\\\\\\\\\\\\\\\\\\\\

// Tasks and timers:

Task          Core  Prio  Purpose
-------------------------------------------------------------------------------
ledloop       0     3     blinks LEDs
spiloop       0     2     reads/writes data on spi interface
IDLE          0     0     ESP32 arduino scheduler -> runs wifi sniffer

lmictask      1     2     MCCI LMiC LORAWAN stack
clockloop     1     4     generates realtime telegrams for external clock
mqttloop      1     2     reads/writes data on ETH interface
timesync_proc 1     3     processes realtime time sync requests
irqhandler    1     2     cyclic tasks (i.e. displayrefresh) triggered by timers
gpsloop       1     1     reads data from GPS via serial or i2c
lorasendtask  1     1     feeds data from lora sendqueue to lmcic
macprocess    1     1     MAC analyzer loop
rmcd_process  1     1     Remote command interpreter loop
IDLE          1     0     ESP32 arduino scheduler -> runs wifi channel rotator

Low priority numbers denote low priority tasks.

NOTE: Changing any timings will have impact on time accuracy of whole code.
So don't do it if you do not own a digital oscilloscope.

// ESP32 hardware timers
-------------------------------------------------------------------------------
0	displayIRQ -> display refresh -> 40ms (DISPLAYREFRESH_MS)
1 ppsIRQ -> pps clock irq -> 1sec
3	MatrixDisplayIRQ -> matrix mux cycle -> 0,5ms (MATRIX_DISPLAY_SCAN_US)


// Interrupt routines
-------------------------------------------------------------------------------

irqHandlerTask (Core 1), see irqhandler.cpp

fired by hardware
DisplayIRQ      -> esp32 timer 0
CLOCKIRQ        -> esp32 timer 1 or external GPIO (RTC_INT or GPS_INT)
MatrixDisplayIRQ-> esp32 timer 3
ButtonIRQ       -> external GPIO
PMUIRQ          -> PMU chip GPIO

fired by software (Ticker.h)
TIMESYNC_IRQ    -> setTimeSyncIRQ()
CYCLIC_IRQ      -> setCyclicIRQ()
SENDCYCLE_IRQ   -> setSendIRQ()
BME_IRQ         -> setBMEIRQ()

ClockTask (Core 1), see timekeeper.cpp

fired by hardware
CLOCKIRQ        -> esp32 timer 1


// External RTC timer (if present)
-------------------------------------------------------------------------------
triggers pps 1 sec impulse

*/

// Basic Config
#include "main.h"
#include "libpax_helpers.h"

configData_t cfg; // struct holds current device configuration
char lmic_event_msg[LMIC_EVENTMSG_LEN]; // display buffer for LMIC event message
uint8_t batt_level = 0;                 // display value
#if !(LIBPAX)   
uint8_t volatile channel = WIFI_CHANNEL_MIN;   // channel rotation counter
#endif
uint8_t volatile rf_load = 0;                  // RF traffic indicator
#if !(LIBPAX)   
uint16_t volatile macs_wifi = 0, macs_ble = 0; // globals for display
#endif

hw_timer_t *ppsIRQ = NULL, *displayIRQ = NULL, *matrixDisplayIRQ = NULL;

TaskHandle_t irqHandlerTask = NULL, ClockTask = NULL;
SemaphoreHandle_t I2Caccess;
bool volatile TimePulseTick = false;
timesource_t timeSource = _unsynced;

// container holding unique MAC address hashes with Memory Alloctor using PSRAM,
// if present
DRAM_ATTR std::set<uint16_t, std::less<uint16_t>, Mallocator<uint16_t>> macs;

// initialize payload encoder
PayloadConvert payload(PAYLOAD_BUFFER_SIZE);

// set Time Zone for user setting from paxcounter.conf
TimeChangeRule myDST = DAYLIGHT_TIME;
TimeChangeRule mySTD = STANDARD_TIME;
Timezone myTZ(myDST, mySTD);


// local Tag for logging
static const char TAG[] = __FILE__;

void setup() {

  char features[100] = "";

  // create some semaphores for syncing / mutexing tasks
  I2Caccess = xSemaphoreCreateMutex(); // for access management of i2c bus
  _ASSERT(I2Caccess != NULL);
  I2C_MUTEX_UNLOCK();

  // disable brownout detection
#ifdef DISABLE_BROWNOUT
  // register with brownout is at address DR_REG_RTCCNTL_BASE + 0xd4
  (*((uint32_t volatile *)ETS_UNCACHED_ADDR((DR_REG_RTCCNTL_BASE + 0xd4)))) = 0;
#endif

  // setup debug output or silence device
#if (VERBOSE)
  Serial.begin(115200);
  esp_log_level_set("*", ESP_LOG_VERBOSE);
#else
  // mute logs completely by redirecting them to silence function
  esp_log_level_set("*", ESP_LOG_NONE);
#endif

  // load device configuration from NVRAM and set runmode
  do_after_reset();

  // print chip information on startup if in verbose mode after coldstart
#if (VERBOSE)

  if (RTC_runmode == RUNMODE_POWERCYCLE) {
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG,
             "This is ESP32 chip with %d CPU cores, WiFi%s%s, silicon revision "
             "%d, %dMB %s Flash",
             chip_info.cores,
             (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
             (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "",
             chip_info.revision, spi_flash_get_chip_size() / (1024 * 1024),
             (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded"
                                                           : "external");
    ESP_LOGI(TAG, "Internal Total heap %d, internal Free Heap %d",
             ESP.getHeapSize(), ESP.getFreeHeap());
#ifdef BOARD_HAS_PSRAM
    ESP_LOGI(TAG, "SPIRam Total heap %d, SPIRam Free Heap %d",
             ESP.getPsramSize(), ESP.getFreePsram());
#endif
    ESP_LOGI(TAG, "ChipRevision %d, Cpu Freq %d, SDK Version %s",
             ESP.getChipRevision(), ESP.getCpuFreqMHz(), ESP.getSdkVersion());
    ESP_LOGI(TAG, "Flash Size %d, Flash Speed %d", ESP.getFlashChipSize(),
             ESP.getFlashChipSpeed());
    ESP_LOGI(TAG, "Wifi/BT software coexist version %s",
             esp_coex_version_get());

#if (HAS_LORA)
    ESP_LOGI(TAG, "IBM LMIC version %d.%d.%d", LMIC_VERSION_MAJOR,
             LMIC_VERSION_MINOR, LMIC_VERSION_BUILD);
    ESP_LOGI(TAG, "Arduino LMIC version %d.%d.%d.%d",
             ARDUINO_LMIC_VERSION_GET_MAJOR(ARDUINO_LMIC_VERSION),
             ARDUINO_LMIC_VERSION_GET_MINOR(ARDUINO_LMIC_VERSION),
             ARDUINO_LMIC_VERSION_GET_PATCH(ARDUINO_LMIC_VERSION),
             ARDUINO_LMIC_VERSION_GET_LOCAL(ARDUINO_LMIC_VERSION));
    showLoraKeys();
#endif // HAS_LORA

#if (HAS_GPS)
    ESP_LOGI(TAG, "TinyGPS+ version %s", TinyGPSPlus::libraryVersion());
#endif
  }
#endif // VERBOSE

  // open i2c bus
  i2c_init();

// setup power on boards with power management logic
#ifdef EXT_POWER_SW
  pinMode(EXT_POWER_SW, OUTPUT);
  digitalWrite(EXT_POWER_SW, EXT_POWER_ON);
  strcat_P(features, " VEXT");
#endif

#if defined HAS_PMU || defined HAS_IP5306
#ifdef HAS_PMU
  AXP192_init();
#elif defined HAS_IP5306
  IP5306_init();
#endif
  strcat_P(features, " PMU");
#endif

  // now that we are powered, we scan i2c bus for devices
  if (RTC_runmode == RUNMODE_POWERCYCLE)
    i2c_scan();

// initialize display
#ifdef HAS_DISPLAY
  strcat_P(features, " OLED");
  DisplayIsOn = cfg.screenon;
  // display verbose info only after a coldstart (note: blocking call!)
  dp_init(RTC_runmode == RUNMODE_POWERCYCLE ? true : false);
#endif

#ifdef BOARD_HAS_PSRAM
  _ASSERT(psramFound());
  ESP_LOGI(TAG, "PSRAM found and initialized");
  strcat_P(features, " PSRAM");
#endif

#ifdef BAT_MEASURE_EN
  pinMode(BAT_MEASURE_EN, OUTPUT);
#endif

// initialize leds
#if (HAS_LED != NOT_A_PIN)
  pinMode(HAS_LED, OUTPUT);
  strcat_P(features, " LED");

#ifdef LED_POWER_SW
  pinMode(LED_POWER_SW, OUTPUT);
  digitalWrite(LED_POWER_SW, LED_POWER_ON);
#endif

#ifdef HAS_TWO_LED
  pinMode(HAS_TWO_LED, OUTPUT);
  strcat_P(features, " LED1");
#endif

// use LED for power display if we have additional RGB LED, else for status
#ifdef HAS_RGB_LED
  switch_LED(LED_ON);
  strcat_P(features, " RGB");
#endif

#endif // HAS_LED

#if (HAS_LED != NOT_A_PIN) || defined(HAS_RGB_LED)
  // start led loop
  ESP_LOGI(TAG, "Starting LED Controller...");
  xTaskCreatePinnedToCore(ledLoop,      // task function
                          "ledloop",    // name of task
                          1024,         // stack size of task
                          (void *)1,    // parameter of the task
                          3,            // priority of the task
                          &ledLoopTask, // task handle
                          0);           // CPU core
#endif

// initialize wifi antenna
#ifdef HAS_ANTENNA_SWITCH
  strcat_P(features, " ANT");
  antenna_init();
  antenna_select(cfg.wifiant);
#endif

// initialize battery status
#if (defined BAT_MEASURE_ADC || defined HAS_PMU || defined HAS_IP5306)
  strcat_P(features, " BATT");
  calibrate_voltage();
  batt_level = read_battlevel();
#ifdef HAS_IP5306
  printIP5306Stats();
#endif
#endif

#if (USE_OTA)
  strcat_P(features, " OTA");
  // reboot to firmware update mode if ota trigger switch is set
  if (RTC_runmode == RUNMODE_UPDATE)
    start_ota_update();
#endif

#if (BOOTMENU)
  // start local webserver after each coldstart
  if (RTC_runmode == RUNMODE_POWERCYCLE)
    start_boot_menu();
#endif

  // start local webserver on rcommand request
  if (RTC_runmode == RUNMODE_MAINTENANCE)
    start_boot_menu();

#if !(LIBPAX)   
  // start mac processing task
  ESP_LOGI(TAG, "Starting MAC processor...");
  macQueueInit();
#else
  ESP_LOGI(TAG, "Starting libpax...");
#if (defined WIFICOUNTER || defined BLECOUNTER) 
  struct libpax_config_t configuration;
  libpax_default_config(&configuration);
  ESP_LOGI(TAG, "BLESCAN: %d", cfg.blescan);
  ESP_LOGI(TAG, "WIFISCAN: %d", cfg.wifiscan);
  configuration.wificounter = cfg.wifiscan;
  configuration.blecounter = cfg.blescan;

  configuration.wifi_channel_map = WIFI_CHANNEL_ALL;
  configuration.wifi_channel_switch_interval = cfg.wifichancycle;
  configuration.wifi_rssi_threshold = cfg.rssilimit;

  configuration.blescantime = cfg.blescantime;

  int config_update = libpax_update_config(&configuration);
  if(config_update != 0) {
    ESP_LOGE(TAG, "Error in libpax configuration.");
  } else {
    init_libpax();
  }
#endif
#endif

  // start rcommand processing task
  ESP_LOGI(TAG, "Starting rcommand interpreter...");
  rcmd_init();

// start BLE scan callback if BLE function is enabled in NVRAM configuration
// or remove bluetooth stack from RAM, if option bluetooth is not compiled
#if (BLECOUNTER)
  strcat_P(features, " BLE");
#if !(LIBPAX)   
  if (cfg.blescan) {
    ESP_LOGI(TAG, "Starting Bluetooth...");
    start_BLEscan();
  } else
    btStop();
#endif
#else
  // remove bluetooth stack to gain more free memory
#if !(LIBPAX)   
  btStop();
  esp_bt_mem_release(ESP_BT_MODE_BTDM);
  esp_coex_preference_set(
      ESP_COEX_PREFER_WIFI); // configure Wifi/BT coexist lib
#endif

#endif

// initialize gps
#if (HAS_GPS)
  strcat_P(features, " GPS");
  if (gps_init()) {
    ESP_LOGI(TAG, "Starting GPS Feed...");
    xTaskCreatePinnedToCore(gps_loop,  // task function
                            "gpsloop", // name of task
                            4096,      // stack size of task
                            (void *)1, // parameter of the task
                            1,         // priority of the task
                            &GpsTask,  // task handle
                            1);        // CPU core
  }
#endif

// initialize sensors
#if (HAS_SENSORS)
#if (HAS_SENSOR_1)
#if (COUNT_ENS)
  ESP_LOGI(TAG, "init CWA-counter");
  if (cwa_init())
    strcat_P(features, " CWA");
#else
  strcat_P(features, " SENS(1)");
  sensor_init();
#endif
#endif
#if (HAS_SENSOR_2)
  strcat_P(features, " SENS(2)");
  sensor_init();
#endif
#if (HAS_SENSOR_3)
  strcat_P(features, " SENS(3)");
  sensor_init();
#endif
#endif

// initialize LoRa
#if (HAS_LORA)
  strcat_P(features, " LORA");
  _ASSERT(lmic_init() == ESP_OK);
#endif

// initialize SPI
#ifdef HAS_SPI
  strcat_P(features, " SPI");
  _ASSERT(spi_init() == ESP_OK);
#endif

// initialize MQTT
#ifdef HAS_MQTT
  strcat_P(features, " MQTT");
  _ASSERT(mqtt_init() == ESP_OK);
#endif

#if (HAS_SDCARD)
  if (sdcard_init())
    strcat_P(features, " SD");
#endif

#if (HAS_SDS011)
  ESP_LOGI(TAG, "init fine-dust-sensor");
  if (sds011_init())
    strcat_P(features, " SDS");
#endif

#if (MACFILTER)
  strcat_P(features, " FILTER");
#endif

// initialize matrix display
#ifdef HAS_MATRIX_DISPLAY
  strcat_P(features, " LED_MATRIX");
  MatrixDisplayIsOn = cfg.screenon;
  init_matrix_display(); // note: blocking call
#endif

// initialize matrix display
#ifdef HAS_E_PAPER_DISPLAY
  strcat_P(features, " E-INK display");
  E_paper_displayIsOn = cfg.screenon;
  ePaper_init(RTC_runmode == RUNMODE_POWERCYCLE);
#endif

// show payload encoder
#if PAYLOAD_ENCODER == 1
  strcat_P(features, " PLAIN");
#elif PAYLOAD_ENCODER == 2
  strcat_P(features, " PACKED");
#elif PAYLOAD_ENCODER == 3
  strcat_P(features, " LPPDYN");
#elif PAYLOAD_ENCODER == 4
  strcat_P(features, " LPPPKD");
#endif

// initialize RTC
#ifdef HAS_RTC
  strcat_P(features, " RTC");
  _ASSERT(rtc_init());
#endif

#if defined HAS_DCF77
  strcat_P(features, " DCF77");
#endif

#if defined HAS_IF482
  strcat_P(features, " IF482");
#endif

#if (WIFICOUNTER)
  strcat_P(features, " WIFI");
#if !(LIBPAX)   
  // install wifi driver in RAM and start channel hopping
  wifi_sniffer_init();
  // start wifi sniffing, if enabled
  if (cfg.wifiscan) {
    ESP_LOGI(TAG, "Starting Wifi...");
    switch_wifi_sniffer(1);
  } else
    switch_wifi_sniffer(0);
#endif

#else
  // remove wifi driver from RAM, if option wifi not compiled
  esp_wifi_deinit();
#endif

  // initialize salt value using esp_random() called by random() in
  // arduino-esp32 core. Note: do this *after* wifi has started, since
  // function gets it's seed from RF noise
#if !(LIBPAX)   
  reset_counters();
#endif

  // start state machine
  ESP_LOGI(TAG, "Starting Interrupt Handler...");
  xTaskCreatePinnedToCore(irqHandler,      // task function
                          "irqhandler",    // name of task
                          4096,            // stack size of task
                          (void *)1,       // parameter of the task
                          2,               // priority of the task
                          &irqHandlerTask, // task handle
                          1);              // CPU core

// initialize BME sensor (BME280/BME680)
#if (HAS_BME)
#ifdef HAS_BME680
  strcat_P(features, " BME680");
#elif defined HAS_BME280
  strcat_P(features, " BME280");
#elif defined HAS_BMP180
  strcat_P(features, " BMP180");
#endif
  if (bme_init())
    ESP_LOGI(TAG, "BME sensor initialized");
  else {
    ESP_LOGE(TAG, "BME sensor could not be initialized");
    cfg.payloadmask &= ~MEMS_DATA; // switch off transmit of BME data
  }
#endif

  // starting timers and interrupts
  _ASSERT(irqHandlerTask != NULL); // has interrupt handler task started?
  ESP_LOGI(TAG, "Starting Timers...");

// display interrupt
#ifdef HAS_DISPLAY
  dp_clear();
  dp_contrast(DISPLAYCONTRAST);
  // https://techtutorialsx.com/2017/10/07/esp32-arduino-timer-interrupts/
  // prescaler 80 -> divides 80 MHz CPU freq to 1 MHz, timer 0, count up
  displayIRQ = timerBegin(0, 80, true);
  timerAttachInterrupt(displayIRQ, &DisplayIRQ, true);
  timerAlarmWrite(displayIRQ, DISPLAYREFRESH_MS * 1000, true);
  timerAlarmEnable(displayIRQ);
#endif

// LED Matrix display interrupt
#ifdef HAS_MATRIX_DISPLAY
  // https://techtutorialsx.com/2017/10/07/esp32-arduino-timer-interrupts/
  // prescaler 80 -> divides 80 MHz CPU freq to 1 MHz, timer 3, count up
  matrixDisplayIRQ = timerBegin(3, 80, true);
  timerAttachInterrupt(matrixDisplayIRQ, &MatrixDisplayIRQ, true);
  timerAlarmWrite(matrixDisplayIRQ, MATRIX_DISPLAY_SCAN_US, true);
  timerAlarmEnable(matrixDisplayIRQ);
#endif

// initialize button
#ifdef HAS_BUTTON
  strcat_P(features, " BTN_");
#ifdef BUTTON_PULLUP
  strcat_P(features, "PU");
#else
  strcat_P(features, "PD");
#endif // BUTTON_PULLUP
  button_init(HAS_BUTTON);
#endif // HAS_BUTTON

  // cyclic function interrupts
  sendTimer.attach(cfg.sendcycle * 2, setSendIRQ);
  cyclicTimer.attach(HOMECYCLE, setCyclicIRQ);

// only if we have a timesource we do timesync
#if ((TIME_SYNC_LORAWAN) || (TIME_SYNC_LORASERVER) || (HAS_GPS) ||             \
     defined HAS_RTC)

#if (defined HAS_IF482 || defined HAS_DCF77)
  ESP_LOGI(TAG, "Starting Clock Controller...");
  clock_init();
#endif

#if (TIME_SYNC_LORASERVER) || (TIME_SYNC_LORAWAN)
  timesync_init(); // create loraserver time sync task
#endif

  ESP_LOGI(TAG, "Starting Timekeeper...");
  _ASSERT(timepulse_init()); // setup pps timepulse
  timepulse_start();         // starts pps and cyclic time sync
  strcat_P(features, " TIME");

#endif // timesync

  // show compiled features
  ESP_LOGI(TAG, "Features:%s", features);

  // set runmode to normal
  RTC_runmode = RUNMODE_NORMAL;

  vTaskDelete(NULL);

} // setup()

void loop() { vTaskDelete(NULL); }
