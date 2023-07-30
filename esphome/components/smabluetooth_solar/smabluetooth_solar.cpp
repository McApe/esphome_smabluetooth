#include "smabluetooth_solar.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace smabluetooth_solar {


static const char *const TAG = "smabluetooth_solar";

void SmaBluetoothSolar::setup() {
  ESP_LOGW(TAG, "Starting setup...");
  //begin
   smaInverter = ESP32_SMA_Inverter::getInstance();
  ESP_LOGW(TAG, "Inverter/pw to setup... %s ", sma_inverter_bluetooth_mac_.c_str());
    smaInverter->setup(sma_inverter_bluetooth_mac_, sma_inverter_password_);
    nextTime = millis();

  hasSetup = true;
}

void SmaBluetoothSolar::loop() {

  if (!hasSetup) {
    return ;
  }
  int adjustedScanRate;
  if (nightTime)  // Scan every 15min
    adjustedScanRate = 900000;
  else
    adjustedScanRate = (60 * 1000); //todo adjust, take 60s for now

  if (!hasBegun){
    hasBegun = true;

    // *** Start BT
    ESP_LOGW(TAG, "start BT ");
    smaInverter->begin("ESP32toSMA", true); // "true" creates this device as a BT Master.
    App.feed_wdt();
  }

  //if not yet connected
  if (nextTime < millis() && !smaInverter->isBtConnected()) {
    nextTime = millis() + adjustedScanRate;

    //reset PcktID
    ESP_LOGW(TAG, "initPcktID ");
    smaInverter->initPcktID();

    //connect
    ESP_LOGW(TAG, "Connecting SMA inverter");
    if (smaInverter->connect()) {
      App.feed_wdt();
      // **** Initialize SMA *******
      ESP_LOGW(TAG, "BT connected");
      E_RC rc = smaInverter->initialiseSMAConnection();
      ESP_LOGI(TAG, "SMA %d \n", rc);

      App.feed_wdt();
      ESP_LOGW(TAG, "get signal strength");
      smaInverter->getBT_SignalStrength();

      App.feed_wdt();
      ESP_LOGW(TAG, "*** logonSMAInverter");
      rc = smaInverter->logonSMAInverter();
      ESP_LOGW(TAG, "Logon return code %d\n", rc);

      App.feed_wdt();
      //reading data

      //smaInverter->ReadCurrentData();
      //skip all for now and try individual
      if (smaInverter->isBtConnected()) {
        ESP_LOGD(TAG, "*** energyreadings");
        //get the inverter readings here 
        //rotate through these inverterDataTypes
        getInverterDataType invDataTypes[] = {
          EnergyProduction, SpotGridFrequency, SpotDCPower, SpotDCVoltage, SpotACPower, SpotACVoltage
        };
        int sizeOfArr = sizeof(invDataTypes) / sizeof(invDataTypes[0]);
        for (int iIdt=0;iIdt<sizeOfArr;iIdt++) {
        //for (getInverterDataType iIdt : invDataTypes) {
          App.feed_wdt(); // watch for ESP32 user task watchdog
          smaInverter->getInverterData(invDataTypes[iIdt]);
        }
      }

      smaInverter->disconnect(); //moved btConnected to inverter class

    }

  }

  App.feed_wdt();
  //delay(100);
}

/**
 * generic publish method 
*/
void SmaBluetoothSolar::updateSensor( sensor::Sensor *sensor,  String sensorName,  float publishValue) {
  ESP_LOGV(TAG, "update sensor %s ", sensorName.c_str());
  App.feed_wdt(); // watch for ESP32 user task watchdog

  if (publishValue != 0.0) {
    if (sensor!=nullptr) sensor->publish_state(publishValue);
      else ESP_LOGV(TAG, "No %s sensor ", sensorName.c_str());
  } else ESP_LOGV(TAG, "No %s value ", sensorName.c_str());

}

