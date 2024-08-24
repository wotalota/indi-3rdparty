/*
 Edited version of the Dome Simulator
 Copyright(c) 2014 Jasem Mutlaq. All rights reserved.
 Copyright(c) 2023 Jasem Mutlaq. All rights reserved.
 Copyright(c) 2024 Jasem Mutlaq. All rights reserved.

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Library General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.
 .
 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Library General Public License for more details.

 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

/*
 * Modified version of the rolloff roof simulator.
 * Uses a simple text string protocol to send messages to an Arduino. The Arduino's USB connection is set
 * by default to 38400 baud. The Arduino code determines which pins open/close a relay to start/stop the
 * roof motor, and read state of switches indicating if the roof is  opened or closed.
 */
#include "rolloffino.h"
#include "indicom.h"
#include "termios.h"
#include <stdlib.h>
#include <cmath>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <fstream>

#define ROLLOFF_DURATION 60               // Seconds until Roof is fully opened or closed
#define INACTIVE_STATUS  4                // Seconds between updating status lights
#define ROR_D_PRESS      1000             // Milliseconds after issuing command before expecting response
#define MAX_CNTRL_COM_ERR 10              // Maximum consecutive errors communicating with Arduino

// Arduino controller interface limits
#define MAXINOCMD        15          // Command buffer
#define MAXINOTARGET     15          // Target buffer
#define MAXINOVAL        127         // Value bufffer, sized to contain NAK error strings
#define MAXINOLINE       63          // Sized to contain outgoing command requests
#define MAXINOBUF        255         // Sized for maximum overall input / output
#define MAXINOERR        255         // System call error message buffer
#define MAXINOWAIT       3           // seconds

// Driver version id
#define VERSION_ID      "20240801"

// We declare an auto pointer to RollOffIno.
std::unique_ptr<RollOffIno> rollOffIno(new RollOffIno());

void ISPoll(void *p);

void ISGetProperties(const char *dev)
{
    rollOffIno->ISGetProperties(dev);
}

void ISNewSwitch(const char *dev, const char *name, ISState *states, char *names[], int n)
{
    rollOffIno->ISNewSwitch(dev, name, states, names, n);
}
void ISNewText(const char *dev, const char *name, char *texts[], char *names[], int n)
{
    rollOffIno->ISNewText(dev, name, texts, names, n);
}

void ISNewNumber(const char *dev, const char *name, double values[], char *names[], int n)
{
    rollOffIno->ISNewNumber(dev, name, values, names, n);
}

void ISNewBLOB(const char *dev, const char *name, int sizes[], int blobsizes[], char *blobs[], char *formats[],
               char *names[], int n)
{
    INDI_UNUSED(dev);
    INDI_UNUSED(name);
    INDI_UNUSED(sizes);
    INDI_UNUSED(blobsizes);
    INDI_UNUSED(blobs);
    INDI_UNUSED(formats);
    INDI_UNUSED(names);
    INDI_UNUSED(n);
}

void ISSnoopDevice(XMLEle *root)
{
    rollOffIno->ISSnoopDevice(root);
}

bool RollOffIno::ISSnoopDevice(XMLEle *root)
{
    return INDI::Dome::ISSnoopDevice(root);
}

RollOffIno::RollOffIno()
{
    SetDomeCapability(DOME_CAN_ABORT | DOME_CAN_PARK);           // Need the DOME_CAN_PARK capability for the scheduler
}

