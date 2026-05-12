import os
import socket
import time

import psutil
import paho.mqtt.client as mqtt


MQTT_HOST = "bemfa.com"
MQTT_PORT = 9501
MQTT_TOPIC = "rtpcstatus001"
CLIENT_ID = "e683645fd0944bd9a2a8b450de95f762"

PUBLISH_INTERVAL_SEC = 8
RECONNECT_DELAY_SEC = 5


def get_local_ip():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("8.8.8.8", 80))
        return sock.getsockname()[0]
    except OSError:
        return "0.0.0.0"
    finally:
        sock.close()


def get_battery_percent():
    battery = psutil.sensors_battery()
    if battery is None:
        return -1
    return int(battery.percent)


def build_payload(last_net, elapsed_sec):
    cpu = int(psutil.cpu_percent(interval=None))
    mem = int(psutil.virtual_memory().percent)

    now_net = psutil.net_io_counters()
    down = int((now_net.bytes_recv - last_net.bytes_recv) / elapsed_sec)
    up = int((now_net.bytes_sent - last_net.bytes_sent) / elapsed_sec)

    if down < 0:
        down = 0
    if up < 0:
        up = 0

    ip = get_local_ip()
    bat = get_battery_percent()

    payload = f"cpu={cpu},mem={mem},down={down},up={up},ip={ip},bat={bat}"
    return payload, now_net


def connect_client():
    if not CLIENT_ID:
        raise RuntimeError(
            "BEMFA_PRIVATE_KEY is empty. Set it to your Bemfa private key first."
        )

    client = mqtt.Client(client_id=CLIENT_ID, protocol=mqtt.MQTTv311)
    client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
    client.loop_start()
    return client


def connect_client_with_retry():
    while True:
        try:
            return connect_client()
        except KeyboardInterrupt:
            raise
        except Exception as exc:
            print(f"connect failed: {exc}; retrying in {RECONNECT_DELAY_SEC}s ...")
            time.sleep(RECONNECT_DELAY_SEC)


def main():
    print(f"Connecting to {MQTT_HOST}:{MQTT_PORT} ...")
    client = connect_client_with_retry()
    print(f"Publishing PC status to {MQTT_TOPIC}")

    psutil.cpu_percent(interval=None)
    last_net = psutil.net_io_counters()
    last_time = time.monotonic()
    next_publish_time = last_time + PUBLISH_INTERVAL_SEC

    while True:
        sleep_time = next_publish_time - time.monotonic()
        if sleep_time > 0:
            time.sleep(sleep_time)

        try:
            now_time = time.monotonic()
            elapsed_sec = now_time - last_time
            if elapsed_sec <= 0:
                elapsed_sec = PUBLISH_INTERVAL_SEC

            payload, last_net = build_payload(last_net, elapsed_sec)
            client.publish(MQTT_TOPIC, payload, qos=0)
            print(f"{time.strftime('%H:%M:%S')} {payload}")

            last_time = now_time
            next_publish_time += PUBLISH_INTERVAL_SEC
            if next_publish_time < time.monotonic():
                next_publish_time = time.monotonic() + PUBLISH_INTERVAL_SEC
        except KeyboardInterrupt:
            break
        except Exception as exc:
            print(f"publish failed: {exc}; reconnecting ...")
            try:
                client.loop_stop()
                client.disconnect()
            except Exception:
                pass
            time.sleep(RECONNECT_DELAY_SEC)
            client = connect_client_with_retry()

    client.loop_stop()
    client.disconnect()


if __name__ == "__main__":
    main()