void SmaBluetoothSolar::update() {
  // If our last send has had no reply yet, and it wasn't that long ago, do nothing.
  uint32_t now = millis();

  this->running_update_ = true;

  ESP_LOGV(TAG, "update sensors ");

  updateSensor(today_production_, String("EToday"), smaInverter->dispData.EToday);
  updateSensor(total_energy_production_, String("ETotal"), smaInverter->dispData.ETotal);
  updateSensor(grid_frequency_sensor_, String("Freq"), smaInverter->dispData.Freq);
  updateSensor(pvs_[0].voltage_sensor_, String("UdcA"), smaInverter->dispData.Udc[0]);
  updateSensor(pvs_[0].current_sensor_, String("IdcA"), smaInverter->dispData.Idc[0]);
  updateSensor(pvs_[0].active_power_sensor_, String("PDC"), smaInverter->dispData.Wdc[0]);

  updateSensor(phases_[0].voltage_sensor_, String("UacA"), smaInverter->dispData.Uac[0]);
  updateSensor(phases_[0].current_sensor_, String("IacA"), smaInverter->dispData.Iac[0]);
  //updateSensor(phases_[0].active_power_sensor_, "UacA", smaInverter->dispData.Uac[0]; // doest exist, could be calculated

	this->running_update_ = false;

  this->last_send_ = millis();
}