/**************************************************************************************
** INDI is asking us for our default device name.
** Check that it matches Ekos selection menu and ParkData.xml names
***************************************************************************************/
const char *RollOffIno::getDefaultName()
{
    return (const char *)"RollOff ino";
}
/**************************************************************************************
** INDI request to init properties. Connected Define properties to Ekos
***************************************************************************************/
bool RollOffIno::initProperties()
{
    char var_act [MAX_LABEL];
    char var_lab [MAX_LABEL];
    INDI::Dome::initProperties();
    // Roof related controls
    IUFillSwitch(&LockS[LOCK_DISABLE], "LOCK_DISABLE", "Off", ISS_ON);
    IUFillSwitch(&LockS[LOCK_ENABLE],  "LOCK_ENABLE",  "On",  ISS_OFF);
    IUFillSwitchVector(&LockSP, LockS, 2, getDeviceName(), "LOCK", "Lock", MAIN_CONTROL_TAB, IP_RW, ISR_1OFMANY, 60, IPS_IDLE);
    IUFillSwitch(&AuxS[AUX_DISABLE],   "AUX_DISABLE",  "Off", ISS_ON);
    IUFillSwitch(&AuxS[AUX_ENABLE],    "AUX_ENABLE",   "On",  ISS_OFF);
    IUFillSwitchVector(&AuxSP,  AuxS,  2, getDeviceName(), "AUX",  "Auxiliary", MAIN_CONTROL_TAB, IP_RW, ISR_1OFMANY, 60, IPS_IDLE);
    //Roof status lights
    IUFillLight(&RoofStatusL[ROOF_STATUS_OPENED],   "ROOF_OPENED", "Opened", IPS_IDLE);
    IUFillLight(&RoofStatusL[ROOF_STATUS_CLOSED],   "ROOF_CLOSED", "Closed", IPS_IDLE);
    IUFillLight(&RoofStatusL[ROOF_STATUS_MOVING],   "ROOF_MOVING", "Moving", IPS_IDLE);
    IUFillLight(&RoofStatusL[ROOF_STATUS_LOCKED],   "ROOF_LOCK",   "Roof Lock", IPS_IDLE);
    IUFillLight(&RoofStatusL[ROOF_STATUS_AUXSTATE], "ROOF_AUXILIARY", "Roof Auxiliary", IPS_IDLE);
    IUFillLightVector(&RoofStatusLP, RoofStatusL, 5, getDeviceName(), "ROOF STATUS", "Roof Status", MAIN_CONTROL_TAB, IPS_BUSY);
    // Options tab
    IUFillNumber(&RoofTimeoutN[0], "ROOF_TIMEOUT", "Timeout in Seconds", "%3.0f", 1, 300, 1, 40);
    IUFillNumberVector(&RoofTimeoutNP, RoofTimeoutN, 1, getDeviceName(), "ROOF_MOVEMENT", "Roof Movement", OPTIONS_TAB, IP_RW, 60, IPS_IDLE);
    // Action labels
    for (int i = 0; i < MAX_ACTIONS; i++)
    {
        sprintf(var_act, "Action %d", i+1);
        sprintf(var_lab, "Label  %d", i+1);
        IUFillText(&labelsT[i], action_labels[i], var_lab, var_act);
        IUFillTextVector(labelsTP[i], &labelsT[i], 1, getDeviceName(), action_labels[i], labelsT[i].text,   ACTION_LABEL_TAB, IP_RW, 60, IPS_IDLE);
        defineProperty(labelsTP[i]);
    }
    loadConfig(true);
    // Actions
    for (int i = 0; i < MAX_ACTIONS; i++)
    {
        char* aLabel = labelsT[i].text;
        sprintf(var_act, "Action %d", i+1);
        sprintf(var_lab, "Label %d", i+1);
        if (strlen(aLabel) == 0)
        {
//            sprintf(aLabel, "Empty Label %d", i);
            aLabel = var_act;
        }
        else
        {
            for (int j = 0; j < i; j++)
            {
                if (strcmp(aLabel, labelsT[j].text) == 0)
                {
                    sprintf(aLabel, "Duplicate Label %d", j);
//                    aLabel = var_act;
                    break;
                }
            }
        }
        IUFillSwitch(&actionSwitches[i][ACTION_DISABLE], "ACTION_DISABLE", "Off", ISS_ON);
        IUFillSwitch(&actionSwitches[i][ACTION_ENABLE],  "ACTION_ENABLE",  "On",  ISS_OFF);
        IUFillSwitchVector(actionSwitchesSP[i], actionSwitches[i], 2, getDeviceName(),
                           actionSwitchesText[i], aLabel,   ACTION_CONTROL_TAB, IP_RW, ISR_ATMOST1, 60, IPS_IDLE);
    // Action Status Lights
        IUFillLight(&ActionStatusL[i], actionSwitchesText[i], aLabel, IPS_IDLE);
    }
    IUFillLightVector(&ActionStatusLP, ActionStatusL, 8, getDeviceName(), "ACTION STATUS","Returned State", ACTION_CONTROL_TAB, IPS_BUSY);
    SetParkDataType(PARK_NONE);
    addAuxControls();         // This is for the standard controls
    loadConfig(true);
    return true;
}
/********************************************************************************************
** INDI request to update the properties because there is a change in CONNECTION status
** This function is called whenever the device is connected or disconnected.
*********************************************************************************************/
bool RollOffIno::updateProperties()
{
    INDI::Dome::updateProperties();
    if (isConnected())
    {
        if (InitPark())
        {
            DEBUG(INDI::Logger::DBG_SESSION, "Dome parking data was obtained");
        }
        // If we do not have Dome parking data
        else
        {
            DEBUG(INDI::Logger::DBG_SESSION, "Dome parking data was not obtained");
        }
        defineProperty(&LockSP);            // Lock Switch,
        defineProperty(&AuxSP);             // Aux Switch,
        defineProperty(&RoofStatusLP);      // Roof status lights
        defineProperty(&RoofTimeoutNP);     // Roof timeout
        for (int i = 0; i < MAX_ACTIONS; i++)
        {
            defineProperty(labelsTP[i]);          // Action labels
        }
        for (int i = 0; i < MAX_ACTIONS; i++)
        {
            defineProperty(actionSwitchesSP[i]);  // Actions
        }
        defineProperty(&ActionStatusLP);                  // Action status lights
        checkConditions();
    }
    else
    {
        deleteProperty(LockSP.name);        // Delete the Lock Switch buttons
        deleteProperty(AuxSP.name);         // Delete the Auxiliary Switch buttons
        deleteProperty(RoofStatusLP.name);  // Delete the roof status lights
        deleteProperty(RoofTimeoutNP.name); // Roof timeout
        for (int i = 0; i < MAX_ACTIONS; i++)
        {
            deleteProperty(actionSwitchesSP[i]->name);
            deleteProperty(labelsTP[i]->name);
        }
        deleteProperty(ActionStatusLP.name);  // Delete the action status lights
    }
    return true;
}

void RollOffIno::ISGetProperties(const char *dev)
{
    INDI::Dome::ISGetProperties(dev);
    defineProperty(&LockSP);            // Lock Switch,
    defineProperty(&AuxSP);             // Aux Switch,
    defineProperty(&RoofTimeoutNP);     // Roof timeout
    for(int i = 0; i < MAX_ACTIONS; i++)
    {
        defineProperty(labelsTP[i]);
        defineProperty(actionSwitchesSP[i]);
    }
}

bool RollOffIno::saveConfigItems(FILE *fp)
{
    INDI::Dome::saveConfigItems(fp);
    IUSaveConfigSwitch(fp, &LockSP);
    IUSaveConfigSwitch(fp, &AuxSP);
    IUSaveConfigNumber(fp, &RoofTimeoutNP);
    for(int i = 0; i < MAX_ACTIONS; i++)
    {
        IUSaveConfigText(fp, labelsTP[i]);
        IUSaveConfigSwitch(fp, actionSwitchesSP[i]);
    }
    return true;
}

/************************************************************************************
 * Called from Dome, BaseDevice to establish contact with device
 ************************************************************************************/
bool RollOffIno::Handshake()
{
    bool status = false;
    LOG_INFO("Documentation: https://github.com/indilib/indi-3rdparty [indi-rolloffino]");
    LOGF_DEBUG("Driver id: %s", VERSION_ID);

    if (PortFD <= 0)
        DEBUG(INDI::Logger::DBG_WARNING, "The connection port has not been established");
    else
    {
        status = initialContact();
        if (!status)
        {
            DEBUG(INDI::Logger::DBG_WARNING, "Initial controller contact failed, retrying");
            msSleep(1000);              // In case it is a Arduino still resetting from upload
            status = initialContact();
        }
        if (!status)
            LOG_ERROR("Unable to contact the roof controller");
    }
    return status;
}

