/*
FILENAME... ANF2Driver.cpp
USAGE...    Motor record driver support for the AMCI ANF2 stepper motor controller over Modbus/TCP.

Kevin Peterson

Based on the AMCI ANG1 Model 3 device driver written by Kurt Goetze

*/


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>

#include <iocsh.h>
#include <epicsThread.h>
#include <epicsString.h>

#include <asynInt32SyncIO.h>

#include "ANF2Driver.h"
#include <epicsExport.h>

#define NINT(f) (int)((f)>0 ? (f)+0.5 : (f)-0.5)

static const char *driverName = "ANF2MotorDriver";

/** Constructor, Creates a new ANF2Controller object.
  * \param[in] portName          The name of the asyn port that will be created for this driver
  * \param[in] ANF2InPortName    The name of the drvAsynSerialPort that was created previously to connect to the ANF2 controller 
  * \param[in] ANF2OutPortName   The name of the drvAsynSerialPort that was created previously to connect to the ANF2 controller 
  * \param[in] numModules        The number of modules on the controller stack 
  * \param[in] axesPerModule     The number of axes per module (ANF1=1, ANF2=2)
  */
ANF2Controller::ANF2Controller(const char *portName, const char *ANF2InPortName, const char *ANF2OutPortName,  
                                 int numModules, int axesPerModule)
  :  asynMotorController(portName, (numModules*axesPerModule), NUM_ANF2_PARAMS, 
                         asynInt32ArrayMask, // One additional interface beyond those in base class
                         asynInt32ArrayMask, // One additional callback interface beyond those in base class
                         ASYN_CANBLOCK | ASYN_MULTIDEVICE, 
                         1, // autoconnect
                         0, 0)  // Default priority and stack size
{
  int i, j;
  asynStatus status = asynSuccess;
  static const char *functionName = "ANF2Controller::ANF2Controller";
  
  // Keep track of the number of axes created, so the poller can wait for all the axes to be created before starting
  axesCreated_ = 0;
  
  inputDriver_ = epicsStrDup(ANF2InPortName);    // Set this before calls to create Axis objects
  
  // Create controller-specific parameters
  createParam(ANF2GetInfoString,         asynParamInt32,       &ANF2GetInfo_);
  createParam(ANF2ReconfigString,        asynParamInt32,       &ANF2Reconfig_);

  numModules_ = numModules;
  axesPerModule_ = axesPerModule;
  numAxes_ = numModules * axesPerModule;
  
  for (j=0; j<numAxes_; j++) {
    /* Connect to ANF2 controller */
    for (i=0; i<MAX_INPUT_REGS; i++) {
      status = pasynInt32SyncIO->connect(ANF2InPortName, i+j*AXIS_REG_OFFSET, &pasynUserInReg_[j][i], NULL);
    }
    status = pasynInt32ArraySyncIO->connect(ANF2OutPortName, j*AXIS_REG_OFFSET, &pasynUserOutReg_[j], NULL);
  }
  if (status) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
      "%s: cannot connect to ANF2 controller\n",
      functionName);
  }

  /* Create the poller thread for this controller (do 2 forced-fast polls)
   * NOTE: at this point the axis objects don't yet exist, but the poller tolerates this */
  //startPoller(movingPollPeriod, idlePollPeriod, 2);
}


/** Creates a new ANF2Controller object.
  * Configuration command, called directly or from iocsh
  * \param[in] portName          The name of the asyn port that will be created for this driver
  * \param[in] ANF2InPortName    The name of the drvAsynIPPPort that was created previously to connect to the ANF2 controller 
  * \param[in] ANF2OutPortName   The name of the drvAsynIPPPort that was created previously to connect to the ANF2 controller 
  * \param[in] numModules        The number of modules on the controller stack 
  * \param[in] axesPerModule     The number of axes per module (ANF1=1, ANF2=2)
  */
extern "C" int ANF2CreateController(const char *portName, const char *ANF2InPortName, const char *ANF2OutPortName, 
                                      int numModules, int axesPerModule)
{
  // Enforce max values
  if (numModules > MAX_MODULES) {
    numModules = MAX_MODULES;
  }
  if (axesPerModule > MAX_AXES_PER_MODULE) {
    axesPerModule = MAX_AXES_PER_MODULE;
  }

  /*
  ANF2Controller *pANF2Controller
    = new ANF2Controller(portName, ANF2InPortName, ANF2OutPortName, numModules, axesPerModule);
  pANF2Controller = NULL;
  */
  new ANF2Controller(portName, ANF2InPortName, ANF2OutPortName, numModules, axesPerModule);
  return(asynSuccess);
}

/** Starts the poller for a given controller
  * \param[in] ANF2Name          The name of the asyn port that for the controller
  * \param[in] movingPollPeriod  The time in ms between polls when any axis is moving
  * \param[in] idlePollPeriod    The time in ms between polls when no axis is moving 
  */
