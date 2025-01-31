/*
 * Copyright 2019-2022 u-blox
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Only #includes of u_* and the C standard library are allowed here,
 * no platform stuff and no OS stuff.  Anything required from
 * the platform/OS must be brought in through u_port* to maintain
 * portability.
 */

/** @file
 * @brief Implementation of the "general" API for GNSS.
 */

#ifdef U_CFG_OVERRIDE
# include "u_cfg_override.h" // For a customer's configuration override
#endif

#include "stdlib.h"    // malloc() and free()
#include "stddef.h"    // NULL, size_t etc.
#include "stdint.h"    // int32_t etc.
#include "stdbool.h"
#include "string.h"    // memset()

#include "u_cfg_sw.h"

#include "u_error_common.h"

#include "u_device_shared.h"

#include "u_port_os.h"
#include "u_port_debug.h"
#include "u_port_gpio.h"

#include "u_gnss_module_type.h"
#include "u_gnss_type.h"
#include "u_gnss.h"
#include "u_gnss_private.h"

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * STATIC VARIABLES
 * -------------------------------------------------------------- */

/** To display some nice text.
 */
//lint -esym(752, gpTransportTypeText) Suppress not referenced, which
// it won't be if diagnostic prints are compiled out
static const char *const gpTransportTypeText[] = {"None",       // U_GNSS_TRANSPORT_NONE
                                                  "ubx UART",   // U_GNSS_TRANSPORT_UBX_UART
                                                  "ubx AT",     // U_GNSS_TRANSPORT_UBX_AT
                                                  "NMEA UART",  // U_GNSS_TRANSPORT_NMEA_UART
                                                  "ubx I2C",    // U_GNSS_TRANSPORT_UBX_I2C
                                                  "NMEA I2C"    // U_GNSS_TRANSPORT_NMEA_I2C
                                                 };

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

// Find a GNSS instance in the list by transport handle.
// gUGnssPrivateMutex should be locked before this is called.
//lint -esym(1746, transportHandle) Suppress could
// be made const: it is!
static uGnssPrivateInstance_t *pGetGnssInstanceTransportHandle(uGnssTransportType_t transportType,
                                                               const uGnssTransportHandle_t transportHandle)
{
    uGnssPrivateInstance_t *pInstance = gpUGnssPrivateInstanceList;
    bool match = false;

    while ((pInstance != NULL) && !match) {
        if (pInstance->transportType == transportType) {
            switch (transportType) {
                case U_GNSS_TRANSPORT_UBX_UART:
                //lint -fallthrough
                case U_GNSS_TRANSPORT_NMEA_UART:
                    match = (pInstance->transportHandle.uart == transportHandle.uart);
                    break;
                case U_GNSS_TRANSPORT_UBX_AT:
                    match = (pInstance->transportHandle.pAt == transportHandle.pAt);
                    break;
                case U_GNSS_TRANSPORT_UBX_I2C:
                //lint -fallthrough
                case U_GNSS_TRANSPORT_NMEA_I2C:
                    match = (pInstance->transportHandle.i2c == transportHandle.i2c);
                    break;
                default:
                    break;
            }
        }
        if (!match) {
            pInstance = pInstance->pNext;
        }
    }

    return pInstance;
}

// Add a GNS instance to the list.
// gUGnssPrivateMutex should be locked before this is called.
// Note: doesn't copy it, just adds it.
static void addGnssInstance(uGnssPrivateInstance_t *pInstance)
{
    pInstance->pNext = gpUGnssPrivateInstanceList;
    gpUGnssPrivateInstanceList = pInstance;
}

