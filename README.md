# motorAMCI
EPICS motor drivers for the following [AMCI](https://www.amci.com/) controllers:<br>
AMCI ANG1 Stepper Motor Controller/Driver, ANF1E/ANF1/ANF2E/ANF2 Stepper Motor Controllers. 

motorAMCI is a submodule of [motor](https://github.com/epics-modules/motor).  When motorAMCI is built in the ``motor/modules`` directory, no manual configuration is needed.

motorAMCI can also be built outside of motor by copying it's ``EXAMPLE_RELEASE.local`` file to ``RELEASE.local`` and defining the paths to ``MOTOR`` and itself.

motorAMCI contains an example IOC that is built if ``CONFIG_SITE.local`` sets ``BUILD_IOCS = YES``.  The example IOC can be built outside of driver module.

The example IOC, amciIOC, requires the EPICS [modbus](https://github.com/epics-modules/modbus) module.
