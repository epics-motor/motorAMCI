TOP=../..

include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE
#=============================

#=============================
# Build the IOC application

PROD_IOC = amci
# amci.dbd will be created and installed
DBD += amci.dbd

# amci.dbd will be made up from these files:
amci_DBD += base.dbd

# Include dbd files from all support applications:
#amci_DBD += xxx.dbd

# Add all the support libraries needed by this IOC
#amci_LIBS += xxx

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

