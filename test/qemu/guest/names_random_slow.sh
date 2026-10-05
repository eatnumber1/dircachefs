#!/bin/sh
# dcfs step 9, slow tier: names_random.sh with 100,000 names, half in each direction.
RANDOM_COUNT=100000
export RANDOM_COUNT
. "$(dirname "$0")/names_random.sh"