extern "C" asynStatus ANF2StartPoller(const char *ANF2Name, int movingPollPeriod, int idlePollPeriod)
{
  ANF2Controller *pC;
  static const char *functionName = "ANF2StartPoller";

  pC = (ANF2Controller*) findAsynPortDriver(ANF2Name);
  if (!pC) {
    printf("%s:%s: Error port %s not found\n",
           driverName, functionName, ANF2Name);
    return asynError;
  }
  
  pC->lock();
  pC->doStartPoller(movingPollPeriod/1000.0, idlePollPeriod/1000.0);
  pC->unlock();
  return asynSuccess;
}

void ANF2Controller::doStartPoller(double movingPollPeriod, double idlePollPeriod)
{
  //
  movingPollPeriod_ = movingPollPeriod;
  idlePollPeriod_ = idlePollPeriod;

  // 
  startPoller(movingPollPeriod_, idlePollPeriod_, 2);
}


/** Reports on status of the driver
  * \param[in] fp The file pointer on which report information will be written
  * \param[in] level The level of report detail desired
  *
  * If details > 0 then information is printed about each axis.
  * After printing controller-specific information it calls asynMotorController::report()
  */
void ANF2Controller::report(FILE *fp, int level)
{
  int i, j;
  ANF2Axis *pAxis;
  
  fprintf(fp, "====================================\n");
  fprintf(fp, "ANF2 motor driver:\n");
  fprintf(fp, "    asyn port: %s\n", this->portName);
  fprintf(fp, "    num axes: %i\n", numAxes_);
  fprintf(fp, "    axes created: %i\n", axesCreated_);
  fprintf(fp, "    moving poll period: %lf\n", movingPollPeriod_);
  fprintf(fp, "    idle poll period: %lf\n", idlePollPeriod_);
  
  for (j=0; j<numAxes_; j++) {
    fprintf(fp, "========\n");
    fprintf(fp, "AXIS #%i\n", j);
    fprintf(fp, "========\n");
  
    pAxis = getAxis(j);
    pAxis->getInfo();
  
    /*for (i=0; i<MAX_INPUT_REGS; i++) {
       fprintf(fp, "    reg %i, pasynUserInReg_[%i][%i]=0x%x\n", i, j, i, pasynUserInReg_[j][i]);
    }*/
  }
  // Call the base class method
  asynMotorController::report(fp, level);
  fprintf(fp, "====================================\n");
}

/** Returns a pointer to an ANF2Axis object.
  * Returns NULL if the axis number encoded in pasynUser is invalid.
  * \param[in] pasynUser asynUser structure that encodes the axis index number. */
ANF2Axis* ANF2Controller::getAxis(asynUser *pasynUser)
{
//  ?  return static_cast<ANF2Axis*>(asynMotorController::getAxis(pANF2Axis methodsasynUser));
  return static_cast<ANF2Axis*>(asynMotorController::getAxis(pasynUser));
}

/** Returns a pointer to an ANF2Axis object.
  * Returns NULL if the axis number encoded in pasynUser is invalid.
  * \param[in] No Axis index number. */
ANF2Axis* ANF2Controller::getAxis(int axisNo)
{
  return static_cast<ANF2Axis*>(asynMotorController::getAxis(axisNo));
}

/** Called when asyn clients call pasynInt32->write().
  * Extracts the function and axis number from pasynUser.
  * Sets the value in the parameter library (?)
  *
  * If the function is ANF2Jerk_ it sets the jerk value in the controller.
  * Calls any registered callbacks for this pasynUser->reason and address.
  *
  * For all other functions it calls asynMotorController::writeInt32.
  * \param[in] pasynUser asynUser structure that encodes the reason and address.
  * \param[in] value Value to write. */
asynStatus ANF2Controller::writeInt32(asynUser *pasynUser, epicsInt32 value)
{
  int function = pasynUser->reason;
  asynStatus status = asynSuccess;
  ANF2Axis *pAxis = getAxis(pasynUser);
  static const char *functionName = "writeInt32";
  
  /* Set the parameter and readback in the parameter library. */
  status = setIntegerParam(pAxis->axisNo_, function, value);
  
  if (function == ANF2GetInfo_)
  {
    // Only get info when value is 1
    if (value == 1) {
        printf("ANF2Controller:writeInt32: Getting info for axis = %d\n", pAxis->axisNo_);
        pAxis->getInfo();

    }
  } else if (function == ANF2Reconfig_)
  {
    // reconfig regardless of the value
    pAxis->reconfig(value);
  } else {
  // Call base class method
    status = asynMotorController::writeInt32(pasynUser, value);
  }
  
  // Call base class method
  status = asynMotorController::writeInt32(pasynUser, value);
  
  /* Do callbacks so higher layers see any changes */
  pAxis->callParamCallbacks();
  if (status)
    asynPrint(pasynUser, ASYN_TRACE_ERROR, 
        "%s:%s: error, status=%d function=%d, value=%d\n", 
        driverName, functionName, status, function, value); 
  else
    asynPrint(pasynUser, ASYN_TRACEIO_DRIVER, 
        "%s:%s: function=%d, value=%d\n", 
        driverName, functionName, function, value); 
  return status;
}

