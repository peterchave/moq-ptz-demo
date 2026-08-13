# Install MOQ5 and dependancies

From the **project root** (`ptz-demo-2/`):

```sh
git clone https://github.com/openmoq/moq5
git clone https://github.com/openmoq/picoquic.git moq5/thirdparty/picoquic
git clone https://github.com/h2o/picotls.git    moq5/thirdparty/picotls

# picotls requires its own submodule (picotest)
cd moq5/thirdparty/picotls && git submodule update --init --recursive
cmake -B build -S . -DWITH_OPENSSL=ON -DBUILD_TESTING=OFF
cmake --build build
cd ../../..

cd moq5
cmake -B build -S . \
  -DMOQ_BUILD_ADAPTER_PICOQUIC=ON \
  -DMOQ_BUILD_PQ_THREADED=ON \
  -DMOQ_BUILD_SERVICE=ON \
  -DMOQ_BUILD_MSF=ON \
  -DMOQ_PICOQUIC_SOURCE_DIR="$(pwd)/thirdparty/picoquic"
cmake --build build
cd ..
```

After cloning, your layout should look like:
```
ptz-demo-2/
  moq5/
    thirdparty/
      picoquic/              ← picoquic source
      picotls/               ← picotls source + built
    build/
      _deps/picoquic/        ← picoquic static libs
      adapters/picoquic/     ← moq adapter libs
      core/                  ← moq-core
      service/               ← moq-service (endpoint + media sender)
  moq2ptz/
  vid2moq/
  ui/
```

This produces all static libraries that `vid2moq` and `moq2ptz` link against.
picotls libs are in `moq5/thirdparty/picotls/build/`; picoquic libs are in `moq5/build/_deps/picoquic/`.