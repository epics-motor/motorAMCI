TOP=../..

include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE
#=============================

# The following are used for debugging messages.
#!USR_CXXFLAGS += -DDEBUG

#=============================
# Build the IOC application

PROD_IOC = amci
# amci.dbd will be created and installed
DBD += amci.dbd

# amci.dbd will be made up from these files:
amci_DBD += base.dbd

# Include dbd files from all support applications:
#ifdef ASYN
amci_DBD += asyn.dbd
amci_DBD += drvAsynIPPort.dbd
#endif
amci_DBD += modbusSupport.dbd
amci_DBD += motorSupport.dbd
amci_DBD += AMCISupport.dbd

# Add all the support libraries needed by this IOC
amci_LIBS += AMCI
amci_LIBS += motor
amci_LIBS += modbus
#ifdef ASYN
amci_LIBS += asyn
#endif
#ifdef SNCSEQ
amci_LIBS += seq pv
#endif

# amci_registerRecordDeviceDriver.cpp derives from amci.dbd
amci_SRCS += amci_registerRecordDeviceDriver.cpp

# Build the main IOC entry point on workstation OSs.
amci_SRCS_DEFAULT += amciMain.cpp
amci_SRCS_vxWorks += -nil-

# Add support from base/src/vxWorks if needed
#amci_OBJS_vxWorks += $(EPICS_BASE_BIN)/vxComLibrary

# Finally link to the EPICS Base libraries
amci_LIBS += $(EPICS_BASE_IOC_LIBS)

#===========================

include $(TOP)/configure/RULES
#----------------------------------------
#  ADD RULES AFTER THIS LINE

