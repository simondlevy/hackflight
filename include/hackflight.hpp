/*
   hackflight.hpp : general header

   This file is part of Hackflight.

   Hackflight is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Hackflight is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   You should have received a copy of the GNU General Public License
   along with Hackflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <cstdlib>
#include <cstdarg>
#include <cstdio>

#include "config.hpp"
#include "board.hpp"
#include "mixer.hpp"
#include "msp.hpp"
#include "common.hpp"
#include "imu.hpp"
#include "rc.hpp"
#include "stabilize.hpp"
#include "timedtask.hpp"

// For logical combinations of stick positions (low, center, high)
#define ROL_LO (1 << (2 * DEMAND_ROLL))
#define ROL_CE (3 << (2 * DEMAND_ROLL))
#define ROL_HI (2 << (2 * DEMAND_ROLL))
#define PIT_LO (1 << (2 * DEMAND_PITCH))
#define PIT_CE (3 << (2 * DEMAND_PITCH))
#define PIT_HI (2 << (2 * DEMAND_PITCH))
#define YAW_LO (1 << (2 * DEMAND_YAW))
#define YAW_CE (3 << (2 * DEMAND_YAW))
#define YAW_HI (2 << (2 * DEMAND_YAW))
#define THR_LO (1 << (2 * DEMAND_THROTTLE))
#define THR_CE (3 << (2 * DEMAND_THROTTLE))
#define THR_HI (2 << (2 * DEMAND_THROTTLE))


namespace hf {

class Hackflight {
    public:
        void init(Board * _board);
        void update(void);

    private:
        void blinkLedForTilt(void);
        bool gotRcUpdate(void);
        void flashLeds(const InitConfig& config);
        void initImuRc(const Config& config);
        void updateImu(void);
        void updateCalibrationState(void);

    private:
        bool       armed;

        IMU        imu;
        RC         rc;
        Mixer      mixer;
        MSP        msp;
        Stabilize  stab;
        Board    * board;

        TimedTask imuTask;
        TimedTask rcTask;
        TimedTask accelCalibrationTask;

        bool     accCalibrated;
        uint16_t calibratingGyroCycles;
        uint16_t calibratingAccelCycles;
        uint16_t accelCalibrationCountdown;
        uint16_t gyroCalibrationCountdown;
        bool     haveSmallAngle;
        uint16_t smallAngle;
};

/********************************************* CPP ********************************************************/

void Hackflight::init(Board * _board)
{  
    board = _board;

    // Do hardware initialization for board
    board->init();

    // Get board configuration
    const Config& config = board->getConfig();

    // Flash the LEDs to indicate startup
    flashLeds(config.init);

    initImuRc(config);

    stab.init(config.pid, &rc, &imu);
    mixer.init(config.pwm, &rc, &stab); 
    msp.init(&imu, &mixer, &rc, board);

    board->extrasInit(&msp);

    armed = false;
    accCalibrated = false;

} // intialize

void Hackflight::update(void)
{
    // If we didn't get new RC values, we can use the time to perform any extra tasks ("outer loop")
    if (!gotRcUpdate()) {

        static int taskOrder;

        board->extrasPerformTask(taskOrder);

        taskOrder++;

        if (taskOrder >= board->extrasGetTaskCount()) // using >= supports zero or more tasks
            taskOrder = 0;
    }

    // Regardless, we always update the IMU ("inner loop")
    updateImu();

} // update

bool Hackflight::gotRcUpdate(void)
{
    // if it's not time to update, and we have no new serial RC values, we're done
    if (!rcTask.checkAndUpdate(board->getMicros()) && !board->rcSerialReady())
        return false;

    // update RC channels
    rc.update(board);

    // useful for simulator
    if (armed)
        board->showAuxStatus(rc.auxState());

    // when landed, reset integral component of PID
    if (rc.throttleIsDown()) 
        stab.resetIntegral();

    if (rc.changed()) {

        // actions during armed
        if (armed) {      

            // Disarm on throttle down + yaw
            if (rc.sticks == THR_LO + YAW_LO + PIT_CE + ROL_CE) {
                if (armed) {
                    armed = false;
                    board->showArmedStatus(armed);
                }
            }

        // actions during not armed
        } else {         

            // gyro calibration
            if (rc.sticks == THR_LO + YAW_LO + PIT_LO + ROL_CE) 
                gyroCalibrationCountdown = calibratingGyroCycles;

            // Arm via throttle-low / yaw-right
            if (rc.sticks == THR_LO + YAW_HI + PIT_CE + ROL_CE)
                if (gyroCalibrationCountdown == 0 && accCalibrated) 
                    if (!rc.auxState()) // aux switch must be in zero position
                        if (!armed) {
                            armed = true;
                            board->showArmedStatus(armed);
                        }

            // accel calibration
            if (rc.sticks == THR_HI + YAW_LO + PIT_LO + ROL_CE)
                accelCalibrationCountdown = calibratingAccelCycles;

        } // not armed

    } // rc.changed()

    // Detect aux switch changes for hover, altitude-hold, etc.
    board->extrasCheckSwitch();

    return true;
}

