#!/bin/bash

OPTS=-inpl

# Source/faustParametricEQ.h also carries settleControls(), added by hand: keep it when regenerating.
faust $OPTS -a arch.cpp -i -O ../Source -o faustParametricEQ.h -scn faustdsp -cn faustParametricEQ  parametric_eq.dsp