// Remove a GNSS instance from the list.
// gUGnssPrivateMutex should be locked before this is called.
static void deleteGnssInstance(uGnssPrivateInstance_t *pInstance)
{
    uGnssPrivateInstance_t *pCurrent;
    uGnssPrivateInstance_t *pPrev = NULL;

    pCurrent = gpUGnssPrivateInstanceList;
    while (pCurrent != NULL) {
        if (pInstance == pCurrent) {
            // Stop any asynchronous position establishment task
            uGnssPrivateCleanUpPosTask(pInstance);
            // Delete the transport mutex
            uPortMutexDelete(pInstance->transportMutex);
            // Deallocate the uDevice instance
            uDeviceDestroyInstance(U_DEVICE_INSTANCE(pInstance->gnssHandle));
            // Unlink the instance from the list
            if (pPrev != NULL) {
                pPrev->pNext = pCurrent->pNext;
            } else {
                gpUGnssPrivateInstanceList = pCurrent->pNext;
            }
            pCurrent = NULL;
            // Free the instance
            free(pInstance);
        } else {
            pPrev = pCurrent;
            pCurrent = pPrev->pNext;
        }
    }
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// Initialise the GNSS driver.
int32_t uGnssInit()
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_SUCCESS;

    if (gUGnssPrivateMutex == NULL) {
        // Create the mutex that protects the linked list
        errorCode = uPortMutexCreate(&gUGnssPrivateMutex);
    }

    return errorCode;
}

// Shut-down the GNSS driver.
void uGnssDeinit()
{
    if (gUGnssPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUGnssPrivateMutex);

        // Remove all GNSS instances
        while (gpUGnssPrivateInstanceList != NULL) {
            deleteGnssInstance(gpUGnssPrivateInstanceList);
        }

        // Unlock the mutex so that we can delete it
        U_PORT_MUTEX_UNLOCK(gUGnssPrivateMutex);
        uPortMutexDelete(gUGnssPrivateMutex);
        gUGnssPrivateMutex = NULL;
    }
}