/**************************************************************************************
** Client is asking us to establish connection to the device
***************************************************************************************/
bool RollOffIno::Connect()
{
    bool status = INDI::Dome::Connect();
    return status;
}

/**************************************************************************************
** Client is asking us to terminate connection to the device
***************************************************************************************/
bool RollOffIno::Disconnect()
{
    bool status = INDI::Dome::Disconnect();
    return status;
}

/********************************************************************************************
** Establish conditions on a connect.
*********************************************************************************************/
bool RollOffIno::checkConditions()
{
    updateRoofStatus();
    updateActionStatus();
    Dome::DomeState curState = getDomeState();

    // If the roof is clearly fully opened or fully closed, set the Dome::IsParked status to match.
    // Otherwise if Dome:: park status different from roof status, o/p message (the roof might be or need to be operating manually)
    // If park status is the same, and Dome:: state does not match, change the Dome:: state.
    if (isParked())
    {
        if (fullyOpenedLimitSwitch == ISS_ON)
        {
            SetParked(false);
        }
        else if (fullyClosedLimitSwitch == ISS_OFF)
        {
            DEBUG(INDI::Logger::DBG_WARNING, "Dome indicates it is parked but roof closed switch not set, manual intervention needed");
        }
        else
        {
            // When Dome status agrees with roof status (closed), but Dome state differs, set Dome state to parked
            if (curState != DOME_PARKED)
            {
                DEBUG(INDI::Logger::DBG_SESSION, "Setting Dome state to DOME_PARKED.");
                setDomeState(DOME_PARKED);
            }
        }
    }
    else
    {
        if (fullyClosedLimitSwitch == ISS_ON)
        {
            SetParked(true);
        }
        else if (fullyOpenedLimitSwitch == ISS_OFF)
        {
            DEBUG(INDI::Logger::DBG_WARNING,
                  "Dome indicates it is unparked but roof open switch is not set, manual intervention needed");
        }
        else
            // When Dome status agrees with roof status (open), but Dome state differs, set Dome state to unparked
        {
            if (curState != DOME_UNPARKED)
            {
                DEBUG(INDI::Logger::DBG_SESSION, "Setting Dome state to DOME_UNPARKED.");
                setDomeState(DOME_UNPARKED);
            }
        }
    }
    return true;
}

bool RollOffIno::ISNewNumber(const char *dev, const char *name, double values[], char *names[], int n)
{
    if (dev != nullptr && strcmp(dev, getDeviceName()) == 0)
    {
        if (!strcmp(RoofTimeoutNP.name, name))
        {
            IUUpdateNumber(&RoofTimeoutNP, values, names, n);
            RoofTimeoutNP.s = IPS_OK;
            IDSetNumber(&RoofTimeoutNP, nullptr);
            return true;
        }
    }
    return INDI::Dome::ISNewNumber(dev, name, values, names, n);
}

bool RollOffIno::ISNewText(const char *dev, const char *name, char *texts[], char *names[], int n)
{
    if (dev != nullptr && strcmp(dev, getDeviceName()) == 0)
    {
        for (int i = 0; i < MAX_ACTIONS; i++)
        {
            if (name != nullptr && strcmp(name, labelsTP[i]->name) == 0)
            {
                labelsTP[i]->s = IPS_OK;
                IUUpdateText(labelsTP[i], texts, names, n);
                IDSetText(labelsTP[i], nullptr);
                saveConfig(true);
                break;
            }
        }
    }
    return INDI::Dome::ISNewText(dev, name, texts, names, n);
}