asynStatus ANF2Controller::writeReg32Array(int axisNo, epicsInt32* output, int nElements, double timeout)
{
  asynStatus status;
  
  asynPrint(this->pasynUserSelf, ASYN_TRACEIO_DRIVER,"writeReg32Array: writing %d elements starting for axis %d\n", nElements, axisNo);  
  status = pasynInt32ArraySyncIO->write(pasynUserOutReg_[axisNo], output, nElements, timeout);
 
  return status;
}

asynStatus ANF2Controller::readReg16(int axisNo, int axisReg, epicsInt32 *input, double timeout)
{
  asynStatus status;
  
  //printf("axisReg = %d\n", axisReg);
  asynPrint(this->pasynUserSelf, ASYN_TRACEIO_DRIVER,"readReg16 reg = %d\n", axisReg);
  status = pasynInt32SyncIO->read(pasynUserInReg_[axisNo][axisReg], input, timeout);
                                  
  return status ;
}

asynStatus ANF2Controller::readReg32(int axisNo, int axisReg, epicsInt32 *combo, double timeout)
{
  asynStatus status;
  epicsInt32 lowerWord32, upperWord32;        // only have pasynInt32SyncIO, not pasynInt16SyncIO ,
  
  //printf("calling readReg16\n");
  status = readReg16(axisNo, axisReg, &upperWord32, timeout);    //get Upper Word

  axisReg++;
  status = readReg16(axisNo, axisReg, &lowerWord32, timeout);  //get Lower Word

  *combo = NINT((upperWord32 << 16) | lowerWord32);
  
  return status ;
}


// ANF2Axis methods Here
// These are the ANF2Axis methods

/** Creates a new ANF2Axis object.
  * \param[in] pC Pointer to the ANF2Controller to which this axis belongs. 
  * \param[in] axisNo Index number of this axis, range 0 to pC->numAxes_-1.
  * 
  * Initializes register numbers, etc.
  */
ANF2Axis::ANF2Axis(ANF2Controller *pC, int axisNo, epicsInt32 config, epicsInt32 baseSpeed, epicsInt32 homingTimeout)
  : asynMotorAxis(pC, axisNo),
    pC_(pC)	
{ 
  int status;

  axisNo_ = axisNo;
  //this->axisNo_ = axisNo;
  baseSpeed_ = baseSpeed;
  homingTimeout_ = homingTimeout;

  // These registers will always be zero
  zeroRegisters(zeroReg_);
  
  status = pasynInt32SyncIO->connect(pC_->inputDriver_, axisNo_*AXIS_REG_OFFSET, &pasynUserForceRead_, "MODBUS_READ");
  if (status) {
    //printf("%s:%s: Error, unable to connect pasynUserForceRead_ to Modbus input driver %s\n", pC_->inputDriver_, pC_->functionName, myModbusInputDriver);
    printf("%s: Error, unable to connect pasynUserForceRead_ to Modbus input driver\n", pC_->inputDriver_);	
  }
  //printf("ANF2Axis::ANF2Axis : pasynUserForceRead_->reason=%d\n", pasynUserForceRead_->reason);

  /* TODO:
   *  test config bits and set status bits to prevent methods from sending commands that would generate errors
   *  reduce the sleeps to see which ones are necessary
   */

  epicsThreadSleep(0.1);

  // Read data that is likely to be stale
  //getInfo();

  // Clear the command/configuration register (a good thing to do but doesn't appear to be necessary)
  //status = pC_->writeReg32Array(axisNo_, zeroReg_, 5, DEFAULT_CONTROLLER_TIMEOUT);

  // Delay
  //epicsThreadSleep(0.05);

  // These registers will always have the last config that was sent to the controller
  zeroRegisters(confReg_);

  // Send the configuration (array) 
  // assemble the configuration bits; set the start speed to a non-zero value (100), which is required for the configuration to be accepted
  confReg_[CONFIGURATION] = config;
  confReg_[BASE_SPEED] = baseSpeed;
  confReg_[HOME_TIMEOUT] = homingTimeout << 16;
  
  // Write all the registers
  status = pC_->writeReg32Array(axisNo_, confReg_, 5, DEFAULT_CONTROLLER_TIMEOUT);

  // Delay
  epicsThreadSleep(0.05);

  // Read the configuration? Or maybe the command registers?
  //getInfo();
  
  // Parse the configuration (mostly for asynReport purposes)
  // MSW
  CaptInput_ = (config & (0x1 << 16)) >> 16;
  ExtInput_ = (config & (0x2 << 16)) >> 17;
  HomeInput_ = (config & (0x4 << 16)) >> 18;
  CWInput_ = (config & (0x18 << 16)) >> 19;
  CCWInput_ = (config & (0x60 << 16)) >> 21;
  BHPO_ = (config & (0x80 << 16)) >> 23;
  QuadEnc_ = (config & (0x100 << 16)) >> 24;
  DiagFbk_ = (config & (0x200 << 16)) >> 25;
  OutPulse_ = (config & (0x400 << 16)) >> 26;
  HomeOp_ = (config & (0x800 << 16)) >> 27; 
  CardAxis_ = (config & (0x4000 << 16)) >> 30;
  OpMode_ = (epicsUInt32)(config & (0x8000 << 16)) >> 31;
  // LSW
  CaptInputAS_ = config & 0x1;
  ExtInputAS_ = (config & 0x2) >> 1;
  HomeInputAS_ = (config & 0x4) >> 2;
  CWInputAS_ = (config & 0x8) >> 3;
  CCWInputAS_ = (config & 0x10) >> 4;
  
  // Only allow UEIP to be used if the axis is configured to have a quadrature encoder
  if (QuadEnc_ != 0x0) {
    setIntegerParam(pC_->motorStatusHasEncoder_, 1);
  } else {
    setIntegerParam(pC_->motorStatusHasEncoder_, 0);
  }
  
  // set position to 0 to clear the "position invalid" status that results from configuring the axis
  setPosition(0);
  // Tell asynMotor device support the position is zero so that autosave will restore the saved position (doesn't appear to be necessary)
  //setDoubleParam(pC_->motorPosition_, 0.0);
  
  // Delay
  //epicsThreadSleep(1.0);
  
  // Read the command registers
  //getInfo();
  
  // Tell the driver the axis has been created
  pC_->axesCreated_ += 1;
  
  //epicsThreadSleep(1.0);
}