// Add a GNSS instance.
//lint -esym(1746, transportHandle) Suppress could
// be made const: it is!
int32_t uGnssAdd(uGnssModuleType_t moduleType,
                 uGnssTransportType_t transportType,
                 const uGnssTransportHandle_t transportHandle,
                 int32_t pinGnssEnablePower,
                 bool leavePowerAlone,
                 uDeviceHandle_t *pGnssHandle)
{
    int32_t errorCodeOrHandle = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uGnssPrivateInstance_t *pInstance = NULL;
    uPortGpioConfig_t gpioConfig;
    int32_t platformError = 0;
    int32_t pinGnssEnablePowerOnState = (pinGnssEnablePower & U_GNSS_PIN_INVERTED) ?
                                        !U_GNSS_PIN_ENABLE_POWER_ON_STATE : U_GNSS_PIN_ENABLE_POWER_ON_STATE;
    uPortGpioDriveMode_t pinGnssEnablePowerDriveMode;

    pinGnssEnablePower &= ~U_GNSS_PIN_INVERTED;

#ifdef U_GNSS_PIN_ENABLE_POWER_DRIVE_MODE
    // User override
    pinGnssEnablePowerDriveMode = U_GNSS_PIN_ENABLE_POWER_DRIVE_MODE;
#else
    // The drive mode is normally open drain so that we
    // can pull the enable power pin low and then let it float
    // afterwards since it is pulled-up by the cellular
    // module
    pinGnssEnablePowerDriveMode = U_PORT_GPIO_DRIVE_MODE_OPEN_DRAIN;
    if (pinGnssEnablePowerOnState == 1) {
        // If enable power is toggling to 1 then there's an
        // inverter between us and the MCU which only needs
        // normal drive mode.
        pinGnssEnablePowerDriveMode = U_PORT_GPIO_DRIVE_MODE_NORMAL;
    }
#endif

    if (gUGnssPrivateMutex != NULL) {
        uDeviceInstance_t *pDevInstance;

        errorCodeOrHandle = (int32_t) U_ERROR_COMMON_NO_MEMORY;
        pDevInstance = pUDeviceCreateInstance(U_DEVICE_TYPE_GNSS);

        if (pDevInstance != NULL) {

            U_PORT_MUTEX_LOCK(gUGnssPrivateMutex);

            // Check parameters
            errorCodeOrHandle = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
            if (((size_t) moduleType < gUGnssPrivateModuleListSize) &&
                ((transportType > U_GNSS_TRANSPORT_NONE) &&
                 (transportType < U_GNSS_TRANSPORT_MAX_NUM)) &&
                ((transportType == U_GNSS_TRANSPORT_UBX_I2C) ||
                 (transportType == U_GNSS_TRANSPORT_NMEA_I2C) ||
                 (pGetGnssInstanceTransportHandle(transportType, transportHandle) == NULL))) {
                errorCodeOrHandle = (int32_t) U_ERROR_COMMON_NO_MEMORY;
                // Allocate memory for the instance
                pInstance = (uGnssPrivateInstance_t *) malloc(sizeof(uGnssPrivateInstance_t));
                if (pInstance != NULL) {
                    // Fill the values in
                    memset(pInstance, 0, sizeof(*pInstance));
                    pInstance->pinGnssEnablePowerOnState = pinGnssEnablePowerOnState;
                    // Create a transport mutex
                    pInstance->gnssHandle = (uDeviceHandle_t)pDevInstance;
                    pInstance->transportMutex = NULL;
                    errorCodeOrHandle = uPortMutexCreate(&pInstance->transportMutex);
                    if (errorCodeOrHandle == 0) {
                        pInstance->transportType = transportType;
                        pInstance->pModule = &(gUGnssPrivateModuleList[moduleType]);
                        pInstance->transportHandle = transportHandle;
                        pInstance->i2cAddress = U_GNSS_I2C_ADDRESS;
                        pInstance->timeoutMs = U_GNSS_DEFAULT_TIMEOUT_MS;
                        pInstance->printUbxMessages = false;
                        pInstance->pinGnssEnablePower = pinGnssEnablePower;
                        pInstance->atModulePinPwr = -1;
                        pInstance->atModulePinDataReady = -1;
                        pInstance->portNumber = 0; // This is the I2C port number inside the GNSS chip
                        if ((transportType == U_GNSS_TRANSPORT_UBX_UART) ||
                            (transportType == U_GNSS_TRANSPORT_NMEA_UART)) {
                            pInstance->portNumber = 1; // This is the UART port number inside the GNSS chip
                        }
                        pInstance->posTask = NULL;
                        pInstance->posMutex = NULL;
                        pInstance->posTaskFlags = 0;
                        pInstance->pNext = NULL;

                        // Now set up the pins
                        uPortLog("U_GNSS: initialising with ENABLE_POWER pin ");
                        if (pinGnssEnablePower >= 0) {
                            uPortLog("%d (0x%02x), set to %d to power on GNSS",
                                     pinGnssEnablePower, pinGnssEnablePower, pinGnssEnablePowerOnState);
                            if (leavePowerAlone) {
                                uPortLog(", leaving the level of the pin alone");
                            }
                        } else {
                            uPortLog("not connected");
                        }
                        uPortLog(", transport type %s.\n", gpTransportTypeText[transportType]);

                        // Sort ENABLE_POWER pin if there is one
                        if (pinGnssEnablePower >= 0) {
                            if (!leavePowerAlone) {
                                // Set ENABLE_POWER high so that we can pull it low
                                platformError = uPortGpioSet(pinGnssEnablePower,
                                                             (int32_t) !pinGnssEnablePowerOnState);
                            }
                            if (platformError == 0) {
                                U_PORT_GPIO_SET_DEFAULT(&gpioConfig);
                                gpioConfig.pin = pinGnssEnablePower;
                                gpioConfig.pullMode = U_PORT_GPIO_PULL_MODE_NONE;
                                gpioConfig.driveMode = pinGnssEnablePowerDriveMode;
                                gpioConfig.direction = U_PORT_GPIO_DIRECTION_OUTPUT;
                                platformError = uPortGpioConfig(&gpioConfig);
                                if (platformError != 0) {
                                    uPortLog("U_GNSS: uPortGpioConfig() for ENABLE_POWER pin %d"
                                             " (0x%02x) returned error code %d.\n",
                                             pinGnssEnablePower, pinGnssEnablePower, platformError);
                                }
                            } else {
                                uPortLog("U_GNSS: uPortGpioSet() for ENABLE_POWER pin %d (0x%02x)"
                                         " returned error code %d.\n",
                                         pinGnssEnablePower, pinGnssEnablePower, platformError);
                            }
                        }
                    }

                    if ((errorCodeOrHandle == 0) && (platformError == 0)) {
                        // Add it to the list
                        addGnssInstance(pInstance);
                        errorCodeOrHandle = (int32_t) U_ERROR_COMMON_SUCCESS;
                        *pGnssHandle = pInstance->gnssHandle;
                    } else {
                        // If we hit an error, free memory again
                        if (pInstance->transportMutex != NULL) {
                            uPortMutexDelete(pInstance->transportMutex);
                        }
                        free(pInstance);
                    }
                }
            }

            if (errorCodeOrHandle != (int32_t) U_ERROR_COMMON_SUCCESS) {
                // Don't forget to deallocate device instance on failure
                uDeviceDestroyInstance(pDevInstance);
            }

            U_PORT_MUTEX_UNLOCK(gUGnssPrivateMutex);
        }
    }

    return (int32_t) errorCodeOrHandle;
}

