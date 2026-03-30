/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <bsp/esp-bsp.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_ota.h>
#include <nvs_flash.h>

#include <app_openthread_config.h>
#include <app_reset.h>
#include <common_macros.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dht.h"

#include "ml_context.h"

static const char *TAG = "app_main";

#define DHT_GPIO GPIO_NUM_2

// DHT22 is often referred to as AM2302/AM2301 in libs.
// If this doesn't compile, open managed_components/esp-idf-lib__dht/include/dht.h
// and choose the correct type macro from there.
#define DHT_TYPE DHT_TYPE_AM2301

static uint16_t g_temp_endpoint_id = 0;
static uint16_t g_humidity_endpoint_id = 0;

// Vendor-specific cluster for context classification result.
// Cluster IDs 0xFC00–0xFFFE are reserved for manufacturer use in Matter.
static constexpr uint32_t kContextClusterId = 0xFC00;
static constexpr uint32_t kContextAttributeId = 0x0000;

// Will be set to the same endpoint as the temperature sensor
static uint16_t g_context_endpoint_id = 0;

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace esp_matter::cluster;
using namespace chip::app::Clusters;

// Application cluster specification, 7.18.2.11. Temperature
// represents a temperature on the Celsius scale with a resolution of 0.01°C.
// temp = (temperature in °C) x 100
static void temp_sensor_notification(uint16_t endpoint_id, float temp, void *user_data)
{
    // schedule the attribute update so that we can report it from matter thread
    chip::DeviceLayer::SystemLayer().ScheduleLambda([endpoint_id, temp]() {
        attribute_t * attribute = attribute::get(endpoint_id,
                                                 TemperatureMeasurement::Id,
                                                 TemperatureMeasurement::Attributes::MeasuredValue::Id);

        esp_matter_attr_val_t val = esp_matter_invalid(NULL);
        attribute::get_val(attribute, &val);
        val.val.i16 = static_cast<int16_t>(temp * 100);

        attribute::update(endpoint_id, TemperatureMeasurement::Id, TemperatureMeasurement::Attributes::MeasuredValue::Id, &val);
    });
}

// Application cluster specification, 2.6.4.1. MeasuredValue Attribute
// represents the humidity in percent.
// humidity = (humidity in %) x 100
static void humidity_sensor_notification(uint16_t endpoint_id, float humidity, void *user_data)
{
    // schedule the attribute update so that we can report it from matter thread
    chip::DeviceLayer::SystemLayer().ScheduleLambda([endpoint_id, humidity]() {
        attribute_t * attribute = attribute::get(endpoint_id,
                                                 RelativeHumidityMeasurement::Id,
                                                 RelativeHumidityMeasurement::Attributes::MeasuredValue::Id);

        esp_matter_attr_val_t val = esp_matter_invalid(NULL);
        attribute::get_val(attribute, &val);
        val.val.u16 = static_cast<uint16_t>(humidity * 100);

        attribute::update(endpoint_id, RelativeHumidityMeasurement::Id, RelativeHumidityMeasurement::Attributes::MeasuredValue::Id, &val);
    });
}

static void dht_task(void *pvParameters)
{
    float temperature = 0.0f;
    float humidity = 0.0f;

    while (true) {
        esp_err_t err = dht_read_float_data(DHT_TYPE, DHT_GPIO, &humidity, &temperature);

        if (err == ESP_OK) {
            temp_sensor_notification(g_temp_endpoint_id, temperature, nullptr);
            humidity_sensor_notification(g_humidity_endpoint_id, humidity, nullptr);

            ESP_LOGI(TAG, "DHT22: T=%.2fC H=%.2f%%", temperature, humidity);

            // Run TFLM context classifier and update the custom attribute.
            // ScheduleLambda posts the attribute update onto the Matter thread —
            // attribute::update must not be called from a FreeRTOS task directly.
            int context = ml_context_run(temperature, humidity);
            if (context >= 0) {
                chip::DeviceLayer::SystemLayer().ScheduleLambda([context]() {
                    esp_matter_attr_val_t val = esp_matter_float(static_cast<float>(context));
                    attribute::update(g_context_endpoint_id,
                                      kContextClusterId,
                                      kContextAttributeId,
                                      &val);
                });
            }
        } else {
            ESP_LOGE(TAG, "DHT22 read failed: %s", esp_err_to_name(err));
        }

        // DHT22: don’t poll faster than ~2s
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static esp_err_t factory_reset_button_register()
{
    button_handle_t push_button;
    esp_err_t err = bsp_iot_button_create(&push_button, NULL, BSP_BUTTON_NUM);
    VerifyOrReturnError(err == ESP_OK, err);
    return app_reset_button_register(push_button);
}

static void open_commissioning_window_if_necessary()
{
    VerifyOrReturn(chip::Server::GetInstance().GetFabricTable().FabricCount() == 0);

    chip::CommissioningWindowManager & commissionMgr = chip::Server::GetInstance().GetCommissioningWindowManager();
    VerifyOrReturn(commissionMgr.IsCommissioningWindowOpen() == false);

    // After removing last fabric, this example does not remove the Wi-Fi credentials
    // and still has IP connectivity so, only advertising on DNS-SD.
    CHIP_ERROR err = commissionMgr.OpenBasicCommissioningWindow(chip::System::Clock::Seconds16(300),
                                    chip::CommissioningWindowAdvertisement::kDnssdOnly);
    if (err != CHIP_NO_ERROR)
    {
        ESP_LOGE(TAG, "Failed to open commissioning window, err:%" CHIP_ERROR_FORMAT, err.Format());
    }
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        break;

    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "Commissioning failed, fail safe timer expired");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        ESP_LOGI(TAG, "Fabric removed successfully");
        open_commissioning_window_if_necessary();
        break;

    case chip::DeviceLayer::DeviceEventType::kBLEDeinitialized:
        ESP_LOGI(TAG, "BLE deinitialized and memory reclaimed");
        break;

    default:
        break;
    }
}