/*
 Configuration Bits:
 0x1  - Caputure Input (0 = Disabled, 1 = Enabled)
 0x2  - External Input (0 = Disabled, 1 = Enabled)
 0x4  - Home Input (0 = Disabled, 1 = Enabled)
 0x8  - 

 */

extern "C" asynStatus ANF2CreateAxis(const char *ANF2Name,  /* specify which controller by port name */
                         int axis,                          /* axis number 0-1 */
                         const char *hexConfig,             /* desired configuration in hex */
                         epicsInt32 baseSpeed,              /* base speed */
                         epicsInt32 homingTimeout)          /* homing timeout */
{
  ANF2Controller *pC;
  epicsInt32 config;
  static const char *functionName = "ANF2CreateAxis";

  pC = (ANF2Controller*) findAsynPortDriver(ANF2Name);
  if (!pC) {
    printf("%s:%s: Error port %s not found\n",
           driverName, functionName, ANF2Name);
    return asynError;
  }

  errno = 0;
  config = strtoul(hexConfig, NULL, 16);
  if (errno != 0) {
    printf("%s:%s: Error invalid config=%s\n",
           driverName, functionName, hexConfig);
    return asynError;
  } else {
    printf("%s:%s: Config=0x%x\n",
           driverName, functionName, config);
  }
  
  // baseSpeed is steps/second (1-1,000,000)
  if (baseSpeed < 1) {
    baseSpeed = 1;
  }
  if (baseSpeed > 1000000) {
    baseSpeed = 1000000;
  }
  
  // homingTimeout is seconds (0-300)
  if (homingTimeout < 0) {
    homingTimeout = 0;
  }
  if (homingTimeout > 300) {
    homingTimeout = 300;
  }
  
  pC->lock();
  new ANF2Axis(pC, axis, config, baseSpeed, homingTimeout);
  pC->unlock();
  return asynSuccess;
}

void ANF2Axis::zeroRegisters(epicsInt32 *reg)
{
  int i;
  
  for(i=0; i<5; i++)
  {
    reg[i] = 0x0;
  }
}

void ANF2Axis::getInfo()
{
  asynStatus status;
  int i;
  epicsInt32 read_val;
  
  // For a read (not sure why this is necessary)
  status = pasynInt32SyncIO->write(pasynUserForceRead_, 1, DEFAULT_CONTROLLER_TIMEOUT);

  printf("Configuration for axis %i:\n", axisNo_);
  printf("  Base Speed: %i\tHoming Timeout: %i\n", baseSpeed_, homingTimeout_);
  printf("  Capture Input: %i\tActive State: %i\n", CaptInput_, CaptInputAS_);
  printf("  External Input: %i\tActive State: %i\n", ExtInput_, ExtInputAS_);
  printf("  Home Input: %i\tActive State: %i\n", HomeInput_, HomeInputAS_);
  printf("  CW Input: %i\tActive State: %i\n", CWInput_, CWInputAS_);
  printf("  CCW Input: %i\tActive State: %i\n", CCWInput_, CCWInputAS_);
  printf("  Backplane Home Proximity Operation: %i\n", BHPO_);
  printf("  Quadrature Encoder: %i\n", QuadEnc_);
  printf("  Diagnostic Feedback: %i\n", DiagFbk_);
  printf("  Output Pulse Type: %i\n", OutPulse_);
  printf("  Home Operation: %i\n", HomeOp_);
  printf("  Card Axis: %i\n", CardAxis_);
  printf("  Operation Mode for Axis: %i\n", OpMode_);

  printf("Registers for axis %i:\n", axisNo_);

  for( i=0; i<MAX_INPUT_REGS; i++)
  {
    status = pC_->readReg16(axisNo_, i, &read_val, DEFAULT_CONTROLLER_TIMEOUT);
    //printf("  status=%d, register=%i, val=0x%x\n", status, i, read_val);
    printf("  register=%i, val=0x%x\n", i, read_val);
  }
}