/********************************************************************************************
** Client has changed the state of a switch, update
*********************************************************************************************/
bool RollOffIno::ISNewSwitch(const char *dev, const char *name, ISState *states, char *names[], int n)
{
    // Make sure the call is for our device
    if(dev != nullptr && strcmp(dev, getDeviceName()) == 0)
    {
        ////////////////////////////////////////
        // Check if the call for our Lock switch
        if (strcmp(name, LockSP.name) == 0)
        {
            // Find out which state is requested by the client
            const char *actionName = IUFindOnSwitchName(states, names, n);
            if (actionName == nullptr)
                return false;

            // If it is the same state as actionName, then we do nothing. i.e.
            // if actionName is LOCK_ON and our Lock switch is already on, we return
            int currentLockIndex = IUFindOnSwitchIndex(&LockSP);
            //DEBUGF(INDI::Logger::DBG_SESSION, "Lock state Requested: %s, Current: %s", actionName, LockS[currentLockIndex].name);
            if (!strcmp(actionName, LockS[currentLockIndex].name))
            {
                //DEBUGF(INDI::Logger::DBG_SESSION, "Lock switch is already %s", LockS[currentLockIndex].label);
                LockSP.s = IPS_IDLE;
                IDSetSwitch(&LockSP, nullptr);
                return true;
            }
            // Update the switch state
            IUUpdateSwitch(&LockSP, states, names, n);
            currentLockIndex = IUFindOnSwitchIndex(&LockSP);
            LockSP.s = IPS_OK;
            IDSetSwitch(&LockSP, nullptr);
            if (strcmp(LockS[currentLockIndex].name, "LOCK_ENABLE") == 0)
                setRoofLock(true);
            else
                setRoofLock(false);
            updateRoofStatus();
            return true;
        }
        
        ////////////////////////////////////////
        // Check if the call for our Aux switch
        if (strcmp(name, AuxSP.name) == 0)
        {
            // Find out which state is requested by the client
            const char *actionName = IUFindOnSwitchName(states, names, n);
            if (actionName == nullptr)
                return false;

            // If it is the same state as the current state, then we do nothing. i.e.
            // if actionName is AUX_ON and our Aux switch is already on, we return
            int currentAuxIndex = IUFindOnSwitchIndex(&AuxSP);
            //DEBUGF(INDI::Logger::DBG_SESSION, "Auxiliary state Requested (actionName): %s, Current: %s", actionName, AuxS[currentAuxIndex].name);
            if (!strcmp(actionName, AuxS[currentAuxIndex].name))
            {
                //DEBUGF(INDI::Logger::DBG_SESSION, "Auxiliary switch is already %s", AuxS[currentAuxIndex].label);
                AuxSP.s = IPS_IDLE;
                IDSetSwitch(&AuxSP, nullptr);
                return true;
            }

            // Different state requested, Update the switch state
            IUUpdateSwitch(&AuxSP, states, names, n);
            currentAuxIndex = IUFindOnSwitchIndex(&AuxSP);
            AuxSP.s = IPS_OK;
            IDSetSwitch(&AuxSP, nullptr);
            if (strcmp(AuxS[currentAuxIndex].name, "AUX_ENABLE") == 0)
                setRoofAux(true);
            else
                setRoofAux(false);
            updateRoofStatus();
            return true;
        }

        ////////////////////////////////////////
        // Check if the call for one of the Action switches
        for (int i = 0; i < MAX_ACTIONS; i++)
        {
            if (strcmp(name, actionSwitchesSP[i]->name) == 0)
            {
                // Find out which state is requested by the Ekos client
                const char *actionName = IUFindOnSwitchName(states, names, n);
                if (actionName == nullptr)
                    return false;
                int currentActIndex = IUFindOnSwitchIndex(actionSwitchesSP[i]);
                //LOGF_DEBUG("Action Requested (actionName) %s", actionName);
                // If the current state is the same as actionName, then we do nothing.
                if (!strcmp(actionName, actionSwitches[i][currentActIndex].name))
                {
                    //LOGF_DEBUG("Action switch is already %s", actionSwitches[i][currentActIndex].label);
                    actionSwitchesSP[i]->s = IPS_IDLE;
                    IDSetSwitch(actionSwitchesSP[i], nullptr);
                    return true;
                }

                // Update the switch state and send command
                IUUpdateSwitch(actionSwitchesSP[i], states, names, n);
                currentActIndex = IUFindOnSwitchIndex(actionSwitchesSP[i]);

                if (strcmp(actionName, "ACTION_ENABLE") == 0)
                    actionSwitchesSP[i]->s = IPS_OK;
                else
                    actionSwitchesSP[i]->s = IPS_IDLE;
                IDSetSwitch(actionSwitchesSP[i], nullptr);
                //LOGF_DEBUG("switchName: %s", actionSwitches[i][currentActIndex].name);
                if (strcmp(actionSwitches[i][currentActIndex].name, "ACTION_ENABLE") == 0)
                {
                    setAction(act_cmd_used[i], true);
                }
                else
                {
                    setAction(act_cmd_used[i], false);
                }
                updateActionStatus();
                return true;
            }
        }
    }
    return INDI::Dome::ISNewSwitch(dev, name, states, names, n);
}

void RollOffIno::updateRoofStatus()
{
    bool auxiliaryState = false;
    bool lockedState = false;
    bool openedState = false;
    bool closedState = false;

    getFullOpenedLimitSwitch(&openedState);
    getFullClosedLimitSwitch(&closedState);
    getRoofLockedSwitch(&lockedState);
    getRoofAuxSwitch(&auxiliaryState);

    if (!openedState && !closedState && !roofOpening && !roofClosing)
        DEBUG(INDI::Logger::DBG_WARNING, "Roof stationary, neither opened or closed, adjust to match PARK button");
    if (openedState && closedState)
        DEBUG(INDI::Logger::DBG_WARNING, "Roof showing it is both opened and closed according to the controller");

    RoofStatusL[ROOF_STATUS_AUXSTATE].s = IPS_IDLE;
    RoofStatusL[ROOF_STATUS_LOCKED].s = IPS_IDLE;
    RoofStatusL[ROOF_STATUS_OPENED].s = IPS_IDLE;
    RoofStatusL[ROOF_STATUS_CLOSED].s = IPS_IDLE;
    RoofStatusL[ROOF_STATUS_MOVING].s = IPS_IDLE;
    RoofStatusLP.s = IPS_IDLE;

    if (auxiliaryState)
    {
        RoofStatusL[ROOF_STATUS_AUXSTATE].s = IPS_OK;
    }
    if (lockedState)
    {
        RoofStatusL[ROOF_STATUS_LOCKED].s = IPS_ALERT;         // Red to indicate lock is on
        if (closedState)
        {
            RoofStatusL[ROOF_STATUS_CLOSED].s = IPS_OK;            // Closed and locked roof status is normal
            RoofStatusLP.s = IPS_OK;                               // Summary roof status
        }
        // An actual roof lock would not be expected unless roof was closed.
        // However the controller might be using it to prevent motion for some other reason.
        else if (openedState)
        {
            RoofStatusL[ROOF_STATUS_OPENED].s = IPS_OK;         // Possible, rely on open/close lights to indicate situation
            RoofStatusLP.s = IPS_OK;
        }
        else if (roofOpening || roofClosing)
        {
            RoofStatusLP.s = IPS_ALERT;                            // Summary roof status
            RoofStatusL[ROOF_STATUS_MOVING].s = IPS_ALERT;         // Should not be moving while locked
        }
    }
    else
    {
        if (openedState || closedState)
        {
            if (openedState && !closedState)
            {
                roofOpening = false;
                RoofStatusL[ROOF_STATUS_OPENED].s = IPS_OK;
                RoofStatusLP.s = IPS_OK;
            }
            if (closedState && !openedState)
            {
                roofClosing = false;
                RoofStatusL[ROOF_STATUS_CLOSED].s = IPS_OK;
                RoofStatusLP.s = IPS_OK;
            }
        }
        else if (roofOpening || roofClosing)
        {
            if (roofOpening)
            {
                RoofStatusL[ROOF_STATUS_OPENED].s = IPS_BUSY;
                RoofStatusL[ROOF_STATUS_MOVING].s = IPS_BUSY;
            }
            else if (roofClosing)
            {
                RoofStatusL[ROOF_STATUS_CLOSED].s = IPS_BUSY;
                RoofStatusL[ROOF_STATUS_MOVING].s = IPS_BUSY;
            }
            RoofStatusLP.s = IPS_BUSY;
        }

        // Roof is stationary, neither opened or closed
        else
        {
            if (roofTimedOut == EXPIRED_OPEN)
                RoofStatusL[ROOF_STATUS_OPENED].s = IPS_ALERT;
            else if (roofTimedOut == EXPIRED_CLOSE)
                RoofStatusL[ROOF_STATUS_CLOSED].s = IPS_ALERT;
            RoofStatusLP.s = IPS_ALERT;
        }
    }
    IDSetLight(&RoofStatusLP, nullptr);
}

