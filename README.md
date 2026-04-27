# ESP32-C3 DHT22 Matter Sensor with TFLite Micro Context Classifier

An ESP32-C3 firmware that reads temperature and humidity from a DHT22 sensor, exposes them as standard Matter clusters, and runs an on-device TFLite Micro classifier to predict environmental context (heating on, normal, window open).

## What it does

- Reads temperature and humidity from a DFRobot DHT22 module every 2 seconds
- Exposes readings over Matter as standard `TemperatureMeasurement` and `RelativeHumidityMeasurement` clusters
- Runs a quantised int8 TFLite Micro neural network on each reading to classify the environmental context into one of three classes:
  - `0` — HEATING_ON
  - `1` — NORMAL
  - `2` — WINDOW_OPEN
- Publishes the predicted class index via the `MinMeasuredValue` attribute on the humidity endpoint (workaround for a CHIP SDK bug that rejects vendor-specific cluster IDs — see Known Limitations)

## Matter endpoints

| Endpoint | Device type | Clusters |
|----------|-------------|---------|
| 0 | Root Node | Basic Information, etc. |
| 1 | Temperature Sensor | TemperatureMeasurement |
| 2 | Humidity Sensor | RelativeHumidityMeasurement (MinMeasuredValue carries context class) |

## Hardware

### Components

- ESP32-C3 development board
- DFRobot DHT22 temperature and humidity sensor module

### Wiring

| ESP32-C3 Pin | DFRobot DHT22 Pin |
|--------------|-------------------|
| GND          | GND               |
| 3V3          | VCC               |
| GPIO 2       | Data              |

The DFRobot module has a built-in pull-up resistor, so no external resistor is needed.

## ML model

The classifier is a small fully-connected neural network trained in Python with scikit-learn and Keras, exported as a quantised int8 `.tflite` file, and converted to a C byte array in `main/context_model_data.h`.

At inference time the firmware:
1. Applies the same MinMaxScaler normalisation used during training
2. Quantises the normalised floats to int8 using the scale/zero-point baked into the model
3. Runs `MicroInterpreter::Invoke()` (FullyConnected → ReLU → FullyConnected → Softmax)
4. Picks the output class with the highest probability

The model uses ~4KB of RAM for the tensor arena and runs in tens of microseconds on the ESP32-C3.

## Building and flashing

See the [ESP Matter docs](https://docs.espressif.com/projects/esp-matter/en/latest/esp32/developing.html) for environment setup.

```bash
idf.py build
idf.py flash monitor
```

## Commissioning with python-matter-server

The device is commissioned using [python-matter-server](https://github.com/home-assistant-libs/python-matter-server). The controller uses Bluetooth to commission the device onto WiFi, so the host machine must have Bluetooth support.

**Step 1 — Set WiFi credentials** (once per controller session):

```python
await client.set_wifi_credentials('<SSID>', '<PASSWORD>')
```

**Step 2 — Commission using the QR code or manual pairing code** printed to the serial monitor on boot:

```python
# Using QR code string (MT:...)
await client.commission_with_code('MT:Y.ABCDEFG123456789')

# Or using the numeric manual pairing code
await client.commission_with_code('35325335079')
```

The controller pairs the device over BLE, pushes the WiFi credentials, then the device connects to the network and advertises itself via DNS-SD. After a few seconds it will appear as a commissioned node.

Alternatively, send raw WebSocket commands when integrating the python-matter-server WebSocket into backend:

```json
{ "message_id": "1", "command": "set_wifi_credentials", "args": { "ssid": "your-ssid", "credentials": "your-password" } }
{ "message_id": "2", "command": "commission_with_code", "args": { "code": "MT:Y.ABCDEFG123456789" } }
```

## Known limitations

- The vendor-specific context cluster (0xFC00) is rejected by python-matter-server 8.1.0 due to a known CHIP SDK bug ([connectedhomeip #32371](https://github.com/project-chip/connectedhomeip/issues/32371)). As a workaround, the predicted class index (0/1/2) is written to `MinMeasuredValue` on the humidity endpoint instead.
- The factory reset button uses the BSP default button. Hold it to clear NVS and re-commission.