void ANF2Axis::reconfig(epicsInt32 value)
{
  asynStatus status;
  epicsInt32 confReg[5];
  
  // TODO: modify this to use the base speed from the parameter, and instead accept a string for a new config
  
  printf("Reconfiguring axis %i\n", axisNo_);

  // Clear the command/configuration register
  status = pC_->writeReg32Array(axisNo_, zeroReg_, 5, DEFAULT_CONTROLLER_TIMEOUT);
  
  // Construct the new config
  zeroRegisters(confReg);
  confReg[CONFIGURATION] = 0x86000000;
  confReg[BASE_SPEED] = 0x00000064;
  //confReg[HOME_TIMEOUT] = 0x0;
  //confReg[CONFIG_REG_3] = 0x0;
  //confReg[CONFIG_REG_4] = 0x0;

  epicsThreadSleep(0.05);
  getInfo();

  // Send the new config
  status = pC_->writeReg32Array(axisNo_, confReg_, 5, DEFAULT_CONTROLLER_TIMEOUT);

  epicsThreadSleep(0.05);
  getInfo();

  // Set the position to clear the invalid position error  
  setPosition(value);

  epicsThreadSleep(0.05);
  getInfo();
}


/** Reports on status of the axis
  * \param[in] fp The file pointer on which report information will be written
  * \param[in] level The level of report detail desired
  *
  * After printing device-specific information calls asynMotorAxis::report()
  */
void ANF2Axis::report(FILE *fp, int level)
{
  // TODO: make this more useful

  if (level > 0) {
    fprintf(fp, "  axis %d\n", axisNo_);
    fprintf(fp, "    this->axisNo_ %i\n", this->axisNo_);
    fprintf(fp, "    this->config_ %x\n", this->config_);
    fprintf(fp, "    config_ %x\n", config_);
  }

  // Call the base class method
  asynMotorAxis::report(fp, level);
}

// SET VEL & ACCEL
asynStatus ANF2Axis::sendAccelAndVelocity(double acceleration, double velocity) 
{
  // static const char *functionName = "ANF2::sendAccelAndVelocity";

  // ANF2 speed range is 1 to 1,000,000 steps/sec
  if (velocity > 1000000.0) {
    velocity = 1000000.0;
  }
  if (velocity < 1.0) {
    velocity = 1.0;
  }

  // Set the velocity register
  motionReg_[2] = NINT(velocity);

  // ANF2 acceleration range 1 to 2000 steps/ms/sec
  // Therefore need to limit range received by motor record from 1000 to 2e6 steps/sec/sec
  if (acceleration < 1000.0) {
    //printf("Acceleration is < 1000: %lf\n", acceleration);
    acceleration = 1000.0;
  }
  if (acceleration > 2000000.0) {
    //printf("Acceleration is > 2000: %lf\n", acceleration);
    acceleration = 2000000.0;
  }
  
  // Set the accel/decel register
  motionReg_[3] = (NINT(acceleration/1000.0) << 16) | (NINT(acceleration/1000.0));
    
  return asynSuccess;
}

/*
 * This driver only sets the base speed at initialization when the configuration is sent.
 * It is possible that the base speed (VBAS) in the motor record is inconsistent with the 
 * base speed set at initialization, since there is no way for an asyn motor driver to force
 * the base speed to be reset when a user changes it. The resulting acceleration calculated 
 * by the motor record is likely to be incorrect.  The following method calculates the 
 * acceleration that will give the correct acceleration time (ACCL) for the base speed that
 * was specified at initialization.
 */
double ANF2Axis::correctAccel(double minVelocity, double maxVelocity, double acceleration)
{
  double accelTime;
  double newAccel;
  
  accelTime = (maxVelocity - minVelocity) / acceleration;
  newAccel = (maxVelocity - (double)baseSpeed_) / accelTime;

  printf("old acceleration = %lf\n", acceleration);
  printf("new acceleration = %lf\n", newAccel);
  
  return newAccel;
}


// MOVE
asynStatus ANF2Axis::move(double position, int relative, double minVelocity, double maxVelocity, double acceleration)
{
  asynStatus status;
  epicsInt32 distance;
  
  //printf(" ** ANF2Axis::move called, relative = %d, axisNo_ = %i\n", relative, this->axisNo_);

  // Clear the command/configuration register
  status = pC_->writeReg32Array(axisNo_, zeroReg_, 5, DEFAULT_CONTROLLER_TIMEOUT);

  epicsThreadSleep(0.05);

  // Clear the motition registers
  zeroRegisters(motionReg_);

  // Correct the acceleration
  acceleration = correctAccel(minVelocity, maxVelocity, acceleration);

  // This sets indices 2 & 3 of motionReg_
  status = sendAccelAndVelocity(acceleration, maxVelocity);
  
  if (relative) {
    //printf(" ** relative move called\n");

    distance = NINT(position);
  
    // Set position and cmd registers
    motionReg_[1] = NINT(position);
    motionReg_[0] = 0x2 << 16;
  
  } else {
    // absolute
    //printf(" ** absolute move called\n");

    distance = NINT(position);
    //printf(" ** distance = %d\n", distance);
  
    // Set position and cmd registers
    motionReg_[1] = NINT(position);
    motionReg_[0] = 0x1 << 16;
  }
  
  // The final registers are zero for absolute and relative moves
  motionReg_[4] = 0x0;
  
  // Write all the registers atomically
  // The number of elements refers to the number of epicsInt32s registers_
  status = pC_->writeReg32Array(axisNo_, motionReg_, 5, DEFAULT_CONTROLLER_TIMEOUT);
  
  // Delay the first status read, give the controller some time to return moving status
  epicsThreadSleep(0.05);
  return status;
}