void RollOffIno::updateActionStatus()
{
    for (int i = 0; i < MAX_ACTIONS; i++)
    {
        actionState[i] = false;
        ActionStatusL[i].s = IPS_IDLE;
        if (getActionSwitch(action_state_used[i], &actionState[i]))
        {
            if (actionState[i])
                actionStatusState[i] = ISS_ON;
            else
                actionStatusState[i] = ISS_OFF;
        }
    }

    ActionStatusLP.s = IPS_IDLE;
    for (int i = 0; i < MAX_ACTIONS; i++)
    {
        if (actionState[i])
        {
            ActionStatusL[i].s = IPS_OK;
            ActionStatusLP.s = IPS_OK;
        }
    }
    IDSetLight(&ActionStatusLP, nullptr);
}

/********************************************************************************************
** Each 1 second timer tick, if roof active
********************************************************************************************/
void RollOffIno::TimerHit()
{
    double timeleft = CalcTimeLeft(MotionStart);
    uint32_t delay = 1000 * INACTIVE_STATUS;   // inactive timer setting to maintain roof status lights
    if (!isConnected())
        return; //  No need to reset timer if we are not connected anymore

    if (isSimulation())
    {
        if (timeleft - 5 <= 0)           // Use timeout approaching to set faux switch indicator
        {
            if (DomeMotionSP[DOME_CW].getState() == ISS_ON)              // Opening
            {
                simRoofOpen = true;
                simRoofClosed = false;
            }
            else if (DomeMotionSP[DOME_CCW].getState() == ISS_ON)        // Closing
            {
                simRoofClosed = true;
                simRoofOpen = false;
            }
        }
    }
    updateRoofStatus();
    updateActionStatus();

    if (DomeMotionSP.getState() == IPS_BUSY)
    {
        // Abort called stop movement.
        if (MotionRequest < 0)
        {
            DEBUG(INDI::Logger::DBG_WARNING, "Roof motion is stopped");
            setDomeState(DOME_IDLE);
        }
        else
        {
            // Roll off is opening
            if (DomeMotionSP[DOME_CW].getState() == ISS_ON)
            {
                if (fullyOpenedLimitSwitch == ISS_ON)
                {
                    DEBUG(INDI::Logger::DBG_DEBUG, "Roof is open");
                    SetParked(false);
                }
                // See if time to open has expired.
                else if (timeleft <= 0)
                {
                    LOG_WARN("Time allowed for opening the roof has expired?");
                    setDomeState(DOME_IDLE);
                    roofOpening = false;
                    SetParked(false);
                    roofTimedOut = EXPIRED_OPEN;
                }
                else
                {
                    delay = 1000;           // opening active
                }
            }
            // Roll Off is closing
            else if (DomeMotionSP[DOME_CCW].getState() == ISS_ON)
            {
                if (fullyClosedLimitSwitch == ISS_ON)
                {
                    DEBUG(INDI::Logger::DBG_DEBUG, "Roof is closed");
                    SetParked(true);
                }
                // See if time to open has expired.
                else if (timeleft <= 0)
                {
                    LOG_WARN("Time allowed for closing the roof has expired?");
                    setDomeState(DOME_IDLE);
                    roofClosing = false;
                    SetParked(false);
                    roofTimedOut = EXPIRED_CLOSE;
                }
                else
                {
                    delay = 1000;           // closing active
                }
            }
        }
    }
    // If the roof is moved outside of indi from parked to unparked. The driver's
    // fully opened switch lets it to detect this. The Dome/Observatory was
    // continued to show parked. This call was added to set parked/unparked in Dome.
    else
    {
        checkConditions();
    }
    // Added to highlight WiFi issues, not able to recover lost connection without a reconnect
    if (communicationErrors > MAX_CNTRL_COM_ERR)
    {
        LOG_ERROR("Too many errors communicating with Arduino");
        LOG_ERROR("Try a fresh connect. Check communication equipment and operation of Arduino controller.");
        INDI::Dome::Disconnect();
        initProperties();
        communicationErrors = 0;
    }

    // Even when no roof movement requested, will come through occasionally. Use timer to update roof status
    // in case roof has been operated externally by a remote control, locks applied...
    SetTimer(delay);
}

float RollOffIno::CalcTimeLeft(timeval start)
{
    double timesince;
    double timeleft;
    struct timeval now
    {
        0, 0
    };
    gettimeofday(&now, nullptr);

    timesince =
        (double)(now.tv_sec * 1000.0 + now.tv_usec / 1000) - (double)(start.tv_sec * 1000.0 + start.tv_usec / 1000);
    timesince = timesince / 1000;
    timeleft  = MotionRequest - timesince;
    return timeleft;
}

/*
 * Direction: DOME_CW Clockwise = Open; DOME-CCW Counter clockwise = Close
 * Operation: MOTION_START, | MOTION_STOP
 */
