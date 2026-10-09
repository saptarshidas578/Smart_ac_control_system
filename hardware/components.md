# Hardware Component Specifications

## Thermal Sensing Stage
- **Amplifiers:** 2x Adafruit MAX31865 SPI RTD-to-Digital Converter ICs
- **Probes:** Class A PT100 Platinum Resistance Temperature Detectors (100 Ω @ 0°C, 3-wire / 4-wire)
- **Reference Resistor:** R_ref = 430.0 Ω (0.1% precision metal film)

## AC Detection & Current Sensing Stage
- **Zero-Crossing/Optoisolator:** PC817 or EL817 optocoupler interfacing 230V AC thermostat line to 3.3V GPIO
- **Current Transformer (CT):** Split-core CT with precision burden resistor and operational amplifier signal conditioning
- **Relay Driver:** 5V optocoupled SPDT relay switching HVAC 24V AC / 230V AC contactor