// HOME (needs work)
asynStatus ANF2Axis::home(double minVelocity, double maxVelocity, double acceleration, int forwards)
{
  asynStatus status;
  // static const char *functionName = "ANF2Axis::home";

  // Clear the command/configuration register
  status = pC_->writeReg32Array(axisNo_, zeroReg_, 5, DEFAULT_CONTROLLER_TIMEOUT);

  epicsThreadSleep(0.05);

  // Clear the motition registers
  zeroRegisters(motionReg_);

  // Correct the acceleration
  acceleration = correctAccel(minVelocity, maxVelocity, acceleration);

  // This sets indices 2 & 3 of motionReg_
  status = sendAccelAndVelocity(acceleration, maxVelocity);

  if (forwards) {
    printf(" ** HOMING FORWARDS **\n");
    motionReg_[0] = 0x20 << 16;
  } else {
    printf(" ** HOMING REVERSE **\n");
    motionReg_[0] = 0x40 << 16;
  }

  // Write all the registers atomically
  status = pC_->writeReg32Array(axisNo_, motionReg_, 5, DEFAULT_CONTROLLER_TIMEOUT);

  return status;
}

// JOG
asynStatus ANF2Axis::moveVelocity(double minVelocity, double maxVelocity, double acceleration)
{
  asynStatus status;
  int velo, distance;
  static const char *functionName = "ANF2Axis::moveVelocity";

  asynPrint(pasynUser_, ASYN_TRACE_FLOW,
    "%s: minVelocity=%f, maxVelocity=%f, acceleration=%f\n",
    functionName, minVelocity, maxVelocity, acceleration);

  /*
  // The jog command doesn't work well.  It seems to require stopping with an immediate stop,
  // but immediate stops cause problems for normal moves.  
  
  // Clear the command/configuration register
  status = pC_->writeReg32Array(axisNo_, zeroReg_, 5, DEFAULT_CONTROLLER_TIMEOUT);

  epicsThreadSleep(0.05);

  // Clear the motition registers
  zeroRegisters(motionReg_);

  // Note: the jog acceleration doesn't need to be corrected; the JAR field has units of egu/s/s

  if (maxVelocity > 0.0) {
    //printf(" ** positive jog called\n");

    // Set cmd register
    motionReg_[0] = 0x80 << 16;
    
    // Do nothing to the velocity
  
  } else {
    //printf(" ** negative jog called\n");

    // Set cmd register
    motionReg_[0] = 0x100 << 16;

    // ANF2 only accepts speeds > 0
    maxVelocity = fabs(maxVelocity);
  }

  // This sets indices 2 & 3 of motionReg_
  status = sendAccelAndVelocity(acceleration, maxVelocity);
  
  // Write all the registers atomically
  status = pC_->writeReg32Array(axisNo_, motionReg_, 5, DEFAULT_CONTROLLER_TIMEOUT);
  */

  velo = NINT(fabs(maxVelocity));

  // Simulate a jog like the ANG1 driver does. Move 1 million steps
  distance = 1000000;
  if (maxVelocity > 0.) {
    // This is a positive move in ANF2 coordinates
    //printf(" ** relative move (JOG pos) called\n");
    status = move(distance, 0, minVelocity, velo, acceleration);
  } else {
    // This is a negative move in ANF2 coordinates
    //printf(" ** relative move (JOG neg) called\n");
    status = move((distance * -1.0), 0, minVelocity, velo, acceleration);
  }
  
  // Delay the first status read, give the controller some time to return moving status
  epicsThreadSleep(0.05);
  return status;
}


// STOP
asynStatus ANF2Axis::stop(double acceleration)
{
  asynStatus status;
  epicsInt32 stopReg;
  //static const char *functionName = "ANF2Axis::stop";
  
  printf("\n  STOP \n\n");
  
  // The stop commands ignore all 32-bit registers beyond the first
  
  // Clear the command/configuration register
  status = pC_->writeReg32Array(axisNo_, zeroReg_, 1, DEFAULT_CONTROLLER_TIMEOUT);

  // Immediate stop works well with jogs, but breaks the first normal move
  //stopReg = 0x10 << 16;     // Immediate stop
  // Hold move works very well with normal moves
  stopReg = 0x4 << 16;      // Hold move
  
  //
  status = pC_->writeReg32Array(axisNo_, &stopReg, 1, DEFAULT_CONTROLLER_TIMEOUT);

  // Clear the command/configuration register
  //status = pC_->writeReg32Array(axisNo_, zeroReg_, 5, DEFAULT_CONTROLLER_TIMEOUT);
  
  return status;
}

