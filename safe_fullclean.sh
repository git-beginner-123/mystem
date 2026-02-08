
#!/usr/bin/env bash
set -e

if [ -f sdkconfig ]; then
  cp sdkconfig sdkconfig.bak
fi

idf.py fullclean

if [ -f sdkconfig.bak ]; then
  cp sdkconfig.bak sdkconfig
fi

idf.py reconfigure
idf.py build

