#!/bin/sh

rm -rf dcc-pkg
mkdir -p dcc-pkg

ldd build/dcc |\
  awk 'NF == 4 {print $3}; NF == 2 {print $1}' |\
  xargs cp -t dcc-pkg/

cp build/dcc dcc-pkg/
cp run.sh dcc-pkg/
