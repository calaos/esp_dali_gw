# Host test for the DALI frame decoder

`dali_frame_decode.c` is the one part of this component that can be checked without a bus, and the
one most likely to be subtly wrong. It has no ESP-IDF dependency other than the layout of
`rmt_symbol_word_t`, which `rmt_symbol_shim.h` reproduces.

```bash
tools/docker-run.sh bash -c '
  gcc -std=c17 -Wall -Wextra -Werror -DDALI_DECODE_HOST_TEST \
      -Icomponents/dali/test -Icomponents/dali/src \
      -o /tmp/dali_decode_test \
      components/dali/src/dali_frame_decode.c components/dali/test/test_dali_frame_decode.c &&
  /tmp/dali_decode_test'
```

The test builds Te-level sequences from frame bits, run-length encodes them into RMT symbols the
way the peripheral would, and checks that the decoder gets the bits back: all 65536 16-bit forward
frames and all 256 backward frames in both line polarities, a set of 24-bit frames, ±10 % Te error,
the RMT end-of-capture marker, a truncated capture, undefined frame lengths, and garbage.

It is deliberately not part of `tests/host/`, which is wired up for the IDF-free application
components.
