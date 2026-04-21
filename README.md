# ambient-now
ambient-now is an IoT system that continuously measures temperature, humidity, and pressure, visualizing the data in a live Grafana dashboard.

Two ESP32 microcontrollers form the hardware layer: one reads environmental data from a BME280 sensor and transmits it via ESP-NOW to a second ESP32, which bridges the data to a home server over MQTT. On the server, a Docker stack of Mosquitto, Node-RED, InfluxDB, and Grafana handles ingestion, storage, and visualization.

## Prerequisites
- git
- [Docker](https://docs.docker.com/get-docker/)
- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)

## Getting Started
### Server Setup
#### 1. Clone the repository
```bash
git clone https://github.com/andreasbachmann/ambient-now
cd ambient-now/server
```
#### 2. Configure environment variables
```bash
cp .env.example .env
```
Edit `.env` with your values: 

| Variable                  | Description                               |
| ------------------------- | ----------------------------------------- |
| `INFLUXDB_ADMIN_USER`     | InfluxDB admin username                   |
| `INFLUXDB_ADMIN_PASSWORD` | InfluxDB admin password                   |
| `INFLUXDB_ORG`            | InfluxDB organization (keep as `homeiot`) |
| `INFLUXDB_BUCKET`         | InfluxDB bucket (keep as `sensors`)       |
| `INFLUXDB_TOKEN`          | InfluxDB API token (keep as is for now)   |
| `GRAFANA_PASSWORD`        | Grafana admin password                    |

> `INFLUXDB_ORG` and `INFLUXDB_BUCKET` are hardcoded in the Node-RED flows and Grafana provisioning. Only change them if you also update those configs.
#### 3. Build and run
The build step is required to create a custom Node-RED image with the InfluxDB plugin pre-installed.
```bash
docker compose build
docker compose up -d
```

#### 4. Generate InfluxDB token
After the stack starts, an InfluxDB token must be generated manually. If running on a remote server, replace localhost with your server's IP address in all subsequent steps:
1. Open InfluxDB at `localhost:8086` and log in with the credentials you set in `.env`
2. Navigate to Load Data -> API tokens
3. Click Generate API Token, select "All Access API Token" and copy it
4. Paste the token to the `.env` as `INFLUXDB_TOKEN`
5. Restart the stack with `docker compose down && docker compose up -d`

#### 5. Configure Node-RED
1. Open Node-RED at `localhost:1880`
2. Double-click the InfluxDB out node and open the server configuration
3. Enter your InfluxDB API token and click **Update**, then **Done**
4. Click **Deploy**

### Firmware Setup
Configure the WiFi credentials, MQTT broker address, MAC address and I2C pins via menuconfig before building.
#### Bridge
```bash
cd firmware/esp-bridge
idf.py menuconfig # set WiFi credentials and MQTT broker IP under "Network Credentials"
idf.py build flash monitor
```
Check the monitor output for the MAC address of the bridge:
`I (683) wifi:mode : sta (f4:2d:c9:6b:ee:38)`
#### Sensor
```bash
cd firmware/esp-sensor
idf.py menuconfig # set the MAC address of the bridge and the pins under "Hardware Configuration"
idf.py build flash monitor
```
Now open `localhost:3000` to access Grafana, enter your credentials, navigate to the dashboard and watch the sensor populate the dashboard.

## Limitations
 - The InfluxDB token must be generated manually after first boot and entered in both `.env` and Node-RED
 - Mosquitto is configured to allow anonymous connections. Do not expose port 1883 to the internet without adding authentication
 - `INFLUXDB_ORG` and `INFLUXDB_BUCKET` are hardcoded in Node-RED and Grafana. Changing them in `.env` alone is not sufficient