// SET
asynStatus ANF2Axis::setPosition(double position)
{
  asynStatus status;
  epicsInt32 set_position;
  epicsInt32 posReg[5];
  //static const char *functionName = "ANF2Axis::setPosition";
  
  printf("setPosition(%lf) for axisNo_=%i\n", position, axisNo_);
  
  // Clear the command/configuration register
  status = pC_->writeReg32Array(axisNo_, zeroReg_, 5, DEFAULT_CONTROLLER_TIMEOUT);

  epicsThreadSleep(0.1);

  set_position = NINT(position);

  zeroRegisters(posReg);
  posReg[0] = 0x200 << 16;
  posReg[1] = set_position;
  //posReg[2] = 0x0;
  //posReg[3] = 0x0;
  //posReg[4] = 0x0;

  // Write all the registers atomically
  status = pC_->writeReg32Array(axisNo_, posReg, 5, DEFAULT_CONTROLLER_TIMEOUT);

  epicsThreadSleep(0.20);

  // The ANG1 driver does this; do we need to?
  // Clear the command/configuration register
  //status = pC_->writeReg32Array(axisNo_, zeroReg_, 5, DEFAULT_CONTROLLER_TIMEOUT);

  return status;
}

// ENABLE TORQUE
asynStatus ANF2Axis::setClosedLoop(bool closedLoop)
{
  //asynStatus status;
  //epicsInt32 clReg[5];
  //static const char *functionName = "ANF2Axis::setClosedLoop";

  // The ANF2 doesn't have a closed-loop enable/disable command, so do nothing.
  // The configuration of an axis:
  //   * can be changed so that an axis is disabled, but that doesn't disable torque
  //   * can be changed to disable the use of encoder inputs, but that isn't currently allowed on-the-fly

  /*printf(" ** setClosedLoop called \n");
  if (closedLoop) {
    printf("setting enable true\n");
  
    setIntegerParam(pC_->motorStatusPowerOn_, 1);
  } else {
    printf("setting disable false\n");
    setIntegerParam(pC_->motorStatusPowerOn_, 0);
  }
  return status;*/

  return asynSuccess;
}

// POLL
/** Polls the axis.
  * This function reads motor position, limit status, home status, and moving status
  * It calls setIntegerParam() and setDoubleParam() for each item that it polls,
  * and then calls callParamCallbacks() at the end.
  * \param[out] moving A flag that is set indicating that the axis is moving (true) or done (false). */
asynStatus ANF2Axis::poll(bool *moving)
{ 
  int done;
  int limit;
  int enabled;
  double position;
  double encPosition;
  asynStatus status;
  epicsInt32 read_val;  // don't use a pointer here.  The _address_ of read_val should be passed to the read function.
  
  // Don't do any polling until ALL the axes have been created; this ensures that we don't interpret the configuration values as command values
  if (pC_->axesCreated_ != pC_->numAxes_) {
    *moving = false;
    return asynSuccess;
  }
  
  //getInfo();
  
  // Force a read operation
  //printf(" . . . . . Calling pasynInt32SyncIO->write\n");
  //printf("Calling pasynInt32SyncIO->write(pasynUserForceRead_, 1, TIMEOUT), pasynUserForceRead_->reason=%d\n", pasynUserForceRead_->reason);
  status = pasynInt32SyncIO->write(pasynUserForceRead_, 1, DEFAULT_CONTROLLER_TIMEOUT);
  //printf(" . . . . . status = %d\n", status);
  // if status goto end

  //getInfo();

  // Read the current motor position
  // 
  //readReg32(int reg, epicsInt32 *combo, double timeout)
  status = pC_->readReg32(axisNo_, POS_RD_UPR, &read_val, DEFAULT_CONTROLLER_TIMEOUT);
  //printf("ANF2Axis::poll:  Motor position raw: %d\n", read_val);
  position = (double) read_val;
  setDoubleParam(pC_->motorPosition_, position);
  //printf("ANF2Axis::poll:  Motor #%i position: %f\n", axisNo_, position);

  //  TODO: read encoder position
  status = pC_->readReg32(axisNo_, EN_POS_UPR, &read_val, DEFAULT_CONTROLLER_TIMEOUT);
  //printf("ANF2Axis::poll:  Motor encoder position raw: %d\n", read_val);
  encPosition = (double) read_val;
  setDoubleParam(pC_->motorEncoderPosition_, encPosition);
  //printf("ANF2Axis::poll:  Motor #%i encoder position: %f\n", axisNo_, encPosition);

  // Read the moving status of this motor
  //
  status = pC_->readReg16(axisNo_, STATUS_1, &read_val, DEFAULT_CONTROLLER_TIMEOUT);
  //printf("status 1 is 0x%X\n", read_val);
  
  // Done logic
  done = ((read_val & 0x8) >> 3);  // status word 1 bit 3 set to 1 when the motor is not in motion.
  setIntegerParam(pC_->motorStatusDone_, done);
  *moving = done ? false:true;
  //printf("done is %d\n", done);

  // Check for enable/disable (not actually the torque status) and set accordingly.
  // Enable/disable is determined by the configuration and it isn't obvious why one would disable an axis.
  enabled = (read_val & 0x4000);
  if (enabled)
    setIntegerParam(pC_->motorStatusPowerOn_, 1);
  else
    setIntegerParam(pC_->motorStatusPowerOn_, 0);
  
  // Read the limit status
  //
  status = pC_->readReg16(axisNo_, STATUS_2, &read_val, DEFAULT_CONTROLLER_TIMEOUT);
  //printf("status 2 is 0x%X\n", read_val);  
  
  limit  = (read_val & 0x8);    // a cw limit has been reached
  setIntegerParam(pC_->motorStatusHighLimit_, limit);
  //printf("+limit %d\n", limit);
    if (limit) {   // reset error and set position so we can move off of the limit
    // Reset error
	setClosedLoop(1);
	// Reset position
	//printf(" Reset Position\n");
	setPosition(position);
  }

  limit  = (read_val & 0x10);    // a ccw limit has been reached
  setIntegerParam(pC_->motorStatusLowLimit_, limit);
  //printf("-limit %d\n", limit);
  if (limit) {   // reset error and set position so we can move off of the limit
    // Reset error
	setClosedLoop(1);
	// Reset position
	setPosition(position);
  }

  // test for home
  
  // Should be in init routine?  Allows CNEN to be used.
  setIntegerParam(pC_->motorStatusGainSupport_, 1);

  // Notify asynMotorController polling routine that we're ready
  callParamCallbacks();

  return status;
}