// This callback is invoked when clients interact with the Identify Cluster.
// In the callback implementation, an endpoint can identify itself. (e.g., by flashing an LED or light).
static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identification callback: type: %u, effect: %u, variant: %u", type, effect_id, effect_variant);
    return ESP_OK;
}

// This callback is called for every attribute update. The callback implementation shall
// handle the desired attributes and return an appropriate error code. If the attribute
// is not of your interest, please do not return an error code and strictly return ESP_OK.
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    // Since this is just a sensor and we don't expect any writes on our temperature sensor,
    // so, return success.
    return ESP_OK;
}

static esp_err_t add_context_cluster(endpoint_t *ep)
{
    // Create a vendor-specific cluster on the temperature sensor endpoint.
    // CLUSTER_FLAG_SERVER means this device is the data source (server side).
    cluster_t *cluster = cluster::create(ep, kContextClusterId, CLUSTER_FLAG_SERVER);
    if (!cluster) {
        ESP_LOGE(TAG, "Failed to create context cluster");
        return ESP_FAIL;
    }

    // Add mandatory global attributes that every Matter cluster must have.
    // The low-level cluster::create() does NOT add these automatically —
    // only the higher-level standard cluster wrappers do.
    // Without ClusterRevision (0xFFFD) and FeatureMap (0xFFFC), the CHIP SDK
    // rejects the cluster as malformed and drops all its attribute reports.
    global::attribute::create_cluster_revision(cluster, 1);
    global::attribute::create_feature_map(cluster, 0);

    // Single float attribute to hold the predicted class ID:
    // 0.0 = HEATING_ON, 1.0 = NORMAL, 2.0 = WINDOW_OPEN
    // Initialised to 1.0 (NORMAL) as a safe default.
    esp_matter_attr_val_t init_val = esp_matter_float(1.0f);
    attribute_t *attr = attribute::create(cluster, kContextAttributeId,
                                          ATTRIBUTE_FLAG_NONE, init_val);
    if (!attr) {
        ESP_LOGE(TAG, "Failed to create context attribute");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Context cluster 0x%04" PRIx32 " created on endpoint %d",
             kContextClusterId, endpoint::get_id(ep));
    return ESP_OK;
}

extern "C" void app_main()
{
    /* Initialize the ESP NVS layer */
    nvs_flash_init();

    /* Initialize push button on the dev-kit to reset the device */
    esp_err_t err = factory_reset_button_register();
    ABORT_APP_ON_FAILURE(ESP_OK == err, ESP_LOGE(TAG, "Failed to initialize reset button, err:%d", err));

    /* Create a Matter node and add the mandatory Root Node device type on endpoint 0 */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));

    // add temperature sensor device
    temperature_sensor::config_t temp_sensor_config;
    endpoint_t * temp_sensor_ep = temperature_sensor::create(node, &temp_sensor_config, ENDPOINT_FLAG_NONE, NULL);
    ABORT_APP_ON_FAILURE(temp_sensor_ep != nullptr, ESP_LOGE(TAG, "Failed to create temperature_sensor endpoint"));
    g_temp_endpoint_id = endpoint::get_id(temp_sensor_ep);

    esp_err_t ctx_err = add_context_cluster(temp_sensor_ep);
    ABORT_APP_ON_FAILURE(ctx_err == ESP_OK, ESP_LOGE(TAG, "Failed to add context cluster"));
    g_context_endpoint_id = g_temp_endpoint_id;

    // Initialise TFLM — after Matter node is configured, before esp_matter::start()
    bool ml_ok = ml_context_init();
    ABORT_APP_ON_FAILURE(ml_ok, ESP_LOGE(TAG, "Failed to initialise TFLM"));

    // add the humidity sensor device
    humidity_sensor::config_t humidity_sensor_config;
    endpoint_t * humidity_sensor_ep = humidity_sensor::create(node, &humidity_sensor_config, ENDPOINT_FLAG_NONE, NULL);
    ABORT_APP_ON_FAILURE(humidity_sensor_ep != nullptr, ESP_LOGE(TAG, "Failed to create humidity_sensor endpoint"));
    g_humidity_endpoint_id = endpoint::get_id(humidity_sensor_ep);

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    /* Set OpenThread platform config */
    esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    set_openthread_platform_config(&config);
#endif

    /* Matter start */
    err = esp_matter::start(app_event_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter, err:%d", err));

    /* Start DHT polling task */
    xTaskCreate(dht_task, "dht_task", 4096, nullptr, 5, nullptr);
}
