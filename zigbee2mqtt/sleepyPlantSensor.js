const {deviceEndpoints, illuminance, temperature, battery, humidity} = require('zigbee-herdsman-converters/lib/modernExtend');
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const fz = require('zigbee-herdsman-converters/converters/fromZigbee');

const e = exposes.presets;

const definition = {
    zigbeeModel: ['SleepyPlantSensor'],
    model: 'SleepyPlantSensor',
    vendor: 'LarsvdLee',
    description: 'Definition for a sleepy plant sensor device.',
    extend: [
        deviceEndpoints({"endpoints":{"9":9,"10":10,"11":11}}),
        illuminance(),
        temperature({ endpointNames: ["10"] }),
        battery(),
        humidity({ endpointNames: ["10"] })
    ],
    fromZigbee: [
        {
            cluster: 'genAnalogInput',
            type: ['attributeReport', 'readResponse'],
            convert: (model, msg, publish, options, meta) => {
                console.log("Hello");
                const ep = msg.endpoint.ID;
                if (!msg.data.hasOwnProperty('presentValue')) return;

                if (ep === 11) {
                    const raw = msg.data.presentValue;
                    const moisture = Math.round((raw / 10) * 10) / 10;
                    console.log(`Soil moisture raw=${raw}, converted=${moisture}`);
                    return { soil_moisture: moisture };
                }
            },
        }
    ],
    exposes: [
        e.numeric('soil_moisture', exposes.access.STATE)
            .withUnit('%')
            .withDescription('Soil moisture')
            .withValueMin(0)
            .withValueMax(100),
    ],
    meta: {
        multiEndpoint: true
    },
    configure: async (device, coordinatorEndpoint, logger) => {
        const epMoisture = device.getEndpoint(11);
        await epMoisture.bind('genAnalogInput', coordinatorEndpoint);
        await epMoisture.configureReporting('genAnalogInput', [{
            attribute: 'presentValue',
            minimumReportInterval: 300,
            maximumReportInterval: 3600,
            reportableChange: 1,
        }]);
    }
};

module.exports = definition;
