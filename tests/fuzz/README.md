# Persistence fuzz targets

Configure with Clang/libFuzzer:

```bash
cmake -S . -B build-fuzz \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DRTSENGINE_BUILD_TESTS=OFF \
  -DRTSENGINE_BUILD_EXAMPLES=OFF \
  -DRTSENGINE_BUILD_FUZZERS=ON
cmake --build build-fuzz --parallel
```

Run with the persistent seed corpus:

```bash
./build-fuzz/tests/fuzz/rts_run_save_envelope_fuzz \
  tests/fuzz/corpus/run_save_envelope \
  -artifact_prefix=fuzz-artifacts/ \
  -max_len=8388608
```

The target exercises both raw `RunSaveSchema` migration and the checksummed
`RunSaveEnvelopeCodec`. Any crashing input should be committed to the corpus
after minimizing it with libFuzzer.