// Set the I2C address of the GNSS device.
int32_t uGnssSetI2cAddress(uDeviceHandle_t gnssHandle, int32_t i2cAddress)
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uGnssPrivateInstance_t *pInstance;

    if (gUGnssPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUGnssPrivateMutex);

        errorCode = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        pInstance = pUGnssPrivateGetInstance(gnssHandle);
        if ((pInstance != NULL) && (i2cAddress > 0)) {
            pInstance->i2cAddress = i2cAddress;
            errorCode = (int32_t) U_ERROR_COMMON_SUCCESS;
        }

        U_PORT_MUTEX_UNLOCK(gUGnssPrivateMutex);
    }

    return errorCode;
}

// Get the I2C address being used for the GNSS device.
int32_t uGnssGetI2cAddress(uDeviceHandle_t gnssHandle)
{
    int32_t errorCodeOrI2cAddress = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uGnssPrivateInstance_t *pInstance;

    if (gUGnssPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUGnssPrivateMutex);

        errorCodeOrI2cAddress = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        pInstance = pUGnssPrivateGetInstance(gnssHandle);
        if (pInstance != NULL) {
            errorCodeOrI2cAddress = pInstance->i2cAddress;
        }

        U_PORT_MUTEX_UNLOCK(gUGnssPrivateMutex);
    }

    return errorCodeOrI2cAddress;
}

// Remove a GNSS instance.
void uGnssRemove(uDeviceHandle_t gnssHandle)
{
    uGnssPrivateInstance_t *pInstance;

    if (gUGnssPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUGnssPrivateMutex);

        pInstance = pUGnssPrivateGetInstance(gnssHandle);
        if (pInstance != NULL) {
            deleteGnssInstance(pInstance);
        }

        U_PORT_MUTEX_UNLOCK(gUGnssPrivateMutex);
    }
}