void SmaBluetoothSolar::on_inverter_data(const std::vector<uint8_t> &data) {
  // Other components might be sending commands to our device. But we don't get called with enough
  // context to know what is what. So if we didn't do a send, we ignore the data.
  ESP_LOGVV(TAG, "on inverter data ");
  if (!this->last_send_)
    return;
  this->last_send_ = 0;

  auto publish_1_reg_sensor_state = [&](sensor::Sensor *sensor, size_t i, float unit) -> void {
    if (sensor == nullptr)
      return;
    float value = encode_uint16(data[i * 2], data[i * 2 + 1]) * unit;
    sensor->publish_state(value);
  };

  auto publish_2_reg_sensor_state = [&](sensor::Sensor *sensor, size_t reg1, size_t reg2, float unit) -> void {
    float value = ((encode_uint16(data[reg1 * 2], data[reg1 * 2 + 1]) << 16) +
                   encode_uint16(data[reg2 * 2], data[reg2 * 2 + 1])) *
                  unit;
    if (sensor != nullptr)
      sensor->publish_state(value);
  };

  switch (this->protocol_version_) {
    case SMANET2: {
      publish_1_reg_sensor_state(this->inverter_status_, 0, 1);

      publish_2_reg_sensor_state(this->pv_active_power_sensor_, 1, 2, ONE_DEC_UNIT);

      publish_1_reg_sensor_state(this->pvs_[0].voltage_sensor_, 3, ONE_DEC_UNIT);
      publish_1_reg_sensor_state(this->pvs_[0].current_sensor_, 4, ONE_DEC_UNIT);
      publish_2_reg_sensor_state(this->pvs_[0].active_power_sensor_, 5, 6, ONE_DEC_UNIT);

      publish_1_reg_sensor_state(this->pvs_[1].voltage_sensor_, 7, ONE_DEC_UNIT);
      publish_1_reg_sensor_state(this->pvs_[1].current_sensor_, 8, ONE_DEC_UNIT);
      publish_2_reg_sensor_state(this->pvs_[1].active_power_sensor_, 9, 10, ONE_DEC_UNIT);

      publish_2_reg_sensor_state(this->grid_active_power_sensor_, 11, 12, ONE_DEC_UNIT);
      publish_1_reg_sensor_state(this->grid_frequency_sensor_, 13, TWO_DEC_UNIT);

      publish_1_reg_sensor_state(this->phases_[0].voltage_sensor_, 14, ONE_DEC_UNIT);
      publish_1_reg_sensor_state(this->phases_[0].current_sensor_, 15, ONE_DEC_UNIT);
      publish_2_reg_sensor_state(this->phases_[0].active_power_sensor_, 16, 17, ONE_DEC_UNIT);

      publish_1_reg_sensor_state(this->phases_[1].voltage_sensor_, 18, ONE_DEC_UNIT);
      publish_1_reg_sensor_state(this->phases_[1].current_sensor_, 19, ONE_DEC_UNIT);
      publish_2_reg_sensor_state(this->phases_[1].active_power_sensor_, 20, 21, ONE_DEC_UNIT);

      publish_1_reg_sensor_state(this->phases_[2].voltage_sensor_, 22, ONE_DEC_UNIT);
      publish_1_reg_sensor_state(this->phases_[2].current_sensor_, 23, ONE_DEC_UNIT);
      publish_2_reg_sensor_state(this->phases_[2].active_power_sensor_, 24, 25, ONE_DEC_UNIT);

      publish_2_reg_sensor_state(this->today_production_, 26, 27, ONE_DEC_UNIT);
      publish_2_reg_sensor_state(this->total_energy_production_, 28, 29, ONE_DEC_UNIT);

      publish_1_reg_sensor_state(this->inverter_module_temp_, 32, ONE_DEC_UNIT);
      break;
    }
    default: {
      publish_1_reg_sensor_state(this->inverter_status_, 0, 1);

      publish_2_reg_sensor_state(this->pv_active_power_sensor_, 1, 2, ONE_DEC_UNIT);

      publish_1_reg_sensor_state(this->pvs_[0].voltage_sensor_, 3, ONE_DEC_UNIT);
      publish_1_reg_sensor_state(this->pvs_[0].current_sensor_, 4, ONE_DEC_UNIT);
      publish_2_reg_sensor_state(this->pvs_[0].active_power_sensor_, 5, 6, ONE_DEC_UNIT);

      publish_1_reg_sensor_state(this->pvs_[1].voltage_sensor_, 7, ONE_DEC_UNIT);
      publish_1_reg_sensor_state(this->pvs_[1].current_sensor_, 8, ONE_DEC_UNIT);
      publish_2_reg_sensor_state(this->pvs_[1].active_power_sensor_, 9, 10, ONE_DEC_UNIT);

      publish_2_reg_sensor_state(this->grid_active_power_sensor_, 35, 36, ONE_DEC_UNIT);
      publish_1_reg_sensor_state(this->grid_frequency_sensor_, 37, TWO_DEC_UNIT);

      publish_1_reg_sensor_state(this->phases_[0].voltage_sensor_, 38, ONE_DEC_UNIT);
      publish_1_reg_sensor_state(this->phases_[0].current_sensor_, 39, ONE_DEC_UNIT);
      publish_2_reg_sensor_state(this->phases_[0].active_power_sensor_, 40, 41, ONE_DEC_UNIT);

      publish_1_reg_sensor_state(this->phases_[1].voltage_sensor_, 42, ONE_DEC_UNIT);
      publish_1_reg_sensor_state(this->phases_[1].current_sensor_, 43, ONE_DEC_UNIT);
      publish_2_reg_sensor_state(this->phases_[1].active_power_sensor_, 44, 45, ONE_DEC_UNIT);

      publish_1_reg_sensor_state(this->phases_[2].voltage_sensor_, 46, ONE_DEC_UNIT);
      publish_1_reg_sensor_state(this->phases_[2].current_sensor_, 47, ONE_DEC_UNIT);
      publish_2_reg_sensor_state(this->phases_[2].active_power_sensor_, 48, 49, ONE_DEC_UNIT);

      publish_2_reg_sensor_state(this->today_production_, 53, 54, ONE_DEC_UNIT);
      publish_2_reg_sensor_state(this->total_energy_production_, 55, 56, ONE_DEC_UNIT);

      publish_1_reg_sensor_state(this->inverter_module_temp_, 93, ONE_DEC_UNIT);
      break;
    }
  }
}

void SmaBluetoothSolar::dump_config() {
  ESP_LOGCONFIG(TAG, "SMABluetooth Solar:");
  ESP_LOGCONFIG(TAG, "  Address: %s", sma_inverter_bluetooth_mac_.c_str());
}

}  // namespace smabluetooth_solar
}  // namespace esphome
