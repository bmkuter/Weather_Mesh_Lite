import paho.mqtt.client as mqtt
import ssl

MQTT_BROKER = "192.168.0.1"   # Change to match your broker's IP
MQTT_PORT = 1883              # Use 8883 for TLS if needed
TOPIC_COMMAND = "mesh/command"

def on_connect(client, userdata, flags, rc):
    print("Connected with result code " + str(rc))

client = mqtt.Client(client_id="MeshExternalClient")
client.on_connect = on_connect

# Uncomment and configure if authentication/TLS is needed:
# client.username_pw_set("username", "password")
# client.tls_set(cert_reqs=ssl.CERT_REQUIRED, tls_version=ssl.PROTOCOL_TLS)

client.connect(MQTT_BROKER, MQTT_PORT, 60)

# Publish user specified command.
user_cmd = input("Enter command to send to mesh: ")
client.publish(TOPIC_COMMAND, user_cmd)
print(f"Published command: {user_cmd}")

client.loop()  # Or loop_forever() for continuous operation