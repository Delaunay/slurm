# Makefile for accounting gather profile plugins

SUBDIRS = none
if BUILD_HDF5
SUBDIRS += hdf5
endif



if WITH_CURL
SUBDIRS += influxdb

if BUILD_NVML
SUBDIRS += mila
endif

endif
