# Weather Mesh Lite

## Project Description
Weather Mesh Lite is a distributed sensor network project built using ESP Mesh Lite and ESP-NOW technologies. The system collects weather data (temperature, humidity, etc.) from multiple sensor nodes, aggregates the data into blockchain-like records, and manages leader election and consensus mechanisms for data integrity and synchronization. In addition, it provides support for WiFi networking to communicate with external servers and interfaces.

## Components
- **Blockchain Module**  
  Handles block creation, hashing (with serialized block data), and blockchain history management.
- **Mesh Networking Module**  
  Facilitates communication between nodes using ESP-NOW; handles broadcast messages, sensor responses, and node discovery.
- **Temperature Sensor Module**  
  Interfaces with the SHT45 sensor via I2C to acquire temperature and humidity readings with CRC verification.
- **Consensus & Election Module**  
  Implements leader election and Proof-of-Participation (PoP) to determine the block creator and validate sensor data.
- **WiFi Networking Module**  
  Provides TCP client functionality and supports both Station and SoftAP modes for additional connectivity.
- **Utility & Logging**  
  Contains helper functions for NVS storage initialization, system logging, and periodic system status reports.

## Achieved Goals
- [x] Sensor data acquisition and CRC validation.
- [x] Blockchain block creation with proper serialization and SHA‑256 hashing.
- [x] Leader election and consensus mechanism for block generation.
- [x] Mesh networking via ESP-NOW for inter-node communication.
- [x] WiFi connectivity for remote TCP messaging.
- [x] Basic system logging and configuration management.

## Goals Still To Be Achieved
- [ ] Implement full blockchain synchronization between nodes.
- [ ] Enhance security with data encryption and secure peer authentication.
- [ ] Optimize the election and consensus algorithms for scalability.
- [ ] Integrate remote monitoring and control capabilities.
- [ ] Extend sensor functionality to capture additional environmental parameters.
