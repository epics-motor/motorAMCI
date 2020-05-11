# motorAMCI Releases

## __R1-0-1 (2020-05-11)__
R1-0-1 is a release based on the master branch.  

### Changes since R1-0

#### New features
* None

#### Modifications to existing features
* None

#### Bug fixes
* Commit [53b22b3](https://github.com/epics-motor/motorAMCI/commit/53b22b302ea43733bd713110b6ecdf1d83b60436): Include ``$(MOTOR)/modules/RELEASE.$(EPICS_HOST_ARCH).local`` instead of ``$(MOTOR)/configure/RELEASE``

## __R1-0 (2019-04-18)__
R1-0 is a release based on the master branch.  

### Changes since motor-6-11

motorAMCI is now a standalone module, as well as a submodule of [motor](https://github.com/epics-modules/motor)

#### New features
* motorAMCI can be built outside of the motor directory
* motorAMCI has a dedicated example IOC that can be built outside of motorAMCI

#### Modifications to existing features
* None

#### Bug fixes
* None
