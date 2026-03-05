# BluepawsV4
version 4 of my on-going cat tracker project. this differs in structure in that both the TX collar and Rx homehub will be in the same repo to ensure congruency 
# Cat Tracker Project – Detailed Work Breakdown Structure (WBS)
## Project Overview
System consists of:
- Collar node: nRF52840, SX1262 LoRa, L76K GNSS, Quectel BG77 LTE Cat■M
- Home hub: ESP32■S3 + LoRa + BLE beacon
- Cloud backend and web interface
- 100■unit pilot manufacturing
Battery target: ≥30 days normal usage
Wake interval: 10 minutes
---
# Level 1 WBS
1.0 Project Management
2.0 System Architecture
3.0 Power Engineering
4.0 Collar Hardware Design
5.0 Hub Hardware Design
6.0 Firmware Development
7.0 Backend Infrastructure
8.0 EVT Validation
9.0 DVT Validation
10.0 Regulatory Compliance
11.0 Manufacturing Preparation
12.0 Pilot Production
---
# 1.0 Project Management
1.1 Project schedule creation
1.1.1 Define milestone gates
1.1.2 Create Gantt timeline
1.1.3 Define deliverables
1.2 Requirements management
1.2.1 Define product requirements document (PRD)
1.2.2 Define hardware requirements
1.2.3 Define firmware requirements
1.3 Risk management
1.3.1 Identify technical risks
1.3.2 Create mitigation strategies
---
# 2.0 System Architecture
2.1 Device architecture design
2.1.1 Collar system block diagram
2.1.2 Hub system block diagram
2.1.3 Communication flow design
2.2 Protocol design
2.2.1 TLV payload specification
2.2.2 Message ID scheme
2.2.3 ACK strategy
2.3 Communication architecture
2.3.1 LoRa network design
2.3.2 LTE heartbeat architecture
2.3.3 BLE beacon suppression logic
---
# 3.0 Power Engineering
3.1 Power measurement setup
3.1.1 Build current measurement rig
3.1.2 Measure LoRa transmit current
3.1.3 Measure GNSS acquisition current
3.2 LTE power analysis
3.2.1 Measure LTE attach spikes
3.2.2 Measure LTE transmit power
3.2.3 Measure PSM sleep current
3.3 Battery modelling
3.3.1 Calculate daily consumption
3.3.2 Create worst case scenario
3.3.3 Validate 30■day battery target
---
# 4.0 Collar Hardware Design
4.1 Schematic development
4.1.1 MCU power domain design
4.1.2 LTE module integration
4.1.3 GNSS circuit integration
4.1.4 LoRa radio integration
4.2 RF layout
4.2.1 Antenna placement study
4.2.2 LTE antenna routing
4.2.3 GNSS antenna isolation
4.2.4 Matching network design
4.3 Power subsystem
4.3.1 Battery charger circuit
4.3.2 Fuel gauge integration
4.3.3 Load switching circuits
4.4 PCB layout
4.4.1 Component placement
4.4.2 Ground plane design
4.4.3 RF trace optimisation
---
# 5.0 Hub Hardware Design
5.1 Hub architecture
5.1.1 ESP32 system design
5.1.2 LoRa receiver integration
5.1.3 BLE beacon transmitter
5.2 Hub PCB design
5.2.1 Power system design
5.2.2 Battery charging system
5.2.3 USB interface
---
# 6.0 Firmware Development
6.1 Collar firmware architecture
6.1.1 RTOS task design
6.1.2 Power state machine
6.1.3 Wake cycle scheduler
6.2 Sensor and radio drivers
6.2.1 GNSS parsing
6.2.2 LoRa driver integration
6.2.3 BG77 AT command stack
6.3 Communication stack
6.3.1 TLV encoding implementation
6.3.2 LoRa packet handling
6.3.3 LTE fallback logic
6.4 Configuration system
6.4.1 Parameter storage
6.4.2 OTA configuration logic
---
# 7.0 Backend Infrastructure
7.1 Cloud infrastructure
7.1.1 VPS provisioning
7.1.2 Firewall configuration
7.1.3 TLS certificate setup
7.2 API development
7.2.1 Device registration endpoint
7.2.2 Location upload endpoint
7.2.3 Configuration endpoint
7.3 Database schema
7.3.1 Device table
7.3.2 Location table
7.3.3 Telemetry table
---
# 8.0 EVT Validation
8.1 Prototype manufacturing
8.1.1 Assemble 10 collar prototypes
8.1.2 Assemble 3 hub prototypes
8.2 Electrical validation
8.2.1 Battery discharge testing
8.2.2 Sleep current verification
8.3 RF validation
8.3.1 LoRa range testing
8.3.2 LTE attach testing
8.3.3 GNSS accuracy testing
---
# 9.0 DVT Validation
9.1 Hardware improvements
9.1.1 PCB revision 2
9.1.2 Antenna tuning
9.2 Environmental testing
9.2.1 Waterproof testing
9.2.2 Thermal testing
---
# 10.0 Regulatory Compliance
10.1 Technical documentation
10.1.1 Technical construction file
10.1.2 Bill of materials archive
10.2 Compliance testing
10.2.1 EMC emissions testing
10.2.2 RF exposure evaluation
---
# 11.0 Manufacturing Preparation
11.1 Supplier selection
11.1.1 PCB fabrication partner
11.1.2 Assembly partner
11.2 Production tooling
11.2.1 Test jig development
11.2.2 Firmware flashing process
---
# 12.0 Pilot Production
12.1 Pilot manufacturing run
12.1.1 Produce 100 collars
12.1.2 Produce 100 hubs
12.2 Field validation
12.2.1 Real world battery testing
12.2.2 Connectivity reliability testing