void Hackflight::updateImu(void)
{
    uint32_t currentTime = board->getMicros();

    if (imuTask.checkAndUpdate(currentTime)) {

        // compute exponential RC commands
        rc.computeExpo();

        imu.update(board, currentTime, armed, accelCalibrationCountdown, gyroCalibrationCountdown);

        // periodically update accelerometer calibration status
        updateCalibrationState();

        // Stabilization, mixing, and MSP are synced to IMU update
        stab.update();
        mixer.update(armed, board);
        msp.update(armed);

    } 
} 

void Hackflight::updateCalibrationState(void)
{
    if (accelCalibrationCountdown > 0)
        accelCalibrationCountdown--;

    if (gyroCalibrationCountdown > 0)
        gyroCalibrationCountdown--;

    haveSmallAngle = 
        abs(imu.angle[0]) < smallAngle && abs(imu.angle[1]) < smallAngle;

    uint32_t currentTime = board->getMicros();

    // use LEDs to indicate calibration and arming status
    if (accelCalibrationCountdown > 0 || gyroCalibrationCountdown > 0) {
        board->ledSet(0, true);
    }
    else {
        if (accCalibrated)
            board->ledSet(0, false);
        if (armed)
            board->ledSet(1, true);
        else
            board->ledSet(1, false);
    }
    

    // If angle too steep, restart accel calibration and flash LED
    if (accelCalibrationTask.check(currentTime)) {
        if (!haveSmallAngle) {
            accCalibrated = false; 
            blinkLedForTilt();
            accelCalibrationTask.update(currentTime);
        } else {
            accCalibrated = true;
        }
    }
}

void Hackflight::blinkLedForTilt(void)
{
    static bool on;

    if (on) {
        board->ledSet(0, false);
        on = false;
    }
    else {
        board->ledSet(0, true);
        on = true;
    }
}

void Hackflight::flashLeds(const InitConfig& config)
{
    uint32_t pauseMilli = config.ledFlashMilli / config.ledFlashCount;
    board->ledSet(0, false);
    board->ledSet(1, false);
    for (uint8_t i = 0; i < config.ledFlashCount; i++) {
        board->ledSet(0, true);
        board->ledSet(1, false);
        board->delayMilliseconds(pauseMilli);
        board->ledSet(0, false);
        board->ledSet(1, true);
        board->delayMilliseconds(pauseMilli);
    }
    board->ledSet(0, false);
    board->ledSet(1, false);
}

void Hackflight::initImuRc(const Config& config)
{
    // Get particulars for board
    LoopConfig loopConfig = config.loop;
    ImuConfig imuConfig = config.imu;

    // Store some for later
    smallAngle = imuConfig.smallAngle;

    // compute loop times based on config from board
    calibratingGyroCycles   = (uint16_t)(1000. * loopConfig.calibratingGyroMilli  / loopConfig.imuLoopMicro);
    calibratingAccelCycles  = (uint16_t)(1000. * loopConfig.calibratingAccelMilli / loopConfig.imuLoopMicro);

    // always do gyro calibration at startup
    gyroCalibrationCountdown = calibratingGyroCycles;

    // assume shallow angle (no accelerometer calibration needed)
    haveSmallAngle = true;

    imu.init(imuConfig, calibratingGyroCycles, calibratingAccelCycles);

    // sleep  a bit to allow IMU to catch up
    board->delayMilliseconds(config.init.delayMilli);

    // initializing timing tasks
    imuTask.init(loopConfig.imuLoopMicro);
    rcTask.init(loopConfig.rcLoopMilli * 1000);
    accelCalibrationTask.init(loopConfig.accelCalibrationPeriodMilli * 1000);

    rc.init(config.rc, config.pwm);
}

} // namespace