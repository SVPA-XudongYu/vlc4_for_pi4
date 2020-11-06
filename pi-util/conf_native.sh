set -e
BASE=`pwd`

MC=`uname -m`
if [ "$MC" == "armv7l" ]; then
  ARM=armv7
elif [ "$MC" == "aarch64" ]; then
  ARM=arm64
else
  echo "Unkown machine name: $MC"
  exit 1
fi
OUT=$BASE/out/$ARM-rel

echo "Configuring in $OUT"
mkdir -p $OUT
cd $OUT
$BASE/configure  --disable-mmal --disable-vdpau --enable-gles2
echo "Configured in $OUT"