/** Code for iocsh registration */

/* ANF2CreateController */
static const iocshArg ANF2CreateControllerArg0 = {"Port name", iocshArgString};
static const iocshArg ANF2CreateControllerArg1 = {"ANF2 In port name", iocshArgString};
static const iocshArg ANF2CreateControllerArg2 = {"ANF2 Out port name", iocshArgString};
static const iocshArg ANF2CreateControllerArg3 = {"Number of modules", iocshArgInt};
static const iocshArg ANF2CreateControllerArg4 = {"Axes per module", iocshArgInt};
static const iocshArg * const ANF2CreateControllerArgs[] = {&ANF2CreateControllerArg0,
                                                             &ANF2CreateControllerArg1,
                                                             &ANF2CreateControllerArg2,
                                                             &ANF2CreateControllerArg3,
                                                             &ANF2CreateControllerArg4};
static const iocshFuncDef ANF2CreateControllerDef = {"ANF2CreateController", 5, ANF2CreateControllerArgs};
static void ANF2CreateControllerCallFunc(const iocshArgBuf *args)
{
  ANF2CreateController(args[0].sval, args[1].sval, args[2].sval, args[3].ival, args[4].ival);
}

/* ANF2StartPoller */
static const iocshArg ANF2StartPollerArg0 = {"Port name", iocshArgString};
static const iocshArg ANF2StartPollerArg1 = {"Moving poll period (ms)", iocshArgInt};
static const iocshArg ANF2StartPollerArg2 = {"Idle poll period (ms)", iocshArgInt};
static const iocshArg * const ANF2StartPollerArgs[] = {&ANF2StartPollerArg0,
                                                       &ANF2StartPollerArg1,
                                                       &ANF2StartPollerArg2};
static const iocshFuncDef ANF2StartPollerDef = {"ANF2StartPoller", 3, ANF2StartPollerArgs};
static void ANF2StartPollerCallFunc(const iocshArgBuf *args)
{
  ANF2StartPoller(args[0].sval, args[1].ival, args[2].ival);
}

/* ANF2CreateAxis */
static const iocshArg ANF2CreateAxisArg0 = {"Port name", iocshArgString};
static const iocshArg ANF2CreateAxisArg1 = {"Axis number", iocshArgInt};
static const iocshArg ANF2CreateAxisArg2 = {"Hex config", iocshArgString};
static const iocshArg ANF2CreateAxisArg3 = {"Base speed", iocshArgInt};
static const iocshArg ANF2CreateAxisArg4 = {"Homing timeout", iocshArgInt};
static const iocshArg * const ANF2CreateAxisArgs[] = {&ANF2CreateAxisArg0,
                                                             &ANF2CreateAxisArg1,
                                                             &ANF2CreateAxisArg2,
                                                             &ANF2CreateAxisArg3,
                                                             &ANF2CreateAxisArg4};
static const iocshFuncDef ANF2CreateAxisDef = {"ANF2CreateAxis", 5, ANF2CreateAxisArgs};
static void ANF2CreateAxisCallFunc(const iocshArgBuf *args)
{
  ANF2CreateAxis(args[0].sval, args[1].ival, args[2].sval, args[3].ival, args[4].ival);
}


static void ANF2Register(void)
{
  iocshRegister(&ANF2CreateControllerDef, ANF2CreateControllerCallFunc);
  iocshRegister(&ANF2StartPollerDef, ANF2StartPollerCallFunc);
  iocshRegister(&ANF2CreateAxisDef, ANF2CreateAxisCallFunc);
}

extern "C" {
epicsExportRegistrar(ANF2Register);
}