IPState RollOffIno::Move(DomeDirection dir, DomeMotionCommand operation)
{
    updateRoofStatus();
    if (operation == MOTION_START)
    {
        if (roofLockedSwitch)
        {
            LOG_WARN("Roof is externally locked, no movement possible");
            return IPS_ALERT;
        }
        if (roofOpening)
        {
            LOG_WARN("Roof is in process of opening, wait for completion.");
            return IPS_OK;
        }
        if (roofClosing)
        {
            LOG_WARN("Roof is in process of closing, wait for completion.");
            return IPS_OK;
        }

        // Open Roof
        // DOME_CW --> OPEN. If we are asked to "open" while we are fully opened as the
        // limit switch indicates, then we simply return false.
        if (dir == DOME_CW)
        {
            if (fullyOpenedLimitSwitch == ISS_ON)
            {
                LOG_WARN("DOME_CW directive received but roof is already fully opened");
                SetParked(false);
                return IPS_ALERT;
            }

            // Initiate action
            if (roofOpen())
            {
                roofOpening = true;
                roofClosing = false;
                LOG_INFO("Roof is opening...");
            }
            else
            {
                LOG_WARN("Failed to operate controller to open roof");
                return IPS_ALERT;
            }
        }

        // Close Roof
        else if (dir == DOME_CCW)
        {
            if (fullyClosedLimitSwitch == ISS_ON)
            {
                SetParked(true);
                LOG_WARN("DOME_CCW directive received but roof is already fully closed");
                return IPS_ALERT;
            }
            else if (INDI::Dome::isLocked())
            {
                DEBUG(INDI::Logger::DBG_WARNING,
                      "Cannot close dome when mount is locking. See: Telescope parkng policy, in options tab");
                return IPS_ALERT;
            }
            // Initiate action
            if (roofClose())
            {
                roofClosing = true;
                roofOpening = false;
                LOG_INFO("Roof is closing...");
            }
            else
            {
                LOG_WARN("Failed to operate controller to close roof");
                return IPS_ALERT;
            }
        }
        roofTimedOut = EXPIRED_CLEAR;
        MotionRequest = (int)RoofTimeoutN[0].value;
        LOGF_DEBUG("Roof motion timeout setting: %d", (int)MotionRequest);
        gettimeofday(&MotionStart, nullptr);
        SetTimer(1000);
        return IPS_BUSY;
    }
    return    IPS_ALERT;
}
/*
 * Close Roof
 *
 */
IPState RollOffIno::Park()
{
    IPState rc = INDI::Dome::Move(DOME_CCW, MOTION_START);

    if (rc == IPS_BUSY)
    {
        LOG_INFO("RollOff ino is parking...");
        return IPS_BUSY;
    }
    else
        return IPS_ALERT;
}

/*
 * Open Roof
 *
 */
IPState RollOffIno::UnPark()
{
    IPState rc = INDI::Dome::Move(DOME_CW, MOTION_START);
    if (rc == IPS_BUSY)
    {
        LOG_INFO("RollOff ino is unparking...");
        return IPS_BUSY;
    }
    else
        return IPS_ALERT;
}

/*
 * Abort motion
 */
bool RollOffIno::Abort()
{
    bool lockState;
    bool openState;
    bool closeState;

    updateRoofStatus();
    lockState = (roofLockedSwitch == ISS_ON);
    openState = (fullyOpenedLimitSwitch == ISS_ON);
    closeState = (fullyClosedLimitSwitch == ISS_ON);

    if (lockState)
    {
        LOG_WARN("Roof is externally locked, no action taken on abort request");
        return true;
    }

    if (closeState)
    {
        LOG_WARN("Roof appears to be closed and stationary, no action taken on abort request");
        return true;
    }
    else if (openState)
    {
        LOG_WARN("Roof appears to be open and stationary, no action taken on abort request");
        return true;
    }
    else if (DomeMotionSP.getState() != IPS_BUSY)
    {
        LOG_WARN("Dome appears to be partially open and stationary, no action taken on abort request");
    }
    else if (DomeMotionSP.getState() == IPS_BUSY)
    {
        if (DomeMotionSP[DOME_CW].getState() == ISS_ON)
        {
            LOG_WARN("Abort roof action requested while the roof was opening. Direction correction may be needed on the next move request.");
        }
        else if (DomeMotionSP[DOME_CCW].getState() == ISS_ON)
        {
            LOG_WARN("Abort roof action requested while the roof was closing. Direction correction may be needed on the next move request.");
        }
        roofClosing = false;
        roofOpening = false;
        MotionRequest = -1;
        roofAbort();
    }

    // If both limit switches are off, then we're neither parked nor unparked.
    if (fullyOpenedLimitSwitch == ISS_OFF && fullyClosedLimitSwitch == ISS_OFF)
    {
        ParkSP.reset();
        ParkSP.setState(IPS_IDLE);
        ParkSP.apply();
    }
    return true;
}

bool RollOffIno::getFullOpenedLimitSwitch(bool* switchState)
{
    if (isSimulation())
    {
        if (simRoofOpen)
        {
            fullyOpenedLimitSwitch = ISS_ON;
            *switchState = true;
        }
        else
        {
            fullyOpenedLimitSwitch = ISS_OFF;
            *switchState = false;
        }
        return true;
    }

    if (readRoofSwitch(ROOF_OPENED_SWITCH, switchState))
    {
        if (*switchState)
            fullyOpenedLimitSwitch = ISS_ON;
        else
            fullyOpenedLimitSwitch = ISS_OFF;
        return true;
    }
    else
    {
        LOGF_WARN("Unable to obtain from the controller whether or not the roof is opened %d", ++communicationErrors);
        return false;
    }
}

bool RollOffIno::getFullClosedLimitSwitch(bool* switchState)
{
    if (isSimulation())
    {
        if (simRoofClosed)
        {
            fullyClosedLimitSwitch = ISS_ON;
            *switchState = true;
        }
        else
        {
            fullyClosedLimitSwitch = ISS_OFF;
            *switchState = false;
        }
        return true;
    }

    if (readRoofSwitch(ROOF_CLOSED_SWITCH, switchState))
    {
        if (*switchState)
            fullyClosedLimitSwitch = ISS_ON;
        else
            fullyClosedLimitSwitch = ISS_OFF;
        return true;
    }
    else
    {
        LOGF_WARN("Unable to obtain from the controller whether or not the roof is closed %d", ++communicationErrors);
        return false;
    }
}

