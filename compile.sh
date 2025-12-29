podman run --rm -it \
  --user="$(id -u):$(id -g)" \
  --userns=keep-id \
  --volume="$PWD:$PWD" \
  --volume="/usr/include/linux/input-event-codes.h:/usr/include/linux/input-event-codes.h:ro" \
  --volume="/usr/include/linux/input-event-codes.h:/tc/arm-nickel-linux-gnueabihf/arm-nickel-linux-gnueabihf/sysroot/usr/include/linux/input-event-codes.h:ro" \
  --workdir="$PWD" \
  --env=HOME \
  --entrypoint=make \
  ghcr.io/pgaskin/nickeltc:1.0 \
  clean all koboroot
