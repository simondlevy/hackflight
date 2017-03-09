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

#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>

#include "crossplatform.h"

#ifndef M_PI
#endif

#include "common.hpp"
#include "board.hpp"
#include "imu.hpp"
#include "rc.hpp"
#include "stabilize.hpp"
#include "mixer.hpp"
#include "msp.hpp"
#include "timedtask.hpp"

// Config =====================================================

#define CONFIG_CALIBRATING_ACC_MSEC                 1400

#define CONFIG_YAW_CONTROL_DIRECTION                1    // 1 or -1 
#define CONFIG_RC_LOOPTIME_MSEC                     21
#define CONFIG_CALIBRATE_ACCTIME_MSEC               500
#define CONFIG_SMALL_ANGLE                          250  // tenths of a degree
#define CONFIG_ALTITUDE_UPDATE_MSEC                 25   // based on accelerometer low-pass filter

namespace hf {

class Hackflight {
    public:
        void init(Board * _board);
        void update(void);

    private:
        void blinkLedForTilt(void);
        bool gotRcUpdate(void);
        void flashLeds(void);
        void initImuRc(void);
        void updateImu(void);
        void updateCalibrationState(void);

    private:
        bool     armed;

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
        uint32_t imuLooptimeUsec;
        uint16_t calibratingGyroCycles;
        uint16_t calibratingAccCycles;
        uint16_t calibratingA;
        uint16_t calibratingG;
        bool     haveSmallAngle;

};

/********************************************* CPP ********************************************************/

inline void Hackflight::init(Board * _board)
{
    board = _board;

    initImuRc();

    stab.init(&rc, &imu);
    mixer.init(&rc, &stab); 
    msp.init(&imu, &mixer, &rc, board);

    board->extrasInit(&msp);

    armed = false;
    accCalibrated = false;

} // intialize

inline void Hackflight::update(void)
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

inline bool Hackflight::gotRcUpdate(void)
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
                calibratingG = calibratingGyroCycles;

            // Arm via throttle-low / yaw-right
            if (rc.sticks == THR_LO + YAW_HI + PIT_CE + ROL_CE)
                if (calibratingG == 0 && accCalibrated) 
                    if (!rc.auxState()) // aux switch must be in zero position
                        if (!armed) {
                            armed = true;
                            board->showArmedStatus(armed);
                        }

            // accel calibration
            if (rc.sticks == THR_HI + YAW_LO + PIT_LO + ROL_CE)
                calibratingA = calibratingAccCycles;

        } // not armed

    } // rc.changed()

    // Detect aux switch changes for hover, altitude-hold, etc.
    board->extrasCheckSwitch();

    return true;
}

inline void Hackflight::updateImu(void)
{
    uint32_t currentTime = board->getMicros();

    if (imuTask.checkAndUpdate(currentTime)) {

        // compute exponential RC commands
        rc.computeExpo();

        imu.update(board, currentTime, armed, calibratingA, calibratingG);

        // periodically update accelerometer calibration status
        updateCalibrationState();

        // Stabilization, mixing, and MSP are synced to IMU update
        stab.update();
        mixer.update(armed, board);
        msp.update(armed);

    } 
} 

inline void Hackflight::updateCalibrationState(void)
{
    if (calibratingA > 0)
        calibratingA--;

    if (calibratingG > 0)
        calibratingG--;

    haveSmallAngle = 
        abs(imu.angle[0]) < CONFIG_SMALL_ANGLE && abs(imu.angle[1]) < CONFIG_SMALL_ANGLE;

    uint32_t currentTime = board->getMicros();

    // use LEDs to indicate calibration and arming status
    if (calibratingA > 0 || calibratingG > 0) {
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

inline void Hackflight::blinkLedForTilt(void)
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

inline void Hackflight::flashLeds(void)
{
    board->ledSet(0, false);
    board->ledSet(1, false);
    for (uint8_t i = 0; i < 10; i++) {
        board->ledSet(1, true);
        board->ledSet(0, true);
        board->delayMilliseconds(50);
        board->ledSet(1, false);
        board->ledSet(0, false);
        board->delayMilliseconds(50);
    }
}

inline void Hackflight::initImuRc(void)
{
    uint16_t acc1G;
    float    gyroScale;
    uint32_t looptimeUsec;
    uint32_t gyroCalibrationMsec;

    // Get particulars for board
    board->init(acc1G, gyroScale, looptimeUsec, gyroCalibrationMsec);

    imuLooptimeUsec = looptimeUsec;

    // compute cycles for calibration based on board's time constant
    calibratingGyroCycles = (uint16_t)(1000. * gyroCalibrationMsec / imuLooptimeUsec);
    calibratingAccCycles  = (uint16_t)(1000. * CONFIG_CALIBRATING_ACC_MSEC  / imuLooptimeUsec);

    // always do gyro calibration at startup
    calibratingG = calibratingGyroCycles;

    // assume shallow angle (no accelerometer calibration needed)
    haveSmallAngle = true;

    imu.init(acc1G, gyroScale, calibratingGyroCycles, calibratingAccCycles);

    // sleep for 100ms
    board->delayMilliseconds(100);

    // flash the LEDs to indicate startup
    flashLeds();

    // initializing timing tasks
    imuTask.init(imuLooptimeUsec);
    rcTask.init(CONFIG_RC_LOOPTIME_MSEC * 1000);
    accelCalibrationTask.init(CONFIG_CALIBRATE_ACCTIME_MSEC * 1000);

    rc.init();
}

} // namespace