bool RollOffIno::getRoofLockedSwitch(bool* switchState)
{
    // If there is no lock switch, return success with status false
    if (isSimulation())
    {
        roofLockedSwitch = ISS_OFF;
        *switchState = false;                     // Not locked
        return true;
    }
    if (readRoofSwitch(ROOF_LOCKED_SWITCH, switchState))
    {
        if (*switchState)
            roofLockedSwitch = ISS_ON;
        else
            roofLockedSwitch = ISS_OFF;
        return true;
    }
    else
    {
        LOGF_WARN("Unable to obtain from the controller whether or not the roof is externally locked %d", ++communicationErrors);
        return false;
    }
}

bool RollOffIno::getRoofAuxSwitch(bool* switchState)
{
    // If there is no Aux switch, return success with status false
    if (isSimulation())
    {
         roofAuxiliarySwitch = ISS_OFF;
         *switchState = false;
         return true;
    }
    if (readRoofSwitch(ROOF_AUX_SWITCH, switchState))
    {
        if (*switchState)
            roofAuxiliarySwitch = ISS_ON;
        else
            roofAuxiliarySwitch = ISS_OFF;
        return true;
    }
    else
    {
        LOGF_WARN("Unable to obtain from the controller whether or not the obs Aux switch is being used %d", ++communicationErrors);
        return false;
    }
}

bool RollOffIno::getActionSwitch(const char* action, bool* switchState)
{
    // If there is no lock switch, return success with status false
    if (isSimulation())
    {
        *switchState = false;
        return true;
    }
    if (!actionSwitchUsed(action))
        return false;
    if (readRoofSwitch(action, switchState))
    {
        return true;
    }
    else
    {
        LOGF_WARN("Unable to obtain from the controller whether or not the action switch is being used %d", ++communicationErrors);
        return false;
    }
}

bool RollOffIno::actionSwitchUsed(const char* action)
{
    if (actionCount > 0)
    {
        for (int i = 0; i < actionCount; i++)
        {
            if (!strcmp(action, action_state_used[i]))
            {
                return true;
            }
        }
    }
    return false;
}

/*
 * -------------------------------------------------------------------------------------------
 *
 */
bool RollOffIno::roofOpen()
{
    if (isSimulation())
    {
        return true;
    }
    return pushRoofButton(ROOF_OPEN_CMD, true, false);
}

bool RollOffIno::roofClose()
{
    if (isSimulation())
    {
        return true;
    }
    return pushRoofButton(ROOF_CLOSE_CMD, true, false);
}

bool RollOffIno::roofAbort()
{
    if (isSimulation())
    {
        return true;
    }
    return pushRoofButton(ROOF_ABORT_CMD, true, false);
}

bool RollOffIno::setRoofLock(bool switchOn)
{
    if (isSimulation())
    {
        return false;
    }
    return pushRoofButton(ROOF_LOCK_CMD, switchOn, true);
}

bool RollOffIno::setRoofAux(bool switchOn)
{
    if (isSimulation())
    {
        return false;
    }
    return pushRoofButton(ROOF_AUX_CMD, switchOn, true);
}

bool RollOffIno::setAction(const char* action, bool switchOn)
{
    if (isSimulation())
    {
        return false;
    }
    if (actionCmdUsed(action))
        return pushRoofButton(action, switchOn, true);
    else
        return false;
}

bool RollOffIno::actionCmdUsed(const char* action)
{
    if (actionCount > 0)
    {
        for(int i = 0; i < actionCount; i++)
        {
            if (!strcmp(action, act_cmd_used[i]))
            {
                return true;
            }
        }
    }
    return false;
}

/*
 * If unable to determine switch state due to errors, return false.
 * If no errors return true. Return in result true if switch and false if switch off.
 */
bool RollOffIno::readRoofSwitch(const char* roofSwitchId, bool *result)
{
    char readBuffer[MAXINOBUF];
    char writeBuffer[MAXINOLINE];
    bool status;

    if (!contactEstablished)
    {
        LOG_WARN("No contact with the roof controller has been established");
        return false;
    }
    if (roofSwitchId == 0)
        return false;
    memset(writeBuffer, 0, sizeof(writeBuffer));
    strcpy(writeBuffer, "(GET:");
    strcat(writeBuffer, roofSwitchId);
    strcat(writeBuffer, ":0)");
    if (!writeIno(writeBuffer))
        return false;
    memset(readBuffer, 0, sizeof(readBuffer));
    if (!readIno(readBuffer))
        return false;
    status = evaluateResponse(readBuffer, result);
    return status;
}

/*
 * See if the controller is running
 */
bool RollOffIno::initialContact(void)
{
    char readBuffer[MAXINOBUF];
    char workBuffer[MAXINOBUF];
    bool result = false;
    contactEstablished = false;
    actionCount = 0;
    if (writeIno("(CON:0:0)"))
    {
        memset(readBuffer, 0, sizeof(readBuffer));
        memset(workBuffer, 0, sizeof(workBuffer));
        if (readIno(readBuffer))
        {
            strcpy(workBuffer, readBuffer);
            contactEstablished = evaluateResponse(workBuffer, &result);
            if (contactEstablished)
            // (ACK:0:V1.3-0  [ACTn])
            {
                char scratch[MAXINOVAL + 1];
                char target[MAXINOTARGET + 1];
                char version[MAXINOTARGET + 1];
                char action[MAXINOTARGET + 1];
                strcpy(scratch, strtok(workBuffer, "(:"));
                strcpy(scratch, strtok(nullptr, ":"));
                strcpy(target, strtok(nullptr, ")"));
                if (strstr(target, "[") != NULL)
                {
                    char* end = action + 6;
                    strcpy(version, strtok(target, "["));
                    strcpy(scratch, strtok(nullptr, "T"));
                    strcpy(action, strtok(nullptr, "]"));
                    LOGF_DEBUG("Remote version: %s, action: %s", version, action);
                    int value = strtol(action, &end, 10);
                    if (value > 0 && value <= MAX_ACTIONS )
                        actionCount = value;
                    else
                        actionCount = 0;
                }
                else
                    LOGF_DEBUG("Remote version %s", target);
            }
            return contactEstablished;
        }
    }
    return false;
}

