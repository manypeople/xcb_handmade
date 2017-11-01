#!/bin/sh

## exit on failed command
#set -e
## error on trying to use unset variables
set -u

## prevent pushd/popd spamming output
pushd () {
    command pushd "$@" > /dev/null
}
popd () {
    command popd "$@" > /dev/null
}

## exit if not run from a directory called ray.
## For i'm sure a totally reasonable reason, a space
## is required between the strings and the [[, ]] and != 
if [[ `basename $PWD` != "ray" ]]; then exit; fi

../build/opt/ctime -begin ray.ctm

d="-g"
o="-Ofast"
CommonCompilerFlags="${o} -Wno-write-strings"
CommonCompilerFlags="-DRAY_LINUX=1 ${CommonCompilerFlags}"
CommonLinkerFlags="-lm"

mkdir -p ../rayBuild
pushd ../rayBuild

gcc ${CommonCompilerFlags} ../ray/ray.cpp ${CommonLinkerFlags} -o ray
## $? is the return code of the last command, which is zero if the last command
## had no errors.
error=$?
popd

../build/opt/ctime -end ray.ctm ${error}

## ((expression)) evaluates the expression inside. behold, shell scripting :/
if ((${error}!=0)); then exit; fi

mkdir -p data
pushd data
../../rayBuild/ray
eog test.bmp
popd