// Get the type and handle of the transport used by the
// given instance.
int32_t uGnssGetTransportHandle(uDeviceHandle_t gnssHandle,
                                uGnssTransportType_t *pTransportType,
                                uGnssTransportHandle_t *pTransportHandle)
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uGnssPrivateInstance_t *pInstance;

    if (gUGnssPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUGnssPrivateMutex);

        errorCode = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        pInstance = pUGnssPrivateGetInstance(gnssHandle);
        if (pInstance != NULL) {
            if (pTransportType != NULL) {
                *pTransportType = pInstance->transportType;
            }
            if (pTransportHandle != NULL) {
                *pTransportHandle = pInstance->transportHandle;
            }
            errorCode = (int32_t) U_ERROR_COMMON_SUCCESS;
        }

        U_PORT_MUTEX_UNLOCK(gUGnssPrivateMutex);
    }

    return errorCode;
}

// Set the cellular/short-range module pin which enables power
// to the GNSS chip.
void uGnssSetAtPinPwr(uDeviceHandle_t gnssHandle, int32_t pin)
{
    uGnssPrivateInstance_t *pInstance;

    if (gUGnssPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUGnssPrivateMutex);

        pInstance = pUGnssPrivateGetInstance(gnssHandle);
        if (pInstance != NULL) {
            pInstance->atModulePinPwr = pin;
        }

        U_PORT_MUTEX_UNLOCK(gUGnssPrivateMutex);
    }
}

// Set the celluar module pin that is used for GNSS data ready.
void uGnssSetAtPinDataReady(uDeviceHandle_t gnssHandle, int32_t pin)
{
    uGnssPrivateInstance_t *pInstance;

    if (gUGnssPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUGnssPrivateMutex);

        pInstance = pUGnssPrivateGetInstance(gnssHandle);
        if (pInstance != NULL) {
            pInstance->atModulePinDataReady = pin;
        }

        U_PORT_MUTEX_UNLOCK(gUGnssPrivateMutex);
    }
}

// Get the maximum time to wait for a response from the GNSS chip.
int32_t uGnssGetTimeout(uDeviceHandle_t gnssHandle)
{
    int32_t errorCodeOrTimeout = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uGnssPrivateInstance_t *pInstance;

    if (gUGnssPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUGnssPrivateMutex);

        errorCodeOrTimeout = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        pInstance = pUGnssPrivateGetInstance(gnssHandle);
        if (pInstance != NULL) {
            errorCodeOrTimeout = pInstance->timeoutMs;
        }

        U_PORT_MUTEX_UNLOCK(gUGnssPrivateMutex);
    }

    return errorCodeOrTimeout;
}

// Set the timeout for getting a response from the GNSS chip.
void uGnssSetTimeout(uDeviceHandle_t gnssHandle, int32_t timeoutMs)
{
    uGnssPrivateInstance_t *pInstance;

    if (gUGnssPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUGnssPrivateMutex);

        pInstance = pUGnssPrivateGetInstance(gnssHandle);
        if (pInstance != NULL) {
            pInstance->timeoutMs = timeoutMs;
        }

        U_PORT_MUTEX_UNLOCK(gUGnssPrivateMutex);
    }
}

// Get whether printing of ubx commands and responses is on or off.
bool uGnssGetUbxMessagePrint(uDeviceHandle_t gnssHandle)
{
    uGnssPrivateInstance_t *pInstance;
    bool isOn = false;

    if (gUGnssPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUGnssPrivateMutex);

        pInstance = pUGnssPrivateGetInstance(gnssHandle);
        if (pInstance != NULL) {
            isOn = pInstance->printUbxMessages;
        }

        U_PORT_MUTEX_UNLOCK(gUGnssPrivateMutex);
    }

    return isOn;
}

// Switch printing of ubx commands and response on or off.
void uGnssSetUbxMessagePrint(uDeviceHandle_t gnssHandle, bool onNotOff)
{
    uGnssPrivateInstance_t *pInstance;

    if (gUGnssPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUGnssPrivateMutex);

        pInstance = pUGnssPrivateGetInstance(gnssHandle);
        if (pInstance != NULL) {
            pInstance->printUbxMessages = onNotOff;
        }

        U_PORT_MUTEX_UNLOCK(gUGnssPrivateMutex);
    }
}

// End of file
