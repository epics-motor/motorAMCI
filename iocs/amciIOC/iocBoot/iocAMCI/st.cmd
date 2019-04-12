#!../../bin/linux-x86_64/amci

< envPaths

cd "${TOP}"

## Register all support components
dbLoadDatabase "dbd/amci.dbd"
amci_registerRecordDeviceDriver pdbbase

cd "${TOP}/iocBoot/${IOC}"

## motorUtil (allstop & alldone)
dbLoadRecords("$(MOTOR)/db/motorUtil.db", "P=amci:")

##
#!< st.cmd.ANG1
< motor.cmd.ANF2

iocInit

< poller.cmd.ANF2

## motorUtil (allstop & alldone)
motorUtilInit("amci:")

# Boot complete