/*
 * Whether roof is moving or stopped in any position along with the nature of the button requested will
 * determine the effect on the roof. This could mean stopping, or starting in a reversed direction.
 */
bool RollOffIno::pushRoofButton(const char* button, bool switchOn, bool ignoreLock)
{
    char readBuffer[MAXINOBUF];
    char writeBuffer[MAXINOBUF];
    bool status;
    bool switchState = false;
    bool responseState = false;  //true if the value in response to command was "ON"

    if (!contactEstablished)
    {
        LOG_WARN("No contact with the roof controller has been established");
        return false;
    }
    status = getRoofLockedSwitch(&switchState);        // In case it has been locked since the driver connected
    if ((status && !switchState) || ignoreLock)
    {
        memset(writeBuffer, 0, sizeof(writeBuffer));
        strcpy(writeBuffer, "(SET:");
        strcat(writeBuffer, button);
        if (switchOn)
            strcat(writeBuffer, ":ON)");
        else
            strcat(writeBuffer, ":OFF)");
        LOGF_DEBUG("Button pushed: %s", writeBuffer);
        if (!writeIno(writeBuffer))             // Push identified button & get response
            return false;
        msSleep(ROR_D_PRESS);
        memset(readBuffer, 0, sizeof(readBuffer));

        status = readIno(readBuffer);
        evaluateResponse(readBuffer, &responseState); // To get a log of what was returned in response to the command
        return status;                                // Did the read itself successfully connect
    }
    else
    {
        LOG_WARN("Roof external lock state prevents roof movement");
        return false;
    }
}

/*
 * if ACK return true and set result true|false indicating if switch is on
 *
 */
bool RollOffIno::evaluateResponse(char* inpBuffer, bool* result)
{
    char workBuffer[MAXINOBUF];
    char inoCmd[MAXINOCMD + 1];
    char inoTarget[MAXINOTARGET + 1];
    char inoVal[MAXINOVAL + 1];
    memset(workBuffer, 0, sizeof(workBuffer));
    strcpy(workBuffer, inpBuffer);
    *result = false;
    strcpy(inoCmd, strtok(workBuffer, "(:"));
    strcpy(inoTarget, strtok(nullptr, ":"));
    strcpy(inoVal, strtok(nullptr, ")"));
    if ((strcmp(inoCmd, "NAK")) == 0)
    {
        LOGF_WARN("Negative response from roof controller error: %s", inoTarget);
        LOGF_WARN("controller response: %s: ", inoVal);
        return false;
    }
    // If it is in response to a connect request return result true
    if (strcmp(inoTarget, "0") == 0)
    {
        *result = true;
        return true;
    }
    if ((strcmp(inoCmd, "ACK")) != 0)
    {
        LOGF_ERROR("Unrecognized response from roof controller: %s", inoCmd);
        return false;
    }

     // Otherwise return result ON/OFF as true/false
    *result = (strcmp(inoVal, "ON") == 0);
    return true;
}

bool RollOffIno::readIno(char* retBuf)
{
    bool stop = false;
    bool startFound = false;
    bool endFound = false;
    int status;
    int retCount = 0;
    int totalCount = 0;
    int delimCount = 0;
    int cmdLen = MAXINOCMD - 5;
    int targetLen = MAXINOTARGET + cmdLen - 5;
    char* bufPtr = retBuf;
    char errMsg[MAXINOERR];
    const char* canned = "(NAK:NONE:OFF)";
    while (!stop)
    {
        if (startFound)
            bufPtr = bufPtr + retCount;
        status = tty_read(PortFD, bufPtr, 1, MAXINOWAIT, &retCount);
        if (status != TTY_OK)
        {
            tty_error_msg(status, errMsg, MAXINOERR);
            LOGF_WARN("Roof control connection error: %s", errMsg);
            communicationErrors++;
            return false;
        }
        if (retCount <= 0)
            continue;
        if (*bufPtr == 0X28)          // '('   Start found
            startFound = true;
        else if (*bufPtr == 0X3A)     // ':'   delim found
            delimCount++;
        else if (*bufPtr == 0X29)     // ')'   End found
            endFound = true;
        if (startFound)
            totalCount += retCount;

        // Protect against input probes
        if ((totalCount >= MAXINOVAL) || ((totalCount >= 2) && !startFound) ||
            ((totalCount >= cmdLen) && (delimCount == 0)) ||
            ((totalCount >= targetLen) && (delimCount < 2)) ||
            (endFound && delimCount != 2))
        {
            communicationErrors++;
            *(++bufPtr) = 0;
            LOGF_ERROR("Received communication protocol not valid %s", retBuf);
            strcpy(retBuf, canned);
            return false;
        }
        else if (endFound)
        {
            *(++bufPtr) = 0;
            stop = true;
        }
    }
    return true;
}



bool RollOffIno::writeIno(const char* msg)
{
    int retMsgLen = 0;
    int status;
    char errMsg[MAXINOERR];

    if (strlen(msg) >= MAXINOLINE)
    {
        LOG_ERROR("Roof controller command message too long");
        return false;
    }
    LOGF_DEBUG("Sent to roof controller: %s", msg);
    tcflush(PortFD, TCIOFLUSH);
    status = tty_write_string(PortFD, msg, &retMsgLen);
    if (status != TTY_OK)
    {
        tty_error_msg(status, errMsg, MAXINOERR);
        LOGF_DEBUG("roof control connection error: %s", errMsg);
        return false;
    }
    return true;
}

void RollOffIno::msSleep (int mSec)
{
    struct timespec req = {0, 0};
    req.tv_sec = 0;
    req.tv_nsec = mSec * 1000000L;
    nanosleep(&req, (struct timespec *)nullptr);
}

