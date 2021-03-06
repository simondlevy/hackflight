/*
   Hackflight core algorithm

   Copyright (c) 2020 Simon D. Levy

   This file is part of Hackflight.

   Hackflight is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Hackflight is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MEReceiverHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   You should have received a copy of the GNU General Public License
   along with Hackflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "debugger.hpp"
#include "mspparser.hpp"
#include "imu.hpp"
#include "board.hpp"
#include "actuator.hpp"
#include "receiver.hpp"
#include "datatypes.hpp"
#include "pidcontroller.hpp"
#include "motor.hpp"
#include "actuators/mixer.hpp"
#include "actuators/rxproxy.hpp"
#include "sensors/surfacemount.hpp"
#include "timertasks/pidtask.hpp"
#include "timertasks/serialtask.hpp"
#include "sensors/surfacemount/gyrometer.hpp"
#include "sensors/surfacemount/quaternion.hpp"

namespace hf {

    class Hackflight {

        private:

            static constexpr float MAX_ARMING_ANGLE_DEGREES = 25.0f;

            // Supports periodic ad-hoc debugging
            Debugger _debugger;

            // Mixer or receiver proxy
            Actuator * _actuator = NULL;

            RXProxy * _proxy = NULL;

            // Sensors 
            Sensor * _sensors[256] = {NULL};
            uint8_t _sensor_count = 0;

            // Safety
            bool _safeToArm = false;

            // Support for headless mode
            float _yawInitial = 0;

            // Timer task for PID controllers
            PidTask _pidTask;

            // Passed to Hackflight::init() for a particular build
            IMU        * _imu      = NULL;
            Mixer      * _mixer    = NULL;

            // Serial timer task for GCS
            SerialTask _serialTask;

             // Mandatory sensors on the board
            Gyrometer _gyrometer;
            Quaternion _quaternion; // not really a sensor, but we treat it like one!
 
            bool safeAngle(uint8_t axis)
            {
                return fabs(_state.rotation[axis]) < Filter::deg2rad(MAX_ARMING_ANGLE_DEGREES);
            }

           void checkQuaternion(void)
            {
                // Some quaternion filters may need to know the current time
                float time = _board->getTime();

                // If quaternion data ready
                if (_quaternion.ready(time)) {

                    // Update state with new quaternion to yield Euler angles
                    _quaternion.modifyState(_state, time);
                }
            }

            void checkGyrometer(void)
            {
                // Some gyrometers may need to know the current time
                float time = _board->getTime();

                // If gyrometer data ready
                if (_gyrometer.ready(time)) {

                    // Update state with gyro rates
                    _gyrometer.modifyState(_state, time);
                }
            }


            Board    * _board    = NULL;
            Receiver * _receiver = NULL;

            // Vehicle state
            state_t _state;

            void checkOptionalSensors(void)
            {
                for (uint8_t k=0; k<_sensor_count; ++k) {
                    Sensor * sensor = _sensors[k];
                    float time = _board->getTime();
                    if (sensor->ready(time)) {
                        sensor->modifyState(_state, time);
                    }
                }
            }

            void add_sensor(Sensor * sensor)
            {
                _sensors[_sensor_count++] = sensor;
            }

            void add_sensor(SurfaceMountSensor * sensor, IMU * imu) 
            {
                add_sensor(sensor);

                sensor->imu = imu;
            }

            void general_init(Board * board, Receiver * receiver, Actuator * actuator)
            {  
                // Store the essentials
                _board    = board;
                _receiver = receiver;
                _actuator = actuator;

                // Ad-hoc debugging support
                _debugger.init(board);

                // Support adding new sensors and PID controllers
                _sensor_count = 0;

                // Initialize state
                memset(&_state, 0, sizeof(state_t));

                // Initialize the receiver
                _receiver->begin();

                // Setup failsafe
                _state.failsafe = false;

                // Initialize timer task for PID controllers
                _pidTask.init(_board, _receiver, _actuator, &_state);
            }

            void checkReceiver(void)
            {
                // Sync failsafe to receiver
                if (_receiver->lostSignal() && _state.armed) {
                    _actuator->cut();
                    _state.armed = false;
                    _state.failsafe = true;
                    _board->showArmedStatus(false);
                    return;
                }

                // Check whether receiver data is available
                if (!_receiver->getDemands(_state.rotation[AXIS_YAW] - _yawInitial)) return;

                // Disarm
                if (_state.armed && !_receiver->getAux1State()) {
                    _state.armed = false;
                } 

                // Avoid arming if aux1 switch down on startup
                if (!_safeToArm) {
                    _safeToArm = !_receiver->getAux1State();
                }

                // Arm (after lots of safety checks!)
                if (_safeToArm && !_state.armed && _receiver->throttleIsDown() && _receiver->getAux1State() && 
                        !_state.failsafe && safeAngle(AXIS_ROLL) && safeAngle(AXIS_PITCH)) {
                    _state.armed = true;
                    _yawInitial = _state.rotation[AXIS_YAW]; // grab yaw for headless mode
                }

                // Cut motors on throttle-down
                if (_state.armed && _receiver->throttleIsDown()) {
                    _actuator->cut();
                }

                // Set LED based on arming status
                _board->showArmedStatus(_state.armed);

            } // checkReceiver

            // Inner classes for full vs. lite version
            class Updater {

                friend class Hackflight;

                private:

                    Hackflight * _h = NULL;

                protected:

                    void init(Hackflight * h) 
                    {
                        _h = h;
                    }

                    virtual void update(void) = 0;

            }; // class Updater

            class UpdateFull : protected Updater {

                friend class Hackflight;

                virtual void update(void) override
                {
                    _h->updateFull();
                }

            }; // class UpdateFull

            class UpdateLite : protected Updater {

                friend class Hackflight;

                virtual void update(void) override
                {
                    _h->updateLite();
                }

            }; // class UpdateLite

            Updater * _updater;
            UpdateFull _updaterFull;
            UpdateLite _updaterLite;

            void updateLite(void)
            {
                // Use proxy to send the correct channel values when not armed
                if (!_state.armed) {
                    _proxy->sendDisarmed();
                }

                // Update serial comms task
                _serialTask.update();
            }

            void updateFull(void)
            {
                // Check mandatory sensors
                checkGyrometer();
                checkQuaternion();

                // Check optional sensors
                checkOptionalSensors();

                // Update serial comms task
                _serialTask.update();
            }

        public:

            void init(Board * board, IMU * imu, Receiver * receiver, Mixer * mixer, Motor * motors, bool armed=false)
            {  
                // Do general initialization
                general_init(board, receiver, mixer);

                // Store pointers to IMU, mixer
                _imu   = imu;
                _mixer = mixer;

                // Initialize serial timer task
                _serialTask.init(board, &_state, receiver, mixer);

                // Support safety override by simulator
                _state.armed = armed;

                // Support for mandatory sensors
                add_sensor(&_quaternion, imu);
                add_sensor(&_gyrometer, imu);

                // Start the IMU
                imu->begin();

                // Tell the mixer which motors to use, and initialize them
                mixer->useMotors(motors);

                // Set the update function
                _updater = &_updaterFull;
                _updater->init(this);

            } // init

            void init(Board * board, Receiver * receiver, RXProxy * proxy) 
            {
                // Do general initialization
                general_init(board, receiver, proxy);

                // Initialize serial timer task (no mixer)
                _serialTask.init(board, &_state, receiver);

                // Store proxy for arming check
                _proxy = proxy;

                // Start proxy
                _proxy->begin();

                // Set the update function
                _updater = &_updaterLite;
                _updater->init(this);
            }

            void addSensor(Sensor * sensor) 
            {
                add_sensor(sensor);
            }

            void addPidController(PidController * pidController, uint8_t auxState=0) 
            {
                _pidTask.addPidController(pidController, auxState);
            }

            void update(void)
            {
                // Grab control signal if available
                checkReceiver();

                // Update PID controllers task
                _pidTask.update();

                // Run full or lite update function
                _updater->update();
            }

    }; // class Hackflight

} // namespace
